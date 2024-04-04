/*
 * xlogrecord.h
 *
 * Definitions for the WAL record format.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlogrecord.h
 */
#ifndef XLOGRECORD_H
#define XLOGRECORD_H

#include "access/rmgr.h"
#include "access/xlogdefs.h"
#include "port/pg_crc32c.h"
#include "storage/block.h"
#include "storage/relfilelocator.h"

/*
 * The overall layout of an XLOG record is:
 * XLOG记录的总体布局是：
 *		Fixed-size header (XLogRecord struct)
 *		XLogRecordBlockHeader struct
 *		XLogRecordBlockHeader struct
 *		...
 *		XLogRecordDataHeader[Short|Long] struct
 *		block data
 *		block data
 *		...
 *		main data
 *
 * There can be zero or more XLogRecordBlockHeaders, and 0 or more bytes of
 * rmgr-specific data not associated with a block.  XLogRecord structs
 * always start on MAXALIGN boundaries in the WAL files, but the rest of
 * the fields are not aligned.
 *
 * The XLogRecordBlockHeader, XLogRecordDataHeaderShort and
 * XLogRecordDataHeaderLong structs all begin with a single 'id' byte. It's
 * used to distinguish between block references, and the main data structs.
 *
 * 可以有零个或多个 XLogRecordBlockHeader，以及 0 个或多个不与块关联的 rmgr 特定数据字节。
 * XLogRecord 结构始终从 WAL 文件中的 MAXALIGN 边界开始，但其余字段未对齐。
 *
 * XLogRecordBlockHeader、XLogRecordDataHeaderShort 和 XLogRecordDataHeaderLong 结构均以单个“id”字节开头。 
 * 它用于区分块引用和主要数据结构。
 */
typedef struct XLogRecord
{
	// 整个记录的总长度
	uint32		xl_tot_len;		/* total len of entire record */
	// 事务 id
	TransactionId xl_xid;		/* xact id */
	// ptr 到日志中的上一条记录
	XLogRecPtr	xl_prev;		/* ptr to previous record in log */
	// 标志位，见下文
	uint8		xl_info;		/* flag bits, see below */
	// 该记录的资源管理器
	RmgrId		xl_rmid;		/* resource manager for this record */

	/* 2 bytes of padding here, initialize to zero */
	/* 这里填充2个字节，初始化为零*/

	pg_crc32c	xl_crc;			/* CRC for this record */

	/* XLogRecordBlockHeaders and XLogRecordDataHeader follow, no padding */
	/* XLogRecordBlockHeaders 和 XLogRecordDataHeader 跟随，无填充 */

} XLogRecord;

#define SizeOfXLogRecord	(offsetof(XLogRecord, xl_crc) + sizeof(pg_crc32c))

/*
 * The high 4 bits in xl_info may be used freely by rmgr. The
 * XLR_SPECIAL_REL_UPDATE and XLR_CHECK_CONSISTENCY bits can be passed by
 * XLogInsert caller. The rest are set internally by XLogInsert.
 */
#define XLR_INFO_MASK			0x0F
#define XLR_RMGR_INFO_MASK		0xF0

/*
 * XLogReader needs to allocate all the data of a WAL record in a single
 * chunk.  This means that a single XLogRecord cannot exceed MaxAllocSize
 * in length if we ignore any allocation overhead of the XLogReader.
 *
 * To accommodate some overhead, this value allows for 4M of allocation
 * overhead, that should be plenty enough for what the XLogReader
 * infrastructure expects as extra.
 */
#define XLogRecordMaxSize	(1020 * 1024 * 1024)

/*
 * If a WAL record modifies any relation files, in ways not covered by the
 * usual block references, this flag is set. This is not used for anything
 * by PostgreSQL itself, but it allows external tools that read WAL and keep
 * track of modified blocks to recognize such special record types.
 */
#define XLR_SPECIAL_REL_UPDATE	0x01

/*
 * Enforces consistency checks of replayed WAL at recovery. If enabled,
 * each record will log a full-page write for each block modified by the
 * record and will reuse it afterwards for consistency checks. The caller
 * of XLogInsert can use this value if necessary, but if
 * wal_consistency_checking is enabled for a rmgr this is set unconditionally.
 */
