/*-------------------------------------------------------------------------
 *
 * blkreftable.c
 *	  Block reference tables.
 *
 * A block reference table is used to keep track of which blocks have
 * been modified by WAL records within a certain LSN range.
 *
 * For each relation fork, we keep track of all blocks that have appeared
 * in block reference in the WAL. We also keep track of the "limit block",
 * which is the smallest relation length in blocks known to have occurred
 * during that range of WAL records.  This should be set to 0 if the relation
 * fork is created or destroyed, and to the post-truncation length if
 * truncated.
 *
 * Whenever we set the limit block, we also forget about any modified blocks
 * beyond that point. Those blocks don't exist any more. Such blocks can
 * later be marked as modified again; if that happens, it means the relation
 * was re-extended.
 *
 * Portions Copyright (c) 2010-2024, PostgreSQL Global Development Group
 *
 * src/common/blkreftable.c
 *
 *-------------------------------------------------------------------------
 */


#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#ifdef FRONTEND
#include "common/logging.h"
#endif

#include "common/blkreftable.h"
#include "common/hashfn.h"
#include "port/pg_crc32c.h"

/*
 * A block reference table keeps track of the status of each relation
 * fork individually.
 *
 * 块引用表单独跟踪每个关系分支的状态。
 */
typedef struct BlockRefTableKey
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
} BlockRefTableKey;

/*
 * We could need to store data either for a relation in which only a
 * tiny fraction of the blocks have been modified or for a relation in
 * which nearly every block has been modified, and we want a
 * space-efficient representation in both cases. To accomplish this,
 * we divide the relation into chunks of 2^16 blocks and choose between
 * an array representation and a bitmap representation for each chunk.
 *
 * 我们可能需要为仅一小部分块被修改的关系或几乎每个块都被修改的关系存储数据，
 * 并且我们希望在这两种情况下都有一个节省空间的表示。
 * 为了实现这一点，我们将关系划分为 2^16 个块，并为每个块选择数组表示形式和位图表示形式。
 *
 * When the number of modified blocks in a given chunk is small, we
 * essentially store an array of block numbers, but we need not store the
 * entire block number: instead, we store each block number as a 2-byte
 * offset from the start of the chunk.
 *
 * When the number of modified blocks in a given chunk is large, we switch
 * to a bitmap representation.
 *
 * These same basic representational choices are used both when a block
 * reference table is stored in memory and when it is serialized to disk.
 *
 * In the in-memory representation, we initially allocate each chunk with
 * space for a number of entries given by INITIAL_ENTRIES_PER_CHUNK and
 * increase that as necessary until we reach MAX_ENTRIES_PER_CHUNK.
 * Any chunk whose allocated size reaches MAX_ENTRIES_PER_CHUNK is converted
 * to a bitmap, and thus never needs to grow further.
 */
// 每大块的块数
#define BLOCKS_PER_CHUNK		(1 << 16)
// 每个条目的块数
#define BLOCKS_PER_ENTRY		(BITS_PER_BYTE * sizeof(uint16))
// 每个大块最大条目数 (位标记时使用)
#define MAX_ENTRIES_PER_CHUNK	(BLOCKS_PER_CHUNK / BLOCKS_PER_ENTRY)
// 每个块的初始条目
#define INITIAL_ENTRIES_PER_CHUNK	16
typedef uint16 *BlockRefTableChunk;

/*
 * State for one relation fork.
 *
 * 'rlocator' and 'forknum' identify the relation fork to which this entry
 * pertains.
 *
 * 'limit_block' is the shortest known length of the relation in blocks
 * within the LSN range covered by a particular block reference table.
 * It should be set to 0 if the relation fork is created or dropped. If the
 * relation fork is truncated, it should be set to the number of blocks that
 * remain after truncation.
 *
 * 'nchunks' is the allocated length of each of the three arrays that follow.
 * We can only represent the status of block numbers less than nchunks *
 * BLOCKS_PER_CHUNK.
 *
 * 'chunk_size' is an array storing the allocated size of each chunk.
 *
 * 'chunk_usage' is an array storing the number of elements used in each
 * chunk. If that value is less than MAX_ENTRIES_PER_CHUNK, the corresponding
 * chunk is used as an array; else the corresponding chunk is used as a bitmap.
 * When used as a bitmap, the least significant bit of the first array element
 * is the status of the lowest-numbered block covered by this chunk.
 *
 * 'chunk_data' is the array of chunks.
 *
 * 状态一个关系叉。
 *
 * 'rlocator' 和 'forknum' 标识该条目所属的关系分支。
 *
 * “limit_block”是特定块引用表覆盖的LSN范围内的块中关系的最短已知长度。
 *  如果创建或删除关系分支，则应将其设置为 0。
 *  如果关系叉被截断，则应将其设置为截断后剩余的块数。
 *
 * 'nchunks' 是后面三个数组中每个数组的分配长度。 
 * 我们只能表示小于 nchunks * BLOCKS_PER_CHUNK 的块号的状态。
 *
 * “chunk_size” 是一个存储每个块的分配大小的数组。
 *
 * “chunk_usage” 是一个数组，存储每个块中使用的元素数量。
 * 如果该值小于 MAX_ENTRIES_PER_CHUNK，则相应的块将用作数组；
 * 否则，相应的块将用作位图。 
 * 当用作位图时，第一个数组元素的最低有效位是该块覆盖的最低编号块的状态。
 *
 * “chunk_data”是块的数组。
 */
struct BlockRefTableEntry
{
	BlockRefTableKey key;
	BlockNumber limit_block;
	char		status;
	uint32		nchunks;
	// 块分配大小
	uint16	   *chunk_size;
	// 存储每个块中使用的元素数量
	uint16	   *chunk_usage;
	BlockRefTableChunk *chunk_data;
};

/* Declare and define a hash table over type BlockRefTableEntry. */
#define SH_PREFIX blockreftable
#define SH_ELEMENT_TYPE BlockRefTableEntry
#define SH_KEY_TYPE BlockRefTableKey
#define SH_KEY key
#define SH_HASH_KEY(tb, key) \
	hash_bytes((const unsigned char *) &key, sizeof(BlockRefTableKey))
#define SH_EQUAL(tb, a, b) (memcmp(&a, &b, sizeof(BlockRefTableKey)) == 0)
#define SH_SCOPE static inline
#ifdef FRONTEND
#define SH_RAW_ALLOCATOR pg_malloc0
#endif
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

/*
 * A block reference table is basically just the hash table, but we don't
 * want to expose that to outside callers.
 *
 * We keep track of the memory context in use explicitly too, so that it's
 * easy to place all of our allocations in the same context.
 */
