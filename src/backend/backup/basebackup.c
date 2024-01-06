/*-------------------------------------------------------------------------
 *
 * basebackup.c
 *	  code for taking a base backup and streaming it to a standby
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/backup/basebackup.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "access/xlog_internal.h"
#include "access/xlogbackup.h"
#include "backup/backup_manifest.h"
#include "backup/basebackup.h"
#include "backup/basebackup_incremental.h"
#include "backup/basebackup_sink.h"
#include "backup/basebackup_target.h"
#include "catalog/pg_tablespace_d.h"
#include "commands/defrem.h"
#include "common/compression.h"
#include "common/file_perm.h"
#include "common/file_utils.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "pgstat.h"
#include "pgtar.h"
#include "port.h"
#include "postmaster/syslogger.h"
#include "postmaster/walsummarizer.h"
#include "replication/walsender.h"
#include "replication/walsender_private.h"
#include "storage/bufpage.h"
#include "storage/checksum.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/reinit.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/ps_status.h"
#include "utils/relcache.h"
#include "utils/resowner.h"

/*
 * How much data do we want to send in one CopyData message? Note that
 * this may also result in reading the underlying files in chunks of this
 * size.
 *
 * NB: The buffer size is required to be a multiple of the system block
 * size, so use that value instead if it's bigger than our preference.
 */
#define SINK_BUFFER_LENGTH			Max(32768, BLCKSZ)

typedef struct
{
	const char *label;
	bool		progress;
	bool		fastcheckpoint;
	bool		nowait;
	bool		includewal;
	// 增量启用状态
	bool		incremental;
	uint32		maxrate;
	bool		sendtblspcmapfile;
	/* 我们是否将档案发送到客户端或其他地方？ */
	bool		send_to_client;
	bool		use_copytblspc;
	BaseBackupTargetHandle *target_handle;
	backup_manifest_option manifest;
	pg_compress_algorithm compression;
	pg_compress_specification compression_specification;
	pg_checksum_type manifest_checksum_type;
} basebackup_options;

static int64 sendTablespace(bbsink *sink, char *path, Oid spcoid, bool sizeonly,
							struct backup_manifest_info *manifest,
							IncrementalBackupInfo *ib);
static int64 sendDir(bbsink *sink, const char *path, int basepathlen, bool sizeonly,
					 List *tablespaces, bool sendtblspclinks,
					 backup_manifest_info *manifest, Oid spcoid,
					 IncrementalBackupInfo *ib);
static bool sendFile(bbsink *sink, const char *readfilename, const char *tarfilename,
					 struct stat *statbuf, bool missing_ok,
					 Oid dboid, Oid spcoid, RelFileNumber relfilenumber,
					 unsigned segno,
					 backup_manifest_info *manifest,
					 unsigned num_incremental_blocks,
					 BlockNumber *incremental_blocks,
					 unsigned truncation_block_length);
static off_t read_file_data_into_buffer(bbsink *sink,
										const char *readfilename, int fd,
										off_t offset, size_t length,
										BlockNumber blkno,
										bool verify_checksum,
										int *checksum_failures);
static void push_to_sink(bbsink *sink, pg_checksum_context *checksum_ctx,
						 size_t *bytes_done, void *data, size_t length);
static bool verify_page_checksum(Page page, XLogRecPtr start_lsn,
								 BlockNumber blkno,
								 uint16 *expected_checksum);
static void sendFileWithContent(bbsink *sink, const char *filename,
								const char *content, int len,
								backup_manifest_info *manifest);
static int64 _tarWriteHeader(bbsink *sink, const char *filename,
							 const char *linktarget, struct stat *statbuf,
							 bool sizeonly);
static void _tarWritePadding(bbsink *sink, int len);
static void convert_link_to_directory(const char *pathbuf, struct stat *statbuf);
static void perform_base_backup(basebackup_options *opt, bbsink *sink,
								IncrementalBackupInfo *ib);
static void parse_basebackup_options(List *options, basebackup_options *opt);
static int	compareWalFileNames(const ListCell *a, const ListCell *b);
static ssize_t basebackup_read_file(int fd, char *buf, size_t nbytes, off_t offset,
									const char *filename, bool partial_read_ok);

/* Was the backup currently in-progress initiated in recovery mode? */
/* 当前正在进行的备份是否在恢复模式下启动 */
static bool backup_started_in_recovery = false;

/* Total number of checksum failures during base backup. */
static long long int total_checksum_failures;

/* Do not verify checksums. */
static bool noverify_checksums = false;

/*
 * Definition of one element part of an exclusion list, used for paths part
 * of checksum validation or base backups.  "name" is the name of the file
 * or path to check for exclusion.  If "match_prefix" is true, any items
 * matching the name as prefix are excluded.
 */
struct exclude_list_item
{
	const char *name;
	// 是否前缀匹配
	bool		match_prefix;
};

/*
 * The contents of these directories are removed or recreated during server
 * start so they are not included in backups.  The directories themselves are
 * kept and included as empty to preserve access permissions.
 *
 * Note: this list should be kept in sync with the filter lists in pg_rewind's
 * filemap.c.
 *
 * 排除其他目录
 */
static const char *const excludeDirContents[] =
{
	/*
	 * Skip temporary statistics files. PG_STAT_TMP_DIR must be skipped
	 * because extensions like pg_stat_statements store data there.
	 */
	PG_STAT_TMP_DIR,

	/*
	 * It is generally not useful to backup the contents of this directory
	 * even if the intention is to restore to another primary. See backup.sgml
	 * for a more detailed description.
	 */
	"pg_replslot",

	/* Contents removed on startup, see dsm_cleanup_for_mmap(). */
	PG_DYNSHMEM_DIR,

	/* Contents removed on startup, see AsyncShmemInit(). */
	"pg_notify",

	/*
	 * Old contents are loaded for possible debugging but are not required for
	 * normal operation, see SerialInit().
	 */
	"pg_serial",

	/* Contents removed on startup, see DeleteAllExportedSnapshotFiles(). */
	"pg_snapshots",

	/* Contents zeroed on startup, see StartupSUBTRANS(). */
	"pg_subtrans",

	/* end of list */
	NULL
};

/*
 * List of files excluded from backups.
 * 从备份中排除的文件列表
 */
static const struct exclude_list_item excludeFiles[] =
{
	/* Skip auto conf temporary file. */
	{PG_AUTOCONF_FILENAME ".tmp", false},

	/* Skip current log file temporary file */
	{LOG_METAINFO_DATAFILE_TMP, false},

	/*
	 * Skip relation cache because it is rebuilt on startup.  This includes
	 * temporary files.
	 */
	{RELCACHE_INIT_FILENAME, true},

	/*
	 * backup_label and tablespace_map should not exist in a running cluster
	 * capable of doing an online backup, but exclude them just in case.
	 */
	{BACKUP_LABEL_FILE, false},
	{TABLESPACE_MAP, false},

	/*
	 * If there's a backup_manifest, it belongs to a backup that was used to
	 * start this server. It is *not* correct for this backup. Our
	 * backup_manifest is injected into the backup separately if users want
	 * it.
	 */
	{"backup_manifest", false},

	{"postmaster.pid", false},
	{"postmaster.opts", false},

	/* end of list */
	{NULL, false}
};

/*
 * Actually do a base backup for the specified tablespaces.
 *
 * This is split out mainly to avoid complaints about "variable might be
 * clobbered by longjmp" from stupider versions of gcc.
 *
 * 实际上对指定的表空间进行基本备份
 *
 * 这样做主要是为了避免愚蠢版本的 gcc 抱怨“变量可能被 longjmp 破坏”
 *
 */
