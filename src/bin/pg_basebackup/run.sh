# 全量
pg_basebackup -F t -D Debug/buackup/`date +%F` -h 127.0.0.1 -p 5432 -U black

# 2023-12-30 00:10:06.736 CST [2254246] LOG:  checkpoint starting: force wait
# 2023-12-30 00:10:06.756 CST [2254246] LOG:  checkpoint complete: wrote 0 buffers (0.0%); 0 WAL file(s) added, 0 removed, 1 recycled; write=0.001 s, sync=0.001 s, total=0.021 s; sync files=0, longest=0.000 s, average=0.000 s; distance=16384 kB, estimate=16384 kB; lsn=0/6000080, redo lsn=0/6000028

show summarize_wal;

SELECT pg_reload_conf();

pg_basebackup -F t -i Debug/buackup/2023-12-30/backup_manifest -D Debug/buackup/`date '+%Fi-%M'` -h 127.0.0.1 -p 5432 -U black

# 2023-12-30 00:10:37.251 CST [2254246] LOG:  checkpoint starting: force wait
# 2023-12-30 00:10:37.367 CST [2254246] LOG:  checkpoint complete: wrote 0 buffers (0.0%); 0 WAL file(s) added, 0 removed, 1 recycled; write=0.001 s, sync=0.001 s, total=0.117 s; sync files=0, longest=0.000 s, average=0.000 s; distance=32768 kB, estimate=32768 kB; lsn=0/8000080, redo lsn=0/8000028