struct BlockRefTable
{
	blockreftable_hash *hash;
#ifndef FRONTEND
	MemoryContext mcxt;
#endif
};

/*
 * On-disk serialization format for block reference table entries.
 * 块引用表条目的磁盘序列化格式。
 */
typedef struct BlockRefTableSerializedEntry
{
	RelFileLocator rlocator;
	ForkNumber	forknum;
	// 限位块
	BlockNumber limit_block;
	uint32		nchunks;
} BlockRefTableSerializedEntry;

/*
 * Buffer size, so that we avoid doing many small I/Os.
 *
 * 缓冲区大小，这样我们就可以避免执行许多小 I/O。
 */
#define BUFSIZE					65536

/*
 * Ad-hoc buffer for file I/O.
 */
typedef struct BlockRefTableBuffer
{
	// 数据读取回调
	io_callback_fn io_callback;
	void	   *io_callback_arg;
	char		data[BUFSIZE];
	// 读取位置
	int			used;
	// 数据移动到 buf 位置
	int			cursor;
	pg_crc32c	crc;
} BlockRefTableBuffer;

/*
 * State for keeping track of progress while incrementally reading a block
 * table reference file from disk.
 *
 * total_chunks means the number of chunks for the RelFileLocator/ForkNumber
 * combination that is currently being read, and consumed_chunks is the number
 * of those that have been read. (We always read all the information for
 * a single chunk at one time, so we don't need to be able to represent the
 * state where a chunk has been partially read.)
 *
 * chunk_size is the array of chunk sizes. The length is given by total_chunks.
 *
 * chunk_data holds the current chunk.
 *
 * chunk_position helps us figure out how much progress we've made in returning
 * the block numbers for the current chunk to the caller. If the chunk is a
 * bitmap, it's the number of bits we've scanned; otherwise, it's the number
 * of chunk entries we've scanned.
 *
 * 用于在从磁盘增量读取块表引用文件时跟踪进度的状态。
 *
 * Total_chunks 表示当前正在读取的 RelFileLocator/ForkNumber 组合的块数，consumed_chunks 是已读取的块数。 
 * （我们总是一次读取单个块的所有信息，因此我们不需要能够表示块已被部分读取的状态。）
 *
 * chunk_size 是块大小的数组。 长度由total_chunks 给出。
 *
 * chunk_data 保存当前块。
 *
 * chunk_position 帮助我们了解在将当前块的块号返回给调用者方面我们取得了多少进展。
 * 如果块是位图，则它是我们扫描的位数； 否则，它是我们扫描过的块条目的数量。
 */
struct BlockRefTableReader
{
	BlockRefTableBuffer buffer;
	char	   *error_filename;
	report_error_fn error_callback;
	void	   *error_callback_arg;
	// 总块数
	uint32		total_chunks;
	// 是已读取的块数
	uint32		consumed_chunks;
	// 块大小数组
	uint16	   *chunk_size;
	// 块存储缓冲区
	uint16		chunk_data[MAX_ENTRIES_PER_CHUNK];
	uint32		chunk_position;
};

/*
 * State for keeping track of progress while incrementally writing a block
 * reference table file to disk.
 *
 * 用于在将块引用表文件增量写入磁盘时跟踪进度的状态。
 */
struct BlockRefTableWriter
{
	BlockRefTableBuffer buffer;
};

/* Function prototypes. */
static int	BlockRefTableComparator(const void *a, const void *b);
static void BlockRefTableFlush(BlockRefTableBuffer *buffer);
static void BlockRefTableRead(BlockRefTableReader *reader, void *data,
							  int length);
static void BlockRefTableWrite(BlockRefTableBuffer *buffer, void *data,
							   int length);
static void BlockRefTableFileTerminate(BlockRefTableBuffer *buffer);

/*
 * Create an empty block reference table.
 * 创建一个空的块引用表。
 */
BlockRefTable *
CreateEmptyBlockRefTable(void)
{
	BlockRefTable *brtab = palloc(sizeof(BlockRefTable));

	/*
	 * Even completely empty database has a few hundred relation forks, so it
	 * seems best to size the hash on the assumption that we're going to have
	 * at least a few thousand entries.
	 */
#ifdef FRONTEND
	brtab->hash = blockreftable_create(4096, NULL);
#else
	brtab->mcxt = CurrentMemoryContext;
	brtab->hash = blockreftable_create(brtab->mcxt, 4096, NULL);
#endif

	return brtab;
}

/*
 * Set the "limit block" for a relation fork and forget any modified blocks
 * with equal or higher block numbers.
 *
 * The "limit block" is the shortest known length of the relation within the
 * range of WAL records covered by this block reference table.
 *
 * 为关系分支设置“限制块”，并忘记任何具有相同或更高块编号的修改块。
 *
 * “限制块”是该块引用表覆盖的 WAL 记录范围内关系的最短已知长度。
 */
void
BlockRefTableSetLimitBlock(BlockRefTable *brtab,
						   const RelFileLocator *rlocator,
						   ForkNumber forknum,
						   BlockNumber limit_block)
{
	BlockRefTableEntry *brtentry;
	BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
	bool		found;

	memcpy(&key.rlocator, rlocator, sizeof(RelFileLocator));
	key.forknum = forknum;
	brtentry = blockreftable_insert(brtab->hash, key, &found);

	if (!found)
	{
		/*
		 * We have no existing data about this relation fork, so just record
		 * the limit_block value supplied by the caller, and make sure other
		 * parts of the entry are properly initialized.
		 *
		 * 我们没有关于此关系分支的现有数据，
		 * 因此只需记录调用者提供的 limit_block 值，
		 * 并确保条目的其他部分已正确初始化。
		 */
		brtentry->limit_block = limit_block;
		brtentry->nchunks = 0;
		brtentry->chunk_size = NULL;
		brtentry->chunk_usage = NULL;
		brtentry->chunk_data = NULL;
		return;
	}

	// 块参考表条目设置限制块
	BlockRefTableEntrySetLimitBlock(brtentry, limit_block);
}

/*
 * Mark a block in a given relation fork as known to have been modified.
 * 将给定关系分支中的块标记为已知已被修改。
 *
 * blknum: 块编号
 */