static void
perform_base_backup(basebackup_options *opt, bbsink *sink,
					IncrementalBackupInfo *ib)
{
	bbsink_state state;
	XLogRecPtr	endptr;
	TimeLineID	endtli;
	backup_manifest_info manifest;
	BackupState *backup_state;
	StringInfo	tablespace_map;

	/* Initial backup state, insofar as we know it now. */
	state.tablespaces = NIL;
	state.tablespace_num = 0;
	state.bytes_done = 0;
	state.bytes_total = 0;
	state.bytes_total_is_valid = false;

	/* we're going to use a BufFile, so we need a ResourceOwner */
	Assert(CurrentResourceOwner == NULL);
	CurrentResourceOwner = ResourceOwnerCreate(NULL, "base backup");

	// 获取是否在系统回复中
	backup_started_in_recovery = RecoveryInProgress();

	InitializeBackupManifest(&manifest, opt->manifest,
							 opt->manifest_checksum_type);

	total_checksum_failures = 0;

	/* Allocate backup related variables. */
	// 分配备份相关的变量
	backup_state = (BackupState *) palloc0(sizeof(BackupState));
	tablespace_map = makeStringInfo();

	// 备份进度等待检查点
	basebackup_progress_wait_checkpoint();

	// 备份开始
	do_pg_backup_start(opt->label, opt->fastcheckpoint, &state.tablespaces,
					   backup_state, tablespace_map);

	state.startptr = backup_state->startpoint;
	state.starttli = backup_state->starttli;

	/*
	 * Once do_pg_backup_start has been called, ensure that any failure causes
	 * us to abort the backup so we don't "leak" a backup counter. For this
	 * reason, *all* functionality between do_pg_backup_start() and the end of
	 * do_pg_backup_stop() should be inside the error cleanup block!
	 */

	PG_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));
	{
		ListCell   *lc;
		tablespaceinfo *newti;

		/* If this is an incremental backup, execute preparatory steps. */
		/* 如果这是增量备份，则执行准备步骤。 */
		if (ib != NULL)
			PrepareForIncrementalBackup(ib, backup_state);

		/* Add a node for the base directory at the end */
		newti = palloc0(sizeof(tablespaceinfo));
		newti->size = -1;
		state.tablespaces = lappend(state.tablespaces, newti);

		/*
		 * Calculate the total backup size by summing up the size of each
		 * tablespace
		 */
		if (opt->progress)
		{
			basebackup_progress_estimate_backup_size();

			foreach(lc, state.tablespaces)
			{
				tablespaceinfo *tmp = (tablespaceinfo *) lfirst(lc);

				if (tmp->path == NULL)
					tmp->size = sendDir(sink, ".", 1, true, state.tablespaces,
										true, NULL, InvalidOid, NULL);
				else
					tmp->size = sendTablespace(sink, tmp->path, tmp->oid, true,
											   NULL, NULL);
				state.bytes_total += tmp->size;
			}
			state.bytes_total_is_valid = true;
		}

		/* notify basebackup sink about start of backup */
		bbsink_begin_backup(sink, &state, SINK_BUFFER_LENGTH);

		/* Send off our tablespaces one by one */
		foreach(lc, state.tablespaces)
		{
			tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

			if (ti->path == NULL)
			{
				struct stat statbuf;
				bool		sendtblspclinks = true;
				char	   *backup_label;

				bbsink_begin_archive(sink, "base.tar");

				/* In the main tar, include the backup_label first... */
				backup_label = build_backup_content(backup_state, false);
				sendFileWithContent(sink, BACKUP_LABEL_FILE,
									backup_label, -1, &manifest);
				pfree(backup_label);

				/* Then the tablespace_map file, if required... */
				if (opt->sendtblspcmapfile)
				{
					sendFileWithContent(sink, TABLESPACE_MAP,
										tablespace_map->data, -1, &manifest);
					sendtblspclinks = false;
				}

				/* Then the bulk of the files... */
				/* 然后是大部分文件... */
				sendDir(sink, ".", 1, false, state.tablespaces,
						sendtblspclinks, &manifest, InvalidOid, ib);

				/* ... and pg_control after everything else. */
				if (lstat(XLOG_CONTROL_FILE, &statbuf) != 0)
					ereport(ERROR,
							(errcode_for_file_access(),
							 errmsg("could not stat file \"%s\": %m",
									XLOG_CONTROL_FILE)));
				sendFile(sink, XLOG_CONTROL_FILE, XLOG_CONTROL_FILE, &statbuf,
						 false, InvalidOid, InvalidOid,
						 InvalidRelFileNumber, 0, &manifest, 0, NULL, 0);
			}
			else
			{
				char	   *archive_name = psprintf("%u.tar", ti->oid);

				bbsink_begin_archive(sink, archive_name);

				sendTablespace(sink, ti->path, ti->oid, false, &manifest, ib);
			}

			/*
			 * If we're including WAL, and this is the main data directory we
			 * don't treat this as the end of the tablespace. Instead, we will
			 * include the xlog files below and stop afterwards. This is safe
			 * since the main data directory is always sent *last*.
			 */
			if (opt->includewal && ti->path == NULL)
			{
				Assert(lnext(state.tablespaces, lc) == NULL);
			}
			else
			{
				/* Properly terminate the tarfile. */
				StaticAssertDecl(2 * TAR_BLOCK_SIZE <= BLCKSZ,
								 "BLCKSZ too small for 2 tar blocks");
				memset(sink->bbs_buffer, 0, 2 * TAR_BLOCK_SIZE);
				bbsink_archive_contents(sink, 2 * TAR_BLOCK_SIZE);

				/* OK, that's the end of the archive. */
				bbsink_end_archive(sink);
			}
		}

		basebackup_progress_wait_wal_archive(&state);
		do_pg_backup_stop(backup_state, !opt->nowait);

		endptr = backup_state->stoppoint;
		endtli = backup_state->stoptli;

		/* Deallocate backup-related variables. */
		destroyStringInfo(tablespace_map);
		pfree(backup_state);
	}
	PG_END_ENSURE_ERROR_CLEANUP(do_pg_abort_backup, BoolGetDatum(false));


	if (opt->includewal)
	{
		/*
		 * We've left the last tar file "open", so we can now append the
		 * required WAL files to it.
		 */
		char		pathbuf[MAXPGPATH];
		XLogSegNo	segno;
		XLogSegNo	startsegno;
		XLogSegNo	endsegno;
		struct stat statbuf;
		List	   *historyFileList = NIL;
		List	   *walFileList = NIL;
		char		firstoff[MAXFNAMELEN];
		char		lastoff[MAXFNAMELEN];
		DIR		   *dir;
		struct dirent *de;
		ListCell   *lc;
		TimeLineID	tli;

		basebackup_progress_transfer_wal();

		/*
		 * I'd rather not worry about timelines here, so scan pg_wal and
		 * include all WAL files in the range between 'startptr' and 'endptr',
		 * regardless of the timeline the file is stamped with. If there are
		 * some spurious WAL files belonging to timelines that don't belong in
		 * this server's history, they will be included too. Normally there
		 * shouldn't be such files, but if there are, there's little harm in
		 * including them.
		 */
		XLByteToSeg(state.startptr, startsegno, wal_segment_size);
		XLogFileName(firstoff, state.starttli, startsegno, wal_segment_size);
		XLByteToPrevSeg(endptr, endsegno, wal_segment_size);
		XLogFileName(lastoff, endtli, endsegno, wal_segment_size);

		dir = AllocateDir("pg_wal");
		while ((de = ReadDir(dir, "pg_wal")) != NULL)
		{
			/* Does it look like a WAL segment, and is it in the range? */
			if (IsXLogFileName(de->d_name) &&
				strcmp(de->d_name + 8, firstoff + 8) >= 0 &&
				strcmp(de->d_name + 8, lastoff + 8) <= 0)
			{
				walFileList = lappend(walFileList, pstrdup(de->d_name));
			}
			/* Does it look like a timeline history file? */
			else if (IsTLHistoryFileName(de->d_name))
			{
				historyFileList = lappend(historyFileList, pstrdup(de->d_name));
			}
		}
		FreeDir(dir);

		/*
		 * Before we go any further, check that none of the WAL segments we
		 * need were removed.
		 */
		CheckXLogRemoved(startsegno, state.starttli);

		/*
		 * Sort the WAL filenames.  We want to send the files in order from
		 * oldest to newest, to reduce the chance that a file is recycled
		 * before we get a chance to send it over.
		 */
		list_sort(walFileList, compareWalFileNames);

		/*
		 * There must be at least one xlog file in the pg_wal directory, since
		 * we are doing backup-including-xlog.
		 */
		if (walFileList == NIL)
			ereport(ERROR,
					(errmsg("could not find any WAL files")));

		/*
		 * Sanity check: the first and last segment should cover startptr and
		 * endptr, with no gaps in between.
		 */
		XLogFromFileName((char *) linitial(walFileList),
						 &tli, &segno, wal_segment_size);
		if (segno != startsegno)
		{
			char		startfname[MAXFNAMELEN];

			XLogFileName(startfname, state.starttli, startsegno,
						 wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", startfname)));
		}
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			XLogSegNo	currsegno = segno;
			XLogSegNo	nextsegno = segno + 1;

			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);
			if (!(nextsegno == segno || currsegno == segno))
			{
				char		nextfname[MAXFNAMELEN];

				XLogFileName(nextfname, tli, nextsegno, wal_segment_size);
				ereport(ERROR,
						(errmsg("could not find WAL file \"%s\"", nextfname)));
			}
		}
		if (segno != endsegno)
		{
			char		endfname[MAXFNAMELEN];

			XLogFileName(endfname, endtli, endsegno, wal_segment_size);
			ereport(ERROR,
					(errmsg("could not find WAL file \"%s\"", endfname)));
		}

		/* Ok, we have everything we need. Send the WAL files. */
		foreach(lc, walFileList)
		{
			char	   *walFileName = (char *) lfirst(lc);
			int			fd;
			ssize_t		cnt;
			pgoff_t		len = 0;

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", walFileName);
			XLogFromFileName(walFileName, &tli, &segno, wal_segment_size);

			fd = OpenTransientFile(pathbuf, O_RDONLY | PG_BINARY);
			if (fd < 0)
			{
				int			save_errno = errno;

				/*
				 * Most likely reason for this is that the file was already
				 * removed by a checkpoint, so check for that to get a better
				 * error message.
				 */
				CheckXLogRemoved(segno, tli);

				errno = save_errno;
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not open file \"%s\": %m", pathbuf)));
			}

			if (fstat(fd, &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m",
								pathbuf)));
			if (statbuf.st_size != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/* send the WAL file itself */
			_tarWriteHeader(sink, pathbuf, NULL, &statbuf, false);

			while ((cnt = basebackup_read_file(fd, sink->bbs_buffer,
											   Min(sink->bbs_buffer_length,
												   wal_segment_size - len),
											   len, pathbuf, true)) > 0)
			{
				CheckXLogRemoved(segno, tli);
				bbsink_archive_contents(sink, cnt);

				len += cnt;

				if (len == wal_segment_size)
					break;
			}

			if (len != wal_segment_size)
			{
				CheckXLogRemoved(segno, tli);
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("unexpected WAL file size \"%s\"", walFileName)));
			}

			/*
			 * wal_segment_size is a multiple of TAR_BLOCK_SIZE, so no need
			 * for padding.
			 */
			Assert(wal_segment_size % TAR_BLOCK_SIZE == 0);

			CloseTransientFile(fd);

			/*
			 * Mark file as archived, otherwise files can get archived again
			 * after promotion of a new node. This is in line with
			 * walreceiver.c always doing an XLogArchiveForceDone() after a
			 * complete segment.
			 */
			StatusFilePath(pathbuf, walFileName, ".done");
			sendFileWithContent(sink, pathbuf, "", -1, &manifest);
		}

		/*
		 * Send timeline history files too. Only the latest timeline history
		 * file is required for recovery, and even that only if there happens
		 * to be a timeline switch in the first WAL segment that contains the
		 * checkpoint record, or if we're taking a base backup from a standby
		 * server and the target timeline changes while the backup is taken.
		 * But they are small and highly useful for debugging purposes, so
		 * better include them all, always.
		 */
		foreach(lc, historyFileList)
		{
			char	   *fname = lfirst(lc);

			snprintf(pathbuf, MAXPGPATH, XLOGDIR "/%s", fname);

			if (lstat(pathbuf, &statbuf) != 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file \"%s\": %m", pathbuf)));

			sendFile(sink, pathbuf, pathbuf, &statbuf, false,
					 InvalidOid, InvalidOid, InvalidRelFileNumber, 0,
					 &manifest, 0, NULL, 0);

			/* unconditionally mark file as archived */
			StatusFilePath(pathbuf, fname, ".done");
			sendFileWithContent(sink, pathbuf, "", -1, &manifest);
		}

		/* Properly terminate the tar file. */
		StaticAssertStmt(2 * TAR_BLOCK_SIZE <= BLCKSZ,
						 "BLCKSZ too small for 2 tar blocks");
		memset(sink->bbs_buffer, 0, 2 * TAR_BLOCK_SIZE);
		bbsink_archive_contents(sink, 2 * TAR_BLOCK_SIZE);

		/* OK, that's the end of the archive. */
		bbsink_end_archive(sink);
	}

	AddWALInfoToBackupManifest(&manifest, state.startptr, state.starttli,
							   endptr, endtli);

	SendBackupManifest(&manifest, sink);

	bbsink_end_backup(sink, endptr, endtli);

	if (total_checksum_failures)
	{
		if (total_checksum_failures > 1)
			ereport(WARNING,
					(errmsg_plural("%lld total checksum verification failure",
								   "%lld total checksum verification failures",
								   total_checksum_failures,
								   total_checksum_failures)));

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("checksum verification failure during base backup")));
	}

	/*
	 * Make sure to free the manifest before the resource owners as manifests
	 * use cryptohash contexts that may depend on resource owners (like
	 * OpenSSL).
	 */
	FreeBackupManifest(&manifest);

	/* clean up the resource owner we created */
	WalSndResourceCleanup(true);

	basebackup_progress_done();
}

