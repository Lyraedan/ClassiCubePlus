#include "MapRenderer.h"
#include "Block.h"
#include "Builder.h"
#include "Camera.h"
#include "Entity.h"
#include "EnvRenderer.h"
#include "Event.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Game.h"
#include "Graphics.h"
#include "InfiniteGen.h"
#include "Platform.h"
#include "TexturePack.h"
#include "Utils.h"
#include "World.h"
#include "Options.h"

int MapRenderer_1DUsedCount;
struct ChunkPartInfo* MapRenderer_PartsNormal;
struct ChunkPartInfo* MapRenderer_PartsTranslucent;

static cc_bool inTranslucent;
static IVec3 chunkPos;

/* The number of non-empty Normal/Translucent ChunkPartInfos (across entire world) for each 1D atlas batch. */
/* 1D atlas batches that do not have any ChunkPartInfos can be entirely skipped. */
static int normPartsCount[ATLAS1D_MAX_ATLASES], tranPartsCount[ATLAS1D_MAX_ATLASES];
/* Whether there are any visible Normal/Translucent ChunkPartInfos for each 1D atlas batch. */
/* 1D atlas batches that do not have any visible ChunkPartInfos can be skipped. */
static cc_bool hasNormParts[ATLAS1D_MAX_ATLASES], hasTranParts[ATLAS1D_MAX_ATLASES];
/* Whether renderer should check if there are any visible Normal/Translucent ChunkPartInfos for each 1D atlas batch. */
static cc_bool checkNormParts[ATLAS1D_MAX_ATLASES], checkTranParts[ATLAS1D_MAX_ATLASES];

/* Render info for all chunks in the world. Unsorted. */
static struct ChunkInfo* mapChunks;
/* Pointers to render info for all chunks in the world, sorted by distance from the camera. */
static struct ChunkInfo** sortedChunks;
/* Pointers to render info for all chunks in the world, sorted by distance from the camera. */
/* Only chunks that can be rendered (i.e. not empty and are visible) are included in this.  */
static struct ChunkInfo** renderChunks;
/* Number of actually used pointers in the renderChunks array. Entries past this are ignored and skipped. */
static int renderChunksCount;
/* Distance of each chunk from the camera. */
static cc_uint32* distances;
/* Maximum number of chunk updates that can be performed in one frame. */
static int maxChunkUpdates;
/* Cached number of chunks in the world */
static int chunksCount;

/* Forward declarations for the infinite world renderer. */
static cc_bool infActive;
static void Inf_RenderNormal(float delta);
static void Inf_RenderTranslucent(float delta);
static void Inf_UpdateChunks(float delta);
static void Inf_RefreshAll(void);
static void Inf_RefreshChunk(int cx, int cy, int cz);

static void ChunkInfo_Init(struct ChunkInfo* chunk, int x, int y, int z) {
	chunk->centreX = x + HALF_CHUNK_SIZE; chunk->centreY = y + HALF_CHUNK_SIZE; 
	chunk->centreZ = z + HALF_CHUNK_SIZE;
#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
	chunk->vb = 0;
#endif

	chunk->visible  = true;  
	chunk->empty    = false;
	chunk->allAir   = false;
	chunk->noData   = true;
	chunk->dirty    = true;
	chunk->skipClip = false;

	chunk->drawXMin = false; chunk->drawXMax = false; chunk->drawZMin = false;
	chunk->drawZMax = false; chunk->drawYMin = false; chunk->drawYMax = false;

	chunk->normalParts      = NULL;
	chunk->translucentParts = NULL;
}

static CC_INLINE void ChunkInfo_Refresh(struct ChunkInfo* chunk) {
	if (chunk->allAir) return; /* do not recreate chunks completely air */

	chunk->empty = false;
	chunk->dirty = true;
}

/* Index of maximum used 1D atlas + 1 */
CC_NOINLINE static int MapRenderer_UsedAtlases(void) {
	TextureLoc maxLoc = 0;
	int i;

	for (i = 0; i < Array_Elems(Blocks.Textures); i++) {
		maxLoc = max(maxLoc, Blocks.Textures[i]);
	}
	return Atlas1D_Index(maxLoc) + 1;
}


/*########################################################################################################################*
*-------------------------------------------------------Map rendering-----------------------------------------------------*
*#########################################################################################################################*/
static void CheckWeather(float delta) {
	IVec3 pos;
	BlockID block;
	cc_bool outside;
	IVec3_Floor(&pos, &Camera.CurrentPos);

	block   = World_SafeGetBlock(pos.x, pos.y, pos.z);
	outside = pos.y < 0 || !World_ContainsXZ(pos.x, pos.z);
	inTranslucent = Blocks.Draw[block] == DRAW_TRANSLUCENT || (pos.y < Env.EdgeHeight && outside);

	/* If we are under water, render weather before to blend properly */
	if (!inTranslucent || Env.Weather == WEATHER_SUNNY) return;
	Gfx_SetAlphaBlending(true);
	EnvRenderer_RenderWeather(delta);
	Gfx_SetAlphaBlending(false);
}

#ifdef CC_CLIPPING_FLAGS
	#define DrawBatch(count, start) Gfx_DrawIndexedTris_T2fC4b(count, start, info->skipClip ? DRAW_HINT_NOCLIP : 0)
#else
	#define DrawBatch(count, start) Gfx_DrawIndexedTris_T2fC4b(count, start, 0)
#endif

#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
	#define DrawFace(face, ign)    Gfx_BindVb(part.vbs[face]); DrawBatch(0, 0);
	#define DrawFaces(f1, f2, ign) DrawFace(f1, ign); DrawFace(f2, ign);
#else
	#define DrawFace(face, offset)    DrawBatch(part.counts[face], offset);
	#define DrawFaces(f1, f2, offset) DrawBatch(part.counts[f1] + part.counts[f2], offset);
#endif

#define DrawNormalFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	Gfx_SetFaceCulling(true); \
	DrawFaces(minFace, maxFace, offset); \
	Gfx_SetFaceCulling(false); \
	Game_Vertices += (part.counts[minFace] + part.counts[maxFace]); \
} else if (drawMin) { \
	DrawFace(minFace, offset); \
	Game_Vertices += part.counts[minFace]; \
} else if (drawMax) { \
	DrawFace(maxFace, offset + part.counts[minFace]); \
	Game_Vertices += part.counts[maxFace]; \
}

