#ifndef CC_INFINITEGEN_H
#define CC_INFINITEGEN_H
#include "Core.h"
#include "Constants.h"
CC_BEGIN_HEADER

/*
Generates terrain for an infinite world, one 16x16x16 chunk at a time.
Terrain is deterministic from the world seed and chunk coordinates, so it is
continuous across chunk boundaries. From the hard floor (INF_FLOOR_Y) upwards
it extends to the sky; below the floor there is solid bedrock, so there is no
bottomless void to fall into.
Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct Chunk;

/* Inclusive range of world Y coordinates the terrain surface can occupy. */
/* Derived from INF_BASE_HEIGHT +/- INF_HEIGHT_AMP used by the generator. */
#define INF_SURFACE_MIN_Y (-20)
#define INF_SURFACE_MAX_Y (44)

/* Hard floor of the world. Everything at or below this Y is bedrock, so
   players cannot fall into the void and sink forever (which would make
   physics generate an endless column of chunks and run out of memory). */
#define INF_FLOOR_Y (-26)

/* Fills the given chunk's block array with procedurally generated terrain. */
void InfiniteGen_GenerateChunk(struct Chunk* chunk, int cx, int cy, int cz);

/* Returns the height of the terrain surface at the given world column. */
int InfiniteGen_GetSurfaceHeight(int x, int z);

CC_END_HEADER
#endif
