#include "LodRenderer.h"
#include "InfiniteGen.h"
#include "World.h"
#include "Block.h"
#include "BlockID.h"
#include "Camera.h"
#include "ExtMath.h"
#include "Game.h"
#include "Graphics.h"
#include "Platform.h"
#include "TexturePack.h"
#include "Vectors.h"

/* Renders far-away terrain of infinite worlds as a coarse heightfield, so the
   view distance can extend far beyond what full-resolution voxel chunks could
   ever stream in. Reads the terrain surface height directly from InfiniteGen,
   never touching the full-resolution ChunkStore.
   Copyright 2025 ClassiCubePlus | Licensed under BSD-3 */

/* Safe upper bound on number of macro-chunks that can be loaded at once. */
#define LOD_MAX_TILES 4096

/* Per-frame budgets: how many new macro-chunks to load and rebuild. */
#define LOD_NEW_TILES_PER_FRAME 24
#define LOD_BUILDS_PER_FRAME    8

typedef struct LodInfo_ {
	GfxResourceID vb;
	/* Vertex ranges within the VB. */
	/*   [0, grassVerts)        = grass tops + grass sides */
	/*   [grassVerts, sandVerts) = sand tops + sand sides */
	/*   [sandVerts, totalVerts) = water plane */
	int grassVerts, sandVerts, totalVerts;
} LodInfo;

typedef struct LodTile_ {
	LodInfo* info;
	int lchx, lchz;  /* macro-chunk coordinates (LOD_CHUNK_SIZE units) */
	int step;        /* sample step used for the last build */
	int minY, maxY;  /* vertical extent of the built surface */
	cc_bool valid;   /* vb is allocated and built */
	cc_bool dirty;   /* needs rebuilding */
	cc_bool visible; /* passed frustum culling this frame */
} LodTile;

/* Open-addressing hash table of macro-chunks. */
static LodTile** lodEntries;
static int lodCap, lodCount;
/* Loaded macro-chunks in creation order, for iteration. */
static LodTile** lodOrder;
static int lodOrderCount, lodOrderCap;
/* Macro-chunks visible this frame, grouped by their top texture. */
static LodTile** lodGrassList;
static LodTile** lodSandList;
static int lodGrassCount, lodSandCount;

static cc_bool lodReady;
static int lodLastCamX, lodLastCamZ;

static CC_INLINE cc_bool Lod_Active(void) {
	return World.Type == WORLD_INFINITE && World.Loaded;
}

/* Distance (in blocks) from the camera the macro-chunk will be sampled at. */
static int Lod_StepForDist(int dist) {
	if (dist < LOD_RING_DIST_1) return 4;
	if (dist < LOD_RING_DIST_2) return 8;
	if (dist < LOD_RING_DIST_3) return 16;
	if (dist < LOD_RING_DIST_4) return 32;
	return 64;
}


/*########################################################################################################################*
*-------------------------------------------------------Hash table--------------------------------------------------------*
*#########################################################################################################################*/
static CC_INLINE cc_uint32 Lod_Hash(int tx, int tz) {
	cc_uint32 h = (cc_uint32)((cc_int32)tx) * 0x9E3779B1u;
	h ^= (cc_uint32)((cc_int32)tz) * 0x85EBCA77u;
	return h;
}

static void Lod_TableInsert(LodTile* tile) {
	cc_uint32 mask, idx;
	if ((lodCount + 1) > lodCap * 3 / 4) {
		int oldCap = lodCap;
		LodTile** old = lodEntries;
		int i;

		lodCap *= 2;
		lodEntries = (LodTile**)Mem_AllocCleared(lodCap, sizeof(LodTile*), "lod store");
		lodCount = 0;
		for (i = 0; i < oldCap; i++) {
			if (old[i]) Lod_TableInsert(old[i]);
		}
		Mem_Free(old);
	}

	mask = (cc_uint32)lodCap - 1;
	idx  = Lod_Hash(tile->lchx, tile->lchz) & mask;
	while (lodEntries[idx]) idx = (idx + 1) & mask;
	lodEntries[idx] = tile;
	lodCount++;
}

static void Lod_TableRemove(LodTile* tile) {
	cc_uint32 mask = (cc_uint32)lodCap - 1;
	cc_uint32 idx  = Lod_Hash(tile->lchx, tile->lchz) & mask;
	while (lodEntries[idx] != tile) idx = (idx + 1) & mask;
	lodEntries[idx] = NULL;
	lodCount--;
}