/*
 * list_sort comparison function, to compare log/seg portion of WAL segment
 * filenames, ignoring the timeline portion.
 */
static int
compareWalFileNames(const ListCell *a, const ListCell *b)
{
	char	   *fna = (char *) lfirst(a);
	char	   *fnb = (char *) lfirst(b);

	return strcmp(fna + 8, fnb + 8);
}

/*
 * Parse the base backup options passed down by the parser
 */
static void
parse_basebackup_options(List *options, basebackup_options *opt)
{
	ListCell   *lopt;
	bool		o_label = false;
	bool		o_progress = false;
	bool		o_checkpoint = false;
	bool		o_nowait = false;
	bool		o_wal = false;
	bool		o_incremental = false;
	bool		o_maxrate = false;
	bool		o_tablespace_map = false;
	bool		o_noverify_checksums = false;
	bool		o_manifest = false;
	bool		o_manifest_checksums = false;
	bool		o_target = false;
	bool		o_target_detail = false;
	char	   *target_str = NULL;
	char	   *target_detail_str = NULL;
	bool		o_compression = false;
	bool		o_compression_detail = false;
	char	   *compression_detail_str = NULL;

	MemSet(opt, 0, sizeof(*opt));
	opt->manifest = MANIFEST_OPTION_NO;
	opt->manifest_checksum_type = CHECKSUM_TYPE_CRC32C;
	opt->compression = PG_COMPRESSION_NONE;
	opt->compression_specification.algorithm = PG_COMPRESSION_NONE;

	foreach(lopt, options)
	{
		DefElem    *defel = (DefElem *) lfirst(lopt);

		if (strcmp(defel->defname, "label") == 0)
		{
			if (o_label)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->label = defGetString(defel);
			o_label = true;
		}
		else if (strcmp(defel->defname, "progress") == 0)
		{
			if (o_progress)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->progress = defGetBoolean(defel);
			o_progress = true;
		}
		else if (strcmp(defel->defname, "checkpoint") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_checkpoint)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (pg_strcasecmp(optval, "fast") == 0)
				opt->fastcheckpoint = true;
			else if (pg_strcasecmp(optval, "spread") == 0)
				opt->fastcheckpoint = false;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized checkpoint type: \"%s\"",
								optval)));
			o_checkpoint = true;
		}
		else if (strcmp(defel->defname, "wait") == 0)
		{
			if (o_nowait)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->nowait = !defGetBoolean(defel);
			o_nowait = true;
		}
		else if (strcmp(defel->defname, "wal") == 0)
		{
			if (o_wal)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->includewal = defGetBoolean(defel);
			o_wal = true;
		}
		else if (strcmp(defel->defname, "incremental") == 0)
		{
			if (o_incremental)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->incremental = defGetBoolean(defel);
			if (opt->incremental && !summarize_wal)
				// 增量必须要汇总预写日志
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("incremental backups cannot be taken unless WAL summarization is enabled")));
			o_incremental = true;
		}
		else if (strcmp(defel->defname, "max_rate") == 0)
		{
			int64		maxrate;

			if (o_maxrate)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));

			maxrate = defGetInt64(defel);
			if (maxrate < MAX_RATE_LOWER || maxrate > MAX_RATE_UPPER)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("%d is outside the valid range for parameter \"%s\" (%d .. %d)",
								(int) maxrate, "MAX_RATE", MAX_RATE_LOWER, MAX_RATE_UPPER)));

			opt->maxrate = (uint32) maxrate;
			o_maxrate = true;
		}
		else if (strcmp(defel->defname, "tablespace_map") == 0)
		{
			if (o_tablespace_map)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			opt->sendtblspcmapfile = defGetBoolean(defel);
			o_tablespace_map = true;
		}
		else if (strcmp(defel->defname, "verify_checksums") == 0)
		{
			if (o_noverify_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			noverify_checksums = !defGetBoolean(defel);
			o_noverify_checksums = true;
		}
		else if (strcmp(defel->defname, "manifest") == 0)
		{
			char	   *optval = defGetString(defel);
			bool		manifest_bool;

			if (o_manifest)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (parse_bool(optval, &manifest_bool))
			{
				if (manifest_bool)
					opt->manifest = MANIFEST_OPTION_YES;
				else
					opt->manifest = MANIFEST_OPTION_NO;
			}
			else if (pg_strcasecmp(optval, "force-encode") == 0)
				opt->manifest = MANIFEST_OPTION_FORCE_ENCODE;
			else
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized manifest option: \"%s\"",
								optval)));
			o_manifest = true;
		}
		else if (strcmp(defel->defname, "manifest_checksums") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_manifest_checksums)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (!pg_checksum_parse_type(optval,
										&opt->manifest_checksum_type))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized checksum algorithm: \"%s\"",
								optval)));
			o_manifest_checksums = true;
		}
		else if (strcmp(defel->defname, "target") == 0)
		{
			if (o_target)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			target_str = defGetString(defel);
			o_target = true;
		}
		else if (strcmp(defel->defname, "target_detail") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_target_detail)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			target_detail_str = optval;
			o_target_detail = true;
		}
		else if (strcmp(defel->defname, "compression") == 0)
		{
			char	   *optval = defGetString(defel);

			if (o_compression)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			if (!parse_compress_algorithm(optval, &opt->compression))
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("unrecognized compression algorithm: \"%s\"",
								optval)));
			o_compression = true;
		}
		else if (strcmp(defel->defname, "compression_detail") == 0)
		{
			if (o_compression_detail)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("duplicate option \"%s\"", defel->defname)));
			compression_detail_str = defGetString(defel);
			o_compression_detail = true;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized base backup option: \"%s\"",
							defel->defname)));
	}

	if (opt->label == NULL)
		opt->label = "base backup";
	if (opt->manifest == MANIFEST_OPTION_NO)
	{
		if (o_manifest_checksums)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("manifest checksums require a backup manifest")));
		opt->manifest_checksum_type = CHECKSUM_TYPE_NONE;
	}

	if (target_str == NULL)
	{
		if (target_detail_str != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("target detail cannot be used without target")));
		opt->use_copytblspc = true;
		opt->send_to_client = true;
	}
	else if (strcmp(target_str, "client") == 0)
	{
		if (target_detail_str != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("target \"%s\" does not accept a target detail",
							target_str)));
		opt->send_to_client = true;
	}
	else
		opt->target_handle =
			BaseBackupGetTargetHandle(target_str, target_detail_str);

	if (o_compression_detail && !o_compression)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("compression detail cannot be specified unless compression is enabled")));

	if (o_compression)
	{
		char	   *error_detail;

		parse_compress_specification(opt->compression, compression_detail_str,
									 &opt->compression_specification);
		error_detail =
			validate_compress_specification(&opt->compression_specification);
		if (error_detail != NULL)
			ereport(ERROR,
					errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid compression specification: %s",
						   error_detail));
	}
}