void
BlockRefTableMarkBlockModified(BlockRefTable *brtab,
							   const RelFileLocator *rlocator,
							   ForkNumber forknum,
							   BlockNumber blknum)
{
	BlockRefTableEntry *brtentry;
	/* 确保任何填充为零 */
	BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
	bool		found;
#ifndef FRONTEND
	MemoryContext oldcontext = MemoryContextSwitchTo(brtab->mcxt);
#endif

	memcpy(&key.rlocator, rlocator, sizeof(RelFileLocator));
	key.forknum = forknum;
	brtentry = blockreftable_insert(brtab->hash, key, &found);

	if (!found)
	{
		/*
		 * We want to set the initial limit block value to something higher
		 * than any legal block number. InvalidBlockNumber fits the bill.
		 *
		 * 我们希望将初始限制块值设置为高于任何合法块号。
		 * InvalidBlockNumber 符合要求。
		 */
		brtentry->limit_block = InvalidBlockNumber;
		brtentry->nchunks = 0;
		brtentry->chunk_size = NULL;
		brtentry->chunk_usage = NULL;
		brtentry->chunk_data = NULL;
	}

	BlockRefTableEntryMarkBlockModified(brtentry, forknum, blknum);

#ifndef FRONTEND
	MemoryContextSwitchTo(oldcontext);
#endif
}

/*
 * Get an entry from a block reference table.
 *
 * If the entry does not exist, this function returns NULL. Otherwise, it
 * returns the entry and sets *limit_block to the value from the entry.
 *
 * 从块引用表中获取条目。
 * 
 * 如果该条目不存在，则该函数返回 NULL
 * 否则，它返回该条目并将 *limit_block 设置为该条目中的值。
 */
BlockRefTableEntry *
BlockRefTableGetEntry(BlockRefTable *brtab, const RelFileLocator *rlocator,
					  ForkNumber forknum, BlockNumber *limit_block)
{
	BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
	BlockRefTableEntry *entry;

	Assert(limit_block != NULL);

	memcpy(&key.rlocator, rlocator, sizeof(RelFileLocator));
	key.forknum = forknum;
	entry = blockreftable_lookup(brtab->hash, key);

	if (entry != NULL)
		*limit_block = entry->limit_block;

	return entry;
}

/*
 * Get block numbers from a table entry.
 *
 * 'blocks' must point to enough space to hold at least 'nblocks' block
 * numbers, and any block numbers we manage to get will be written there.
 * The return value is the number of block numbers actually written.
 *
 * We do not return block numbers unless they are greater than or equal to
 * start_blkno and strictly less than stop_blkno.
 */
int
BlockRefTableEntryGetBlocks(BlockRefTableEntry *entry,
							BlockNumber start_blkno,
							BlockNumber stop_blkno,
							BlockNumber *blocks,
							int nblocks)
{
	uint32		start_chunkno;
	uint32		stop_chunkno;
	uint32		chunkno;
	int			nresults = 0;

	Assert(entry != NULL);

	/*
	 * Figure out which chunks could potentially contain blocks of interest.
	 *
	 * We need to be careful about overflow here, because stop_blkno could be
	 * InvalidBlockNumber or something very close to it.
	 */
	start_chunkno = start_blkno / BLOCKS_PER_CHUNK;
	stop_chunkno = stop_blkno / BLOCKS_PER_CHUNK;
	if ((stop_blkno % BLOCKS_PER_CHUNK) != 0)
		++stop_chunkno;
	if (stop_chunkno > entry->nchunks)
		stop_chunkno = entry->nchunks;

	/*
	 * Loop over chunks.
	 */
	for (chunkno = start_chunkno; chunkno < stop_chunkno; ++chunkno)
	{
		uint16		chunk_usage = entry->chunk_usage[chunkno];
		BlockRefTableChunk chunk_data = entry->chunk_data[chunkno];
		unsigned	start_offset = 0;
		unsigned	stop_offset = BLOCKS_PER_CHUNK;

		/*
		 * If the start and/or stop block number falls within this chunk, the
		 * whole chunk may not be of interest. Figure out which portion we
		 * care about, if it's not the whole thing.
		 */
		if (chunkno == start_chunkno)
			start_offset = start_blkno % BLOCKS_PER_CHUNK;
		if (chunkno == stop_chunkno - 1)
		{
			Assert(stop_blkno > chunkno * BLOCKS_PER_CHUNK);
			stop_offset = stop_blkno - (chunkno * BLOCKS_PER_CHUNK);
			Assert(stop_offset <= BLOCKS_PER_CHUNK);
		}

		/*
		 * Handling differs depending on whether this is an array of offsets
		 * or a bitmap.
		 */
		if (chunk_usage == MAX_ENTRIES_PER_CHUNK)
		{
			unsigned	i;

			/* It's a bitmap, so test every relevant bit. */
			for (i = start_offset; i < stop_offset; ++i)
			{
				uint16		w = chunk_data[i / BLOCKS_PER_ENTRY];

				if ((w & (1 << (i % BLOCKS_PER_ENTRY))) != 0)
				{
					BlockNumber blkno = chunkno * BLOCKS_PER_CHUNK + i;

					blocks[nresults++] = blkno;

					/* Early exit if we run out of output space. */
					if (nresults == nblocks)
						return nresults;
				}
			}
		}
		else
		{
			unsigned	i;

			/* It's an array of offsets, so check each one. */
			for (i = 0; i < chunk_usage; ++i)
			{
				uint16		offset = chunk_data[i];

				if (offset >= start_offset && offset < stop_offset)
				{
					BlockNumber blkno = chunkno * BLOCKS_PER_CHUNK + offset;

					blocks[nresults++] = blkno;

					/* Early exit if we run out of output space. */
					if (nresults == nblocks)
						return nresults;
				}
			}
		}
	}

	return nresults;
}

/*
 * Serialize a block reference table to a file.
 */
