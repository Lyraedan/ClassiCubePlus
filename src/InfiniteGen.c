#include "InfiniteGen.h"
#include "ChunkStore.h"
#include "World.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "BlockID.h"

/* Generates terrain for an infinite world, one 16x16x16 chunk at a time.
   Terrain is deterministic from the world seed and chunk coordinates, so it is
   continuous across chunk boundaries. From the hard floor (INF_FLOOR_Y) upwards
   it extends to the sky; below the floor there is solid bedrock, so there is no
   bottomless void to fall into.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3 */

/* Base height of the terrain surface around which noise varies. */
#define INF_BASE_HEIGHT 12
/* Amplitude of the terrain surface variation. */
#define INF_HEIGHT_AMP  32
/* Height below which (still) lava fills the terrain. */
#define INF_LAVA_LEVEL  -24
/* Threshold above which 3D noise carves a cave. */
#define INF_CAVE_THRESHOLD 0.62f


/*########################################################################################################################*
*-------------------------------------------------------Noise helpers-----------------------------------------------------*
*#########################################################################################################################*/
/* Deterministic hash for a lattice point. */
static CC_INLINE cc_uint32 Inf_Hash(int x, int y, int z, cc_uint32 seed) {
	cc_uint32 n = (cc_uint32)((cc_int32)x) * 374761393u
	            + (cc_uint32)((cc_int32)y) * 668265263u
	            + (cc_uint32)((cc_int32)z) * 2246822519u
	            + seed * 3266489917u;
	n = (n ^ (n >> 13)) * 1274126177u;
	n = n ^ (n >> 16);
	return n;
}

/* Smooth value noise in 2D, returns a value in [0, 1]. */
static float Inf_Noise2D(float x, float z, cc_uint32 seed) {
	int x0 = Math_Floor(x), z0 = Math_Floor(z);
	float tx = x - x0, tz = z - z0;
	cc_uint32 h00, h10, h01, h11;
	float v00, v10, v01, v11, a, b;

	tx = tx * tx * (3.0f - 2.0f * tx);
	tz = tz * tz * (3.0f - 2.0f * tz);

	h00 = Inf_Hash(x0,     z0,     0, seed);
	h10 = Inf_Hash(x0 + 1, z0,     0, seed);
	h01 = Inf_Hash(x0,     z0 + 1, 0, seed);
	h11 = Inf_Hash(x0 + 1, z0 + 1, 0, seed);

	v00 = (float)(h00 & 0xFFFF) / 65535.0f;
	v10 = (float)(h10 & 0xFFFF) / 65535.0f;
	v01 = (float)(h01 & 0xFFFF) / 65535.0f;
	v11 = (float)(h11 & 0xFFFF) / 65535.0f;

	a = v00 + (v10 - v00) * tx;
	b = v01 + (v11 - v01) * tx;
	return a + (b - a) * tz;
}

/* Fractal Brownian motion over 2D value noise, returns a value in [0, 1]. */
static float Inf_Fbm2D(float x, float z, int octaves, cc_uint32 seed) {
	float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
	int i;
	for (i = 0; i < octaves; i++) {
		sum  += Inf_Noise2D(x * freq, z * freq, seed + i) * amp;
		norm += amp;
		amp  *= 0.5f;
		freq *= 2.0f;
	}
	return sum / norm;
}

/* Smooth value noise in 3D, returns a value in [0, 1]. */
static float Inf_Noise3D(float x, float y, float z, cc_uint32 seed) {
	int x0 = Math_Floor(x), y0 = Math_Floor(y), z0 = Math_Floor(z);
	float tx = x - x0, ty = y - y0, tz = z - z0;
	cc_uint32 h000, h100, h010, h110, h001, h101, h011, h111;
	float v000, v100, v010, v110, v001, v101, v011, v111;
	float a, b, c, d, e, f;

	tx = tx * tx * (3.0f - 2.0f * tx);
	ty = ty * ty * (3.0f - 2.0f * ty);
	tz = tz * tz * (3.0f - 2.0f * tz);

	h000 = Inf_Hash(x0,     y0,     z0,     seed);
	h100 = Inf_Hash(x0 + 1, y0,     z0,     seed);
	h010 = Inf_Hash(x0,     y0 + 1, z0,     seed);
	h110 = Inf_Hash(x0 + 1, y0 + 1, z0,     seed);
	h001 = Inf_Hash(x0,     y0,     z0 + 1, seed);
	h101 = Inf_Hash(x0 + 1, y0,     z0 + 1, seed);
	h011 = Inf_Hash(x0,     y0 + 1, z0 + 1, seed);
	h111 = Inf_Hash(x0 + 1, y0 + 1, z0 + 1, seed);

	v000 = (float)(h000 & 0xFFFF) / 65535.0f;
	v100 = (float)(h100 & 0xFFFF) / 65535.0f;
	v010 = (float)(h010 & 0xFFFF) / 65535.0f;
	v110 = (float)(h110 & 0xFFFF) / 65535.0f;
	v001 = (float)(h001 & 0xFFFF) / 65535.0f;
	v101 = (float)(h101 & 0xFFFF) / 65535.0f;
	v011 = (float)(h011 & 0xFFFF) / 65535.0f;
	v111 = (float)(h111 & 0xFFFF) / 65535.0f;

	a = v000 + (v100 - v000) * tx;
	b = v010 + (v110 - v010) * tx;
	c = v001 + (v101 - v001) * tx;
	d = v011 + (v111 - v011) * tx;
	e = a + (b - a) * ty;
	f = c + (d - c) * ty;
	return e + (f - e) * tz;
}

