./configure CFLAGS='-g -O0' --enable-debug --prefix=/run/media/black/Data/lib/postgres/zh_master
./configure CFLAGS='-g -O0' --enable-debug --with-wal-blocksize=64

# 编译安装
make install

export PATH=/run/media/black/Data/lib/postgres/zh_master/bin:$PATH
export LD_LIBRARY_PATH=/run/media/black/Data/lib/postgres/zh_master/lib/:$LD_LIBRARY_PATH

# 初始化数据库 (需要安装后执行, 内部查找 postgres)
initdb -d -D Debug/database
initdb -d -D Debug/database --wal-segsize=1024

ls Debug/database
# base    pg_commit_ts  pg_hba.conf    pg_logical    pg_notify    pg_serial     pg_stat      pg_subtrans  pg_twophase  pg_wal   postgresql.auto.conf
# global  pg_dynshmem   pg_ident.conf  pg_multixact  pg_replslot  pg_snapshots  pg_stat_tmp  pg_tblspc    PG_VERSION   pg_xact  postgresql.conf

# 启动数据库服务
pg_ctl -D Debug/database -l logfile start

# 数据构造
pgbench -h 127.0.0.1 -i -s 5 postgres

❯ ps aux|grep postgres
black     717685  0.0  0.1 203732 22272 ?        Ss   21:07   0:00 /media/black/Data/lib/postgres/zh_master/bin/postgres -D Debug/database
black     717686  0.0  0.0 203864  3488 ?        Ss   21:07   0:00 postgres: checkpointer
black     717687  0.0  0.0 203888  4896 ?        Ss   21:07   0:00 postgres: background writer
black     717689  0.0  0.0 203732  7712 ?        Ss   21:07   0:00 postgres: walwriter
black     717690  0.0  0.0 205332  6560 ?        Ss   21:07   0:00 postgres: autovacuum launcher
black     717691  0.0  0.0 205312  5920 ?        Ss   21:07   0:00 postgres: logical replication launcher

# 停止数据库服务
pg_ctl -D Debug/database stop

# 连接(debug 建议用此)
psql postgres
psql -h 127.0.0.1 postgres
# black 为 linux 当前登陆用户
pgcli -h 127.0.0.1 -U black postgres


# 启动前
❯ ipcs -a

--------- 消息队列 -----------
键        msqid      拥有者  权限     已用字节数 消息

------------ 共享内存段 --------------
键        shmid      拥有者  权限     字节     连接数  状态
0x00000000 4          black      600        4194304    2          目标
0x00000000 7          black      600        524288     2          目标
0x00000000 10         black      600        524288     2          目标
0x00000000 13         black      600        1048576    2          目标
0x00000000 26         black      606        24576000   2          目标
0x00000000 27         black      606        24576000   2          目标
0x00000000 41         black      600        4194304    2          目标

--------- 信号量数组 -----------
键        semid      拥有者  权限     nsems


# 启动后
❯ ipcs -a

--------- 消息队列 -----------
键        msqid      拥有者  权限     已用字节数 消息

------------ 共享内存段 --------------
键        shmid      拥有者  权限     字节     连接数  状态
0x00000000 4          black      600        4194304    2          目标
0x00000000 7          black      600        524288     2          目标
0x00000000 10         black      600        524288     2          目标
0x00000000 13         black      600        1048576    2          目标
0x00000000 26         black      606        24576000   2          目标
0x00000000 27         black      606        24576000   2          目标
0x00000000 41         black      600        4194304    2          目标
0x00724065 131121     black      600        56         7

--------- 信号量数组 -----------
键        semid      拥有者  权限     nsems




# wal 分析

# XLOG记录的总体布局是：
#    Fixed-size header (XLogRecord struct)
#    XLogRecordBlockHeader struct
#    XLogRecordBlockHeader struct
#    ...
#    XLogRecordDataHeader[Short|Long] struct
#    block data
#    block data
#    ...
#    main data

XLogLongPageHeaderData
   XLogPageHeaderData std
上页不足缺少部分
XLOG记录

/run/media/black/Data/Documents/AIO/libpostgres_data
hexdump -C 0000000F00000000000000C0 > 0000000F00000000000000C0.hex

## src/include/access/xlog_internal.h:XLogLongPageHeaderData: XLogPageHeaderData std
### src/include/access/xlog_internal.h:XLogPageHeaderData

- src/include/access/xlog_internal.h:XLogPageHeaderData: uint16 xlp_magic
hexdump -C 0000000F00000000000000C0 -s 0 -n 2
00000000  06 d1                                             |..|
00000002

# define XLOG_PAGE_MAGIC 0xD106

- src/include/access/xlog_internal.h:XLogPageHeaderData: uint16 xlp_info
hexdump -C 0000000F00000000000000C0 -s 2 -n 2
00000002  06 00                                             |..|
00000004

#define XLP_LONG_HEADER				0x0002
#define XLP_BKP_REMOVABLE			0x0004
# XLP_LONG_HEADER | XLP_BKP_REMOVABLE