// 将块引用表序列化到文件。
void
WriteBlockRefTable(BlockRefTable *brtab,
				   io_callback_fn write_callback,
				   void *write_callback_arg)
{
	BlockRefTableSerializedEntry *sdata = NULL;
	BlockRefTableBuffer buffer;
	uint32		magic = BLOCKREFTABLE_MAGIC;

	/* Prepare buffer. */
	memset(&buffer, 0, sizeof(BlockRefTableBuffer));
	buffer.io_callback = write_callback;
	buffer.io_callback_arg = write_callback_arg;
	INIT_CRC32C(buffer.crc);

	/* Write magic number. */
	BlockRefTableWrite(&buffer, &magic, sizeof(uint32));

	/* Write the entries, assuming there are some. */
	if (brtab->hash->members > 0)
	{
		unsigned	i = 0;
		blockreftable_iterator it;
		BlockRefTableEntry *brtentry;

		/* Extract entries into serializable format and sort them. */
		sdata =
			palloc(brtab->hash->members * sizeof(BlockRefTableSerializedEntry));
		blockreftable_start_iterate(brtab->hash, &it);
		while ((brtentry = blockreftable_iterate(brtab->hash, &it)) != NULL)
		{
			BlockRefTableSerializedEntry *sentry = &sdata[i++];

			sentry->rlocator = brtentry->key.rlocator;
			sentry->forknum = brtentry->key.forknum;
			sentry->limit_block = brtentry->limit_block;
			sentry->nchunks = brtentry->nchunks;

			/* trim trailing zero entries */
			while (sentry->nchunks > 0 &&
				   brtentry->chunk_usage[sentry->nchunks - 1] == 0)
				sentry->nchunks--;
		}
		Assert(i == brtab->hash->members);
		qsort(sdata, i, sizeof(BlockRefTableSerializedEntry),
			  BlockRefTableComparator);

		/* Loop over entries in sorted order and serialize each one. */
		for (i = 0; i < brtab->hash->members; ++i)
		{
			BlockRefTableSerializedEntry *sentry = &sdata[i];
			BlockRefTableKey key = {{0}};	/* make sure any padding is zero */
			unsigned	j;

			/* Write the serialized entry itself. */
			BlockRefTableWrite(&buffer, sentry,
							   sizeof(BlockRefTableSerializedEntry));

			/* Look up the original entry so we can access the chunks. */
			memcpy(&key.rlocator, &sentry->rlocator, sizeof(RelFileLocator));
			key.forknum = sentry->forknum;
			brtentry = blockreftable_lookup(brtab->hash, key);
			Assert(brtentry != NULL);

			/* Write the untruncated portion of the chunk length array. */
			if (sentry->nchunks != 0)
				BlockRefTableWrite(&buffer, brtentry->chunk_usage,
								   sentry->nchunks * sizeof(uint16));

			/* Write the contents of each chunk. */
			for (j = 0; j < brtentry->nchunks; ++j)
			{
				if (brtentry->chunk_usage[j] == 0)
					continue;
				BlockRefTableWrite(&buffer, brtentry->chunk_data[j],
								   brtentry->chunk_usage[j] * sizeof(uint16));
			}
		}
	}

	/* Write out appropriate terminator and CRC and flush buffer. */
	BlockRefTableFileTerminate(&buffer);
}

/*
 * Prepare to incrementally read a block reference table file.
 *
 * 'read_callback' is a function that can be called to read data from the
 * underlying file (or other data source) into our internal buffer.
 *
 * 'read_callback_arg' is an opaque argument to be passed to read_callback.
 *
 * 'error_filename' is the filename that should be included in error messages
 * if the file is found to be malformed. The value is not copied, so the
 * caller should ensure that it remains valid until done with this
 * BlockRefTableReader.
 *
 * 'error_callback' is a function to be called if the file is found to be
 * malformed. This is not used for I/O errors, which must be handled internally
 * by read_callback.
 *
 * 'error_callback_arg' is an opaque argument to be passed to error_callback.
 */
BlockRefTableReader *
CreateBlockRefTableReader(io_callback_fn read_callback,
						  void *read_callback_arg,
						  char *error_filename,
						  report_error_fn error_callback,
						  void *error_callback_arg)
{
	BlockRefTableReader *reader;
	uint32		magic;

	/* Initialize data structure. */
	reader = palloc0(sizeof(BlockRefTableReader));
	reader->buffer.io_callback = read_callback;
	reader->buffer.io_callback_arg = read_callback_arg;
	reader->error_filename = error_filename;
	reader->error_callback = error_callback;
	reader->error_callback_arg = error_callback_arg;
	INIT_CRC32C(reader->buffer.crc);

	/* Verify magic number. */
	/* 验证幻数。 */
	BlockRefTableRead(reader, &magic, sizeof(uint32));
	if (magic != BLOCKREFTABLE_MAGIC)
		error_callback(error_callback_arg,
					   "file \"%s\" has wrong magic number: expected %u, found %u",
					   error_filename,
					   BLOCKREFTABLE_MAGIC, magic);

	return reader;
}

/*
 * Read next relation fork covered by this block reference table file.
 *
 * After calling this function, you must call BlockRefTableReaderGetBlocks
 * until it returns 0 before calling it again.
 * 读取该块引用表文件覆盖的下一个关系分支。
 *
 * 调用该函数后，必须调用BlockRefTableReaderGetBlocks，直到返回0后才能再次调用。
 */
bool
BlockRefTableReaderNextRelation(BlockRefTableReader *reader,
								RelFileLocator *rlocator,
								ForkNumber *forknum,
								BlockNumber *limit_block)
{
	BlockRefTableSerializedEntry sentry;
	BlockRefTableSerializedEntry zentry = {{0}};

	/*
	 * Sanity check: caller must read all blocks from all chunks before moving
	 * on to the next relation.
	 */
	Assert(reader->total_chunks == reader->consumed_chunks);

	/* Read serialized entry. */
	/* 读取序列化条目。 */
	BlockRefTableRead(reader, &sentry,
					  sizeof(BlockRefTableSerializedEntry));

	/*
	 * If we just read the sentinel entry indicating that we've reached the
	 * end, read and check the CRC.
	 *
	 * 如果我们只是读取哨兵条目表明我们已到达末尾，请读取并检查 CRC。
	 *
	 * 空代表到尾部了
	 */
	if (memcmp(&sentry, &zentry, sizeof(BlockRefTableSerializedEntry)) == 0)
	{
		pg_crc32c	expected_crc;
		pg_crc32c	actual_crc;

		/*
		 * We want to know the CRC of the file excluding the 4-byte CRC
		 * itself, so copy the current value of the CRC accumulator before
		 * reading those bytes, and use the copy to finalize the calculation.
		 */
		expected_crc = reader->buffer.crc;
		FIN_CRC32C(expected_crc);

		/* Now we can read the actual value. */
		/* 现在我们可以读取实际值了。 */
		BlockRefTableRead(reader, &actual_crc, sizeof(pg_crc32c));

		/* Throw an error if there is a mismatch. */
		/* 如果不匹配则抛出错误。 */
		if (!EQ_CRC32C(expected_crc, actual_crc))
			reader->error_callback(reader->error_callback_arg,
								   "file \"%s\" has wrong checksum: expected %08X, found %08X",
								   reader->error_filename, expected_crc, actual_crc);

		return false;
	}

	/* Read chunk size array. */
	/* 读取块大小数组。 */
	if (reader->chunk_size != NULL)
		pfree(reader->chunk_size);
	reader->chunk_size = palloc(sentry.nchunks * sizeof(uint16));
	// 读取块大小数组
	BlockRefTableRead(reader, reader->chunk_size,
					  sentry.nchunks * sizeof(uint16));

	/* Set up for chunk scan. */
	/* 设置块扫描。 */
	reader->total_chunks = sentry.nchunks;
	// 设置块扫描初始位置
	reader->consumed_chunks = 0;

	/* Return data to caller. */
	/* 将数据返回给调用者。 */
	memcpy(rlocator, &sentry.rlocator, sizeof(RelFileLocator));
	*forknum = sentry.forknum;
	*limit_block = sentry.limit_block;
	return true;
}