/*
 * SendBaseBackup() - send a complete base backup.
 *
 * The function will put the system into backup mode like pg_backup_start()
 * does, so that the backup is consistent even though we read directly from
 * the filesystem, bypassing the buffer cache.
 *
 * SendBaseBackup() - 发送完整的基础备份
 *
 * 该函数将像 pg_backup_start() 一样将系统置于备份模式，这样即使我们绕过缓冲区缓存直接从文件系统读取，备份也是一致的
 */
void
SendBaseBackup(BaseBackupCmd *cmd, IncrementalBackupInfo *ib)
{
	basebackup_options opt;
	bbsink	   *sink;
	SessionBackupState status = get_backup_status();

	if (status == SESSION_BACKUP_RUNNING)
		// 备份运行中
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("a backup is already in progress in this session")));

	parse_basebackup_options(cmd->options, &opt);

	WalSndSetState(WALSNDSTATE_BACKUP);

	if (update_process_title)
	{
		char		activitymsg[50];

		snprintf(activitymsg, sizeof(activitymsg), "sending backup \"%s\"",
				 opt.label);
		// 修改名
		set_ps_display(activitymsg);
	}

	/*
	 * If we're asked to perform an incremental backup and the user has not
	 * supplied a manifest, that's an ERROR.
	 *
	 * If we're asked to perform a full backup and the user did supply a
	 * manifest, just ignore it.
	 */
	if (!opt.incremental)
		ib = NULL;
	else if (ib == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("must UPLOAD_MANIFEST before performing an incremental BASE_BACKUP")));

	/*
	 * If the target is specifically 'client' then set up to stream the backup
	 * to the client; otherwise, it's being sent someplace else and should not
	 * be sent to the client. BaseBackupGetSink has the job of setting up a
	 * sink to send the backup data wherever it needs to go.
	 */
	sink = bbsink_copystream_new(opt.send_to_client);
	if (opt.target_handle != NULL)
		sink = BaseBackupGetSink(opt.target_handle, sink);

	/* Set up network throttling, if client requested it */
	/* 设置网络限制 */
	if (opt.maxrate > 0)
		sink = bbsink_throttle_new(sink, opt.maxrate);

	/* Set up server-side compression, if client requested it */
	/* 如果客户端请求，设置服务器端压缩 */
	if (opt.compression == PG_COMPRESSION_GZIP)
		sink = bbsink_gzip_new(sink, &opt.compression_specification);
	else if (opt.compression == PG_COMPRESSION_LZ4)
		sink = bbsink_lz4_new(sink, &opt.compression_specification);
	else if (opt.compression == PG_COMPRESSION_ZSTD)
		sink = bbsink_zstd_new(sink, &opt.compression_specification);

	/* Set up progress reporting. */
	/* 设置进度报告 */
	sink = bbsink_progress_new(sink, opt.progress);

	/*
	 * Perform the base backup, but make sure we clean up the bbsink even if
	 * an error occurs.
	 */
	PG_TRY();
	{
		// 来吧,执行备份
		perform_base_backup(&opt, sink, ib);
	}
	PG_FINALLY();
	{
		bbsink_cleanup(sink);
	}
	PG_END_TRY();
}

/*
 * Inject a file with given name and content in the output tar stream.
 *
 * "len" can optionally be set to an arbitrary length of data sent.  If set
 * to -1, the content sent is treated as a string with strlen() as length.
 */
static void
sendFileWithContent(bbsink *sink, const char *filename, const char *content,
					int len, backup_manifest_info *manifest)
{
	struct stat statbuf;
	int			bytes_done = 0;
	pg_checksum_context checksum_ctx;

	if (pg_checksum_init(&checksum_ctx, manifest->checksum_type) < 0)
		elog(ERROR, "could not initialize checksum of file \"%s\"",
			 filename);

	if (len < 0)
		len = strlen(content);

	/*
	 * Construct a stat struct for the file we're injecting in the tar.
	 */

	/* Windows doesn't have the concept of uid and gid */
#ifdef WIN32
	statbuf.st_uid = 0;
	statbuf.st_gid = 0;
#else
	statbuf.st_uid = geteuid();
	statbuf.st_gid = getegid();
#endif
	statbuf.st_mtime = time(NULL);
	statbuf.st_mode = pg_file_create_mode;
	statbuf.st_size = len;

	_tarWriteHeader(sink, filename, NULL, &statbuf, false);

	if (pg_checksum_update(&checksum_ctx, (uint8 *) content, len) < 0)
		elog(ERROR, "could not update checksum of file \"%s\"",
			 filename);

	while (bytes_done < len)
	{
		size_t		remaining = len - bytes_done;
		size_t		nbytes = Min(sink->bbs_buffer_length, remaining);

		memcpy(sink->bbs_buffer, content, nbytes);
		bbsink_archive_contents(sink, nbytes);
		bytes_done += nbytes;
		content += nbytes;
	}

	_tarWritePadding(sink, len);

	AddFileToBackupManifest(manifest, InvalidOid, filename, len,
							(pg_time_t) statbuf.st_mtime, &checksum_ctx);
}

/*
 * Include the tablespace directory pointed to by 'path' in the output tar
 * stream.  If 'sizeonly' is true, we just calculate a total length and return
 * it, without actually sending anything.
 *
 * Only used to send auxiliary tablespaces, not PGDATA.
 */
static int64
sendTablespace(bbsink *sink, char *path, Oid spcoid, bool sizeonly,
			   backup_manifest_info *manifest, IncrementalBackupInfo *ib)
{
	int64		size;
	char		pathbuf[MAXPGPATH];
	struct stat statbuf;

	/*
	 * 'path' points to the tablespace location, but we only want to include
	 * the version directory in it that belongs to us.
	 */
	snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path,
			 TABLESPACE_VERSION_DIRECTORY);

	/*
	 * Store a directory entry in the tar file so we get the permissions
	 * right.
	 */
	if (lstat(pathbuf, &statbuf) != 0)
	{
		if (errno != ENOENT)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not stat file or directory \"%s\": %m",
							pathbuf)));

		/* If the tablespace went away while scanning, it's no error. */
		return 0;
	}

	size = _tarWriteHeader(sink, TABLESPACE_VERSION_DIRECTORY, NULL, &statbuf,
						   sizeonly);

	/* Send all the files in the tablespace version directory */
	size += sendDir(sink, pathbuf, strlen(path), sizeonly, NIL, true, manifest,
					spcoid, ib);

	return size;
}

