/*-------------------------------------------------------------------------
 *
 * basebackup.h
 *	  Exports from replication/basebackup.c.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/include/backup/basebackup.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _BASEBACKUP_H
#define _BASEBACKUP_H

#include "nodes/replnodes.h"

/*
 * Minimum and maximum values of MAX_RATE option in BASE_BACKUP command.
 */
#define MAX_RATE_LOWER	32
#define MAX_RATE_UPPER	1048576

/*
 * Information about a tablespace
 *
 * In some usages, "path" can be NULL to denote the PGDATA directory itself.
 *
 * 有关表空间的信息
 */
typedef struct
{
	// 表空间 oid
	Oid			oid;			/* tablespace's OID */
	// 全量路径
	char	   *path;			/* full path to tablespace's directory */
	// 相对路径
	char	   *rpath;			/* relative path if it's within PGDATA, else
								 * NULL */
	/* 发送的总大小； -1 如果不知道 */
	int64		size;			/* total size as sent; -1 if not known */
} tablespaceinfo;

struct IncrementalBackupInfo;

extern void SendBaseBackup(BaseBackupCmd *cmd,
						   struct IncrementalBackupInfo *ib);

#endif							/* _BASEBACKUP_H */