static void RenderNormalBatch(int batch) {
	int batchOffset = chunksCount * batch;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int i, offset, count;

	for (i = 0; i < renderChunksCount; i++) {
		info = renderChunks[i];
		if (!info->normalParts) continue;

		part = info->normalParts[batchOffset];
		if (part.offset < 0) continue;
		hasNormParts[batch] = true;

#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
		Gfx_BindVb_Textured(info->vb);
#endif

		offset  = part.offset + part.spriteCount;
		drawMin = info->drawXMin && part.counts[FACE_XMIN];
		drawMax = info->drawXMax && part.counts[FACE_XMAX];
		DrawNormalFaces(FACE_XMIN, FACE_XMAX);

		offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
		drawMin = info->drawZMin && part.counts[FACE_ZMIN];
		drawMax = info->drawZMax && part.counts[FACE_ZMAX];
		DrawNormalFaces(FACE_ZMIN, FACE_ZMAX);

		offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
		drawMin = info->drawYMin && part.counts[FACE_YMIN];
		drawMax = info->drawYMax && part.counts[FACE_YMAX];
		DrawNormalFaces(FACE_YMIN, FACE_YMAX);

		if (!part.spriteCount) continue;
		offset = part.offset;
		count  = part.spriteCount >> 2; /* 4 per sprite */

		Gfx_SetFaceCulling(true);
		/* TODO: fix to not render them all */
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
		Gfx_BindVb(part.vbs[FACE_COUNT]);
		DrawBatch(0, 0);
		Game_Vertices += count * 4;
		Gfx_SetFaceCulling(false);
		continue;
#endif
		if (info->drawXMax || info->drawZMin) {
			DrawBatch(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->drawXMin || info->drawZMax) {
			DrawBatch(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->drawXMin || info->drawZMin) {
			DrawBatch(count, offset); Game_Vertices += count;
		} offset += count;

		if (info->drawXMax || info->drawZMax) {
			DrawBatch(count, offset); Game_Vertices += count;
		}
		Gfx_SetFaceCulling(false);
	}
}

void MapRenderer_RenderNormal(float delta) {
	int batch;
	if (World.Type == WORLD_INFINITE) { Inf_RenderNormal(delta); return; }
	if (!mapChunks) return;

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaTest(true);
	
	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) 
	{
		if (normPartsCount[batch] <= 0) continue;
		if (hasNormParts[batch] || checkNormParts[batch]) {
			Atlas1D_Bind(batch);
			RenderNormalBatch(batch);
			checkNormParts[batch] = false;
		}
	}
	Gfx_DisableMipmaps();

	CheckWeather(delta);
	Gfx_SetAlphaTest(false);
#if DEBUG_OCCLUSION
	DebugPickedPos();
#endif
}

#define DrawTranslucentFaces(minFace, maxFace) \
if (drawMin && drawMax) { \
	DrawFaces(minFace, maxFace, offset); \
	Game_Vertices += (part.counts[minFace] + part.counts[maxFace]); \
} else if (drawMin) { \
	DrawFace(minFace, offset); \
	Game_Vertices += part.counts[minFace]; \
} else if (drawMax) { \
	DrawFace(maxFace, offset + part.counts[minFace]); \
	Game_Vertices += part.counts[maxFace]; \
}

static void RenderTranslucentBatch(int batch) {
	int batchOffset = chunksCount * batch;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int i, offset;

	for (i = 0; i < renderChunksCount; i++) {
		info = renderChunks[i];
		if (!info->translucentParts) continue;

		part = info->translucentParts[batchOffset];
		if (part.offset < 0) continue;
		hasTranParts[batch] = true;

#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
		Gfx_BindVb_Textured(info->vb);
#endif

		offset  = part.offset;
		drawMin = (inTranslucent || info->drawXMin) && part.counts[FACE_XMIN];
		drawMax = (inTranslucent || info->drawXMax) && part.counts[FACE_XMAX];
		DrawTranslucentFaces(FACE_XMIN, FACE_XMAX);

		offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
		drawMin = (inTranslucent || info->drawZMin) && part.counts[FACE_ZMIN];
		drawMax = (inTranslucent || info->drawZMax) && part.counts[FACE_ZMAX];
		DrawTranslucentFaces(FACE_ZMIN, FACE_ZMAX);

		offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
		drawMin = (inTranslucent || info->drawYMin) && part.counts[FACE_YMIN];
		drawMax = (inTranslucent || info->drawYMax) && part.counts[FACE_YMAX];
		DrawTranslucentFaces(FACE_YMIN, FACE_YMAX);
	}
}

void MapRenderer_RenderTranslucent(float delta) {
	int vertices, batch;
	if (World.Type == WORLD_INFINITE) { Inf_RenderTranslucent(delta); return; }
	if (!mapChunks) return;

	/* First fill depth buffer */
	vertices = Game_Vertices;
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaBlending(false);
	Gfx_DepthOnlyRendering(true);

	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) 
	{
		if (tranPartsCount[batch] <= 0) continue;
		if (hasTranParts[batch] || checkTranParts[batch]) {
			RenderTranslucentBatch(batch);
			checkTranParts[batch] = false;
		}
	}
	Game_Vertices = vertices;

	/* Then actually draw the transluscent blocks */
	Gfx_SetAlphaBlending(true);
	Gfx_DepthOnlyRendering(false);
	Gfx_SetDepthWrite(false); /* already calculated depth values in depth pass */

	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) 
	{
		if (tranPartsCount[batch] <= 0) continue;
		if (!hasTranParts[batch]) continue;

		Atlas1D_Bind(batch);
		RenderTranslucentBatch(batch);
	}
	Gfx_DisableMipmaps();

	Gfx_SetDepthWrite(true);
	/* If we weren't under water, render weather after to blend properly */
	if (!inTranslucent && Env.Weather != WEATHER_SUNNY) {
		Gfx_SetAlphaTest(true);
		EnvRenderer_RenderWeather(delta);
		Gfx_SetAlphaTest(false);
	}
	Gfx_SetAlphaBlending(false);
}


/*########################################################################################################################*
*---------------------------------------------------Chunk functionality---------------------------------------------------*
*#########################################################################################################################*/
/* Deletes vertex buffer associated with the given chunk and updates internal state */
static void DeleteChunk(struct ChunkInfo* chunk) {
	struct ChunkPartInfo* ptr;
	int i;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
	int j;
#else
	Gfx_DeleteVb(&chunk->vb);
#endif

	chunk->empty  = false; 
	chunk->allAir = false;
	chunk->noData = true;
	chunk->dirty  = true;

#ifdef OCCLUSION
	chunk.OcclusionFlags = 0;
	chunk.OccludedFlags = 0;
#endif

	if (chunk->normalParts) {
		ptr = chunk->normalParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset < 0) continue; 
			normPartsCount[i]--;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
			for (j = 0; j < CHUNKPART_MAX_VBS; j++) Gfx_DeleteVb(&ptr->vbs[j]);
#endif
		}
		chunk->normalParts = NULL;
	}

	if (chunk->translucentParts) {
		ptr = chunk->translucentParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset < 0) continue;
			tranPartsCount[i]--;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
			for (j = 0; j < CHUNKPART_MAX_VBS; j++) Gfx_DeleteVb(&ptr->vbs[j]);
#endif
		}
		chunk->translucentParts = NULL;
	}
}

/* Builds the mesh (hence vertex buffer) for the given chunk, and updates internal state */
static void BuildChunk(struct ChunkInfo* chunk, int* chunkUpdates) {
	struct ChunkPartInfo* ptr;
	int i;

	Game.ChunkUpdates++;
	(*chunkUpdates)++;
	if (!Builder_MakeChunk(chunk)) return;

	chunk->dirty  = false;
	chunk->noData = !chunk->normalParts && !chunk->translucentParts;
	chunk->empty  = chunk->noData;
	if (chunk->empty) return;
	
	if (chunk->normalParts) {
		ptr = chunk->normalParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset >= 0) normPartsCount[i]++;
		}
	}

	if (chunk->translucentParts) {
		ptr = chunk->translucentParts;
		for (i = 0; i < MapRenderer_1DUsedCount; i++, ptr += chunksCount) {
			if (ptr->offset >= 0) tranPartsCount[i]++;
		}
	}
}