static LodTile* Lod_Find(int tx, int tz) {
	cc_uint32 mask, idx;
	if (!lodEntries) return NULL;
	mask = (cc_uint32)lodCap - 1;
	idx  = Lod_Hash(tx, tz) & mask;
	while (lodEntries[idx]) {
		LodTile* t = lodEntries[idx];
		if (t->lchx == tx && t->lchz == tz) return t;
		idx = (idx + 1) & mask;
	}
	return NULL;
}

static void Lod_EnsureRenderCapacity(int count) {
	if (lodGrassList && count <= lodCap) return;
	lodGrassList = (LodTile**)Mem_Realloc(lodGrassList, lodCap, sizeof(LodTile*), "lod render");
	lodSandList  = (LodTile**)Mem_Realloc(lodSandList,  lodCap, sizeof(LodTile*), "lod render");
}

static void Lod_FreeInfo(LodTile* tile) {
	if (!tile->info) return;
	if (tile->info->vb) Gfx_DeleteVb(&tile->info->vb);
	Mem_Free(tile->info);
	tile->info = NULL;
}

static LodTile* Lod_Create(int tx, int tz) {
	LodTile* tile;
	if (lodOrderCount >= LOD_MAX_TILES) return NULL;

	tile = (LodTile*)Mem_AllocCleared(1, sizeof(LodTile), "lod tile");
	tile->lchx = tx; tile->lchz = tz;
	tile->info = (LodInfo*)Mem_AllocCleared(1, sizeof(LodInfo), "lod info");
	tile->dirty = true;
	tile->valid = false;

	if (lodOrderCount == lodOrderCap) {
		lodOrderCap = lodOrderCap ? lodOrderCap * 2 : 64;
		lodOrder    = (LodTile**)Mem_Realloc(lodOrder, lodOrderCap, sizeof(LodTile*), "lod order");
	}
	lodOrder[lodOrderCount++] = tile;
	Lod_TableInsert(tile);
	return tile;
}

static void Lod_Remove(LodTile* tile) {
	int i;
	Lod_TableRemove(tile);
	for (i = 0; i < lodOrderCount; i++) {
		if (lodOrder[i] == tile) break;
	}
	lodOrderCount--;
	lodOrder[i] = lodOrder[lodOrderCount];
	Lod_FreeInfo(tile);
	Mem_Free(tile);
}

static void Lod_DropTiles(void) {
	int i;
	if (!lodEntries) return;
	for (i = 0; i < lodOrderCount; i++) {
		LodTile* t = lodOrder[i];
		if (t->info) {
			if (t->info->vb) Gfx_DeleteVb(&t->info->vb);
			Mem_Free(t->info);
		}
		Mem_Free(t);
	}
	lodOrderCount = 0;
	Mem_Set(lodEntries, 0, lodCap * sizeof(LodTile*));
	lodCount = 0;
	lodGrassCount = 0; lodSandCount = 0;
}


/*########################################################################################################################*
*------------------------------------------------------Mesh building------------------------------------------------------*
*#########################################################################################################################*/
/* Surface height of the top block face, from the sum of a cell's 4 corner heights. */
static CC_INLINE int Lod_TopFrom4(int sum4) {
	return sum4 / 4 + 1;
}

static void EmitTop(struct VertexTextured** v, int x0, int z0, int step, float y, TextureLoc loc, PackedCol col) {
	struct VertexTextured* p = *v;
	float vOrigin = Atlas1D_RowId(loc) * Atlas1D.InvTileSize;
	float u1 = 0.0f, u2 = (step - 1) + UV2_Scale;
	float v1 = vOrigin + Atlas1D.InvTileSize, v2 = vOrigin;
	float X1 = (float)x0, X2 = X1 + step;
	float Z1 = (float)z0, Z2 = Z1 + step;

	p->x = X2; p->y = y; p->z = Z1; p->Col = col; p->U = u2; p->V = v1; p++;
	p->x = X1; p->y = y; p->z = Z1; p->Col = col; p->U = u1; p->V = v1; p++;
	p->x = X1; p->y = y; p->z = Z2; p->Col = col; p->U = u1; p->V = v2; p++;
	p->x = X2; p->y = y; p->z = Z2; p->Col = col; p->U = u2; p->V = v2; p++;
	*v = p;
}