/*
 * Get modified blocks associated with the relation fork returned by
 * the most recent call to BlockRefTableReaderNextRelation.
 *
 * On return, block numbers will be written into the 'blocks' array, whose
 * length should be passed via 'nblocks'. The return value is the number of
 * entries actually written into the 'blocks' array, which may be less than
 * 'nblocks' if we run out of modified blocks in the relation fork before
 * we run out of room in the array.
 *
 * 获取与最近一次调用 BlockRefTableReaderNextRelation 返回的关系分支关联的修改块。
 *
 * 返回时，块号将被写入“blocks”数组，其长度应通过“nblocks”传递。 
 * 返回值是实际写入“blocks”数组的条目数，如果在数组中的空间耗尽之前用完关系分支中的修改块，则该值可能小于“nblocks”。
 */
unsigned
BlockRefTableReaderGetBlocks(BlockRefTableReader *reader,
							 BlockNumber *blocks,
							 int nblocks)
{
	// 找到块计数器
	unsigned	blocks_found = 0;

	/* Must provide space for at least one block number to be returned. */
	/* 必须为至少一个要返回的块号提供空间。 */
	Assert(nblocks > 0);

	/* Loop collecting blocks to return to caller. */
	/* 循环收集块以返回给调用者。 */
	for (;;)
	{
		uint16		next_chunk_size;

		/*
		 * If we've read at least one chunk, maybe it contains some block
		 * numbers that could satisfy caller's request.
		 *
		 * 如果我们至少读取了一个块，那么它可能包含一些可以满足调用者请求的块号。
		 */
		if (reader->consumed_chunks > 0)
		{
			// chunkno 块号
			uint32		chunkno = reader->consumed_chunks - 1;
			uint16		chunk_size = reader->chunk_size[chunkno];

			if (chunk_size == MAX_ENTRIES_PER_CHUNK)
			{
				/* Bitmap format, so search for bits that are set. */
				/* 位图格式，因此搜索已设置的位。 */
				while (reader->chunk_position < BLOCKS_PER_CHUNK &&
					   blocks_found < nblocks)
				{
					// chunkoffset 块偏移量
					uint16		chunkoffset = reader->chunk_position;
					uint16		w;

					w = reader->chunk_data[chunkoffset / BLOCKS_PER_ENTRY];
					if ((w & (1u << (chunkoffset % BLOCKS_PER_ENTRY))) != 0)
						blocks[blocks_found++] =
							chunkno * BLOCKS_PER_CHUNK + chunkoffset;
					++reader->chunk_position;
				}
			}
			else
			{
				/* Not in bitmap format, so each entry is a 2-byte offset. */
				/* 不是位图格式，因此每个条目都是 2 字节偏移量。 */
				while (reader->chunk_position < chunk_size &&
					   blocks_found < nblocks)
				{
					blocks[blocks_found++] = chunkno * BLOCKS_PER_CHUNK
						+ reader->chunk_data[reader->chunk_position];
					++reader->chunk_position;
				}
			}
		}

		/* We found enough blocks, so we're done. */
		/* 我们找到了足够的块，所以我们完成了。 */
		if (blocks_found >= nblocks)
			break;

		/*
		 * We didn't find enough blocks, so we must need the next chunk. If
		 * there are none left, though, then we're done anyway.
		 *
		 * 我们没有找到足够的块，所以我们必须需要下一个块。
		 * 不过，如果没有剩下的，那么我们就完成了。
		 */
		if (reader->consumed_chunks == reader->total_chunks)
			break;

		/*
		 * Read data for next chunk and reset scan position to beginning of
		 * chunk. Note that the next chunk might be empty, in which case we
		 * consume the chunk without actually consuming any bytes from the
		 * underlying file.
		 *
		 * 读取下一个块的数据并将扫描位置重置为块的开头。
		 * 请注意，下一个块可能是空的，在这种情况下，
		 * 我们消耗该块而不实际消耗底层文件中的任何字节。
		 */
		next_chunk_size = reader->chunk_size[reader->consumed_chunks];
		if (next_chunk_size > 0)
			BlockRefTableRead(reader, reader->chunk_data,
							  next_chunk_size * sizeof(uint16));
		++reader->consumed_chunks;
		reader->chunk_position = 0;
	}

	return blocks_found;
}

/*
 * Release memory used while reading a block reference table from a file.
 */
void
DestroyBlockRefTableReader(BlockRefTableReader *reader)
{
	if (reader->chunk_size != NULL)
	{
		pfree(reader->chunk_size);
		reader->chunk_size = NULL;
	}
	pfree(reader);
}

/*
 * Prepare to write a block reference table file incrementally.
 *
 * Caller must be able to supply BlockRefTableEntry objects sorted in the
 * appropriate order.
 *
 * 准备增量写入块引用表文件。
 *
 * 调用者必须能够提供按适当顺序排序的 BlockRefTableEntry 对象。
 */
BlockRefTableWriter *
CreateBlockRefTableWriter(io_callback_fn write_callback,
						  void *write_callback_arg)
{
	BlockRefTableWriter *writer;
	uint32		magic = BLOCKREFTABLE_MAGIC;

	/* Prepare buffer and CRC check and save callbacks. */
	writer = palloc0(sizeof(BlockRefTableWriter));
	writer->buffer.io_callback = write_callback;
	writer->buffer.io_callback_arg = write_callback_arg;
	INIT_CRC32C(writer->buffer.crc);

	/* Write magic number. */
	/* 写入幻数 */
	BlockRefTableWrite(&writer->buffer, &magic, sizeof(uint32));

	return writer;
}

/*
 * Append one entry to a block reference table file.
 *
 * Note that entries must be written in the proper order, that is, sorted by
 * tablespace, then database, then relfilenumber, then fork number. Caller
 * is responsible for supplying data in the correct order. If that seems hard,
 * use an in-memory BlockRefTable instead.
 *
 * 将一项添加到块引用表文件中。
 *
 * 请注意，条目必须按正确的顺序写入，即按表空间排序，然后是数据库，然后是 relfilenumber，然后是 fork 编号。
 * 调用者负责以正确的顺序提供数据。 如果这看起来很难，请改用内存中的 BlockRefTable。
 */