- src/include/access/xlog_internal.h:XLogPageHeaderData: TimeLineID(uint32) xlp_tli
hexdump -C 0000000F00000000000000C0 -s 4 -n 4
00000004  0f 00 00 00                                       |....| (0x0000000F)
00000008



- src/include/access/xlog_internal.h:XLogPageHeaderData: XLogRecPtr(uint64)	xlp_pageaddr
hexdump -C 0000000F00000000000000C0 -s 8 -n 8
00000008  00 00 00 c0 00 00 00 00                           |........| (0x00000000 c0000000)
00000010


- src/include/access/xlog_internal.h:XLogPageHeaderData: uint32 xlp_rem_len
hexdump -C 0000000F00000000000000C0 -s 16 -n 4
00000010  00 00 00 00                                       |....| (0x00000000)
00000014

(-s 24 是结构体内存需要对齐)
- src/include/access/xlog_internal.h:XLogLongPageHeaderData: uint64 xlp_sysid
hexdump -C 0000000F00000000000000C0 -s 24 -n 8
00000018  00 44 ce d3 d6 24 f4 65                           |.D...$.e| (0x65f424d6d3ce4400)
00000020

# pg_controldata /data/antdb/data
# pg_control version number:            1300
# Catalog version number:               202007201
# Database system identifier:           7346537397243233280
# .....

# hex(7346537397243233280)
# '0x65f424d6d3ce4400'

# sysidentifier = ((uint64) tv.tv_sec) << 32;
# sysidentifier |= ((uint64) tv.tv_usec) << 12;
# sysidentifier |= getpid() & 0xFFF;


- src/include/access/xlog_internal.h:XLogLongPageHeaderData: uint32 xlp_seg_size
hexdump -C 0000000F00000000000000C0 -s 32 -n 4
00000020  00 00 00 01                                       |....| (0x0100 0000) (wal 16M size)
00000024


- src/include/access/xlog_internal.h:XLogLongPageHeaderData: uint32 xlp_xlog_blcksz
hexdump -C 0000000F00000000000000C0 -s 36 -n 4
00000024  00 00 01 00                                       |....| (0x0001 0000)
00000028


"读取上一页不足占用的空间 (xlp_rem_len)(需要补齐 16Bytes?)"

### src/include/access/xlogrecord.h:XLogRecord
- src/include/access/xlogrecord.h:XLogRecord: uint32 xl_tot_len
hexdump -C 0000000F00000000000000C0 -s 40 -n 4
00000028  2e 08 00 00                                       |....| (0x0000 082e, 2094) (xlog record 长度)
0000002c


- src/include/access/xlogrecord.h:XLogRecord: TransactionId(uint32) xl_xid
hexdump -C 0000000F00000000000000C0 -s 44 -n 4
0000002c  d5 76 00 00                                       |.v..| (0x76d5, 30421)
00000030


- src/include/access/xlogrecord.h:XLogRecord: XLogRecPtr(uint64)	xl_prev
hexdump -C 0000000F00000000000000C0 -s 48 -n 8
00000030  88 3f 00 bf 00 00 00 00                           |.?......| (0x0000 0000 bf00 3f88) (上一个日志的偏移量)
00000038


- src/include/access/xlogrecord.h:XLogRecord: uint8		xl_info
hexdump -C 0000000F00000000000000C0 -s 56 -n 1
00000038  00                                                |.| (0x00) (XLOG_BRIN_CREATE_INDEX)
00000039

#define XLOG_BRIN_CREATE_INDEX		0x00
#define XLOG_BRIN_INSERT			0x10
# ...


- src/include/access/xlogrecord.h:XLogRecord: RmgrId(uint8)  xl_rmid
hexdump -C 0000000F00000000000000C0 -s 57 -n 1
00000039  0a                                                |.| (0x0a) (资源管理器)
0000003a


# 填充 2 个字节
hexdump -C 0000000F00000000000000C0 -s 58 -n 2
0000003a  00 00                                             |..|
0000003c


- src/include/access/xlogrecord.h:XLogRecord: pg_crc32c(uint32)	xl_crc
hexdump -C 0000000F00000000000000C0 -s 60 -n 4
0000003c  ce 53 64 26                                       |.Sd-| (0x2664 53ce)
00000040


### src/include/access/xlogrecord.h:XLogRecordBlockHeader


- src/include/access/xlogrecord.h:XLogRecordBlockHeader: uint8		id
hexdump -C 0000000F00000000000000C0 -s 64 -n 1
00000040  00                                                |.| (0x00)
00000041


- src/include/access/xlogrecord.h:XLogRecordBlockHeader: uint8		fork_flags
hexdump -C 0000000F00000000000000C0 -s 65 -n 1
00000041  10                                                |.| (0x10)
00000042

#define BKPBLOCK_FORK_MASK	0x0F
#define BKPBLOCK_FLAG_MASK	0xF0
#define BKPBLOCK_HAS_IMAGE	0x10	/* block data is an XLogRecordBlockImage */
# ...


- src/include/access/xlogrecord.h:XLogRecordBlockHeader: uint16		data_length
hexdump -C 0000000F00000000000000C0 -s 66 -n 2
00000042  00 00                                             |..| (0x0000)
00000044