/* Emits a vertical face at x = X facing towards -X. Characteristic of the XMin face. */
static void EmitSideXMin(struct VertexTextured** v, int x, int z0, int step, int yBot, int yTop, TextureLoc loc, PackedCol col) {
	struct VertexTextured* p = *v;
	float vOrigin = Atlas1D_RowId(loc) * Atlas1D.InvTileSize;
	int hgt = yTop - yBot;
	float u1 = 0.0f, u2 = (step - 1) + UV2_Scale;
	float v1 = vOrigin + Atlas1D.InvTileSize * hgt, v2 = vOrigin;
	float X = (float)x, Z1 = (float)z0, Z2 = Z1 + step;
	float Y2 = (float)yTop, Y1 = (float)yBot;

	p->x = X; p->y = Y2; p->z = Z2; p->Col = col; p->U = u2; p->V = v1; p++;
	p->x = X; p->y = Y2; p->z = Z1; p->Col = col; p->U = u1; p->V = v1; p++;
	p->x = X; p->y = Y1; p->z = Z1; p->Col = col; p->U = u1; p->V = v2; p++;
	p->x = X; p->y = Y1; p->z = Z2; p->Col = col; p->U = u2; p->V = v2; p++;
	*v = p;
}

/* Emits a vertical face at x = X facing towards +X. Characteristic of the XMax face. */
static void EmitSideXMax(struct VertexTextured** v, int x, int z0, int step, int yBot, int yTop, TextureLoc loc, PackedCol col) {
	struct VertexTextured* p = *v;
	float vOrigin = Atlas1D_RowId(loc) * Atlas1D.InvTileSize;
	int hgt = yTop - yBot;
	float u1 = 0.0f, u2 = (step - 1) + UV2_Scale;
	float v1 = vOrigin + Atlas1D.InvTileSize * hgt, v2 = vOrigin;
	float X = (float)x, Z1 = (float)z0, Z2 = Z1 + step;
	float Y2 = (float)yTop, Y1 = (float)yBot;

	p->x = X; p->y = Y2; p->z = Z1; p->Col = col; p->U = u1; p->V = v1; p++;
	p->x = X; p->y = Y2; p->z = Z2; p->Col = col; p->U = u2; p->V = v1; p++;
	p->x = X; p->y = Y1; p->z = Z2; p->Col = col; p->U = u2; p->V = v2; p++;
	p->x = X; p->y = Y1; p->z = Z1; p->Col = col; p->U = u1; p->V = v2; p++;
	*v = p;
}

/* Emits a vertical face at z = Z facing towards -Z. Characteristic of the ZMin face. */
static void EmitSideZMin(struct VertexTextured** v, int x0, int z, int step, int yBot, int yTop, TextureLoc loc, PackedCol col) {
	struct VertexTextured* p = *v;
	float vOrigin = Atlas1D_RowId(loc) * Atlas1D.InvTileSize;
	int hgt = yTop - yBot;
	float u1 = 0.0f, u2 = (step - 1) + UV2_Scale;
	float v1 = vOrigin + Atlas1D.InvTileSize * hgt, v2 = vOrigin;
	float Z = (float)z, X1 = (float)x0, X2 = X1 + step;
	float Y2 = (float)yTop, Y1 = (float)yBot;

	p->x = X2; p->y = Y1; p->z = Z; p->Col = col; p->U = u2; p->V = v2; p++;
	p->x = X1; p->y = Y1; p->z = Z; p->Col = col; p->U = u1; p->V = v2; p++;
	p->x = X1; p->y = Y2; p->z = Z; p->Col = col; p->U = u1; p->V = v1; p++;
	p->x = X2; p->y = Y2; p->z = Z; p->Col = col; p->U = u2; p->V = v1; p++;
	*v = p;
}

