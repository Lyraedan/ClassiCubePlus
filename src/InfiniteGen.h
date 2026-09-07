#ifndef CC_INFINITEGEN_H
#define CC_INFINITEGEN_H
#include "Core.h"
#include "Constants.h"
CC_BEGIN_HEADER

/*
Generates terrain for an infinite world, one 16x16x16 chunk at a time.
Terrain is deterministic from the world seed and chunk coordinates, so it is
continuous across chunk boundaries. There is no bedrock layer: terrain extends
downwards indefinitely and air upwards indefinitely.
Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct Chunk;

/* Fills the given chunk's block array with procedurally generated terrain. */
void InfiniteGen_GenerateChunk(struct Chunk* chunk, int cx, int cy, int cz);

/* Returns the height of the terrain surface at the given world column. */
int InfiniteGen_GetSurfaceHeight(int x, int z);

CC_END_HEADER
#endif