#define XLR_CHECK_CONSISTENCY	0x02

/*
 * Header info for block data appended to an XLOG record.
 *
 * 'data_length' is the length of the rmgr-specific payload data associated
 * with this block. It does not include the possible full page image, nor
 * XLogRecordBlockHeader struct itself.
 *
 * Note that we don't attempt to align the XLogRecordBlockHeader struct!
 * So, the struct must be copied to aligned local storage before use.
 */
// 附加到 XLOG 记录的块数据的标头信息。
typedef struct XLogRecordBlockHeader
{
	/* 块引用 ID */
	uint8		id;				/* block reference ID */
	/* 关系内的分叉和标志 */
	uint8		fork_flags;		/* fork within the relation, and flags */
	/* 有效负载字节数（不包括页面图像）*/
	uint16		data_length;	/* number of payload bytes (not including page
								 * image) */

	/* If BKPBLOCK_HAS_IMAGE, an XLogRecordBlockImageHeader struct follows */
	/* If BKPBLOCK_SAME_REL is not set, a RelFileLocator follows */
	/* BlockNumber follows */
} XLogRecordBlockHeader;

#define SizeOfXLogRecordBlockHeader (offsetof(XLogRecordBlockHeader, data_length) + sizeof(uint16))

/*
 * Additional header information when a full-page image is included
 * (i.e. when BKPBLOCK_HAS_IMAGE is set).
 *
 * The XLOG code is aware that PG data pages usually contain an unused "hole"
 * in the middle, which contains only zero bytes.  Since we know that the
 * "hole" is all zeros, we remove it from the stored data (and it's not counted
 * in the XLOG record's CRC, either).  Hence, the amount of block data actually
 * present is (BLCKSZ - <length of "hole" bytes>).
 *
 * Additionally, when wal_compression is enabled, we will try to compress full
 * page images using one of the supported algorithms, after removing the
 * "hole". This can reduce the WAL volume, but at some extra cost of CPU spent
 * on the compression during WAL logging. In this case, since the "hole"
 * length cannot be calculated by subtracting the number of page image bytes
 * from BLCKSZ, basically it needs to be stored as an extra information.
 * But when no "hole" exists, we can assume that the "hole" length is zero
 * and no such an extra information needs to be stored. Note that
 * the original version of page image is stored in WAL instead of the
 * compressed one if the number of bytes saved by compression is less than
 * the length of extra information. Hence, when a page image is successfully
 * compressed, the amount of block data actually present is less than
 * BLCKSZ - the length of "hole" bytes - the length of extra information.
 */
// 包含整页图像时的附加标题信息
typedef struct XLogRecordBlockImageHeader
{
	/* 页面图像字节数 */
	uint16		length;			/* number of page image bytes */
	/* “hole” 之前的字节数 */
	uint16		hole_offset;	/* number of bytes before "hole" */
	/* 标志位，见下文 */
	uint8		bimg_info;		/* flag bits, see below */

	/*
	 * If BKPIMAGE_HAS_HOLE and BKPIMAGE_COMPRESSED(), an
	 * XLogRecordBlockCompressHeader struct follows.
	 */
} XLogRecordBlockImageHeader;

#define SizeOfXLogRecordBlockImageHeader	\
	(offsetof(XLogRecordBlockImageHeader, bimg_info) + sizeof(uint8))

/* Information stored in bimg_info */
/* bimg_info中存储的信息 */

/* 页面图像有“洞” */
#define BKPIMAGE_HAS_HOLE		0x01	/* page image has "hole" */
/* 重播期间应恢复页面图像 */
#define BKPIMAGE_APPLY			0x02	/* page image should be restored
										 * during replay */
/* compression methods supported */
/* 支持的压缩方法 */
#define BKPIMAGE_COMPRESS_PGLZ	0x04
#define BKPIMAGE_COMPRESS_LZ4	0x08
#define BKPIMAGE_COMPRESS_ZSTD	0x10