/* Emits a vertical face at z = Z facing towards +Z. Characteristic of the ZMax face. */
static void EmitSideZMax(struct VertexTextured** v, int x0, int z, int step, int yBot, int yTop, TextureLoc loc, PackedCol col) {
	struct VertexTextured* p = *v;
	float vOrigin = Atlas1D_RowId(loc) * Atlas1D.InvTileSize;
	int hgt = yTop - yBot;
	float u1 = 0.0f, u2 = (step - 1) + UV2_Scale;
	float v1 = vOrigin + Atlas1D.InvTileSize * hgt, v2 = vOrigin;
	float Z = (float)z, X1 = (float)x0, X2 = X1 + step;
	float Y2 = (float)yTop, Y1 = (float)yBot;

	p->x = X2; p->y = Y2; p->z = Z; p->Col = col; p->U = u2; p->V = v1; p++;
	p->x = X1; p->y = Y2; p->z = Z; p->Col = col; p->U = u1; p->V = v1; p++;
	p->x = X1; p->y = Y1; p->z = Z; p->Col = col; p->U = u1; p->V = v2; p++;
	p->x = X2; p->y = Y1; p->z = Z; p->Col = col; p->U = u2; p->V = v2; p++;
	*v = p;
}

static void Lod_Build(LodTile* tile) {
	int step = tile->step;
	int bx = tile->lchx << LOD_CHUNK_SHIFT, bz = tile->lchz << LOD_CHUNK_SHIFT;
	int n = LOD_CHUNK_SIZE / step, gs = n + 3, gsz = gs * gs;
	int* h = (int*)Mem_Alloc(gsz, sizeof(int), "lod grid");
	int u, v, ci, cj;
	LodInfo* info;
	struct VertexTextured* verts;
	struct VertexTextured* gp; /* grass cursor */
	struct VertexTextured* sp; /* sand cursor */
	struct VertexTextured* wp; /* water cursor */
	int grass = 0, sand = 0, water = 0, totalV;
	int minY = Int32_MaxValue, maxY = Int32_MinValue;

	/* Sample the heightfield grid, plus a 1 cell halo so boundary skirts can
	   be decided consistently between neighbouring macro-chunks. */
#define H(u, v) h[(v) * gs + (u)]
	for (v = 0; v < gs; v++) {
		for (u = 0; u < gs; u++) {
			H(u, v) = InfiniteGen_GetSurfaceHeight(bx + (u - 1) * step, bz + (v - 1) * step);
		}
	}

	/* Counting pass, so vertex buffer sizes are known before allocating.
	   The emit pass below MUST produce exactly these counts (both use the
	   Lod_TopFrom4 based side-height comparisons). */
	for (cj = 0; cj < n; cj++) {
		for (ci = 0; ci < n; ci++) {
			int mid4 = H(ci + 1, cj + 1) + H(ci + 2, cj + 1) + H(ci + 1, cj + 2) + H(ci + 2, cj + 2);
			int nXp4 = H(ci + 2, cj + 1) + H(ci + 3, cj + 1) + H(ci + 2, cj + 2) + H(ci + 3, cj + 2);
			int nXm4 = H(ci,     cj + 1) + H(ci + 1, cj + 1) + H(ci,     cj + 2) + H(ci + 1, cj + 2);
			int nZp4 = H(ci + 1, cj + 2) + H(ci + 2, cj + 2) + H(ci + 1, cj + 3) + H(ci + 2, cj + 3);
			int nZm4 = H(ci + 1, cj)     + H(ci + 2, cj)     + H(ci + 1, cj + 1) + H(ci + 2, cj + 1);
			int topY = Lod_TopFrom4(mid4);
			int sides;

			if (topY < minY) minY = topY;
			if (topY > maxY) maxY = topY;

			sides = 0;
			if (Lod_TopFrom4(nXp4) > topY) sides += 4;
			if (Lod_TopFrom4(nXm4) > topY) sides += 4;
			if (Lod_TopFrom4(nZp4) > topY) sides += 4;
			if (Lod_TopFrom4(nZm4) > topY) sides += 4;

			if (mid4 < INF_WATER_LEVEL * 4) {
				sand  += 4 + sides;
				water += 4;
			} else {
				grass += 4 + sides;
			}
		}
	}
#undef H

	totalV = grass + sand + water;
	info = tile->info;
	if (info->vb) Gfx_DeleteVb(&info->vb);

	if (!totalV) {
		Mem_Free(h);
		info->grassVerts = info->sandVerts = info->totalVerts = 0;
		tile->valid = false;
		tile->minY = minY; tile->maxY = maxY;
		return;
	}

	info->vb = Gfx_TryCreateStaticVb(VERTEX_FORMAT_TEXTURED, totalV + 1);
	if (!info->vb) {
		Mem_Free(h);
		info->grassVerts = info->sandVerts = info->totalVerts = 0;
		tile->valid = false;
		tile->minY = minY; tile->maxY = maxY;
		tile->dirty = true;
		return;
	}

	verts = (struct VertexTextured*)Gfx_LockVb(info->vb, VERTEX_FORMAT_TEXTURED, totalV + 1);
	gp = verts; sp = verts + grass; wp = verts + grass + sand;

#define H(u, v) h[(v) * gs + (u)]
	for (cj = 0; cj < n; cj++) {
		for (ci = 0; ci < n; ci++) {
			int mid4 = H(ci + 1, cj + 1) + H(ci + 2, cj + 1) + H(ci + 1, cj + 2) + H(ci + 2, cj + 2);
			int nXp4 = H(ci + 2, cj + 1) + H(ci + 3, cj + 1) + H(ci + 2, cj + 2) + H(ci + 3, cj + 2);
			int nXm4 = H(ci,     cj + 1) + H(ci + 1, cj + 1) + H(ci,     cj + 2) + H(ci + 1, cj + 2);
			int nZp4 = H(ci + 1, cj + 2) + H(ci + 2, cj + 2) + H(ci + 1, cj + 3) + H(ci + 2, cj + 3);
			int nZm4 = H(ci + 1, cj)     + H(ci + 2, cj)     + H(ci + 1, cj + 1) + H(ci + 2, cj + 1);
			int topY = Lod_TopFrom4(mid4);
			int topXp = Lod_TopFrom4(nXp4), topXm = Lod_TopFrom4(nXm4);
			int topZp = Lod_TopFrom4(nZp4), topZm = Lod_TopFrom4(nZm4);
			int x0 = bx + ci * step, z0 = bz + cj * step;
			cc_bool sandCell = mid4 < INF_WATER_LEVEL * 4;
			BlockID blk = sandCell ? BLOCK_SAND : BLOCK_GRASS;
			TextureLoc topLoc = Block_Tex(blk, FACE_YMAX);
			struct VertexTextured** target = sandCell ? &sp : &gp;
			int hgt;

			EmitTop(target, x0, z0, step, (float)topY, topLoc, Env.SunCol);

			hgt = topXp - topY;
			if (hgt > 0) EmitSideXMin(target, x0 + step, z0, step, topY, topXp, Block_Tex(blk, FACE_XMIN), Env.SunXSide);
			hgt = topXm - topY;
			if (hgt > 0) EmitSideXMax(target, x0,         z0, step, topY, topXm, Block_Tex(blk, FACE_XMAX), Env.SunXSide);
			hgt = topZp - topY;
			if (hgt > 0) EmitSideZMin(target, x0, z0 + step, step, topY, topZp, Block_Tex(blk, FACE_ZMIN), Env.SunZSide);
			hgt = topZm - topY;
			if (hgt > 0) EmitSideZMax(target, x0, z0,         step, topY, topZm, Block_Tex(blk, FACE_ZMAX), Env.SunZSide);

			if (sandCell) {
				EmitTop(&wp, x0, z0, step, (float)INF_WATER_LEVEL + 0.95f,
					Block_Tex(BLOCK_STILL_WATER, FACE_YMAX), Env.SunCol);
			}
		}
	}
#undef H
	Mem_Free(h);
	Gfx_UnlockVb(info->vb);

	info->grassVerts = grass;
	info->sandVerts  = grass + sand;
	info->totalVerts = totalV;
	tile->minY = minY; tile->maxY = maxY;
	tile->valid = true;
	tile->dirty = false;
}


