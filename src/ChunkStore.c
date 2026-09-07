#include "ChunkStore.h"
#include "BlockID.h"
#include "Platform.h"

/* Stores blocks of an infinite (procedurally generated) world in chunks.
   Each chunk holds a 16x16x16 array of blocks, and chunks are stored in a
   hash table keyed by their (cx,cy,cz) chunk coordinates.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3 */

ChunkStore_GenFunc ChunkStore_Generator;
ChunkStore_FreeInfoFunc ChunkStore_FreeInfo;

/* Open-addressing hash table of chunks. */
typedef struct ChunkEntry_ {
	struct Chunk* chunk; /* NULL when empty */
} ChunkEntry;

static ChunkEntry* entries;
static int capacity;
static int count;

/* Dynamic array of loaded chunks, for iteration. */
static struct Chunk** order;
static int orderCount;
static int orderCap;

static CC_INLINE cc_uint32 Inf_HashChunk(int cx, int cy, int cz) {
	cc_uint32 h = (cc_uint32)((cc_int32)cx) * 0x9E3779B1u;
	h ^= (cc_uint32)((cc_int32)cy) * 0x85EBCA77u;
	h ^= (cc_uint32)((cc_int32)cz) * 0xC2B2AE3Du;
	return h;
}

static void Table_Insert(struct Chunk* chunk) {
	cc_uint32 mask, idx;
	if ((count + 1) > capacity * 3 / 4) {
		int oldCap = capacity;
		ChunkEntry* old = entries;
		int i;

		capacity *= 2;
		entries = (ChunkEntry*)Mem_AllocCleared(capacity, sizeof(ChunkEntry), "chunk store");
		count = 0;
		for (i = 0; i < oldCap; i++) {
			if (old[i].chunk) Table_Insert(old[i].chunk);
		}
		Mem_Free(old);
	}

	mask = (cc_uint32)capacity - 1;
	idx  = Inf_HashChunk(chunk->cx, chunk->cy, chunk->cz) & mask;
	while (entries[idx].chunk) idx = (idx + 1) & mask;
	entries[idx].chunk = chunk;
	count++;
}

static void Table_Remove(struct Chunk* chunk) {
	cc_uint32 mask = (cc_uint32)capacity - 1;
	cc_uint32 idx  = Inf_HashChunk(chunk->cx, chunk->cy, chunk->cz) & mask;
	while (entries[idx].chunk != chunk) idx = (idx + 1) & mask;
	entries[idx].chunk = NULL;
	count--;
}

static void Order_Add(struct Chunk* chunk) {
	if (orderCount == orderCap) {
		orderCap = orderCap ? orderCap * 2 : 64;
		order = (struct Chunk**)Mem_Realloc(order, orderCap, sizeof(struct Chunk*), "chunk order");
	}
	order[orderCount++] = chunk;
}

static int Order_Index(struct Chunk* chunk) {
	int i;
	for (i = 0; i < orderCount; i++) {
		if (order[i] == chunk) return i;
	}
	return -1;
}

static void Order_Remove(struct Chunk* chunk) {
	int i = Order_Index(chunk);
	if (i < 0) return;
	orderCount--;
	order[i] = order[orderCount];
}

/* Rebuilds the hash table from the order array. */
static void Table_Rebuild(void) {
	int i;
	Mem_Set(entries, 0, capacity * sizeof(ChunkEntry));
	count = 0;
	for (i = 0; i < orderCount; i++) {
		Table_Insert(order[i]);
	}
}

void ChunkStore_Init(void) {
	ChunkStore_Generator = NULL;
	entries  = (ChunkEntry*)Mem_AllocCleared(256, sizeof(ChunkEntry), "chunk store");
	capacity = 256;
	count    = 0;

	order    = NULL;
	orderCap = 0;
	orderCount = 0;
}

void ChunkStore_Clear(void) {
	int i;
	if (!entries) return;
	for (i = 0; i < orderCount; i++) {
		struct Chunk* chunk = order[i];
		if (chunk->info) {
			if (ChunkStore_FreeInfo) ChunkStore_FreeInfo(chunk);
			else Mem_Free(chunk->info);
		}
		Mem_Free(chunk);
	}
	orderCount = 0;
	Table_Rebuild();
}

void ChunkStore_Free(void) {
	ChunkStore_Clear();
	Mem_Free(entries);
	entries = NULL;
	capacity = 0;
	Mem_Free(order);
	order = NULL;
	orderCap = 0;
}

struct Chunk* ChunkStore_Find(int cx, int cy, int cz) {
	cc_uint32 mask, idx;
	if (!entries) return NULL;
	mask = (cc_uint32)capacity - 1;
	idx  = Inf_HashChunk(cx, cy, cz) & mask;
	while (entries[idx].chunk) {
		struct Chunk* c = entries[idx].chunk;
		if (c->cx == cx && c->cy == cy && c->cz == cz) return c;
		idx = (idx + 1) & mask;
	}
	return NULL;
}

struct Chunk* ChunkStore_Get(int cx, int cy, int cz) {
	struct Chunk* chunk = ChunkStore_Find(cx, cy, cz);
	if (chunk) return chunk;
	if (count >= CHUNKSTORE_MAX_CHUNKS) return NULL;

	chunk = (struct Chunk*)Mem_AllocCleared(1, sizeof(struct Chunk), "chunk");
	chunk->cx = cx; chunk->cy = cy; chunk->cz = cz;
	chunk->generated = true;

	if (ChunkStore_Generator) ChunkStore_Generator(chunk, cx, cy, cz);

	Table_Insert(chunk);
	Order_Add(chunk);
	return chunk;
}

void ChunkStore_Remove(int cx, int cy, int cz) {
	struct Chunk* chunk = ChunkStore_Find(cx, cy, cz);
	if (!chunk) return;

	Order_Remove(chunk);
	Table_Remove(chunk);
	if (chunk->info) {
		if (ChunkStore_FreeInfo) ChunkStore_FreeInfo(chunk);
		else Mem_Free(chunk->info);
	}
	Mem_Free(chunk);
}

BlockID ChunkStore_GetBlock(int x, int y, int z) {
	struct Chunk* c = ChunkStore_Find(x >> CHUNK_SHIFT, y >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
	int i;
	if (!c) return BLOCK_AIR;
	i = ChunkStore_PackLocal(x, y, z);
#ifdef EXTENDED_BLOCKS
	return (BlockID)(c->blocks[i] | (c->blocks2[i] << 8));
#else
	return c->blocks[i];
#endif
}

BlockID ChunkStore_GetBlockEnsure(int x, int y, int z) {
	struct Chunk* c = ChunkStore_Get(x >> CHUNK_SHIFT, y >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
	int i;
	if (!c) return BLOCK_AIR;
	i = ChunkStore_PackLocal(x, y, z);
#ifdef EXTENDED_BLOCKS
	return (BlockID)(c->blocks[i] | (c->blocks2[i] << 8));
#else
	return c->blocks[i];
#endif
}

void ChunkStore_SetBlock(int x, int y, int z, BlockID block) {
	struct Chunk* c = ChunkStore_Get(x >> CHUNK_SHIFT, y >> CHUNK_SHIFT, z >> CHUNK_SHIFT);
	int i;
	if (!c) return;
	i = ChunkStore_PackLocal(x, y, z);
	c->blocks[i] = (BlockRaw)block;
#ifdef EXTENDED_BLOCKS
	c->blocks2[i] = (BlockRaw)(block >> 8);
#endif
}

int ChunkStore_GetCount(void) {
	return orderCount;
}

struct Chunk* ChunkStore_GetAt(int index) {
	return order[index];
}
