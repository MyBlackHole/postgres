./configure CFLAGS='-g -O0' --enable-debug --prefix=/media/black/Data/lib/postgres/zh_master

# 编译安装
make install

export PATH=/media/black/Data/lib/postgres/zh_master/bin:$PATH
export LD_LIBRARY_PATH=/media/black/Data/lib/postgres/zh_master/lib/:$LD_LIBRARY_PATH

# 初始化数据库
initdb -d -D Debug/database

ls Debug/database
# base    pg_commit_ts  pg_hba.conf    pg_logical    pg_notify    pg_serial     pg_stat      pg_subtrans  pg_twophase  pg_wal   postgresql.auto.conf
# global  pg_dynshmem   pg_ident.conf  pg_multixact  pg_replslot  pg_snapshots  pg_stat_tmp  pg_tblspc    PG_VERSION   pg_xact  postgresql.conf

# 启动数据库服务
pg_ctl -D Debug/database -l logfile start

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
