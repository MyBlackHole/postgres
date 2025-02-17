/*-------------------------------------------------------------------------
 *
 * walsummary.c
 *	  Functions for accessing and managing WAL summary data.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/backend/backup/walsummary.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xlog_internal.h"
#include "backup/walsummary.h"
#include "common/int.h"
#include "utils/wait_event.h"

static bool IsWalSummaryFilename(char *filename);
static int	ListComparatorForWalSummaryFiles(const ListCell *a,
											 const ListCell *b);

/*
 * Get a list of WAL summaries.
 *
 * If tli != 0, only WAL summaries with the indicated TLI will be included.
 *
 * If start_lsn != InvalidXLogRecPtr, only summaries that end after the
 * indicated LSN will be included.
 *
 * If end_lsn != InvalidXLogRecPtr, only summaries that start before the
 * indicated LSN will be included.
 *
 * The intent is that you can call GetWalSummaries(tli, start_lsn, end_lsn)
 * to get all WAL summaries on the indicated timeline that overlap the
 * specified LSN range.
 *
 * 获取 WAL 摘要列表。
 * 
 * 如果 tli != 0，则仅包含具有指定 TLI 的 WAL 摘要。
 * 
 * 如果 start_lsn != InvalidXLogRecPtr，则仅包含在指示的 LSN 之后结束的摘要。
 * 
 * 如果 end_lsn != InvalidXLogRecPtr，则仅包含在指示的 LSN 之前开始的摘要。
 * 
 * 目的是您可以调用 GetWalSummaries(tli, start_lsn, end_lsn) 来获取指定时间线上与指定 LSN 范围重叠的所有 WAL 摘要。
 */
List *
GetWalSummaries(TimeLineID tli, XLogRecPtr start_lsn, XLogRecPtr end_lsn)
{
	DIR		   *sdir;
	struct dirent *dent;
	List	   *result = NIL;

	sdir = AllocateDir(XLOGDIR "/summaries");
	while ((dent = ReadDir(sdir, XLOGDIR "/summaries")) != NULL)
	{
		WalSummaryFile *ws;
		uint32		tmp[5];
		TimeLineID	file_tli;
		XLogRecPtr	file_start_lsn;
		XLogRecPtr	file_end_lsn;

		/* Decode filename, or skip if it's not in the expected format. */
		/* 解码文件名，如果不是预期格式则跳过。 */
		if (!IsWalSummaryFilename(dent->d_name))
			continue;
		// 好家伙信息在文件名里
		sscanf(dent->d_name, "%08X%08X%08X%08X%08X",
			   &tmp[0], &tmp[1], &tmp[2], &tmp[3], &tmp[4]);
		file_tli = tmp[0];
		file_start_lsn = ((uint64) tmp[1]) << 32 | tmp[2];
		file_end_lsn = ((uint64) tmp[3]) << 32 | tmp[4];

		/* Skip if it doesn't match the filter criteria. */
		if (tli != 0 && tli != file_tli)
			continue;
		if (!XLogRecPtrIsInvalid(start_lsn) && start_lsn >= file_end_lsn)
			continue;
		if (!XLogRecPtrIsInvalid(end_lsn) && end_lsn <= file_start_lsn)
			continue;

		/* Add it to the list. */
		ws = palloc(sizeof(WalSummaryFile));
		ws->tli = file_tli;
		ws->start_lsn = file_start_lsn;
		ws->end_lsn = file_end_lsn;
		result = lappend(result, ws);
	}
	FreeDir(sdir);

	return result;
}

/*
 * Build a new list of WAL summaries based on an existing list, but filtering
 * out summaries that don't match the search parameters.
 *
 * If tli != 0, only WAL summaries with the indicated TLI will be included.
 *
 * If start_lsn != InvalidXLogRecPtr, only summaries that end after the
 * indicated LSN will be included.
 *
 * If end_lsn != InvalidXLogRecPtr, only summaries that start before the
 * indicated LSN will be included.
 *
 * 基于现有列表构建新的 WAL 摘要列表，但过滤掉与搜索参数不匹配的摘要。
 * 
 * 如果 tli != 0，则仅包含具有指定 TLI 的 WAL 摘要。
 * 
 * 如果 start_lsn != InvalidXLogRecPtr，则仅包含在指示的 LSN 之后结束的摘要。
 * 
 * 如果 end_lsn != InvalidXLogRecPtr，则仅包含在指示的 LSN 之前开始的摘要。
 */
