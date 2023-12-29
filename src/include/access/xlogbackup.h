/*-------------------------------------------------------------------------
 *
 * xlogbackup.h
 *		Definitions for internals of base backups.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/xlogbackup.h
 *-------------------------------------------------------------------------
 */

#ifndef XLOG_BACKUP_H
#define XLOG_BACKUP_H

#include "access/xlogdefs.h"
#include "pgtime.h"

/* Structure to hold backup state. */
/* 保存备份状态的结构 */
typedef struct BackupState
{
	/* Fields saved at backup start */
	/* Backup label name one extra byte for null-termination */
	/* 备份开始时保存的字段 */
	/* 备份标签名称多一个字节用于空终止 */
	char		name[MAXPGPATH + 1];
	// 备份开始 wal 位置
	XLogRecPtr	startpoint;		/* backup start WAL location */
	// 备份开始时间线
	TimeLineID	starttli;		/* backup start TLI */
	// 最后检查点位置
	XLogRecPtr	checkpointloc;	/* last checkpoint location */
	// 备份开始时间
	pg_time_t	starttime;		/* backup start time */
	// 备份在恢复中开始?
	bool		started_in_recovery;	/* backup started in recovery? */
	// 基于此 LSN 的备份的增量
	XLogRecPtr	istartpoint;	/* incremental based on backup at this LSN */
	// 基于此 TLI 上的备份的增量
	TimeLineID	istarttli;		/* incremental based on backup on this TLI */

	/* Fields saved at the end of backup */
	/* 备份结束时保存的字段 */
	// 备份停止WAL位置
	XLogRecPtr	stoppoint;		/* backup stop WAL location */
	// 备份停止 TLI
	TimeLineID	stoptli;		/* backup stop TLI */
	// 备份停止时间
	pg_time_t	stoptime;		/* backup stop time */
} BackupState;

extern char *build_backup_content(BackupState *state,
								  bool ishistoryfile);

#endif							/* XLOG_BACKUP_H */