/* Fractal Brownian motion over 3D value noise, returns a value in [0, 1]. */
static float Inf_Fbm3D(float x, float y, float z, int octaves, cc_uint32 seed) {
	float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
	int i;
	for (i = 0; i < octaves; i++) {
		sum  += Inf_Noise3D(x * freq, y * freq, z * freq, seed + i) * amp;
		norm += amp;
		amp  *= 0.5f;
		freq *= 2.0f;
	}
	return sum / norm;
}


/*########################################################################################################################*
*------------------------------------------------------Terrain gen--------------------------------------------------------*
*#########################################################################################################################*/
static int Inf_GetSurfaceHeight(int x, int z) {
	float n = Inf_Fbm2D((float)x * 0.008f, (float)z * 0.008f, 5, (cc_uint32)World.Seed);
	return INF_BASE_HEIGHT + (int)((n - 0.5f) * INF_HEIGHT_AMP);
}

int InfiniteGen_GetSurfaceHeight(int x, int z) {
	return Inf_GetSurfaceHeight(x, z);
}

void InfiniteGen_GenerateChunk(struct Chunk* chunk, int cx, int cy, int cz) {
	cc_uint32 seed = (cc_uint32)World.Seed;
	int x1 = cx << CHUNK_SHIFT, y1 = cy << CHUNK_SHIFT, z1 = cz << CHUNK_SHIFT;
	int x, y, z, i = 0;
	int heights[CHUNK_SIZE * CHUNK_SIZE];
	int maxH = INF_SURFACE_MIN_Y, h;
	cc_bool rowCarves;

	/* Precompute the surface height of each column, as it does not depend on y. */
	for (z = 0; z < CHUNK_SIZE; z++) {
		for (x = 0; x < CHUNK_SIZE; x++) {
			h = Inf_GetSurfaceHeight(x1 + x, z1 + z);
			heights[z * CHUNK_SIZE + x] = h;
			maxH = max(maxH, h);
		}
	}

	/* NOTE: ChunkStore_PackLocal uses index = y*256 + z*16 + x, so the loops
	   must iterate y outer, then z, then x to match the block storage layout. */
	for (y = 0; y < CHUNK_SIZE; y++) {
		int yc = y1 + y;
		/* Cave carving is only possible below the surface of some column in
		   this chunk. Rows above the highest column's surface can never carve,
		   so the 3D noise can be skipped for the whole row. */
		rowCarves = (yc + 2) < maxH;
		for (z = 0; z < CHUNK_SIZE; z++) {
			int zc = z1 + z;
			for (x = 0; x < CHUNK_SIZE; x++) {
				int xc = x1 + x;
				int h  = heights[z * CHUNK_SIZE + x];
				BlockRaw block;

				if (yc <= INF_FLOOR_Y) {
					/* Hard floor of the world; never carved into. */
					block = BLOCK_BEDROCK;
				} else if (yc < h) {
					/* Below the surface, solid terrain. */
					if (yc >= h - 1) {
						block = (h < INF_WATER_LEVEL) ? BLOCK_SAND : BLOCK_GRASS;
					} else if (yc >= h - 4) {
						block = BLOCK_DIRT;
					} else if (yc <= INF_LAVA_LEVEL) {
						block = BLOCK_STILL_LAVA;
					} else {
						block = BLOCK_STONE;
					}

					/* Carve caves below the surface with 3D noise. */
					if (yc < h - 2 && rowCarves && Inf_Fbm3D((float)xc * 0.06f, (float)yc * 0.06f,
							(float)zc * 0.06f, 2, seed + 9) > INF_CAVE_THRESHOLD) {
						block = BLOCK_AIR;
					}
				} else {
					/* At or above the surface. */
					if (h < INF_WATER_LEVEL && yc <= INF_WATER_LEVEL) {
						block = BLOCK_STILL_WATER;
					} else {
						block = BLOCK_AIR;
					}
				}

				chunk->blocks[i++] = block;
			}
		}
	}
}