/*########################################################################################################################*
*---------------------------------------------------------Streaming-------------------------------------------------------*
*#########################################################################################################################*/
static void Lod_Unload(int camX, int camZ, int farSqrM) {
	int i;
	for (i = lodOrderCount - 1; i >= 0; i--) {
		LodTile* t = lodOrder[i];
		cc_int64 dx = (cc_int64)((t->lchx << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2)) - camX;
		cc_int64 dz = (cc_int64)((t->lchz << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2)) - camZ;
		if (dx * dx + dz * dz > farSqrM) Lod_Remove(t);
	}
}

static void Lod_StreamIn(int camX, int camZ, int farSqrM, cc_bool hasFwd, float fx, float fz) {
	int budget = LOD_NEW_TILES_PER_FRAME;
	int camTx = camX >> LOD_CHUNK_SHIFT, camTz = camZ >> LOD_CHUNK_SHIFT;
	int radius = (int)(Math_SqrtF((float)farSqrM) / LOD_CHUNK_SIZE) + 2;
	int r, sweep, dz, dx;

	for (r = 0; r <= radius && budget > 0; r++) {
		for (sweep = 0; sweep < 2 && budget > 0; sweep++) {
			for (dz = -r; dz <= r && budget > 0; dz++) {
				for (dx = -r; dx <= r && budget > 0; dx++) {
					LodTile* t;
					cc_int64 wx, wz, d;
					if (Math_AbsI(dx) < r && Math_AbsI(dz) < r) continue;
					if (Lod_Find(camTx + dx, camTz + dz)) continue;
					if (hasFwd && (sweep == 0) != ((dx * fx + dz * fz) >= 0.0f)) continue;

					wx = (cc_int64)((camTx + dx) << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2) - camX;
					wz = (cc_int64)((camTz + dz) << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2) - camZ;
					d  = wx * wx + wz * wz;
					if (d > farSqrM) continue;

					t = Lod_Create(camTx + dx, camTz + dz);
					if (!t) return; /* at max tile count */
					budget--;
				}
			}
		}
	}
}