void
BlockRefTableWriteEntry(BlockRefTableWriter *writer, BlockRefTableEntry *entry)
{
	BlockRefTableSerializedEntry sentry;
	unsigned	j;

	/* Convert to serialized entry format. */
	/* 转换为序列化条目格式。 */
	sentry.rlocator = entry->key.rlocator;
	sentry.forknum = entry->key.forknum;
	sentry.limit_block = entry->limit_block;
	sentry.nchunks = entry->nchunks;

	/* Trim trailing zero entries. */
	/* 修剪尾随零条目。 */
	while (sentry.nchunks > 0 && entry->chunk_usage[sentry.nchunks - 1] == 0)
		sentry.nchunks--;

	/* Write the serialized entry itself. */
	/* 写入序列化条目本身。 */
	BlockRefTableWrite(&writer->buffer, &sentry,
					   sizeof(BlockRefTableSerializedEntry));

	/* Write the untruncated portion of the chunk length array. */
	/* 写入块长度数组的未截断部分。 */
	if (sentry.nchunks != 0)
		BlockRefTableWrite(&writer->buffer, entry->chunk_usage,
						   sentry.nchunks * sizeof(uint16));

	/* Write the contents of each chunk. */
	/* 写入每个块的内容。 */
	for (j = 0; j < entry->nchunks; ++j)
	{
		if (entry->chunk_usage[j] == 0)
			continue;
		BlockRefTableWrite(&writer->buffer, entry->chunk_data[j],
						   entry->chunk_usage[j] * sizeof(uint16));
	}
}

/*
 * Finalize an incremental write of a block reference table file.
 */
void
DestroyBlockRefTableWriter(BlockRefTableWriter *writer)
{
	BlockRefTableFileTerminate(&writer->buffer);
	pfree(writer);
}

/*
 * Allocate a standalone BlockRefTableEntry.
 *
 * When we're manipulating a full in-memory BlockRefTable, the entries are
 * part of the hash table and are allocated by simplehash. This routine is
 * used by callers that want to write out a BlockRefTable to a file without
 * needing to store the whole thing in memory at once.
 *
 * Entries allocated by this function can be manipulated using the functions
 * BlockRefTableEntrySetLimitBlock and BlockRefTableEntryMarkBlockModified
 * and then written using BlockRefTableWriteEntry and freed using
 * BlockRefTableFreeEntry.
 */
BlockRefTableEntry *
CreateBlockRefTableEntry(RelFileLocator rlocator, ForkNumber forknum)
{
	BlockRefTableEntry *entry = palloc0(sizeof(BlockRefTableEntry));

	memcpy(&entry->key.rlocator, &rlocator, sizeof(RelFileLocator));
	entry->key.forknum = forknum;
	entry->limit_block = InvalidBlockNumber;

	return entry;
}

/*
 * Update a BlockRefTableEntry with a new value for the "limit block" and
 * forget any equal-or-higher-numbered modified blocks.
 *
 * The "limit block" is the shortest known length of the relation within the
 * range of WAL records covered by this block reference table.
 *
 * 使用“限制块”的新值更新 BlockRefTableEntry，并忘记任何等于或更高编号的修改块。
 * 
 * “限制块”是该块引用表覆盖的 WAL 记录范围内关系的最短已知长度。
 */
void
BlockRefTableEntrySetLimitBlock(BlockRefTableEntry *entry,
								BlockNumber limit_block)
{
	unsigned	chunkno;
	unsigned	limit_chunkno;
	unsigned	limit_chunkoffset;
	BlockRefTableChunk limit_chunk;

	/* If we already have an equal or lower limit block, do nothing. */
	/* 如果我们已经有一个等于或下限块，则不执行任何操作。 */
	if (limit_block >= entry->limit_block)
		return;

	/* Record the new limit block value. */
	/* 记录新的限制块值。 */
	entry->limit_block = limit_block;

	/*
	 * Figure out which chunk would store the state of the new limit block,
	 * and which offset within that chunk.
	 */
	limit_chunkno = limit_block / BLOCKS_PER_CHUNK;
	limit_chunkoffset = limit_block % BLOCKS_PER_CHUNK;

	/*
	 * If the number of chunks is not large enough for any blocks with equal
	 * or higher block numbers to exist, then there is nothing further to do.
	 */
	if (limit_chunkno >= entry->nchunks)
		return;

	/* Discard entire contents of any higher-numbered chunks. */
	for (chunkno = limit_chunkno + 1; chunkno < entry->nchunks; ++chunkno)
		entry->chunk_usage[chunkno] = 0;

	/*
	 * Next, we need to discard any offsets within the chunk that would
	 * contain the limit_block. We must handle this differently depending on
	 * whether the chunk that would contain limit_block is a bitmap or an
	 * array of offsets.
	 */
	limit_chunk = entry->chunk_data[limit_chunkno];
	if (entry->chunk_usage[limit_chunkno] == MAX_ENTRIES_PER_CHUNK)
	{
		unsigned	chunkoffset;

		/* It's a bitmap. Unset bits. */
		/* 这是一个位图。 未设置位。 */
		for (chunkoffset = limit_chunkoffset; chunkoffset < BLOCKS_PER_CHUNK;
			 ++chunkoffset)
			limit_chunk[chunkoffset / BLOCKS_PER_ENTRY] &=
				~(1 << (chunkoffset % BLOCKS_PER_ENTRY));
	}
	else
	{
		unsigned	i,
					j = 0;

		/* It's an offset array. Filter out large offsets. */
		for (i = 0; i < entry->chunk_usage[limit_chunkno]; ++i)
		{
			Assert(j <= i);
			if (limit_chunk[i] < limit_chunkoffset)
				limit_chunk[j++] = limit_chunk[i];
		}
		Assert(j <= entry->chunk_usage[limit_chunkno]);
		entry->chunk_usage[limit_chunkno] = j;
	}
}

/*
 * Mark a block in a given BlockRefTableEntry as known to have been modified.
 * 将给定 BlockRefTableEntry 中的块标记为已知已被修改。
 */