/*
 * Include all files from the given directory in the output tar stream. If
 * 'sizeonly' is true, we just calculate a total length and return it, without
 * actually sending anything.
 *
 * Omit any directory in the tablespaces list, to avoid backing up
 * tablespaces twice when they were created inside PGDATA.
 *
 * If sendtblspclinks is true, we need to include symlink
 * information in the tar file. If not, we can skip that
 * as it will be sent separately in the tablespace_map file.
 */
static int64
sendDir(bbsink *sink, const char *path, int basepathlen, bool sizeonly,
		List *tablespaces, bool sendtblspclinks, backup_manifest_info *manifest,
		Oid spcoid, IncrementalBackupInfo *ib)
{
	DIR		   *dir;
	struct dirent *de;
	char		pathbuf[MAXPGPATH * 2];
	struct stat statbuf;
	// 空间大小
	int64		size = 0;
	const char *lastDir;		/* Split last dir from parent path. */
	// 目录是否包含关系(数据文件)
	bool		isRelationDir = false;	/* Does directory contain relations? */
	bool		isGlobalDir = false;
	Oid			dboid = InvalidOid;
	BlockNumber *relative_block_numbers = NULL;

	/*
	 * Since this array is relatively large, avoid putting it on the stack.
	 * But we don't need it at all if this is not an incremental backup.
	 *
	 * 由于这个数组比较大，避免将其放入堆栈。 但如果这不是增量备份，我们根本不需要它。
	 */
	if (ib != NULL)
		// 相对块号
		relative_block_numbers = palloc(sizeof(BlockNumber) * RELSEG_SIZE);

	/*
	 * Determine if the current path is a database directory that can contain
	 * relations.
	 *
	 * Start by finding the location of the delimiter between the parent path
	 * and the current path.
	 *
	 * 确定当前路径是否是可以包含关系的数据库目录
	 *
	 * 首先查找父路径和当前路径之间的分隔符的位置。
	 */
	lastDir = last_dir_separator(path);

	/* Does this path look like a database path (i.e. all digits)? */
	/* 该路径看起来像数据库路径吗（即全是数字）？ */
	if (lastDir != NULL &&
		strspn(lastDir + 1, "0123456789") == strlen(lastDir + 1))
	{
		/* Part of path that contains the parent directory. */
		/* 包含父目录的路径部分。 */
		int			parentPathLen = lastDir - path;

		/*
		 * Mark path as a database directory if the parent path is either
		 * $PGDATA/base or a tablespace version path.
		 *
		 * 如果父路径是 $PGDATA/base 或表空间版本路径，则将路径标记为数据库目录。
		 */
		if (strncmp(path, "./base", parentPathLen) == 0 ||
			(parentPathLen >= (sizeof(TABLESPACE_VERSION_DIRECTORY) - 1) &&
			 strncmp(lastDir - (sizeof(TABLESPACE_VERSION_DIRECTORY) - 1),
					 TABLESPACE_VERSION_DIRECTORY,
					 sizeof(TABLESPACE_VERSION_DIRECTORY) - 1) == 0))
		{
			isRelationDir = true;
			dboid = atooid(lastDir + 1);
		}
	}
	else if (strcmp(path, "./global") == 0)
	{
		isRelationDir = true;
		isGlobalDir = true;
	}

	// 分配目录结构
	dir = AllocateDir(path);
	while ((de = ReadDir(dir, path)) != NULL)
	{
		int			excludeIdx;
		bool		excludeFound;
		RelFileNumber relfilenumber = InvalidRelFileNumber;
		ForkNumber	relForkNum = InvalidForkNumber;
		unsigned	segno = 0;
		bool		isRelationFile = false;

		/* Skip special stuff */
		/* 跳过 "." ".." */
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
			continue;

		/* Skip temporary files */
		/* 跳过临时文件 */
		if (strncmp(de->d_name,
					PG_TEMP_FILE_PREFIX,
					strlen(PG_TEMP_FILE_PREFIX)) == 0)
			continue;

		/* Skip macOS system files */
		if (strcmp(de->d_name, ".DS_Store") == 0)
			continue;

		/*
		 * Check if the postmaster has signaled us to exit, and abort with an
		 * error in that case. The error handler further up will call
		 * do_pg_abort_backup() for us. Also check that if the backup was
		 * started while still in recovery, the server wasn't promoted.
		 * do_pg_backup_stop() will check that too, but it's better to stop
		 * the backup early than continue to the end and fail there.
		 * 
		 * 检查邮政局长是否已向我们发出退出信号，并在这种情况下因错误而中止
		 * 上面的错误处理程序将为我们调用 do_pg_abort_backup()
		 * 另请检查备份是否在恢复期间启动，服务器是否未升级
		 * do_pg_backup_stop() 也会检查这一点，但最好尽早停止备份，而不是继续到最后并在那里失败
		 */
		CHECK_FOR_INTERRUPTS();
		if (RecoveryInProgress() != backup_started_in_recovery)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("the standby was promoted during online backup"),
					 errhint("This means that the backup being taken is corrupt "
							 "and should not be used. "
							 "Try taking another online backup.")));

		/* Scan for files that should be excluded */
		/* 扫描应排除的文件 */
		excludeFound = false;
		for (excludeIdx = 0; excludeFiles[excludeIdx].name != NULL; excludeIdx++)
		{
			int			cmplen = strlen(excludeFiles[excludeIdx].name);

			if (!excludeFiles[excludeIdx].match_prefix)
				cmplen++;
			if (strncmp(de->d_name, excludeFiles[excludeIdx].name, cmplen) == 0)
			{
				elog(DEBUG1, "file \"%s\" excluded from backup", de->d_name);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			// 跳过排除文件
			continue;

		/*
		 * If there could be non-temporary relation files in this directory,
		 * try to parse the filename.
		 *
		 * 如果该目录中可能存在非临时关系文件，请尝试解析文件名。
		 */
		if (isRelationDir)
			isRelationFile =
				parse_filename_for_nontemp_relation(de->d_name,
													&relfilenumber,
													&relForkNum, &segno);

		/* Exclude all forks for unlogged tables except the init fork */
		/* 排除除 init 分叉之外的未记录表的所有分叉 */
		if (isRelationFile && relForkNum != INIT_FORKNUM)
		{
			char		initForkFile[MAXPGPATH];

			/*
			 * If any other type of fork, check if there is an init fork with
			 * the same RelFileNumber. If so, the file can be excluded.
			 *
			 * 如果有任何其他类型的 fork，请检查是否存在具有相同 RelFileNumber 的 init fork
			 * 如果是这样，则可以排除该文件
			 */
			snprintf(initForkFile, sizeof(initForkFile), "%s/%u_init",
					 path, relfilenumber);

			if (lstat(initForkFile, &statbuf) == 0)
			{
				elog(DEBUG2,
					 "unlogged relation file \"%s\" excluded from backup",
					 de->d_name);

				continue;
			}
		}

		/* Exclude temporary relations */
		/* 排除临时关系 */
		if (OidIsValid(dboid) && looks_like_temp_rel_name(de->d_name))
		{
			elog(DEBUG2,
				 "temporary relation file \"%s\" excluded from backup",
				 de->d_name);

			continue;
		}

		snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, de->d_name);

		/* Skip pg_control here to back up it last */
		/* 这里跳过pg_control，最后备份它 */
		if (strcmp(pathbuf, "./global/pg_control") == 0)
			continue;

		if (lstat(pathbuf, &statbuf) != 0)
		{
			if (errno != ENOENT)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not stat file or directory \"%s\": %m",
								pathbuf)));

			/* If the file went away while scanning, it's not an error. */
			continue;
		}

		/* Scan for directories whose contents should be excluded */
		/* 扫描应排除其内容的目录 */
		excludeFound = false;
		for (excludeIdx = 0; excludeDirContents[excludeIdx] != NULL; excludeIdx++)
		{
			if (strcmp(de->d_name, excludeDirContents[excludeIdx]) == 0)
			{
				elog(DEBUG1, "contents of directory \"%s\" excluded from backup", de->d_name);
				convert_link_to_directory(pathbuf, &statbuf);
				size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL,
										&statbuf, sizeonly);
				excludeFound = true;
				break;
			}
		}

		if (excludeFound)
			continue;

		/*
		 * We can skip pg_wal, the WAL segments need to be fetched from the
		 * WAL archive anyway. But include it as an empty directory anyway, so
		 * we get permissions right.
		 *
		 * 我们可以跳过 pg_wal，无论如何都需要从 WAL 存档中获取 WAL 段
		 * 但无论如何将其作为空目录包含在内，这样我们就可以获得正确的权限
		 */
		if (strcmp(pathbuf, "./pg_wal") == 0)
		{
			/* If pg_wal is a symlink, write it as a directory anyway */
			convert_link_to_directory(pathbuf, &statbuf);
			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL,
									&statbuf, sizeonly);

			/*
			 * Also send archive_status and summaries directories (by
			 * hackishly reusing statbuf from above ...).
			 */
			size += _tarWriteHeader(sink, "./pg_wal/archive_status", NULL,
									&statbuf, sizeonly);
			size += _tarWriteHeader(sink, "./pg_wal/summaries", NULL,
									&statbuf, sizeonly);

			continue;			/* don't recurse into pg_wal */
		}

		/* Allow symbolic links in pg_tblspc only */
		/* 仅允许 pg_tblspc 中的符号链接 */
		if (strcmp(path, "./pg_tblspc") == 0 && S_ISLNK(statbuf.st_mode))
		{
			char		linkpath[MAXPGPATH];
			int			rllen;

			rllen = readlink(pathbuf, linkpath, sizeof(linkpath));
			if (rllen < 0)
				ereport(ERROR,
						(errcode_for_file_access(),
						 errmsg("could not read symbolic link \"%s\": %m",
								pathbuf)));
			if (rllen >= sizeof(linkpath))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("symbolic link \"%s\" target is too long",
								pathbuf)));
			linkpath[rllen] = '\0';

			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, linkpath,
									&statbuf, sizeonly);
		}
		else if (S_ISDIR(statbuf.st_mode))
		{
			bool		skip_this_dir = false;
			ListCell   *lc;

			/*
			 * Store a directory entry in the tar file so we can get the
			 * permissions right.
			 */
			size += _tarWriteHeader(sink, pathbuf + basepathlen + 1, NULL, &statbuf,
									sizeonly);

			/*
			 * Call ourselves recursively for a directory, unless it happens
			 * to be a separate tablespace located within PGDATA.
			 */
			foreach(lc, tablespaces)
			{
				tablespaceinfo *ti = (tablespaceinfo *) lfirst(lc);

				/*
				 * ti->rpath is the tablespace relative path within PGDATA, or
				 * NULL if the tablespace has been properly located somewhere
				 * else.
				 *
				 * Skip past the leading "./" in pathbuf when comparing.
				 */
				if (ti->rpath && strcmp(ti->rpath, pathbuf + 2) == 0)
				{
					skip_this_dir = true;
					break;
				}
			}

			/*
			 * skip sending directories inside pg_tblspc, if not required.
			 * 如果不需要，跳过在 pg_tblspc 内发送目录。
			 */
			if (strcmp(pathbuf, "./pg_tblspc") == 0 && !sendtblspclinks)
				skip_this_dir = true;

			if (!skip_this_dir)
				size += sendDir(sink, pathbuf, basepathlen, sizeonly, tablespaces,
								sendtblspclinks, manifest, spcoid, ib);
		}
		else if (S_ISREG(statbuf.st_mode))
		{
			// 常规文件
			bool		sent = false;
			unsigned	num_blocks_required = 0;
			unsigned	truncation_block_length = 0;
			char		tarfilenamebuf[MAXPGPATH * 2];
			char	   *tarfilename = pathbuf + basepathlen + 1;
			FileBackupMethod method = BACK_UP_FILE_FULLY;

			if (ib != NULL && isRelationFile)
			{
				// 是 base 里的文件
				Oid			relspcoid;
				char	   *lookup_path;

				if (OidIsValid(spcoid))
				{
					relspcoid = spcoid;
					lookup_path = psprintf("pg_tblspc/%u/%s", spcoid,
										   tarfilename);
				}
				else
				{
					if (isGlobalDir)
						relspcoid = GLOBALTABLESPACE_OID;
					else
						relspcoid = DEFAULTTABLESPACE_OID;
					lookup_path = pstrdup(tarfilename);
				}

				// 获取文件备份方式
				method = GetFileBackupMethod(ib, lookup_path, dboid, relspcoid,
											 relfilenumber, relForkNum,
											 segno, statbuf.st_size,
											 &num_blocks_required,
											 relative_block_numbers,
											 &truncation_block_length);
				if (method == BACK_UP_FILE_INCREMENTALLY)
				{
					// 获取增量大小
					statbuf.st_size =
						GetIncrementalFileSize(num_blocks_required);
					snprintf(tarfilenamebuf, sizeof(tarfilenamebuf),
							 "%s/INCREMENTAL.%s",
							 path + basepathlen + 1,
							 de->d_name);
					tarfilename = tarfilenamebuf;
				}

				pfree(lookup_path);
			}

			if (!sizeonly)
				sent = sendFile(sink, pathbuf, tarfilename, &statbuf,
								true, dboid, spcoid,
								relfilenumber, segno, manifest,
								num_blocks_required,
								method == BACK_UP_FILE_INCREMENTALLY ? relative_block_numbers : NULL,
								truncation_block_length);

			if (sent || sizeonly)
			{
				/* Add size. */
				size += statbuf.st_size;

				/* Pad to a multiple of the tar block size. */
				/* 填充为 tar 块大小的倍数。 */
				size += tarPaddingBytesRequired(statbuf.st_size);

				/* Size of the header for the file. */
				size += TAR_BLOCK_SIZE;
			}
		}
		else
			ereport(WARNING,
					(errmsg("skipping special file \"%s\"", pathbuf)));
	}

	if (relative_block_numbers != NULL)
		pfree(relative_block_numbers);

	FreeDir(dir);
	return size;
}