/*########################################################################################################################*
*----------------------------------------------------Chunks mangagement---------------------------------------------------*
*#########################################################################################################################*/
static void FreeParts(void) {
	Mem_Free(MapRenderer_PartsNormal);
	MapRenderer_PartsNormal      = NULL;
	MapRenderer_PartsTranslucent = NULL;
}

static void FreeChunks(void) {
	Mem_Free(mapChunks);
	Mem_Free(sortedChunks);
	Mem_Free(renderChunks);
	Mem_Free(distances);

	mapChunks    = NULL;
	sortedChunks = NULL;
	renderChunks = NULL;
	distances    = NULL;
}

static void AllocateParts(void) {
	struct ChunkPartInfo* ptr;
	cc_uint32 count = chunksCount * MapRenderer_1DUsedCount;

	ptr = (struct ChunkPartInfo*)Mem_AllocCleared(count * 2, sizeof(struct ChunkPartInfo), "chunk parts");
	MapRenderer_PartsNormal      = ptr;
	MapRenderer_PartsTranslucent = ptr + count;
}

static void AllocateChunks(void) {
	mapChunks    = (struct ChunkInfo*) Mem_Alloc(chunksCount, sizeof(struct ChunkInfo),  "chunk info");
	sortedChunks = (struct ChunkInfo**)Mem_Alloc(chunksCount, sizeof(struct ChunkInfo*), "sorted chunk info");
	renderChunks = (struct ChunkInfo**)Mem_Alloc(chunksCount, sizeof(struct ChunkInfo*), "render chunk info");
	distances    = (cc_uint32*)Mem_Alloc(chunksCount, 4, "chunk distances");
}

static void ResetPartFlags(void) {
	int i;
	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		checkNormParts[i] = true;
		hasNormParts[i]   = false;
		checkTranParts[i] = true;
		hasTranParts[i]   = false;
	}
}

static void ResetPartCounts(void) {
	int i;
	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		normPartsCount[i] = 0;
		tranPartsCount[i] = 0;
	}
}

static void InitChunks(void) {
	int x, y, z, index = 0;
	for (z = 0; z < World.Length; z += CHUNK_SIZE) {
		for (y = 0; y < World.Height; y += CHUNK_SIZE) {
			for (x = 0; x < World.Width; x += CHUNK_SIZE) {
				ChunkInfo_Init(&mapChunks[index], x, y, z);
				sortedChunks[index] = &mapChunks[index];
				renderChunks[index] = &mapChunks[index];
				distances[index]    = 0;
				index++;
			}
		}
	}
}

static void RefreshChunks(void) {
	int i;
	if (!mapChunks) return;

	for (i = 0; i < chunksCount; i++) 
	{
		ChunkInfo_Refresh(&mapChunks[i]);
	}
}

static void DeleteChunks(void) {
	int i;
	if (!mapChunks) return;

	for (i = 0; i < chunksCount; i++) 
	{
		DeleteChunk(&mapChunks[i]);
	}
	ResetPartCounts();
}

void MapRenderer_Refresh(void) {
	int oldCount;
	if (World.Type == WORLD_INFINITE) {
		Inf_RefreshAll();
		return;
	}

	chunkPos = IVec3_MaxValue();

	if (mapChunks && World.Blocks) {
		DeleteChunks();

		oldCount = MapRenderer_1DUsedCount;
		MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
		/* Need to reallocate parts array in this case */
		if (MapRenderer_1DUsedCount != oldCount) {
			FreeParts();
			AllocateParts();
		}
	}
	ResetPartCounts();
}

/* Refreshes chunks on the border of the map whose y is less than 'maxHeight'. */
static void RefreshBorderChunks(int maxHeight) {
	int cx, cy, cz;
	cc_bool onBorder;

	chunkPos = IVec3_MaxValue();
	if (!mapChunks || !World.Blocks) return;

	for (cz = 0; cz < World.ChunksZ; cz++) {
		for (cy = 0; cy < World.ChunksY; cy++) {
			for (cx = 0; cx < World.ChunksX; cx++) {
				onBorder = cx == 0 || cz == 0 || cx == (World.ChunksX - 1) || cz == (World.ChunksZ - 1);

				if (onBorder && (cy * CHUNK_SIZE) < maxHeight) {
					MapRenderer_RefreshChunk(cx, cy, cz);
				}
			}
		}
	}
}


/*########################################################################################################################*
*--------------------------------------------------Chunks updating/sorting------------------------------------------------*
*#########################################################################################################################*/
#define CHUNK_TARGET_TIME ((1.0f/30) + 0.01f)
static int chunksTarget = 12;
static Vec3 lastCamPos;
static float lastYaw, lastPitch;
/* Max distance from camera that chunks are rendered within */
/* This may differ from the view distance configured by the user */
static int renderDistSquared;
/* Max distance from camera that chunks are built within */
/* Chunks past this distance are automatically unloaded */
static int buildDistSquared;

static int AdjustDist(int dist) {
	if (dist < CHUNK_SIZE) dist = CHUNK_SIZE;
	dist = Utils_AdjViewDist(dist);
	return (dist + 24) * (dist + 24);
}

static void CalcViewDists(void) {
	buildDistSquared  = AdjustDist(Game_UserViewDistance);
	renderDistSquared = AdjustDist(Game_ViewDistance);
	if (World.Type == WORLD_INFINITE) {
		/* Infinite worlds stream chunks in as the player moves; cap how far
		   ahead chunks are built/loaded so an unbounded view distance does not
		   require loading an impractical number of chunks. */
		buildDistSquared  = AdjustDist(WORLD_INF_MAX_VIEWDIST);
		renderDistSquared = AdjustDist(WORLD_INF_MAX_VIEWDIST);
	}
}

static int UpdateChunksAndVisibility(int* chunkUpdates) {
	int renderDistSqr = renderDistSquared;
	int buildDistSqr  = buildDistSquared;

	struct ChunkInfo* chunk;
	int i, j = 0, distSqr, res;

	for (i = 0; i < chunksCount; i++) 
	{
		chunk = sortedChunks[i];
		if (chunk->empty) continue;
		distSqr = distances[i];
		
		/* Auto unload chunks far away chunks */
		if (!chunk->noData && distSqr >= buildDistSqr + 32 * 16) {
			DeleteChunk(chunk); continue;
		}

		if (chunk->dirty && distSqr <= buildDistSqr && *chunkUpdates < chunksTarget) {
			DeleteChunk(chunk);
			BuildChunk(chunk, chunkUpdates);
		}

		if (distSqr > renderDistSqr) {
			chunk->visible  = false;
		} else {
			res = Frustum_TestSphere(chunk->centreX, chunk->centreY, chunk->centreZ, 14); /* 14 ~ sqrt(3 * 8^2) */
			chunk->visible  = res != FRUSTUM_OUTSIDE;
			chunk->skipClip = Gfx_CanSphereSkipClipping(chunk->centreX, chunk->centreY, chunk->centreZ, 14);
		}

		if (chunk->visible && !chunk->empty) { renderChunks[j] = chunk; j++; }
	}
	return j;
}

