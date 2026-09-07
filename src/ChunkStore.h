#ifndef CC_CHUNKSTORE_H
#define CC_CHUNKSTORE_H
#include "Core.h"
#include "Constants.h"
CC_BEGIN_HEADER

/*
Stores blocks of an infinite (procedurally generated) world in chunks.
Each chunk holds a 16x16x16 array of blocks, and chunks are stored in a
hash table keyed by their (cx,cy,cz) chunk coordinates.
Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct ChunkInfo;

/* A single 16x16x16 chunk of blocks. */
struct Chunk {
	BlockRaw blocks[CHUNK_SIZE_3];
#ifdef EXTENDED_BLOCKS
	BlockRaw blocks2[CHUNK_SIZE_3];
#endif
	/* Render info for this chunk, owned and managed by MapRenderer. */
	struct ChunkInfo* info;
	int cx, cy, cz;
	cc_bool generated;
};

/* Called to fill the blocks of a newly created chunk. */
/* Set to NULL if no generation is to be performed. */
typedef void (*ChunkStore_GenFunc)(struct Chunk* chunk, int cx, int cy, int cz);
CC_VAR extern ChunkStore_GenFunc ChunkStore_Generator;

/* Initialises the chunk store. */
void ChunkStore_Init(void);
/* Frees all chunks and internal state. */
void ChunkStore_Free(void);
/* Removes and frees all chunks. */
void ChunkStore_Clear(void);

/* Returns the chunk at the given coordinates, or NULL if not loaded. */
struct Chunk* ChunkStore_Find(int cx, int cy, int cz);
/* Returns the chunk at the given coordinates, generating it if needed. */
struct Chunk* ChunkStore_Get(int cx, int cy, int cz);
/* Removes and frees the chunk at the given coordinates. */
void ChunkStore_Remove(int cx, int cy, int cz);

/* Reads the block at the given world coordinates. */
/* Returns BLOCK_AIR if the chunk is not loaded. */
BlockID ChunkStore_GetBlock(int x, int y, int z);
/* Reads the block at the given world coordinates, generating the chunk if needed. */
BlockID ChunkStore_GetBlockEnsure(int x, int y, int z);
/* Sets the block at the given world coordinates, generating the chunk if needed. */
void ChunkStore_SetBlock(int x, int y, int z, BlockID block);

/* Number of chunks currently loaded. */
int ChunkStore_GetCount(void);
/* Returns the chunk at the given index in the iteration order. */
struct Chunk* ChunkStore_GetAt(int index);

/* Converts world coordinates to the local block index inside a chunk. */
static CC_INLINE int ChunkStore_PackLocal(int x, int y, int z) {
	return ((y & CHUNK_MASK) << 8) | ((z & CHUNK_MASK) << 4) | (x & CHUNK_MASK);
}

CC_END_HEADER
#endif