/*
 * Given the member, write the TAR header & send the file.
 *
 * If 'missing_ok' is true, will not throw an error if the file is not found.
 *
 * If dboid is anything other than InvalidOid then any checksum failures
 * detected will get reported to the cumulative stats system.
 *
 * If the file is to be sent incrementally, then num_incremental_blocks
 * should be the number of blocks to be sent, and incremental_blocks
 * an array of block numbers relative to the start of the current segment.
 * If the whole file is to be sent, then incremental_blocks should be NULL,
 * and num_incremental_blocks can have any value, as it will be ignored.
 *
 * Returns true if the file was successfully sent, false if 'missing_ok',
 * and the file did not exist.
 *
 * 给定成员，写入 TAR 标头并发送文件
 *
 * 如果 'missing_ok' 为 true，则在找不到文件时不会抛出错误
 *
 * 如果 dboid 不是 InvalidOid，则检测到的任何校验和失败都将报告给累积统计系统
 *
 * 如果要增量发送文件，则 num_incremental_blocks 应该是要发送的块数，incremental_blocks 是相对于当前段开头的块号数组
 * 如果要发送整个文件，则incremental_blocks应为NULL，并且 num_incremental_blocks 可以为任何值，因为它将被忽略
 *
 * 如果文件已成功发送，则返回 true；如果“missing_ok”且文件不存在，则返回 false
 *
 */