static void Lod_Rebuild(int camX, int camZ) {
	int builds = 0;
	int i;
	for (i = 0; i < lodOrderCount && builds < LOD_BUILDS_PER_FRAME; i++) {
		LodTile* t = lodOrder[i];
		int cx = (t->lchx << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2);
		int cz = (t->lchz << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2);
		int dx = cx - camX, dz = cz - camZ;
		int dist = (int)(Math_SqrtF((float)(dx * dx + dz * dz)));
		int step = Lod_StepForDist(dist);

		if (!t->dirty && t->valid && t->step == step) continue;
		t->step = step;
		Lod_Build(t);
		builds++;
	}
}

static void Lod_Cull(void) {
	int i;
	lodGrassCount = 0; lodSandCount = 0;
	Lod_EnsureRenderCapacity(lodCap);

	for (i = 0; i < lodOrderCount; i++) {
		LodTile* t = lodOrder[i];
		LodInfo* info = t->info;
		int cx = (t->lchx << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2);
		int cz = (t->lchz << LOD_CHUNK_SHIFT) + (LOD_CHUNK_SIZE / 2);
		int res;
		t->visible = false;
		if (!t->valid || !info) continue;

		res = Frustum_TestSphere((float)cx, (float)(t->minY + t->maxY) / 2.0f, (float)cz,
			181.0f + (t->maxY - t->minY));
		if (res == FRUSTUM_OUTSIDE) continue;
		t->visible = true;

		if (info->grassVerts > 0)            lodGrassList[lodGrassCount++] = t;
		if (info->sandVerts > info->grassVerts) lodSandList[lodSandCount++] = t;
	}
}


/*########################################################################################################################*
*---------------------------------------------------------Public API------------------------------------------------------*
*#########################################################################################################################*/
void Lod_Update(float delta) {
	IVec3 cam;
	int farBlocks, farSqrM;
	cc_int64 vx, vz;
	cc_bool hasFwd;
	float fx, fz, flen;

	if (!Lod_Active() || !lodReady) return;
	(void)delta;
	IVec3_Floor(&cam, &Camera.CurrentPos);

	farBlocks = Game_ViewDistance;
	if (farBlocks < 16) farBlocks = 16;
	farSqrM = (farBlocks + LOD_CHUNK_SIZE) * (farBlocks + LOD_CHUNK_SIZE);

	/* Bias streaming towards the direction the camera is moving. */
	vx = cam.x - lodLastCamX; vz = cam.z - lodLastCamZ;
	hasFwd = (vx * vx + vz * vz) > 1;
	if (hasFwd) {
		flen = Math_SqrtF((float)(vx * vx + vz * vz));
		fx = (float)vx / flen; fz = (float)vz / flen;
	} else {
		fx = 0.0f; fz = 0.0f;
	}
	lodLastCamX = cam.x; lodLastCamZ = cam.z;

	Lod_Unload(cam.x, cam.z, farSqrM);
	Lod_StreamIn(cam.x, cam.z, farSqrM, hasFwd, fx, fz);
	Lod_Rebuild(cam.x, cam.z);
	Lod_Cull();
}