static int UpdateChunksStill(int* chunkUpdates) {
	int renderDistSqr = renderDistSquared;
	int buildDistSqr  = buildDistSquared;

	struct ChunkInfo* chunk;
	int i, j = 0, distSqr, res;

	for (i = 0; i < chunksCount; i++) 
	{
		chunk = sortedChunks[i];
		if (chunk->empty) continue;
		distSqr = distances[i];

		/* Auto unload chunks far away chunks */
		if (!chunk->noData && distSqr >= buildDistSqr + 32 * 16) {
			DeleteChunk(chunk); continue;
		}

		if (chunk->dirty && distSqr <= buildDistSqr && *chunkUpdates < chunksTarget) {
			DeleteChunk(chunk);
			BuildChunk(chunk, chunkUpdates);

			/* only need to update the visibility of chunks in range. */
			if (distSqr > renderDistSqr) {
				chunk->visible  = false;
			} else {
				res = Frustum_TestSphere(chunk->centreX, chunk->centreY, chunk->centreZ, 14); /* 14 ~ sqrt(3 * 8^2) */
				chunk->visible  = res != FRUSTUM_OUTSIDE;
				chunk->skipClip = Gfx_CanSphereSkipClipping(chunk->centreX, chunk->centreY, chunk->centreZ, 14);
			}

			if (chunk->visible && !chunk->empty) { renderChunks[j] = chunk; j++; }
		} else if (chunk->visible) {
			renderChunks[j] = chunk; j++;
		}
	}
	return j;
}

static void UpdateChunks(float delta) {
	struct LocalPlayer* p;
	cc_bool samePos;
	int chunkUpdates = 0;

	/* Build more chunks if 30 FPS or over, otherwise slowdown */
	chunksTarget += delta < CHUNK_TARGET_TIME ? 1 : -1; 
	Math_Clamp(chunksTarget, 4, maxChunkUpdates);

	p = Entities.CurPlayer;
	samePos = Vec3_Equals(&Camera.CurrentPos, &lastCamPos)
		&& p->Base.Pitch == lastPitch && p->Base.Yaw == lastYaw;

	renderChunksCount = samePos ?
		UpdateChunksStill(&chunkUpdates) :
		UpdateChunksAndVisibility(&chunkUpdates);

	lastCamPos = Camera.CurrentPos;
	lastPitch  = p->Base.Pitch;
	lastYaw    = p->Base.Yaw;

	if (!samePos || chunkUpdates) ResetPartFlags();
}

static void SortMapChunks(int left, int right) {
	struct ChunkInfo** values = sortedChunks; struct ChunkInfo* value;
	cc_uint32* keys = distances; cc_uint32 key;

	while (left < right) {
		int i = left, j = right;
		cc_uint32 pivot = keys[(i + j) >> 1];

		/* partition the list */
		while (i <= j) {
			while (pivot > keys[i]) i++;
			while (pivot < keys[j]) j--;
			QuickSort_Swap_KV_Maybe();
		}
		/* recurse into the smaller subset */
		QuickSort_Recurse(SortMapChunks)
	}
}

static void UpdateSortOrder(void) {
	struct ChunkInfo* info;
	IVec3 pos;
	int i, dx, dy, dz;

	/* pos is centre coordinate of chunk camera is in */
	IVec3_Floor(&pos, &Camera.CurrentPos);
	pos.x = (pos.x & ~CHUNK_MASK) + HALF_CHUNK_SIZE;
	pos.y = (pos.y & ~CHUNK_MASK) + HALF_CHUNK_SIZE;
	pos.z = (pos.z & ~CHUNK_MASK) + HALF_CHUNK_SIZE;

	/* If in same chunk, don't need to recalculate sort order */
	if (pos.x == chunkPos.x && pos.y == chunkPos.y && pos.z == chunkPos.z) return;
	chunkPos = pos;
	if (!chunksCount) return;

	for (i = 0; i < chunksCount; i++) {
		info = sortedChunks[i];
		/* Calculate distance to chunk centre */
		dx = info->centreX - pos.x; dy = info->centreY - pos.y; dz = info->centreZ - pos.z;
		distances[i] = dx * dx + dy * dy + dz * dz;

		/* Consider these 3 chunks: */
		/* |       X-1      |        X        |       X+1      | */
		/* |################|########@########|################| */
		/* Assume the player is standing at @, then DrawXMin/XMax is calculated as this */
		/*    X-1: DrawXMin = false, DrawXMax = true  */
		/*    X  : DrawXMin = true,  DrawXMax = true  */
		/*    X+1: DrawXMin = true,  DrawXMax = false */

		info->drawXMin = dx >= 0; info->drawXMax = dx <= 0;
		info->drawZMin = dz >= 0; info->drawZMax = dz <= 0;
		info->drawYMin = dy >= 0; info->drawYMax = dy <= 0;
	}

	SortMapChunks(0, chunksCount - 1);
	ResetPartFlags();
	/*SimpleOcclusionCulling();*/
}

void MapRenderer_Update(float delta) {
	if (World.Type == WORLD_INFINITE) {
		if (!infActive) return;
		Inf_UpdateChunks(delta);
		return;
	}
	if (!mapChunks) return;
	UpdateSortOrder();
	UpdateChunks(delta);
}


/*########################################################################################################################*
*--------------------------------------------------Infinite renderer-----------------------------------------------------*
*#########################################################################################################################*/
/* The finite renderer above uses a fixed grid of chunks. Infinite worlds instead
   stream chunks in/out around the camera using the ChunkStore. */
/* Chunk layers above/below the camera to keep loaded while moving vertically.
   The terrain surface only ever occupies layers [INF_SURFACE_MIN_Y/16,
   INF_SURFACE_MAX_Y/16], so loading a full-height cube around the camera
   wastes time on deep solid columns that are rarely (if ever) visible. */
#define INF_VERT_SPAN 3
static struct Chunk** infSorted;
static struct Chunk** infRender;
static cc_uint32* infDist;
static int infSortedCount, infRenderCount, infCap;

static void Inf_EnsureCapacity(int count) {
	if (count <= infCap) return;
	if (infCap == 0) infCap = 64;
	while (infCap < count) infCap *= 2;
	infSorted = (struct Chunk**)Mem_Realloc(infSorted, infCap, sizeof(struct Chunk*), "inf sorted");
	infRender = (struct Chunk**)Mem_Realloc(infRender, infCap, sizeof(struct Chunk*), "inf render");
	infDist   = (cc_uint32*)Mem_Realloc(infDist, infCap, 4, "inf dist");
}

static void Inf_ResetPartOffsets(struct Chunk* chunk) {
	struct ChunkInfo* info = chunk->info;
	int i;
	if (!info) return;
	for (i = 0; i < MapRenderer_1DUsedCount; i++) {
		info->normalParts[i].offset      = -1;
		info->translucentParts[i].offset = -1;
	}
}

