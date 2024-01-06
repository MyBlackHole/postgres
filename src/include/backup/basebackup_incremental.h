/*-------------------------------------------------------------------------
 *
 * basebackup_incremental.h
 *	  API for incremental backup support
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup_incremental.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BASEBACKUP_INCREMENTAL_H
#define BASEBACKUP_INCREMENTAL_H

#include "access/xlogbackup.h"
#include "common/relpath.h"
#include "storage/block.h"
#include "utils/palloc.h"

#define INCREMENTAL_MAGIC			0xd3ae1f0d

typedef enum
{
	// 全量备份文件
	BACK_UP_FILE_FULLY,
	// 增量备份文件
	BACK_UP_FILE_INCREMENTALLY
} FileBackupMethod;

struct IncrementalBackupInfo;
typedef struct IncrementalBackupInfo IncrementalBackupInfo;

extern IncrementalBackupInfo *CreateIncrementalBackupInfo(MemoryContext);

// 接收数据到 ib.buf
extern void AppendIncrementalManifestData(IncrementalBackupInfo *ib,
										  const char *data,
										  int len);
// 解析 ib.buf 增量清单信息(信息是上一次备份清单)
extern void FinalizeIncrementalManifest(IncrementalBackupInfo *ib);

// 增量备份准备
extern void PrepareForIncrementalBackup(IncrementalBackupInfo *ib,
										BackupState *backup_state);

extern char *GetIncrementalFilePath(Oid dboid, Oid spcoid,
									RelFileNumber relfilenumber,
									ForkNumber forknum, unsigned segno);
// 获取备份方法(全量或增量)
extern FileBackupMethod GetFileBackupMethod(IncrementalBackupInfo *ib,
											const char *path,
											Oid dboid, Oid spcoid,
											RelFileNumber relfilenumber,
											ForkNumber forknum,
											unsigned segno, size_t size,
											unsigned *num_blocks_required,
											BlockNumber *relative_block_numbers,
											unsigned *truncation_block_length);
// 获取增量大小
extern size_t GetIncrementalFileSize(unsigned num_blocks_required);
extern size_t GetIncrementalHeaderSize(unsigned num_blocks_required);

#endif
