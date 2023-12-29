/*-------------------------------------------------------------------------
 *
 * receivelog.h
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		  src/bin/pg_basebackup/receivelog.h
 *-------------------------------------------------------------------------
 */

#ifndef RECEIVELOG_H
#define RECEIVELOG_H

#include "access/xlogdefs.h"
#include "libpq-fe.h"
#include "walmethods.h"

/*
 * Called before trying to read more data or when a segment is
 * finished. Return true to stop streaming.
 */
typedef bool (*stream_stop_callback) (XLogRecPtr segendpos, uint32 timeline, bool segment_finished);

/*
 * Global parameters when receiving xlog stream. For details about the individual fields,
 * see the function comment for ReceiveXlogStream().
 */
typedef struct StreamCtl
{
	// 流式传输的起始位置
	XLogRecPtr	startpos;		/* Start position for streaming */
	// 流式传输数据的时间线
	TimeLineID	timeline;		/* Timeline to stream data from */
	// 用于验证该系统标识符和时间表
	char	   *sysidentifier;	/* Validate this system identifier and
								 * timeline */
	// 经常发送状态消息
	int			standby_message_timeout;	/* Send status messages this often */
	// 写入时立即刷新 WAL 数据
	bool		synchronous;	/* Flush immediately WAL data on write */
	// 在生成的存档中将段标记为已完成
	bool		mark_done;		/* Mark segment as done in generated archive */
	// 刷新到磁盘以确保数据的一致状态
	bool		do_sync;		/* Flush to disk to ensure consistent state of
								 * data */

	// 执行的回调, 返回 true 时停止流式传输
	stream_stop_callback stream_stop;	/* Stop streaming when returns true */

	// 如果有效，则监视此套接字上的输入，并在有任何输入时检查 stream_stop()
	pgsocket	stop_socket;	/* if valid, watch for input on this socket
								 * and check stream_stop() when there is any */

	// 如何写WAL
	WalWriteMethod *walmethod;	/* How to write the WAL */
	// 附加到部分接收的文件的后缀
	char	   *partial_suffix; /* Suffix appended to partially received files */
	// 要使用的复制槽，或 NULL
	char	   *replication_slot;	/* Replication slot to use, or NULL */
} StreamCtl;



extern bool CheckServerVersionForStreaming(PGconn *conn);
extern bool ReceiveXlogStream(PGconn *conn,
							  StreamCtl *stream);

#endif							/* RECEIVELOG_H */