void
BlockRefTableEntryMarkBlockModified(BlockRefTableEntry *entry,
									ForkNumber forknum,
									BlockNumber blknum)
{
	unsigned	chunkno;
	unsigned	chunkoffset;
	unsigned	i;

	/*
	 * Which chunk should store the state of this block? And what is the
	 * offset of this block relative to the start of that chunk?
	 *
	 * 哪个块应该存储该块的状态？ 
	 * 该块相对于该块的开头的偏移量是多少？
	 */
	chunkno = blknum / BLOCKS_PER_CHUNK;
	chunkoffset = blknum % BLOCKS_PER_CHUNK;

	/*
	 * If 'nchunks' isn't big enough for us to be able to represent the state
	 * of this block, we need to enlarge our arrays.
	 *
	 * 如果“nchunks”不够大，无法代表该块的状态，则需要扩大数组。
	 */
	if (chunkno >= entry->nchunks)
	{
		unsigned	max_chunks;
		unsigned	extra_chunks;

		/*
		 * New array size is a power of 2, at least 16, big enough so that
		 * chunkno will be a valid array index.
		 *
		 * 新数组大小是 2 的幂，至少 16，足够大，以便 chunkno 将成为有效的数组索引。
		 */
		max_chunks = Max(16, entry->nchunks);
		while (max_chunks < chunkno + 1)
			max_chunks *= 2; /* NOTE: 此处有问题? */
		// 额外块
		extra_chunks = max_chunks - entry->nchunks;

		if (entry->nchunks == 0)
		{
			entry->chunk_size = palloc0(sizeof(uint16) * max_chunks);
			entry->chunk_usage = palloc0(sizeof(uint16) * max_chunks);
			entry->chunk_data =
				palloc0(sizeof(BlockRefTableChunk) * max_chunks);
		}
		else
		{
			entry->chunk_size = repalloc(entry->chunk_size,
										 sizeof(uint16) * max_chunks);
			memset(&entry->chunk_size[entry->nchunks], 0,
				   extra_chunks * sizeof(uint16));
			entry->chunk_usage = repalloc(entry->chunk_usage,
										  sizeof(uint16) * max_chunks);
			memset(&entry->chunk_usage[entry->nchunks], 0,
				   extra_chunks * sizeof(uint16));
			entry->chunk_data = repalloc(entry->chunk_data,
										 sizeof(BlockRefTableChunk) * max_chunks);
			memset(&entry->chunk_data[entry->nchunks], 0,
				   extra_chunks * sizeof(BlockRefTableChunk));
		}
		entry->nchunks = max_chunks;
	}

	/*
	 * If the chunk that covers this block number doesn't exist yet, create it
	 * as an array and add the appropriate offset to it. We make it pretty
	 * small initially, because there might only be 1 or a few block
	 * references in this chunk and we don't want to use up too much memory.
	 */
	if (entry->chunk_size[chunkno] == 0)
	{
		entry->chunk_data[chunkno] =
			palloc(sizeof(uint16) * INITIAL_ENTRIES_PER_CHUNK);
		entry->chunk_size[chunkno] = INITIAL_ENTRIES_PER_CHUNK;
		entry->chunk_data[chunkno][0] = chunkoffset;
		entry->chunk_usage[chunkno] = 1;
		return;
	}

	/*
	 * If the number of entries in this chunk is already maximum, it must be a
	 * bitmap. Just set the appropriate bit.
	 *
	 * 如果该块中的条目数已经达到最大值，则它必须是位图。 只需设置适当的位即可。
	 */
	if (entry->chunk_usage[chunkno] == MAX_ENTRIES_PER_CHUNK)
	{
		BlockRefTableChunk chunk = entry->chunk_data[chunkno];

		chunk[chunkoffset / BLOCKS_PER_ENTRY] |=
			1 << (chunkoffset % BLOCKS_PER_ENTRY);
		return;
	}

	/*
	 * There is an existing chunk and it's in array format. Let's find out
	 * whether it already has an entry for this block. If so, we do not need
	 * to do anything.
	 *
	 * 有一个现有的块，它是数组格式的。
	 * 让我们看看它是否已经有该块的条目。
	 * 如果是这样，我们不需要做任何事情。
	 */
	for (i = 0; i < entry->chunk_usage[chunkno]; ++i)
	{
		if (entry->chunk_data[chunkno][i] == chunkoffset)
			return;
	}

	/*
	 * If the number of entries currently used is one less than the maximum,
	 * it's time to convert to bitmap format.
	 *
	 * 如果当前使用的条目数比最大值少 1，则需要转换为位图格式。
	 */
	if (entry->chunk_usage[chunkno] == MAX_ENTRIES_PER_CHUNK - 1)
	{
		BlockRefTableChunk newchunk;
		unsigned	j;

		/* Allocate a new chunk. */
		/* 分配一个新块。 */
		newchunk = palloc0(MAX_ENTRIES_PER_CHUNK * sizeof(uint16));

		/* Set the bit for each existing entry. */
		/* 为每个现有条目设置位。 */
		for (j = 0; j < entry->chunk_usage[chunkno]; ++j)
		{
			unsigned	coff = entry->chunk_data[chunkno][j];

			newchunk[coff / BLOCKS_PER_ENTRY] |=
				1 << (coff % BLOCKS_PER_ENTRY);
		}

		/* Set the bit for the new entry. */
		/* 设置新条目的位。 */
		newchunk[chunkoffset / BLOCKS_PER_ENTRY] |=
			1 << (chunkoffset % BLOCKS_PER_ENTRY);

		/* Swap the new chunk into place and update metadata. */
		/* 将新块交换到位并更新元数据。 */
		pfree(entry->chunk_data[chunkno]);
		entry->chunk_data[chunkno] = newchunk;
		entry->chunk_size[chunkno] = MAX_ENTRIES_PER_CHUNK;
		entry->chunk_usage[chunkno] = MAX_ENTRIES_PER_CHUNK;
		return;
	}

	/*
	 * OK, we currently have an array, and we don't need to convert to a
	 * bitmap, but we do need to add a new element. If there's not enough
	 * room, we'll have to expand the array.
	 *
	 * 好的，我们目前有一个数组，我们不需要转换为位图，
	 * 但我们确实需要添加一个新元素。
	 * 如果没有足够的空间，我们就必须扩展阵列。
	 */
	if (entry->chunk_usage[chunkno] == entry->chunk_size[chunkno])
	{
		unsigned	newsize = entry->chunk_size[chunkno] * 2;

		Assert(newsize <= MAX_ENTRIES_PER_CHUNK);
		entry->chunk_data[chunkno] = repalloc(entry->chunk_data[chunkno],
											  newsize * sizeof(uint16));
		entry->chunk_size[chunkno] = newsize;
	}

	/* Now we can add the new entry. */
	/* 现在我们可以添加新条目。 */
	entry->chunk_data[chunkno][entry->chunk_usage[chunkno]] =
		chunkoffset;
	entry->chunk_usage[chunkno]++;
}

/*
 * Release memory for a BlockRefTableEntry that was created by
 * CreateBlockRefTableEntry.
 */
