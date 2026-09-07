#ifndef CC_LODRENDERER_H
#define CC_LODRENDERER_H
#include "Core.h"
#include "Constants.h"
CC_BEGIN_HEADER

/*
Renders far-away terrain of infinite worlds as a coarse heightfield, so the
view distance can extend far beyond what full-resolution voxel chunks could
ever stream in. Reads the terrain surface height directly from InfiniteGen,
never touching the full-resolution ChunkStore.

Terrain is subdivided into large macro-chunks (LOD_CHUNK_SIZE blocks square),
each meshed as flat-top quads sampled from the deterministic heightfield at a
step that depends on the macro-chunk's distance from the camera.
Copyright 2025 ClassiCubePlus | Licensed under BSD-3
*/

/* Side of one LOD macro-chunk in blocks. */
#define LOD_CHUNK_SIZE  256
#define LOD_CHUNK_SHIFT 8

/* Number of macro-chunks (by distance ring) with each sampling step. */
enum {
	LOD_RING_COUNT = 5,
	/* Distance (in blocks) from the camera at which each ring starts.
	   Rings beyond the last always use the coarsest step. */
	LOD_RING_DIST_1 = 256,
	LOD_RING_DIST_2 = 512,
	LOD_RING_DIST_3 = 1024,
	LOD_RING_DIST_4 = 2048
};

/* Called once per frame to stream/rebuild/cull far-away terrain. */
/* NOTE: Infinite worlds only, call from MapRenderer_Update. */
void Lod_Update(float delta);
/* Draws the far-away terrain. Called from the normal render pass. */
void Lod_Render(void);
/* Draws far-away water into the depth-only pass of the translucent renderer. */
void Lod_RenderTranslucentDepth(void);
/* Draws far-away water in the blend pass of the translucent renderer. */
void Lod_RenderTranslucent(void);

/* Frees all far-away terrain state. */
void Lod_OnNewMap(void);
/* Prepares far-away terrain streaming for a newly loaded map. */
void Lod_OnNewMapLoaded(void);
/* Drops all far-away terrain meshes (e.g. graphics context lost). */
void Lod_OnContextLost(void);
/* Marks all far-away terrain as needing to be rebuilt. */
void Lod_RefreshAll(void);
/* Marks the macro-chunk containing the given full-resolution chunk as dirty. */
void Lod_RefreshChunk(int cx, int cz);

CC_END_HEADER
#endif