static void Inf_ChunkInfoInit(struct Chunk* chunk) {
	struct ChunkInfo* info;
	int i;
	info = (struct ChunkInfo*)Mem_Alloc(1, sizeof(struct ChunkInfo), "chunk info");
	chunk->info = info;

	info->centreX = (chunk->cx << CHUNK_SHIFT) + HALF_CHUNK_SIZE;
	info->centreY = (chunk->cy << CHUNK_SHIFT) + HALF_CHUNK_SIZE;
	info->centreZ = (chunk->cz << CHUNK_SHIFT) + HALF_CHUNK_SIZE;
#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
	info->vb = 0;
#endif
	info->visible = true; info->empty = false; info->allAir = false;
	info->noData = true; info->dirty = true; info->skipClip = false;
	info->drawXMin = false; info->drawXMax = false; info->drawZMin = false;
	info->drawZMax = false; info->drawYMin = false; info->drawYMax = false;

	info->normalParts = (struct ChunkPartInfo*)Mem_Alloc(MapRenderer_1DUsedCount, sizeof(struct ChunkPartInfo), "chunk parts");
	info->translucentParts = (struct ChunkPartInfo*)Mem_Alloc(MapRenderer_1DUsedCount, sizeof(struct ChunkPartInfo), "chunk parts");
	for (i = 0; i < MapRenderer_1DUsedCount; i++) {
		info->normalParts[i].offset      = -1;
		info->translucentParts[i].offset = -1;
	}
}

static void Inf_DeleteChunk(struct Chunk* chunk) {
	struct ChunkInfo* info = chunk->info;
	int i, j;
	if (!info) return;
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
	for (i = 0; i < MapRenderer_1DUsedCount; i++) {
		for (j = 0; j < CHUNKPART_MAX_VBS; j++) {
			Gfx_DeleteVb(&info->normalParts[i].vbs[j]);
			Gfx_DeleteVb(&info->translucentParts[i].vbs[j]);
		}
	}
#else
	Gfx_DeleteVb(&info->vb);
#endif
	info->empty = false; info->allAir = false;
	info->noData = true; info->dirty = true;
	Inf_ResetPartOffsets(chunk);
}

static cc_bool Inf_HasData(struct Chunk* chunk) {
	struct ChunkInfo* info = chunk->info;
	int i;
	if (!info) return false;
	for (i = 0; i < MapRenderer_1DUsedCount; i++) {
		if (info->normalParts[i].offset >= 0) return true;
		if (info->translucentParts[i].offset >= 0) return true;
	}
	return false;
}

/* Fully frees a chunk's render info: the vertex buffers plus the per-atlas
   normal/translucent parts arrays allocated by Inf_ChunkInfoInit.
   Registered as ChunkStore_FreeInfo so chunks evicted from the ChunkStore
   do not leak the parts arrays. */
static void Inf_FreeChunkInfo(struct Chunk* chunk) {
	struct ChunkInfo* info = chunk->info;
	if (!info) return;
	Inf_DeleteChunk(chunk);
	Mem_Free(info->normalParts);
	Mem_Free(info->translucentParts);
	Mem_Free(info);
	chunk->info = NULL;
}

static void Inf_SortChunks(int left, int right) {
	struct Chunk** values = infSorted; struct Chunk* value;
	cc_uint32* keys = infDist; cc_uint32 key;

	while (left < right) {
		int i = left, j = right;
		cc_uint32 pivot = keys[(i + j) >> 1];

		while (i <= j) {
			while (pivot > keys[i]) i++;
			while (pivot < keys[j]) j--;
			QuickSort_Swap_KV_Maybe();
		}
		QuickSort_Recurse(Inf_SortChunks)
	}
}

static void Inf_RenderNormal(float delta) {
	int batch, i;
	struct Chunk* chunk;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int offset, count;

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaTest(true);
	Gfx_EnableMipmaps();

	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) {
		Atlas1D_Bind(batch);
		for (i = 0; i < infRenderCount; i++) {
			chunk = infRender[i];
			info  = chunk->info;
			if (!info || !info->normalParts) continue;
			part = info->normalParts[batch];
			if (part.offset < 0) continue;

#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
			Gfx_BindVb_Textured(info->vb);
#endif
			offset  = part.offset + part.spriteCount;
			drawMin = info->drawXMin && part.counts[FACE_XMIN];
			drawMax = info->drawXMax && part.counts[FACE_XMAX];
			DrawNormalFaces(FACE_XMIN, FACE_XMAX);

			offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
			drawMin = info->drawZMin && part.counts[FACE_ZMIN];
			drawMax = info->drawZMax && part.counts[FACE_ZMAX];
			DrawNormalFaces(FACE_ZMIN, FACE_ZMAX);

			offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
			drawMin = info->drawYMin && part.counts[FACE_YMIN];
			drawMax = info->drawYMax && part.counts[FACE_YMAX];
			DrawNormalFaces(FACE_YMIN, FACE_YMAX);

			if (!part.spriteCount) continue;
			offset = part.offset;
			count  = part.spriteCount >> 2; /* 4 per sprite */

			Gfx_SetFaceCulling(true);
#if CC_GFX_BACKEND == CC_GFX_BACKEND_GL11
			Gfx_BindVb(part.vbs[FACE_COUNT]);
			DrawBatch(0, 0);
			Game_Vertices += count * 4;
			Gfx_SetFaceCulling(false);
			continue;
#endif
			if (info->drawXMax || info->drawZMin) {
				DrawBatch(count, offset); Game_Vertices += count;
			} offset += count;

			if (info->drawXMin || info->drawZMax) {
				DrawBatch(count, offset); Game_Vertices += count;
			} offset += count;

			if (info->drawXMin || info->drawZMin) {
				DrawBatch(count, offset); Game_Vertices += count;
			} offset += count;

			if (info->drawXMax || info->drawZMax) {
				DrawBatch(count, offset); Game_Vertices += count;
			}
			Gfx_SetFaceCulling(false);
		}
	}
	Gfx_DisableMipmaps();

	CheckWeather(delta);
	Gfx_SetAlphaTest(false);
}