static bool
sendFile(bbsink *sink, const char *readfilename, const char *tarfilename,
		 struct stat *statbuf, bool missing_ok, Oid dboid, Oid spcoid,
		 RelFileNumber relfilenumber, unsigned segno,
		 backup_manifest_info *manifest, unsigned num_incremental_blocks,
		 BlockNumber *incremental_blocks, unsigned truncation_block_length)
{
	int			fd;
	BlockNumber blkno = 0;
	int			checksum_failures = 0;
	off_t		cnt;
	pgoff_t		bytes_done = 0;
	bool		verify_checksum = false;
	pg_checksum_context checksum_ctx;
	int			ibindex = 0;

	if (pg_checksum_init(&checksum_ctx, manifest->checksum_type) < 0)
		elog(ERROR, "could not initialize checksum of file \"%s\"",
			 readfilename);

	// 打开事务文件
	fd = OpenTransientFile(readfilename, O_RDONLY | PG_BINARY);
	if (fd < 0)
	{
		if (errno == ENOENT && missing_ok)
			return false;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", readfilename)));
	}

	_tarWriteHeader(sink, tarfilename, NULL, statbuf, false);

	/*
	 * Checksums are verified in multiples of BLCKSZ, so the buffer length
	 * should be a multiple of the block size as well.
	 */
	Assert((sink->bbs_buffer_length % BLCKSZ) == 0);

	/*
	 * If we weren't told not to verify checksums, and if checksums are
	 * enabled for this cluster, and if this is a relation file, then verify
	 * the checksum.
	 */
	if (!noverify_checksums && DataChecksumsEnabled() &&
		RelFileNumberIsValid(relfilenumber))
		verify_checksum = true;

	/*
	 * If we're sending an incremental file, write the file header.
	 */
	if (incremental_blocks != NULL)
	{
		unsigned	magic = INCREMENTAL_MAGIC;
		size_t		header_bytes_done = 0;
		char		padding[BLCKSZ];
		size_t		paddinglen;

		/* Emit header data. */
		push_to_sink(sink, &checksum_ctx, &header_bytes_done,
					 &magic, sizeof(magic));
		push_to_sink(sink, &checksum_ctx, &header_bytes_done,
					 &num_incremental_blocks, sizeof(num_incremental_blocks));
		push_to_sink(sink, &checksum_ctx, &header_bytes_done,
					 &truncation_block_length, sizeof(truncation_block_length));
		push_to_sink(sink, &checksum_ctx, &header_bytes_done,
					 incremental_blocks,
					 sizeof(BlockNumber) * num_incremental_blocks);

		/*
		 * Add padding to align header to a multiple of BLCKSZ, but only if
		 * the incremental file has some blocks, and the alignment is actually
		 * needed (i.e. header is not already a multiple of BLCKSZ). If there
		 * are no blocks we don't want to make the file unnecessarily large,
		 * as that might make some filesystem optimizations impossible.
		 */
		if ((num_incremental_blocks > 0) && (header_bytes_done % BLCKSZ != 0))
		{
			paddinglen = (BLCKSZ - (header_bytes_done % BLCKSZ));

			memset(padding, 0, paddinglen);
			bytes_done += paddinglen;

			push_to_sink(sink, &checksum_ctx, &header_bytes_done,
						 padding, paddinglen);
		}

		/* Flush out any data still in the buffer so it's again empty. */
		/* 清除仍在缓冲区中的所有数据，使其再次为空。 */
		if (header_bytes_done > 0)
		{
			bbsink_archive_contents(sink, header_bytes_done);
			if (pg_checksum_update(&checksum_ctx,
								   (uint8 *) sink->bbs_buffer,
								   header_bytes_done) < 0)
				elog(ERROR, "could not update checksum of base backup");
		}

		/* Update our notion of file position. */
		bytes_done += sizeof(magic);
		bytes_done += sizeof(num_incremental_blocks);
		bytes_done += sizeof(truncation_block_length);
		bytes_done += sizeof(BlockNumber) * num_incremental_blocks;
	}

	/*
	 * Loop until we read the amount of data the caller told us to expect. The
	 * file could be longer, if it was extended while we were sending it, but
	 * for a base backup we can ignore such extended data. It will be restored
	 * from WAL.
	 *
	 * 循环直到我们读取调用者告诉我们期望的数据量
	 * 如果我们在发送文件时扩展了文件，则文件可能会更长，但对于基本备份，我们可以忽略此类扩展数据
	 * 它将从 WAL 恢复
	 */
	while (1)
	{
		/*
		 * Determine whether we've read all the data that we need, and if not,
		 * read some more.
		 */
		if (incremental_blocks == NULL)
		{
			size_t		remaining = statbuf->st_size - bytes_done;

			/*
			 * If we've read the required number of bytes, then it's time to
			 * stop.
			 */
			if (bytes_done >= statbuf->st_size)
				break;

			/*
			 * Read as many bytes as will fit in the buffer, or however many
			 * are left to read, whichever is less.
			 */
			cnt = read_file_data_into_buffer(sink, readfilename, fd,
											 bytes_done, remaining,
											 blkno + segno * RELSEG_SIZE,
											 verify_checksum,
											 &checksum_failures);
		}
		else
		{
			BlockNumber relative_blkno;

			/*
			 * If we've read all the blocks, then it's time to stop.
			 * 如果我们读完了所有的块，那么就该停止了
			 */
			if (ibindex >= num_incremental_blocks)
				break;

			/*
			 * Read just one block, whichever one is the next that we're
			 * supposed to include.
			 *
			 * 只读取一个块，无论我们应该包含的下一个块是什么
			 */
			relative_blkno = incremental_blocks[ibindex++];
			cnt = read_file_data_into_buffer(sink, readfilename, fd,
											 relative_blkno * BLCKSZ,
											 BLCKSZ,
											 relative_blkno + segno * RELSEG_SIZE,
											 verify_checksum,
											 &checksum_failures);

			/*
			 * If we get a partial read, that must mean that the relation is
			 * being truncated. Ultimately, it should be truncated to a
			 * multiple of BLCKSZ, since this path should only be reached for
			 * relation files, but we might transiently observe an
			 * intermediate value.
			 *
			 * It should be fine to treat this just as if the entire block had
			 * been truncated away - i.e. fill this and all later blocks with
			 * zeroes. WAL replay will fix things up.
			 *
			 * 如果我们得到部分读取，则必定意味着关系被截断
			 * 最终，它应该被截断为 BLCKSZ 的倍数，因为只有关系文件才应该到达此路径，但我们可能会暂时观察到一个中间值
			 *
			 * 最好将其视为整个块已被截断 - 即用零填充此块以及后面的所有块
			 * WAL 重播会解决问题
			 *
			 */
			if (cnt < BLCKSZ)
				break;
		}

		/*
		 * If the amount of data we were able to read was not a multiple of
		 * BLCKSZ, we cannot verify checksums, which are block-level.
		 */
		if (verify_checksum && (cnt % BLCKSZ != 0))
		{
			ereport(WARNING,
					(errmsg("could not verify checksum in file \"%s\", block "
							"%u: read buffer size %d and page size %d "
							"differ",
							readfilename, blkno, (int) cnt, BLCKSZ)));
			verify_checksum = false;
		}

		/*
		 * If we hit end-of-file, a concurrent truncation must have occurred.
		 * That's not an error condition, because WAL replay will fix things
		 * up.
		 */
		if (cnt == 0)
			break;

		/* Update block number and # of bytes done for next loop iteration. */
		blkno += cnt / BLCKSZ;
		bytes_done += cnt;

		/*
		 * Make sure incremental files with block data are properly aligned
		 * (header is a multiple of BLCKSZ, blocks are BLCKSZ too).
		 */
		Assert(!((incremental_blocks != NULL && num_incremental_blocks > 0) &&
				 (bytes_done % BLCKSZ != 0)));

		/* Archive the data we just read. */
		/* 将我们刚刚读取的数据归档 */
		bbsink_archive_contents(sink, cnt);

		/* Also feed it to the checksum machinery. */
		if (pg_checksum_update(&checksum_ctx,
							   (uint8 *) sink->bbs_buffer, cnt) < 0)
			elog(ERROR, "could not update checksum of base backup");
	}

	/* If the file was truncated while we were sending it, pad it with zeros */
	/* 如果文件在发送时被截断，则用零填充 */
	while (bytes_done < statbuf->st_size)
	{
		size_t		remaining = statbuf->st_size - bytes_done;
		size_t		nbytes = Min(sink->bbs_buffer_length, remaining);

		MemSet(sink->bbs_buffer, 0, nbytes);
		if (pg_checksum_update(&checksum_ctx,
							   (uint8 *) sink->bbs_buffer,
							   nbytes) < 0)
			elog(ERROR, "could not update checksum of base backup");
		bbsink_archive_contents(sink, nbytes);
		bytes_done += nbytes;
	}

	/*
	 * Pad to a block boundary, per tar format requirements. (This small piece
	 * of data is probably not worth throttling, and is not checksummed
	 * because it's not actually part of the file.)
	 */
	_tarWritePadding(sink, bytes_done);

	// 关闭打开的临时文件
	CloseTransientFile(fd);

	if (checksum_failures > 1)
	{
		ereport(WARNING,
				(errmsg_plural("file \"%s\" has a total of %d checksum verification failure",
							   "file \"%s\" has a total of %d checksum verification failures",
							   checksum_failures,
							   readfilename, checksum_failures)));

		pgstat_report_checksum_failures_in_db(dboid, checksum_failures);
	}

	total_checksum_failures += checksum_failures;

	// 添加文件到备份清单
	AddFileToBackupManifest(manifest, spcoid, tarfilename, statbuf->st_size,
							(pg_time_t) statbuf->st_mtime, &checksum_ctx);

	return true;
}

/*
 * Read some more data from the file into the bbsink's buffer, verifying
 * checksums as required.
 *
 * 'offset' is the file offset from which we should begin to read, and
 * 'length' is the amount of data that should be read. The actual amount
 * of data read will be less than the requested amount if the bbsink's
 * buffer isn't big enough to hold it all, or if the underlying file has
 * been truncated. The return value is the number of bytes actually read.
 *
 * 'blkno' is the block number of the first page in the bbsink's buffer
 * relative to the start of the relation.
 *
 * 'verify_checksum' indicates whether we should try to verify checksums
 * for the blocks we read. If we do this, we'll update *checksum_failures
 * and issue warnings as appropriate.
 */