void
BlockRefTableFreeEntry(BlockRefTableEntry *entry)
{
	if (entry->chunk_size != NULL)
	{
		pfree(entry->chunk_size);
		entry->chunk_size = NULL;
	}

	if (entry->chunk_usage != NULL)
	{
		pfree(entry->chunk_usage);
		entry->chunk_usage = NULL;
	}

	if (entry->chunk_data != NULL)
	{
		pfree(entry->chunk_data);
		entry->chunk_data = NULL;
	}

	pfree(entry);
}

/*
 * Comparator for BlockRefTableSerializedEntry objects.
 *
 * We make the tablespace OID the first column of the sort key to match
 * the on-disk tree structure.
 */
static int
BlockRefTableComparator(const void *a, const void *b)
{
	const BlockRefTableSerializedEntry *sa = a;
	const BlockRefTableSerializedEntry *sb = b;

	if (sa->rlocator.spcOid > sb->rlocator.spcOid)
		return 1;
	if (sa->rlocator.spcOid < sb->rlocator.spcOid)
		return -1;

	if (sa->rlocator.dbOid > sb->rlocator.dbOid)
		return 1;
	if (sa->rlocator.dbOid < sb->rlocator.dbOid)
		return -1;

	if (sa->rlocator.relNumber > sb->rlocator.relNumber)
		return 1;
	if (sa->rlocator.relNumber < sb->rlocator.relNumber)
		return -1;

	if (sa->forknum > sb->forknum)
		return 1;
	if (sa->forknum < sb->forknum)
		return -1;

	return 0;
}

/*
 * Flush any buffered data out of a BlockRefTableBuffer.
 */
static void
BlockRefTableFlush(BlockRefTableBuffer *buffer)
{
	buffer->io_callback(buffer->io_callback_arg, buffer->data, buffer->used);
	buffer->used = 0;
}

/*
 * Read data from a BlockRefTableBuffer, and update the running CRC
 * calculation for the returned data (but not any data that we may have
 * buffered but not yet actually returned).
 *
 * 从 BlockRefTableBuffer 读取数据，并更新返回数据的运行 CRC 计算（但不是我们可能已缓冲但尚未实际返回的任何数据）。
 */
static void
BlockRefTableRead(BlockRefTableReader *reader, void *data, int length)
{
	BlockRefTableBuffer *buffer = &reader->buffer;

	/* Loop until read is fully satisfied. */
	/* 循环直到读取完全满足。 */
	while (length > 0)
	{
		if (buffer->cursor < buffer->used)
		{
			/*
			 * If any buffered data is available, use that to satisfy as much
			 * of the request as possible.
			 *
			 * 如果有任何缓冲数据可用，请使用它来满足尽可能多的请求。
			 */
			int			bytes_to_copy = Min(length, buffer->used - buffer->cursor);

			memcpy(data, &buffer->data[buffer->cursor], bytes_to_copy);
			COMP_CRC32C(buffer->crc, &buffer->data[buffer->cursor],
						bytes_to_copy);
			buffer->cursor += bytes_to_copy;
			data = ((char *) data) + bytes_to_copy;
			length -= bytes_to_copy;
		}
		else if (length >= BUFSIZE)
		{
			/*
			 * If the request length is long, read directly into caller's
			 * buffer.
			 *
			 * 如果请求长度很长，则直接读入调用者的缓冲区。
			 */
			int			bytes_read;

			// io_callback: ReadWalSummary
			bytes_read = buffer->io_callback(buffer->io_callback_arg,
											 data, length);
			COMP_CRC32C(buffer->crc, data, bytes_read);
			data = ((char *) data) + bytes_read;
			length -= bytes_read;

			/* If we didn't get anything, that's bad. */
			/* 如果我们没有得到任何东西，那就糟糕了。 */
			if (bytes_read == 0)
				reader->error_callback(reader->error_callback_arg,
									   "file \"%s\" ends unexpectedly",
									   reader->error_filename);
		}
		else
		{
			/*
			 * Refill our buffer.
			 *
			 * 重新填充我们的缓冲区。
			 */
			// io_callback: ReadWalSummary
			buffer->used = buffer->io_callback(buffer->io_callback_arg,
											   buffer->data, BUFSIZE);
			buffer->cursor = 0;

			/* If we didn't get anything, that's bad. */
			/* 如果我们没有得到任何东西，那就糟糕了。 */
			if (buffer->used == 0)
				reader->error_callback(reader->error_callback_arg,
									   "file \"%s\" ends unexpectedly",
									   reader->error_filename);
		}
	}
}

/*
 * Supply data to a BlockRefTableBuffer for write to the underlying File,
 * and update the running CRC calculation for that data.
 *
 * 向 BlockRefTableBuffer 提供数据以写入底层文件，并更新该数据正在运行的 CRC 计算。
 */
static void
BlockRefTableWrite(BlockRefTableBuffer *buffer, void *data, int length)
{
	/* Update running CRC calculation. */
	COMP_CRC32C(buffer->crc, data, length);

	/* If the new data can't fit into the buffer, flush the buffer. */
	/* 如果新数据无法放入缓冲区，则刷新缓冲区。 */
	if (buffer->used + length > BUFSIZE)
	{
		buffer->io_callback(buffer->io_callback_arg, buffer->data,
							buffer->used);
		buffer->used = 0;
	}

	/* If the new data would fill the buffer, or more, write it directly. */
	/* 如果新数据将填满缓冲区或更多，则直接写入。 */
	if (length >= BUFSIZE)
	{
		buffer->io_callback(buffer->io_callback_arg, data, length);
		return;
	}

	/* Otherwise, copy the new data into the buffer. */
	/* 否则，将新数据复制到缓冲区中 */
	memcpy(&buffer->data[buffer->used], data, length);
	buffer->used += length;
	Assert(buffer->used <= BUFSIZE);
}

/*
 * Generate the sentinel and CRC required at the end of a block reference
 * table file and flush them out of our internal buffer.
 */
static void
BlockRefTableFileTerminate(BlockRefTableBuffer *buffer)
{
	BlockRefTableSerializedEntry zentry = {{0}};
	pg_crc32c	crc;

	/* Write a sentinel indicating that there are no more entries. */
	BlockRefTableWrite(buffer, &zentry,
					   sizeof(BlockRefTableSerializedEntry));

	/*
	 * Writing the checksum will perturb the ongoing checksum calculation, so
	 * copy the state first and finalize the computation using the copy.
	 */
	crc = buffer->crc;
	FIN_CRC32C(crc);
	BlockRefTableWrite(buffer, &crc, sizeof(pg_crc32c));

	/* Flush any leftover data out of our buffer. */
	BlockRefTableFlush(buffer);
}