static void Inf_RenderTranslucent(float delta) {
	int vertices, batch, i;
	struct Chunk* chunk;
	struct ChunkInfo* info;
	struct ChunkPartInfo part;
	cc_bool drawMin, drawMax;
	int offset;

	/* First fill depth buffer */
	vertices = Game_Vertices;
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaBlending(false);
	Gfx_DepthOnlyRendering(true);

	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) {
		for (i = 0; i < infRenderCount; i++) {
			chunk = infRender[i];
			info  = chunk->info;
			if (!info || !info->translucentParts) continue;
			part = info->translucentParts[batch];
			if (part.offset < 0) continue;

#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
			Gfx_BindVb_Textured(info->vb);
#endif
			offset  = part.offset;
			drawMin = (inTranslucent || info->drawXMin) && part.counts[FACE_XMIN];
			drawMax = (inTranslucent || info->drawXMax) && part.counts[FACE_XMAX];
			DrawTranslucentFaces(FACE_XMIN, FACE_XMAX);

			offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
			drawMin = (inTranslucent || info->drawZMin) && part.counts[FACE_ZMIN];
			drawMax = (inTranslucent || info->drawZMax) && part.counts[FACE_ZMAX];
			DrawTranslucentFaces(FACE_ZMIN, FACE_ZMAX);

			offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
			drawMin = (inTranslucent || info->drawYMin) && part.counts[FACE_YMIN];
			drawMax = (inTranslucent || info->drawYMax) && part.counts[FACE_YMAX];
			DrawTranslucentFaces(FACE_YMIN, FACE_YMAX);
		}
	}
	Game_Vertices = vertices;

	/* Then actually draw the translucent blocks */
	Gfx_SetAlphaBlending(true);
	Gfx_DepthOnlyRendering(false);
	Gfx_SetDepthWrite(false);

	Gfx_EnableMipmaps();
	for (batch = 0; batch < MapRenderer_1DUsedCount; batch++) {
		Atlas1D_Bind(batch);
		for (i = 0; i < infRenderCount; i++) {
			chunk = infRender[i];
			info  = chunk->info;
			if (!info || !info->translucentParts) continue;
			part = info->translucentParts[batch];
			if (part.offset < 0) continue;

#if CC_GFX_BACKEND != CC_GFX_BACKEND_GL11
			Gfx_BindVb_Textured(info->vb);
#endif
			offset  = part.offset;
			drawMin = (inTranslucent || info->drawXMin) && part.counts[FACE_XMIN];
			drawMax = (inTranslucent || info->drawXMax) && part.counts[FACE_XMAX];
			DrawTranslucentFaces(FACE_XMIN, FACE_XMAX);

			offset  += part.counts[FACE_XMIN] + part.counts[FACE_XMAX];
			drawMin = (inTranslucent || info->drawZMin) && part.counts[FACE_ZMIN];
			drawMax = (inTranslucent || info->drawZMax) && part.counts[FACE_ZMAX];
			DrawTranslucentFaces(FACE_ZMIN, FACE_ZMAX);

			offset  += part.counts[FACE_ZMIN] + part.counts[FACE_ZMAX];
			drawMin = (inTranslucent || info->drawYMin) && part.counts[FACE_YMIN];
			drawMax = (inTranslucent || info->drawYMax) && part.counts[FACE_YMAX];
			DrawTranslucentFaces(FACE_YMIN, FACE_YMAX);
		}
	}
	Gfx_DisableMipmaps();

	Gfx_SetDepthWrite(true);
	if (!inTranslucent && Env.Weather != WEATHER_SUNNY) {
		Gfx_SetAlphaTest(true);
		EnvRenderer_RenderWeather(delta);
		Gfx_SetAlphaTest(false);
	}
	Gfx_SetAlphaBlending(false);
}

static void Inf_SetDrawFlags(struct ChunkInfo* info, int camCx, int camCy, int camCz) {
	int dx = info->centreX - camCx;
	int dy = info->centreY - camCy;
	int dz = info->centreZ - camCz;
	info->drawXMin = dx >= 0; info->drawXMax = dx <= 0;
	info->drawZMin = dz >= 0; info->drawZMax = dz <= 0;
	info->drawYMin = dy >= 0; info->drawYMax = dy <= 0;
}