// TODO BlackHole
#if PG_VERSION_NUM >= 130000
#define	BKPIMAGE_COMPRESSED(info) \
	((info & (BKPIMAGE_COMPRESS_PGLZ | BKPIMAGE_COMPRESS_LZ4 | \
			  BKPIMAGE_COMPRESS_ZSTD)) != 0)
#else
#define BKPIMAGE_IS_COMPRESSED		0x02	/* page image is compressed */
#define	BKPIMAGE_COMPRESSED(info) \
	(info & BKPIMAGE_IS_COMPRESSED)
#endif

/*
 * Extra header information used when page image has "hole" and
 * is compressed.
 */
// 当页面图像有“洞”并被压缩时使用额外的标头信息。
typedef struct XLogRecordBlockCompressHeader
{
	uint16		hole_length;	/* number of bytes in "hole" */
} XLogRecordBlockCompressHeader;

#define SizeOfXLogRecordBlockCompressHeader \
	sizeof(XLogRecordBlockCompressHeader)

/*
 * Maximum size of the header for a block reference. This is used to size a
 * temporary buffer for constructing the header.
 */
// 块引用的标头的最大大小。 
// 这用于确定用于构建标头的临时缓冲区的大小。
#define MaxSizeOfXLogRecordBlockHeader \
	(SizeOfXLogRecordBlockHeader + \
	 SizeOfXLogRecordBlockImageHeader + \
	 SizeOfXLogRecordBlockCompressHeader + \
	 sizeof(RelFileLocator) + \
	 sizeof(BlockNumber))

/*
 * The fork number fits in the lower 4 bits in the fork_flags field. The upper
 * bits are used for flags.
 */
// 分叉号位于 fork_flags 字段的低 4 位中。 高位用于标志。
#define BKPBLOCK_FORK_MASK	0x0F
#define BKPBLOCK_FLAG_MASK	0xF0
/* 块数据是一个 XLogRecordBlockImage */
#define BKPBLOCK_HAS_IMAGE	0x10	/* block data is an XLogRecordBlockImage */
#define BKPBLOCK_HAS_DATA	0x20
/* redo 将重新初始化页面 */
#define BKPBLOCK_WILL_INIT	0x40	/* redo will re-init the page */
// RelFileLocator 省略，同上一条 xlog record
#define BKPBLOCK_SAME_REL	0x80	/* RelFileLocator omitted, same as
									 * previous */

/*
 * XLogRecordDataHeaderShort/Long are used for the "main data" portion of
 * the record. If the length of the data is less than 256 bytes, the short
 * form is used, with a single byte to hold the length. Otherwise the long
 * form is used.
 *
 * (These structs are currently not used in the code, they are here just for
 * documentation purposes).
 */
typedef struct XLogRecordDataHeaderShort
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_SHORT */
	uint8		data_length;	/* number of payload bytes */
}			XLogRecordDataHeaderShort;

#define SizeOfXLogRecordDataHeaderShort (sizeof(uint8) * 2)

typedef struct XLogRecordDataHeaderLong
{
	uint8		id;				/* XLR_BLOCK_ID_DATA_LONG */
	/* followed by uint32 data_length, unaligned */
}			XLogRecordDataHeaderLong;

#define SizeOfXLogRecordDataHeaderLong (sizeof(uint8) + sizeof(uint32))

/*
 * Block IDs used to distinguish different kinds of record fragments. Block
 * references are numbered from 0 to XLR_MAX_BLOCK_ID. A rmgr is free to use
 * any ID number in that range (although you should stick to small numbers,
 * because the WAL machinery is optimized for that case). A few ID
 * numbers are reserved to denote the "main" data portion of the record,
 * as well as replication-supporting transaction metadata.
 *
 * The maximum is currently set at 32, quite arbitrarily. Most records only
 * need a handful of block references, but there are a few exceptions that
 * need more.
 */
#define XLR_MAX_BLOCK_ID			32

#define XLR_BLOCK_ID_DATA_SHORT		255
#define XLR_BLOCK_ID_DATA_LONG		254
#define XLR_BLOCK_ID_ORIGIN			253
#define XLR_BLOCK_ID_TOPLEVEL_XID	252

#endif							/* XLOGRECORD_H */