static off_t
read_file_data_into_buffer(bbsink *sink, const char *readfilename, int fd,
						   off_t offset, size_t length, BlockNumber blkno,
						   bool verify_checksum, int *checksum_failures)
{
	off_t		cnt;
	int			i;
	char	   *page;

	/* Try to read some more data. */
	/* 尝试读取更多数据 */
	cnt = basebackup_read_file(fd, sink->bbs_buffer,
							   Min(sink->bbs_buffer_length, length),
							   offset, readfilename, true);

	/* Can't verify checksums if read length is not a multiple of BLCKSZ. */
	if (!verify_checksum || (cnt % BLCKSZ) != 0)
		return cnt;

	/* Verify checksum for each block. */
	for (i = 0; i < cnt / BLCKSZ; i++)
	{
		int			reread_cnt;
		uint16		expected_checksum;

		page = sink->bbs_buffer + BLCKSZ * i;

		/* If the page is OK, go on to the next one. */
		/* 如果页面正常，则继续下一页。 */
		if (verify_page_checksum(page, sink->bbs_state->startptr, blkno + i,
								 &expected_checksum))
			continue;

		/*
		 * Retry the block on the first failure.  It's possible that we read
		 * the first 4K page of the block just before postgres updated the
		 * entire block so it ends up looking torn to us. If, before we retry
		 * the read, the concurrent write of the block finishes, the page LSN
		 * will be updated and we'll realize that we should ignore this block.
		 *
		 * There's no guarantee that this will actually happen, though: the
		 * torn write could take an arbitrarily long time to complete.
		 * Retrying multiple times wouldn't fix this problem, either, though
		 * it would reduce the chances of it happening in practice. The only
		 * real fix here seems to be to have some kind of interlock that
		 * allows us to wait until we can be certain that no write to the
		 * block is in progress. Since we don't have any such thing right now,
		 * we just do this and hope for the best.
		 */
		reread_cnt =
			basebackup_read_file(fd, sink->bbs_buffer + BLCKSZ * i,
								 BLCKSZ, offset + BLCKSZ * i,
								 readfilename, false);
		if (reread_cnt == 0)
		{
			/*
			 * If we hit end-of-file, a concurrent truncation must have
			 * occurred, so reduce cnt to reflect only the blocks already
			 * processed and break out of this loop.
			 */
			cnt = BLCKSZ * i;
			break;
		}

		/* If the page now looks OK, go on to the next one. */
		if (verify_page_checksum(page, sink->bbs_state->startptr, blkno + i,
								 &expected_checksum))
			continue;

		/* Handle checksum failure. */
		/* 处理校验和失败 */
		(*checksum_failures)++;
		if (*checksum_failures <= 5)
			ereport(WARNING,
					(errmsg("checksum verification failed in "
							"file \"%s\", block %u: calculated "
							"%X but expected %X",
							readfilename, blkno + i, expected_checksum,
							((PageHeader) page)->pd_checksum)));
		if (*checksum_failures == 5)
			ereport(WARNING,
					(errmsg("further checksum verification "
							"failures in file \"%s\" will not "
							"be reported", readfilename)));
	}

	return cnt;
}

/*
 * Push data into a bbsink.
 *
 * It's better, when possible, to read data directly into the bbsink's buffer,
 * rather than using this function to copy it into the buffer; this function is
 * for cases where that approach is not practical.
 *
 * bytes_done should point to a count of the number of bytes that are
 * currently used in the bbsink's buffer. Upon return, the bytes identified by
 * data and length will have been copied into the bbsink's buffer, flushing
 * as required, and *bytes_done will have been updated accordingly. If the
 * buffer was flushed, the previous contents will also have been fed to
 * checksum_ctx.
 *
 * Note that after one or more calls to this function it is the caller's
 * responsibility to perform any required final flush.
 */
static void
push_to_sink(bbsink *sink, pg_checksum_context *checksum_ctx,
			 size_t *bytes_done, void *data, size_t length)
{
	while (length > 0)
	{
		size_t		bytes_to_copy;

		/*
		 * We use < here rather than <= so that if the data exactly fills the
		 * remaining buffer space, we trigger a flush now.
		 */
		if (length < sink->bbs_buffer_length - *bytes_done)
		{
			/* Append remaining data to buffer. */
			memcpy(sink->bbs_buffer + *bytes_done, data, length);
			*bytes_done += length;
			return;
		}

		/* Copy until buffer is full and flush it. */
		bytes_to_copy = sink->bbs_buffer_length - *bytes_done;
		memcpy(sink->bbs_buffer + *bytes_done, data, bytes_to_copy);
		data = ((char *) data) + bytes_to_copy;
		length -= bytes_to_copy;
		bbsink_archive_contents(sink, sink->bbs_buffer_length);
		if (pg_checksum_update(checksum_ctx, (uint8 *) sink->bbs_buffer,
							   sink->bbs_buffer_length) < 0)
			elog(ERROR, "could not update checksum");
		*bytes_done = 0;
	}
}

/*
 * Try to verify the checksum for the provided page, if it seems appropriate
 * to do so.
 *
 * Returns true if verification succeeds or if we decide not to check it,
 * and false if verification fails. When return false, it also sets
 * *expected_checksum to the computed value.
 */
static bool
verify_page_checksum(Page page, XLogRecPtr start_lsn, BlockNumber blkno,
					 uint16 *expected_checksum)
{
	PageHeader	phdr;
	uint16		checksum;

	/*
	 * Only check pages which have not been modified since the start of the
	 * base backup. Otherwise, they might have been written only halfway and
	 * the checksum would not be valid.  However, replaying WAL would
	 * reinstate the correct page in this case. We also skip completely new
	 * pages, since they don't have a checksum yet.
	 */
	if (PageIsNew(page) || PageGetLSN(page) >= start_lsn)
		return true;

	/* Perform the actual checksum calculation. */
	checksum = pg_checksum_page(page, blkno);

	/* See whether it matches the value from the page. */
	phdr = (PageHeader) page;
	if (phdr->pd_checksum == checksum)
		return true;
	*expected_checksum = checksum;
	return false;
}

static int64
_tarWriteHeader(bbsink *sink, const char *filename, const char *linktarget,
				struct stat *statbuf, bool sizeonly)
{
	enum tarError rc;

	if (!sizeonly)
	{
		/*
		 * As of this writing, the smallest supported block size is 1kB, which
		 * is twice TAR_BLOCK_SIZE. Since the buffer size is required to be a
		 * multiple of BLCKSZ, it should be safe to assume that the buffer is
		 * large enough to fit an entire tar block. We double-check by means
		 * of these assertions.
		 */
		StaticAssertDecl(TAR_BLOCK_SIZE <= BLCKSZ,
						 "BLCKSZ too small for tar block");
		Assert(sink->bbs_buffer_length >= TAR_BLOCK_SIZE);

		rc = tarCreateHeader(sink->bbs_buffer, filename, linktarget,
							 statbuf->st_size, statbuf->st_mode,
							 statbuf->st_uid, statbuf->st_gid,
							 statbuf->st_mtime);

		switch (rc)
		{
			case TAR_OK:
				break;
			case TAR_NAME_TOO_LONG:
				ereport(ERROR,
						(errmsg("file name too long for tar format: \"%s\"",
								filename)));
				break;
			case TAR_SYMLINK_TOO_LONG:
				ereport(ERROR,
						(errmsg("symbolic link target too long for tar format: "
								"file name \"%s\", target \"%s\"",
								filename, linktarget)));
				break;
			default:
				elog(ERROR, "unrecognized tar error: %d", rc);
		}

		bbsink_archive_contents(sink, TAR_BLOCK_SIZE);
	}

	return TAR_BLOCK_SIZE;
}

/*
 * Pad with zero bytes out to a multiple of TAR_BLOCK_SIZE.
 */
static void
_tarWritePadding(bbsink *sink, int len)
{
	int			pad = tarPaddingBytesRequired(len);

	/*
	 * As in _tarWriteHeader, it should be safe to assume that the buffer is
	 * large enough that we don't need to do this in multiple chunks.
	 */
	Assert(sink->bbs_buffer_length >= TAR_BLOCK_SIZE);
	Assert(pad <= TAR_BLOCK_SIZE);

	if (pad > 0)
	{
		MemSet(sink->bbs_buffer, 0, pad);
		bbsink_archive_contents(sink, pad);
	}
}

/*
 * If the entry in statbuf is a link, then adjust statbuf to make it look like a
 * directory, so that it will be written that way.
 */
static void
convert_link_to_directory(const char *pathbuf, struct stat *statbuf)
{
	/* If symlink, write it as a directory anyway */
	if (S_ISLNK(statbuf->st_mode))
		statbuf->st_mode = S_IFDIR | pg_dir_create_mode;
}

/*
 * Read some data from a file, setting a wait event and reporting any error
 * encountered.
 *
 * If partial_read_ok is false, also report an error if the number of bytes
 * read is not equal to the number of bytes requested.
 *
 * Returns the number of bytes read.
 */
static ssize_t
basebackup_read_file(int fd, char *buf, size_t nbytes, off_t offset,
					 const char *filename, bool partial_read_ok)
{
	ssize_t		rc;

	pgstat_report_wait_start(WAIT_EVENT_BASEBACKUP_READ);
	rc = pg_pread(fd, buf, nbytes, offset);
	pgstat_report_wait_end();

	if (rc < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m", filename)));
	if (!partial_read_ok && rc > 0 && rc != nbytes)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": read %zd of %zu",
						filename, rc, nbytes)));

	return rc;
}