List *
FilterWalSummaries(List *wslist, TimeLineID tli,
				   XLogRecPtr start_lsn, XLogRecPtr end_lsn)
{
	List	   *result = NIL;
	ListCell   *lc;

	/* Loop over input. */
	foreach(lc, wslist)
	{
		WalSummaryFile *ws = lfirst(lc);

		/* Skip if it doesn't match the filter criteria. */
		if (tli != 0 && tli != ws->tli)
			continue;
		if (!XLogRecPtrIsInvalid(start_lsn) && start_lsn > ws->end_lsn)
			continue;
		if (!XLogRecPtrIsInvalid(end_lsn) && end_lsn < ws->start_lsn)
			continue;

		/* Add it to the result list. */
		result = lappend(result, ws);
	}

	return result;
}

/*
 * Check whether the supplied list of WalSummaryFile objects covers the
 * whole range of LSNs from start_lsn to end_lsn. This function ignores
 * timelines, so the caller should probably filter using the appropriate
 * timeline before calling this.
 *
 * If the whole range of LSNs is covered, returns true, otherwise false.
 * If false is returned, *missing_lsn is set either to InvalidXLogRecPtr
 * if there are no WAL summary files in the input list, or to the first LSN
 * in the range that is not covered by a WAL summary file in the input list.
 *
 * 检查提供的 WalSummaryFile 对象列表是否涵盖从 start_lsn 到 end_lsn 的整个 LSN 范围
 * 此函数忽略时间线，因此调用者应该在调用此函数之前使用适当的时间线进行过滤
 * 如果覆盖了整个 LSN 范围，则返回 true，否则返回 false
 * 如果返回 false，则 *missing_lsn 设置为 InvalidXLogRecPtr（如果输入列表中没有 WAL 摘要文件），
 * 或者设置为输入列表中 WAL 摘要文件未覆盖的范围内的第一个 LSN。
 */
bool
WalSummariesAreComplete(List *wslist, XLogRecPtr start_lsn,
						XLogRecPtr end_lsn, XLogRecPtr *missing_lsn)
{
	XLogRecPtr	current_lsn = start_lsn;
	ListCell   *lc;

	/* Special case for empty list. */
	/* 空列表的特殊情况。 */
	if (wslist == NIL)
	{
		*missing_lsn = InvalidXLogRecPtr;
		return false;
	}

	/* Make a private copy of the list and sort it by start LSN. */
	/* 制作列表的私有副本并按起始 LSN 对其进行排序。 */
	wslist = list_copy(wslist);
	list_sort(wslist, ListComparatorForWalSummaryFiles);

	/*
	 * Consider summary files in order of increasing start_lsn, advancing the
	 * known-summarized range from start_lsn toward end_lsn.
	 *
	 * Normally, the summary files should cover non-overlapping WAL ranges,
	 * but this algorithm is intended to be correct even in case of overlap.
	 */
	foreach(lc, wslist)
	{
		WalSummaryFile *ws = lfirst(lc);

		if (ws->start_lsn > current_lsn)
		{
			/* We found a gap. */
			break;
		}
		if (ws->end_lsn > current_lsn)
		{
			/*
			 * Next summary extends beyond end of previous summary, so extend
			 * the end of the range known to be summarized.
			 */
			current_lsn = ws->end_lsn;

			/*
			 * If the range we know to be summarized has reached the required
			 * end LSN, we have proved completeness.
			 */
			if (current_lsn >= end_lsn)
				// 没问题
				return true;
		}
	}

	/*
	 * We either ran out of summary files without reaching the end LSN, or we
	 * hit a gap in the sequence that resulted in us bailing out of the loop
	 * above.
	 */
	*missing_lsn = current_lsn;
	return false;
}

/*
 * Open a WAL summary file.
 *
 * This will throw an error in case of trouble. As an exception, if
 * missing_ok = true and the trouble is specifically that the file does
 * not exist, it will not throw an error and will return a value less than 0.
 *
 * 打开 WAL 摘要文件。
 * 
 * 如果出现问题，这将引发错误。 
 * 作为例外，如果 Missing_ok = true 并且问题具体在于该文件不存在，则它不会抛出错误并返回一个小于 0 的值。
 */