static void Inf_UpdateChunks(float delta) {
	int chunkUpdates = 0;
	IVec3 camPos, camChunk;
	int radius, range;
	int cx, cy, cz, i, res, dx, dy, dz, distSqr, r;
	int ox, oy, oz;
	int r2 = buildDistSquared;
	int margin = 32 * 16;
	int dyFrom, dyTo;
	int bx, by, bz;
	struct Chunk* chunk;
	struct ChunkInfo* info;
	int camCx, camCy, camCz;
	/* How many new chunks to generate per frame. */
	int genBudget = 32;
	/* Direction of movement for prioritising chunks in front of it. */
	Vec2 rot;
	Vec3 fwd;
	cc_bool hasFwd;
	int sweeps, sweep;
	float dot;

	chunksTarget += delta < CHUNK_TARGET_TIME ? 1 : -1;
	Math_Clamp(chunksTarget, 4, maxChunkUpdates);

	IVec3_Floor(&camPos, &Camera.CurrentPos);
	camChunk.x = camPos.x >> CHUNK_SHIFT;
	camChunk.y = camPos.y >> CHUNK_SHIFT;
	camChunk.z = camPos.z >> CHUNK_SHIFT;
	camCx = (camChunk.x << CHUNK_SHIFT) + HALF_CHUNK_SIZE;
	camCy = (camChunk.y << CHUNK_SHIFT) + HALF_CHUNK_SIZE;
	camCz = (camChunk.z << CHUNK_SHIFT) + HALF_CHUNK_SIZE;

	/* Stream terrain in the direction the player is actually moving, so the
	   leading edge of new terrain is generated/meshed first even when
	   strafing or walking backwards. When still, fall back to the camera
	   orientation (so turning reveals terrain ahead of it). Horizontal motion
	   only: vertical velocity (gravity, flying down) must not bias the
	   horizontally-centre streaming. The raw velocity length does not matter,
	   only the sign of the resulting dot products. */
	hasFwd = Camera.Active != NULL;
	if (hasFwd) {
		Vec3 v;
		struct LocalPlayer* p = Entities.CurPlayer;
		rot = Camera.Active->GetOrientation();
		fwd = Vec3_GetDirVector(rot.x, rot.y);
		if (p) {
			v = p->Base.Velocity;
			v.y = 0.0f;
			if (v.x * v.x + v.z * v.z > 1.0f) fwd = v;
		}
	}
	sweeps = hasFwd ? 2 : 1;

	radius = (int)(Math_SqrtF((float)buildDistSquared) / CHUNK_SIZE) + 1;
	range  = radius + 1;

	/* Only consider chunk layers that the terrain surface can occupy, plus a
	   camera-following window so flying/digging stays covered. Without this,
	   every column is loaded up to ~2*range layers tall (including deep solid
	   chunks that are the most expensive to generate and rarely visible). */
	dyFrom = min(camChunk.y - INF_VERT_SPAN, INF_SURFACE_MIN_Y >> CHUNK_SHIFT);
	dyTo   = max(camChunk.y + INF_VERT_SPAN, (INF_SURFACE_MAX_Y + (CHUNK_SIZE - 1)) >> CHUNK_SHIFT);

	/* Pass 0: unload chunks that are too far away before generating new ones,
	   so memory is freed before new allocations. ChunkStore_Remove frees the
	   render info and vertex buffers via ChunkStore_FreeInfo. */
	for (i = ChunkStore_GetCount() - 1; i >= 0; i--) {
		int ddx, ddy, ddz, ddistSqr;
		chunk = ChunkStore_GetAt(i);
		ddx = (chunk->cx << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.x;
		ddy = (chunk->cy << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.y;
		ddz = (chunk->cz << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.z;
		ddistSqr = ddx * ddx + ddy * ddy + ddz * ddz;
		if (ddistSqr >= r2 + margin) ChunkStore_Remove(chunk->cx, chunk->cy, chunk->cz);
	}

	/* Pass 1: stream in new chunks, limited to a budget per frame so spawning a
	   large area doesn't stall the game. Within each Chebyshev shell the front
	   sweep (relative to movement direction) is processed before the back sweep,
	   and generation is restricted to the build sphere so the same set of chunks
	   that would soon be unloaded is not generated in the first place. */
	for (r = 0; r <= range && genBudget > 0; r++) {
		for (sweep = 0; sweep < sweeps && genBudget > 0; sweep++) {
			for (dz = -r; dz <= r && genBudget > 0; dz++) {
				for (dy = max(-r, dyFrom); dy <= min(r, dyTo) && genBudget > 0; dy++) {
					for (dx = -r; dx <= r && genBudget > 0; dx++) {
						/* Only chunks on the surface of this Chebyshev shell. */
						if (Math_AbsI(dx) < r && Math_AbsI(dy) < r && Math_AbsI(dz) < r) continue;
						cx = camChunk.x + dx; cy = camChunk.y + dy; cz = camChunk.z + dz;
						bx = (cx << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.x;
						by = (cy << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.y;
						bz = (cz << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.z;
						distSqr = bx * bx + by * by + bz * bz;

						/* Only generate chunks within the build distance. */
						if (distSqr > r2) continue;
						if (hasFwd) {
							dot = (float)bx * fwd.x + (float)by * fwd.y + (float)bz * fwd.z;
							/* Sweep 0 handles the front hemisphere, sweep 1 the back. */
							if ((sweep == 0) != (dot >= 0.0f)) continue;
						}
						if (ChunkStore_Find(cx, cy, cz)) continue;

						chunk = ChunkStore_Get(cx, cy, cz);
						if (!chunk) continue; /* chunk store at its max size */
						if (!chunk->info) Inf_ChunkInfoInit(chunk);
						/* A new chunk appeared, so its neighbours may need rebuilding. */
						Inf_RefreshChunk(cx - 1, cy, cz);
						Inf_RefreshChunk(cx + 1, cy, cz);
						Inf_RefreshChunk(cx, cy - 1, cz);
						Inf_RefreshChunk(cx, cy + 1, cz);
						Inf_RefreshChunk(cx, cy, cz - 1);
						Inf_RefreshChunk(cx, cy, cz + 1);
						genBudget--;
					}
				}
			}
		}
	}

	/* Pass 2: build the meshes of dirty chunks within the build distance.
	   Uses the same expanding-shell, front-first order as Pass 1, since meshing
	   is the usual bottleneck and the previous cz/cy/cx scan order spent the
	   per-frame budget on arbitrary chunks instead of visible ones. */
	for (r = 0; r <= range && chunkUpdates < chunksTarget; r++) {
		for (sweep = 0; sweep < sweeps && chunkUpdates < chunksTarget; sweep++) {
			for (oz = -r; oz <= r && chunkUpdates < chunksTarget; oz++) {
				for (oy = max(-r, dyFrom); oy <= min(r, dyTo) && chunkUpdates < chunksTarget; oy++) {
					for (ox = -r; ox <= r && chunkUpdates < chunksTarget; ox++) {
						/* Only chunks on the surface of this Chebyshev shell. */
						if (Math_AbsI(ox) < r && Math_AbsI(oy) < r && Math_AbsI(oz) < r) continue;
						cx = camChunk.x + ox; cy = camChunk.y + oy; cz = camChunk.z + oz;

						dx = (cx << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.x;
						dy = (cy << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.y;
						dz = (cz << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.z;
						distSqr = dx * dx + dy * dy + dz * dz;
						if (distSqr > r2) continue;
						if (hasFwd) {
							dot = (float)dx * fwd.x + (float)dy * fwd.y + (float)dz * fwd.z;
							if ((sweep == 0) != (dot >= 0.0f)) continue;
						}

						chunk = ChunkStore_Find(cx, cy, cz);
						if (!chunk || !chunk->info) continue;

						info = chunk->info;
						if (info->dirty) {
							Inf_DeleteChunk(chunk);
							if (Builder_MakeChunk(info)) {
								info->dirty = false;
								info->empty = info->allAir || !Inf_HasData(chunk);
								info->noData = info->empty;
								chunkUpdates++;
								Game.ChunkUpdates++;
							} else {
								/* Failed to allocate vertex buffer; retry next frame. */
								info->noData = true;
								info->empty = true;
							}
						}
					}
				}
			}
		}
	}

	/* Unload chunks that are too far away. */
	for (i = ChunkStore_GetCount() - 1; i >= 0; i--) {
		int dx, dy, dz, distSqr;
		chunk = ChunkStore_GetAt(i);
		dx = (chunk->cx << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.x;
		dy = (chunk->cy << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.y;
		dz = (chunk->cz << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.z;
		distSqr = dx * dx + dy * dy + dz * dz;
		if (distSqr >= r2 + margin) {
			ChunkStore_Remove(chunk->cx, chunk->cy, chunk->cz);
		}
	}

	/* Build the sorted list of loaded chunks. */
	infSortedCount = 0;
	Inf_EnsureCapacity(ChunkStore_GetCount());
	for (i = 0; i < ChunkStore_GetCount(); i++) {
		int dx, dy, dz;
		chunk = ChunkStore_GetAt(i);
		/* Chunks can be created outside the renderer (e.g. physics/collision
		   generating terrain on demand); make sure they have render info. */
		if (!chunk->info) Inf_ChunkInfoInit(chunk);
		infSorted[infSortedCount] = chunk;
		dx = (chunk->cx << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.x;
		dy = (chunk->cy << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.y;
		dz = (chunk->cz << CHUNK_SHIFT) + HALF_CHUNK_SIZE - camPos.z;
		infDist[infSortedCount] = dx * dx + dy * dy + dz * dz;
		infSortedCount++;
	}
	Inf_SortChunks(0, infSortedCount - 1);

	/* Build the render list, with frustum culling. */
	infRenderCount = 0;
	for (i = 0; i < infSortedCount; i++) {
		chunk = infSorted[i];
		info  = chunk->info;
		if (!info || info->empty || info->noData) continue;

		res = Frustum_TestSphere(info->centreX, info->centreY, info->centreZ, 14);
		info->visible = res != FRUSTUM_OUTSIDE;
		info->skipClip = Gfx_CanSphereSkipClipping(info->centreX, info->centreY, info->centreZ, 14);
		if (!info->visible) continue;

		Inf_SetDrawFlags(info, camCx, camCy, camCz);
		infRender[infRenderCount++] = chunk;
	}
}

static void Inf_FreeAll(void) {
	int i;
	for (i = 0; i < ChunkStore_GetCount(); i++) {
		struct Chunk* chunk = ChunkStore_GetAt(i);
		if (chunk->info) {
			Inf_FreeChunkInfo(chunk);
		}
	}
	Mem_Free(infSorted);
	Mem_Free(infRender);
	Mem_Free(infDist);
	infSorted = NULL; infRender = NULL; infDist = NULL;
	infCap = 0; infSortedCount = 0; infRenderCount = 0;
}

static void Inf_RefreshAll(void) {
	int i;
	for (i = 0; i < ChunkStore_GetCount(); i++) {
		struct Chunk* chunk = ChunkStore_GetAt(i);
		if (chunk->info && !chunk->info->allAir) {
			chunk->info->empty = false;
			chunk->info->dirty = true;
		}
	}
}

static void Inf_RefreshChunk(int cx, int cy, int cz) {
	struct Chunk* chunk = ChunkStore_Find(cx, cy, cz);
	if (chunk && chunk->info) {
		chunk->info->empty = false;
		chunk->info->dirty = true;
	}
}

static void Inf_ReallocParts(void) {
	int i;
	for (i = 0; i < ChunkStore_GetCount(); i++) {
		struct Chunk* chunk = ChunkStore_GetAt(i);
		if (!chunk->info) continue;
		Inf_FreeChunkInfo(chunk);
		Inf_ChunkInfoInit(chunk);
	}
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
void MapRenderer_RefreshChunk(int cx, int cy, int cz) {
	struct ChunkInfo* chunk;
	if (World.Type == WORLD_INFINITE) {
		Inf_RefreshChunk(cx, cy, cz);
		return;
	}
	if (cx < 0 || cy < 0 || cz < 0 || cx >= World.ChunksX || cy >= World.ChunksY || cz >= World.ChunksZ) return;

	chunk = &mapChunks[World_ChunkPack(cx, cy, cz)];
	ChunkInfo_Refresh(chunk);
}

void MapRenderer_OnBlockChanged(int x, int y, int z, BlockID block) {
	int cx = x >> CHUNK_SHIFT, cy = y >> CHUNK_SHIFT, cz = z >> CHUNK_SHIFT;
	struct ChunkInfo* chunk;

	if (World.Type == WORLD_INFINITE) {
		struct Chunk* c = ChunkStore_Find(cx, cy, cz);
		if (c && c->info) c->info->allAir &= Blocks.Draw[block] == DRAW_GAS;
		Inf_RefreshChunk(cx, cy, cz);
		Inf_RefreshChunk(cx - 1, cy, cz);
		Inf_RefreshChunk(cx + 1, cy, cz);
		Inf_RefreshChunk(cx, cy - 1, cz);
		Inf_RefreshChunk(cx, cy + 1, cz);
		Inf_RefreshChunk(cx, cy, cz - 1);
		Inf_RefreshChunk(cx, cy, cz + 1);
		return;
	}

	chunk = &mapChunks[World_ChunkPack(cx, cy, cz)];
	chunk->allAir &= Blocks.Draw[block] == DRAW_GAS;
	/* TODO: Don't lookup twice, refresh directly using chunk pointer */
	ChunkInfo_Refresh(chunk);
}

static void OnEnvVariableChanged(void* obj, int envVar) {
	if (World.Type == WORLD_INFINITE) {
		if (envVar == ENV_VAR_SUN_COLOR || envVar == ENV_VAR_SHADOW_COLOR) {
			Inf_RefreshAll();
		}
		return;
	}

	if (envVar == ENV_VAR_SUN_COLOR || envVar == ENV_VAR_SHADOW_COLOR) {
		RefreshChunks();
	} else if (envVar == ENV_VAR_EDGE_HEIGHT || envVar == ENV_VAR_SIDES_OFFSET) {
		int oldClip        = Builder_EdgeLevel;
		Builder_SidesLevel = max(0, Env_SidesHeight);
		Builder_EdgeLevel  = max(0, Env.EdgeHeight);

		/* Only need to refresh chunks on map borders up to highest edge level.*/
		RefreshBorderChunks(max(oldClip, Builder_EdgeLevel));
	}
}

static void OnTerrainAtlasChanged(void* obj) {
	static int tilesPerAtlas;
	/* e.g. If old atlas was 256x256 and new is 256x256, don't need to refresh */
	if (MapRenderer_1DUsedCount && tilesPerAtlas != Atlas1D.TilesPerAtlas) {
		MapRenderer_Refresh();
	}

	MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
	if (World.Type == WORLD_INFINITE) {
		Inf_ReallocParts();
		Inf_RefreshAll();
	}
	tilesPerAtlas = Atlas1D.TilesPerAtlas;
	ResetPartFlags();
}

static void OnBlockDefinitionChanged(void* obj) {
	MapRenderer_Refresh();
	MapRenderer_1DUsedCount = MapRenderer_UsedAtlases();
	if (World.Type == WORLD_INFINITE) {
		Inf_ReallocParts();
		Inf_RefreshAll();
	}
	ResetPartFlags();
}

static void OnVisibilityChanged(void* obj) {
	lastCamPos = Vec3_BigPos();
	CalcViewDists();
}
static void DeleteChunks_(void* obj) {
	if (World.Type == WORLD_INFINITE) {
		int i;
		for (i = 0; i < ChunkStore_GetCount(); i++) {
			struct Chunk* chunk = ChunkStore_GetAt(i);
			if (chunk->info) Inf_DeleteChunk(chunk);
		}
		return;
	}
	DeleteChunks();
}
static void Refresh_(void* obj) {
	if (World.Type == WORLD_INFINITE) { Inf_RefreshAll(); return; }
	MapRenderer_Refresh();
}

static void OnNewMap(void) {
	Game.ChunkUpdates = 0;
	infActive = false;
	Inf_FreeAll();

	if (World.Type == WORLD_INFINITE) return;
	DeleteChunks();
	ResetPartCounts();

	chunkPos = IVec3_MaxValue();
	FreeChunks();
	FreeParts();
}

static void OnNewMapLoaded(void) {
	if (World.Type == WORLD_INFINITE) {
		infActive = true;
		lastCamPos = Vec3_BigPos();
		CalcViewDists();
		return;
	}

	chunksCount = World.ChunksCount;
	/* TODO: Only perform reallocation when map volume has changed */
	/*if (chunksCount != World.ChunksCount) { */
		/* chunksCount = World.ChunksCount; */
		FreeChunks();
		FreeParts();
		AllocateChunks();
		AllocateParts();
	/*}*/

	InitChunks();
	lastCamPos = Vec3_BigPos();
}

static void OnInit(void) {
	Event_Register_(&TextureEvents.AtlasChanged,  NULL, OnTerrainAtlasChanged);
	Event_Register_(&WorldEvents.EnvVarChanged,   NULL, OnEnvVariableChanged);
	Event_Register_(&BlockEvents.BlockDefChanged, NULL, OnBlockDefinitionChanged);

	Event_Register_(&GfxEvents.ViewDistanceChanged, NULL, OnVisibilityChanged);
	Event_Register_(&GfxEvents.ProjectionChanged,   NULL, OnVisibilityChanged);
	Event_Register_(&GfxEvents.ContextLost,         NULL, DeleteChunks_);
	Event_Register_(&GfxEvents.ContextRecreated,    NULL, Refresh_);

	/* ChunkStore frees render info through MapRenderer, since it owns it. */
	ChunkStore_FreeInfo = Inf_FreeChunkInfo;

	/* This = 87 fixes map being invisible when no textures */
	MapRenderer_1DUsedCount = 87; /* Atlas1D_UsedAtlasesCount(); */
	chunkPos   = IVec3_MaxValue();
	maxChunkUpdates = Options_GetInt(OPT_MAX_CHUNK_UPDATES, 4, 1024, 30);
	CalcViewDists();
}

struct IGameComponent MapRenderer_Component = {
	OnInit, /* Init */
	OnNewMap, /* Free */
	OnNewMap, /* Reset */
	OnNewMap, /* OnNewMap */
	OnNewMapLoaded /* OnNewMapLoaded */
};