void Lod_Render(void) {
	int i;
	if (!Lod_Active()) return;
	if (!lodGrassCount && !lodSandCount) return;

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetFaceCulling(false);

	if (lodGrassCount) {
		Atlas1D_Bind(Atlas1D_Index(Block_Tex(BLOCK_GRASS, FACE_YMAX)));
		for (i = 0; i < lodGrassCount; i++) {
			LodTile* t = lodGrassList[i];
			Gfx_BindVb_Textured(t->info->vb);
			Gfx_DrawIndexedTris_T2fC4b(t->info->grassVerts, 0, 0);
			Game_Vertices += t->info->grassVerts;
		}
	}
	if (lodSandCount) {
		Atlas1D_Bind(Atlas1D_Index(Block_Tex(BLOCK_SAND, FACE_YMAX)));
		for (i = 0; i < lodSandCount; i++) {
			LodTile* t = lodSandList[i];
			Gfx_BindVb_Textured(t->info->vb);
			Gfx_DrawIndexedTris_T2fC4b(t->info->sandVerts - t->info->grassVerts, t->info->grassVerts, 0);
			Game_Vertices += (t->info->sandVerts - t->info->grassVerts);
		}
	}
}

static void Lod_RenderWater(cc_bool bindAtlas) {
	int i;
	if (!Lod_Active() || lodOrderCount == 0) return;

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetFaceCulling(false);
	if (bindAtlas) {
		Atlas1D_Bind(Atlas1D_Index(Block_Tex(BLOCK_STILL_WATER, FACE_YMAX)));
	}
	for (i = 0; i < lodOrderCount; i++) {
		LodTile* t = lodOrder[i];
		LodInfo* info;
		int count;
		if (!t->visible || !t->info) continue;
		info = t->info;
		count = info->totalVerts - info->sandVerts;
		if (count <= 0) continue;

		Gfx_BindVb_Textured(info->vb);
		Gfx_DrawIndexedTris_T2fC4b(count, info->sandVerts, 0);
		Game_Vertices += count;
	}
}

void Lod_RenderTranslucentDepth(void) {
	Lod_RenderWater(false);
}

void Lod_RenderTranslucent(void) {
	Lod_RenderWater(true);
}

/* Frees all far-away terrain state. */
void Lod_OnNewMap(void) {
	Lod_DropTiles();
	lodReady = false;
	Mem_Free(lodEntries);
	Mem_Free(lodOrder);
	Mem_Free(lodGrassList);
	Mem_Free(lodSandList);
	lodEntries    = NULL;
	lodOrder      = NULL;
	lodGrassList  = NULL;
	lodSandList   = NULL;
	lodCap        = 0;
	lodOrderCap   = 0;
}

/* Prepares far-away terrain streaming for a newly loaded map. */
void Lod_OnNewMapLoaded(void) {
	if (!lodEntries) {
		lodCap    = 64;
		lodEntries = (LodTile**)Mem_AllocCleared(lodCap, sizeof(LodTile*), "lod store");
		Lod_EnsureRenderCapacity(lodCap);
	}
	lodLastCamX = lodLastCamZ = Int32_MaxValue;
	lodReady = true;
}

/* Drops all far-away terrain meshes (e.g. graphics context lost).
   Keeps the store ready so the terrain is simply streamed back in. */
void Lod_OnContextLost(void) {
	Lod_DropTiles();
	lodLastCamX = lodLastCamZ = Int32_MaxValue;
	lodReady = true;
}

/* Marks all far-away terrain as needing to be rebuilt. */
void Lod_RefreshAll(void) {
	int i;
	if (!lodEntries) return;
	for (i = 0; i < lodOrderCount; i++) {
		lodOrder[i]->dirty = true;
	}
}

/* Marks the macro-chunk containing the given full-resolution chunk as dirty. */
void Lod_RefreshChunk(int cx, int cz) {
	LodTile* t;
	if (!lodEntries) return;
	t = Lod_Find(cx >> (LOD_CHUNK_SHIFT - CHUNK_SHIFT), cz >> (LOD_CHUNK_SHIFT - CHUNK_SHIFT));
	if (t) t->dirty = true;
}