File
OpenWalSummaryFile(WalSummaryFile *ws, bool missing_ok)
{
	char		path[MAXPGPATH];
	File		file;

	// 例子: 00000001000000000C000028000000000D000060.summary
	snprintf(path, MAXPGPATH,
			 XLOGDIR "/summaries/%08X%08X%08X%08X%08X.summary",
			 ws->tli,
			 LSN_FORMAT_ARGS(ws->start_lsn),
			 LSN_FORMAT_ARGS(ws->end_lsn));

	file = PathNameOpenFile(path, O_RDONLY);
	if (file < 0 && (errno != EEXIST || !missing_ok))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\": %m", path)));

	return file;
}

/*
 * Remove a WAL summary file if the last modification time precedes the
 * cutoff time.
 */
void
RemoveWalSummaryIfOlderThan(WalSummaryFile *ws, time_t cutoff_time)
{
	char		path[MAXPGPATH];
	struct stat statbuf;

	snprintf(path, MAXPGPATH,
			 XLOGDIR "/summaries/%08X%08X%08X%08X%08X.summary",
			 ws->tli,
			 LSN_FORMAT_ARGS(ws->start_lsn),
			 LSN_FORMAT_ARGS(ws->end_lsn));

	if (lstat(path, &statbuf) != 0)
	{
		if (errno == ENOENT)
			return;
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	}
	if (statbuf.st_mtime >= cutoff_time)
		return;
	if (unlink(path) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not stat file \"%s\": %m", path)));
	ereport(DEBUG2,
			(errmsg_internal("removing file \"%s\"", path)));
}

/*
 * Test whether a filename looks like a WAL summary file.
 *
 * 测试文件名是否看起来像 WAL 摘要文件。
 * 例如:
 * 000000010000000008000028000000000A000028.summary
 */
static bool
IsWalSummaryFilename(char *filename)
{
	return strspn(filename, "0123456789ABCDEF") == 40 &&
		strcmp(filename + 40, ".summary") == 0;
}

/*
 * Data read callback for use with CreateBlockRefTableReader.
 *
 * 与 CreateBlockRefTableReader 一起使用的数据读取回调。
 */
int
ReadWalSummary(void *wal_summary_io, void *data, int length)
{
	WalSummaryIO *io = wal_summary_io;
	int			nbytes;

	nbytes = FileRead(io->file, data, length, io->filepos,
					  WAIT_EVENT_WAL_SUMMARY_READ);
	if (nbytes < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read file \"%s\": %m",
						FilePathName(io->file))));

	io->filepos += nbytes;
	return nbytes;
}

/*
 * Data write callback for use with WriteBlockRefTable.
 */
int
WriteWalSummary(void *wal_summary_io, void *data, int length)
{
	WalSummaryIO *io = wal_summary_io;
	int			nbytes;

	nbytes = FileWrite(io->file, data, length, io->filepos,
					   WAIT_EVENT_WAL_SUMMARY_WRITE);
	if (nbytes < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": %m",
						FilePathName(io->file))));
	if (nbytes != length)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write file \"%s\": wrote only %d of %d bytes at offset %u",
						FilePathName(io->file), nbytes,
						length, (unsigned) io->filepos),
				 errhint("Check free disk space.")));

	io->filepos += nbytes;
	return nbytes;
}

/*
 * Error-reporting callback for use with CreateBlockRefTableReader.
 */
void
ReportWalSummaryError(void *callback_arg, char *fmt,...)
{
	StringInfoData buf;
	va_list		ap;
	int			needed;

	initStringInfo(&buf);
	for (;;)
	{
		va_start(ap, fmt);
		needed = appendStringInfoVA(&buf, fmt, ap);
		va_end(ap);
		if (needed == 0)
			break;
		enlargeStringInfo(&buf, needed);
	}
	ereport(ERROR,
			errcode(ERRCODE_DATA_CORRUPTED),
			errmsg_internal("%s", buf.data));
}

/*
 * Comparator to sort a List of WalSummaryFile objects by start_lsn.
 */
static int
ListComparatorForWalSummaryFiles(const ListCell *a, const ListCell *b)
{
	WalSummaryFile *ws1 = lfirst(a);
	WalSummaryFile *ws2 = lfirst(b);

	return pg_cmp_u64(ws1->start_lsn, ws2->start_lsn);
}
