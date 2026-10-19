// chicago3ds - low-poly open-city walking demo for Nintendo 3DS (original assets)
// Top screen: 3D city. Bottom screen: live street map that tracks the player.
// Controls: Circle Pad = move, hold B = run, C-Stick / L / R = turn, D-Pad Up/Down = look,
// touch-drag = look, touch HACK LIGHTS / BLACKOUT to pull out the phone,
// SELECT = pause menu (weather, graphics, title). START quits from the title screen.

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "vshader_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define GRID      8          // 8 x 8 city blocks
#define ROAD_SP   40.0f      // distance between road centre lines
#define ROAD_HW   5.5f       // half width of the asphalt
#define SW_W      2.5f       // sidewalk width
#define SW_OFF    (ROAD_HW + SW_W * 0.5f) // pedestrians walk this far from the road centre line
#define XW        (ROAD_HW + 1.2f)        // crosswalks sit this far from the junction centre
#define LANE      2.8f       // car lane offset from the road centre line
#define BLK_H     (ROAD_SP * 0.5f - ROAD_HW)  // half size of a block (sidewalk included)
#define LOT_H     (BLK_H - SW_W)              // half size of the buildable lot
#define CITY_H    (GRID * ROAD_SP * 0.5f)     // half size of the whole city (160)
#define ROAD_END  (CITY_H + 10.0f)            // traffic and pedestrians wrap here
#define FLOOR_H   3.5f
#define MAX_VERTS 40000
#define MAX_BLD   420
#define MAX_PATCH 520
#define RAIN_N    140
#define N_DROPS   11
#define N_CARS    22
#define N_PEDS    20
#define N_STARS   70
#define ROADS     7
#define PLAYER_R  0.6f
#define TEX_N     128

typedef struct { float pos[3]; float clr[4]; float uv[2]; } Vertex;
// kind: 0 building, 1 tree, 2 fountain. deco bits: 1 rooftop unit, 2 antenna spire, 4 fountain spout
typedef struct { float x0, z0, x1, z1, h; int kind, deco; float r, g, b, uo, vo; } Building;
typedef struct { float x0, z0, x1, z1, y, r, g, b; } Patch;   // flat coloured ground rectangle

static Vertex*  vbo;
static int      vcount = 0, staticCount = 0, wallStart = 0, wallEnd = 0;
static Building bldg[MAX_BLD];
static int      nBld = 0;
static Patch    patch[MAX_PATCH];
static int      nPatch = 0;
enum { BT_TOWER, BT_LOW, BT_MID, BT_PARK, BT_PLAZA };
static int      blkType[GRID][GRID];

// quality switches (pause menu > GRAPHICS)
static int gFog = 1, gWin = 1, gFps = 1;
static int gFpsValue = 60;

// camera position used to skip far-away dynamic geometry (keeps the frame rate at 60)
static float gCamX = 0.0f, gCamZ = 0.0f, gCullR = 140.0f;
static float gFwdX = 0.0f, gFwdZ = -1.0f; // camera forward on the ground plane
static int   gCone = 1;                   // 1 = also skip things outside the view cone

static unsigned hash2(int a, int b) {
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static int inView(float x, float z) {
	float dx = x - gCamX, dz = z - gCamZ, d2 = dx * dx + dz * dz;
	if (d2 >= gCullR * gCullR) return 0;
	if (!gCone || d2 < 196.0f) return 1;                    // close objects are always kept
	return (dx * gFwdX + dz * gFwdZ) > 0.5f * sqrtf(d2) - 6.0f; // roughly +-60 degrees of the view direction
}

static float gAlpha = 1.0f; // alpha written into new vertices (rain, sky bodies, shadows, droplets)

static void addVT(float x, float y, float z, float r, float g, float b, float u, float v) {
	if (vcount >= MAX_VERTS) return;
	Vertex* p = &vbo[vcount++];
	p->pos[0] = x; p->pos[1] = y; p->pos[2] = z;
	p->clr[0] = r; p->clr[1] = g; p->clr[2] = b; p->clr[3] = gAlpha;
	p->uv[0] = u; p->uv[1] = v;
}

static void addV(float x, float y, float z, float r, float g, float b) {
	addVT(x, y, z, r, g, b, 0.0f, 0.0f);
}

static void quad(const float a[3], const float b[3], const float c[3], const float d[3],
                 float r, float g, float bl) {
	addV(a[0], a[1], a[2], r, g, bl); addV(b[0], b[1], b[2], r, g, bl); addV(c[0], c[1], c[2], r, g, bl);
	addV(a[0], a[1], a[2], r, g, bl); addV(c[0], c[1], c[2], r, g, bl); addV(d[0], d[1], d[2], r, g, bl);
}

// quad with a brightness multiplier per corner (vertical gradient = fake ambient occlusion)
static void quadShade(const float a[3], const float b[3], const float c[3], const float d[3],
                      float r, float g, float bl, float ma, float mb, float mc, float md) {
	addV(a[0], a[1], a[2], r * ma, g * ma, bl * ma);
	addV(b[0], b[1], b[2], r * mb, g * mb, bl * mb);
	addV(c[0], c[1], c[2], r * mc, g * mc, bl * mc);
	addV(a[0], a[1], a[2], r * ma, g * ma, bl * ma);
	addV(c[0], c[1], c[2], r * mc, g * mc, bl * mc);
	addV(d[0], d[1], d[2], r * md, g * md, bl * md);
}

// Box with sun-side shading per face and a bottom (lo) to top (hi) brightness gradient on the walls.
// Sun comes from +X / +Z, so those faces are bright and -Z / -X are in shade.
static void addBoxG(float x0, float z0, float x1, float z1, float y0, float y1,
                    float r, float g, float b, float lo, float hi) {
	float p000[3] = {x0, y0, z0}, p100[3] = {x1, y0, z0}, p110[3] = {x1, y1, z0}, p010[3] = {x0, y1, z0};
	float p001[3] = {x0, y0, z1}, p101[3] = {x1, y0, z1}, p111[3] = {x1, y1, z1}, p011[3] = {x0, y1, z1};
	quadShade(p000, p100, p110, p010, r * 0.55f, g * 0.55f, b * 0.55f, lo, lo, hi, hi); // -Z
	quadShade(p101, p001, p011, p111, r * 0.78f, g * 0.78f, b * 0.78f, lo, lo, hi, hi); // +Z
	quadShade(p001, p000, p010, p011, r * 0.60f, g * 0.60f, b * 0.60f, lo, lo, hi, hi); // -X
	quadShade(p100, p101, p111, p110, r * 0.88f, g * 0.88f, b * 0.88f, lo, lo, hi, hi); // +X
	quad(p010, p110, p111, p011, r, g, b);                                              // roof
}

static void addBoxY(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) {
	addBoxG(x0, z0, x1, z1, y0, y1, r, g, b, 1.0f, 1.0f);
}

static void addBox(float x0, float z0, float x1, float z1, float h, float r, float g, float b) {
	addBoxY(x0, z0, x1, z1, 0.0f, h, r, g, b);
}

// ---- window textures ---------------------------------------------------------------
// One 128x128 tile = 4 window bays x 4 floors. texDay is the facade, texNight holds only the lit
// windows (black elsewhere) and is added on top of the walls at dusk and night.
static C3D_Tex texDay, texNight;
static int texOK = 0;

static u32 pack8(int r, int g, int b) {
	r = r < 0 ? 0 : (r > 255 ? 255 : r);
	g = g < 0 ? 0 : (g > 255 ? 255 : g);
	b = b < 0 ? 0 : (b > 255 ? 255 : b);
	return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | 0xFFu;
}

static void genWindowTex(u32* img, int night) {
	for (int y = 0; y < TEX_N; y++) {
		for (int x = 0; x < TEX_N; x++) {
			int cx = x >> 5, cy = y >> 5, lx = x & 31, ly = y & 31;
			unsigned h = hash2(cx * 7 + 1, cy * 13 + 2);
			int lit = (h % 100) < 42;
			int inWin = (lx >= 7 && lx < 25 && ly >= 6 && ly < 26);
			int noise = (int)(hash2(x, y * 3 + 5) & 7) - 3;
			u32 px;
			if (night) {
				if (inWin && lit) {
					int kind = (int)((h >> 8) & 3);
					if (kind == 2)      px = pack8(190 + noise, 215 + noise, 255);
					else if (kind == 1) px = pack8(255, 232 + noise, 175);
					else                px = pack8(255, 206 + noise, 120);
				} else px = pack8(0, 0, 0);
			} else if (inWin) {
				int t = ly - 6; // 0..19, glass is lighter at the top (sky reflection)
				int rr = 72 - t * 2 + noise, gg = 92 - t * 2 + noise, bb = 118 - t * 2 + noise;
				if (((h >> 4) % 3) == 0 && (lx - 7) + (ly - 6) < 10) { rr += 28; gg += 28; bb += 28; }
				px = pack8(rr, gg, bb);
			} else if (lx == 6 || lx == 25 || ly == 5 || ly == 26) {
				px = pack8(206, 206, 201);
			} else if (ly >= 28) {
				px = pack8(212 + noise, 210 + noise, 205 + noise);
			} else {
				px = pack8(238 + noise, 236 + noise, 230 + noise);
			}
			img[y * TEX_N + x] = px;
		}
	}
}

// PICA200 textures are stored in 8x8 tiles, Z-order inside each tile, starting from the bottom row.
static void swizzle(u32* dst, const u32* src, int w, int h) {
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int tile = (y >> 3) * (w >> 3) + (x >> 3);
			int xi = x & 7, yi = y & 7;
			int m = (xi & 1) | ((yi & 1) << 1) | ((xi & 2) << 1) | ((yi & 2) << 2) | ((xi & 4) << 2) | ((yi & 4) << 3);
			dst[tile * 64 + m] = src[(h - 1 - y) * w + x];
		}
	}
}

static int initWindowTex(void) {
	u32* img = (u32*)malloc(TEX_N * TEX_N * 4);
	u32* sw  = (u32*)malloc(TEX_N * TEX_N * 4);
	int ok = (img && sw);
	for (int n = 0; ok && n < 2; n++) {
		C3D_Tex* t = n ? &texNight : &texDay;
		if (!C3D_TexInitMipmap(t, TEX_N, TEX_N, GPU_RGBA8)) { ok = 0; break; }
		genWindowTex(img, n);
		swizzle(sw, img, TEX_N, TEX_N);
		C3D_TexUpload(t, sw);
		C3D_TexGenerateMipmap(t, GPU_TEXFACE_2D);
		C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
		C3D_TexSetFilterMipmap(t, GPU_LINEAR);
		C3D_TexSetWrap(t, GPU_REPEAT, GPU_REPEAT);
	}
	free(img); free(sw);
	return ok;
}

// ---- city -------------------------------------------------------------------------
// Roads run along x = ROAD_C(k) and z = ROAD_C(k), k = 0..6 (40 m apart). Intersections are (a, b).
// Between the roads lie 8 x 8 city blocks. Every block is one of: glass tower on a plaza, low-rise
// brick houses with gaps between them, a mid-rise with a car park, a park (grass, paths, trees, pond)
// or a fountain plaza - so the streets open up instead of forming one wall of buildings.
#define ROAD_C(k) (-(float)(ROADS - 1) * 0.5f * ROAD_SP + ROAD_SP * (float)(k))

// Textured facade: 4 walls, tile = 16 m wide x 14 m tall, shifted per building so no two look alike.
static void wallFace(const float a[3], const float b[3], const float c[3], const float d[3],
                     float r, float g, float bl, float u0, float u1, float v0, float v1) {
	const float lo = 0.55f, hi = 1.0f;
	addVT(a[0], a[1], a[2], r * lo, g * lo, bl * lo, u0, v0);
	addVT(b[0], b[1], b[2], r * lo, g * lo, bl * lo, u1, v0);
	addVT(c[0], c[1], c[2], r * hi, g * hi, bl * hi, u1, v1);
	addVT(a[0], a[1], a[2], r * lo, g * lo, bl * lo, u0, v0);
	addVT(c[0], c[1], c[2], r * hi, g * hi, bl * hi, u1, v1);
	addVT(d[0], d[1], d[2], r * hi, g * hi, bl * hi, u0, v1);
}

static void addWalls(float x0, float z0, float x1, float z1, float h, float r, float g, float b,
                     float uo, float vo) {
	float p000[3] = {x0, 0, z0}, p100[3] = {x1, 0, z0}, p110[3] = {x1, h, z0}, p010[3] = {x0, h, z0};
	float p001[3] = {x0, 0, z1}, p101[3] = {x1, 0, z1}, p111[3] = {x1, h, z1}, p011[3] = {x0, h, z1};
	float u = (x1 - x0) / 16.0f, w = (z1 - z0) / 16.0f, v = h / (4.0f * FLOOR_H);
	wallFace(p000, p100, p110, p010, r * 0.55f, g * 0.55f, b * 0.55f, uo,         uo + u,         vo, vo + v);
	wallFace(p101, p001, p011, p111, r * 0.78f, g * 0.78f, b * 0.78f, uo + 0.25f, uo + 0.25f + u, vo, vo + v);
	wallFace(p001, p000, p010, p011, r * 0.60f, g * 0.60f, b * 0.60f, uo + 0.50f, uo + 0.50f + w, vo, vo + v);
	wallFace(p100, p101, p111, p110, r * 0.88f, g * 0.88f, b * 0.88f, uo + 0.75f, uo + 0.75f + w, vo, vo + v);
}

#define CELLS 16 // 4x4 districts of 2x2 blocks; each gets its own vertex ranges so it can be skipped when out of view
#define CELL_W (CITY_H * 0.5f)
static int cellAStart[CELLS], cellACount[CELLS], cellWStart[CELLS], cellWCount[CELLS];
static int globalEnd = 0; // ground + skyline, always drawn

static int cellOf(float x, float z) {
	int ix = (int)floorf((x + CITY_H) / CELL_W), iz = (int)floorf((z + CITY_H) / CELL_W);
	ix = ix < 0 ? 0 : (ix > 3 ? 3 : ix);
	iz = iz < 0 ? 0 : (iz > 3 ? 3 : iz);
	return iz * 4 + ix;
}

static unsigned lrnd(unsigned* s) { // tiny LCG for layout decisions
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

static void addP(float x0, float z0, float x1, float z1, float y, float r, float g, float b) {
	if (nPatch >= MAX_PATCH) return;
	Patch* p = &patch[nPatch++];
	p->x0 = x0; p->z0 = z0; p->x1 = x1; p->z1 = z1; p->y = y; p->r = r; p->g = g; p->b = b;
}

static Building* addB(int kind, float x0, float z0, float x1, float z1, float h, float r, float g, float b, unsigned hs) {
	if (nBld >= MAX_BLD) return NULL;
	Building* p = &bldg[nBld++];
	p->x0 = x0; p->z0 = z0; p->x1 = x1; p->z1 = z1; p->h = h; p->kind = kind; p->deco = 0;
	p->r = r; p->g = g; p->b = b;
	p->uo = (float)((hs >> 20) & 3) * 0.25f; p->vo = (float)((hs >> 24) & 3) * 0.25f;
	return p;
}

static void addTree(float x, float z, unsigned hs) {
	static const float leaf[4][3] = {{0.16f, 0.42f, 0.18f}, {0.22f, 0.50f, 0.22f}, {0.12f, 0.34f, 0.20f}, {0.30f, 0.52f, 0.20f}};
	float top = 4.6f + (float)(hs % 25) * 0.1f;
	int roll = (int)((hs >> 8) % 100);
	float r, g, b;
	if (roll < 12)      { r = 0.86f; g = 0.44f; b = 0.12f; } // October: a few orange maples
	else if (roll < 20) { r = 0.86f; g = 0.72f; b = 0.20f; } // and some yellow ones
	else { const float* l = leaf[(hs >> 12) & 3]; r = l[0]; g = l[1]; b = l[2]; }
	Building* t = addB(1, x - 0.3f, z - 0.3f, x + 0.3f, z + 0.3f, top, r, g, b, hs);
	if (t && ((hs >> 16) & 1)) t->deco = 8; // round (diamond) crown instead of a cone
}

static float dtFactor(float bx, float bz) { // 1 in the Loop, fading to 0 far away: taller buildings downtown
	float dx = bx - 40.0f, dz = bz - 100.0f, v = 1.0f - sqrtf(dx * dx + dz * dz) / 170.0f;
	return v < 0.0f ? 0.0f : v;
}

static int pickBlock(int i, int j) {
	float bx = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)i, bz = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)j;
	int r = (int)(hash2(i * 31 + 7, j * 17 + 3) % 100);
	if (i >= 6 && j >= 5) return r < 72 ? BT_PARK : BT_PLAZA;           // Grant Park by the lake
	if (i >= 2 && i <= 3 && j >= 2 && j <= 3 && r < 62) return BT_PARK; // Lincoln Park
	float dt = dtFactor(bx, bz);
	if (dt > 0.45f) {                                                    // downtown
		if (r < 62) return BT_TOWER;
		if (r < 70) return BT_PLAZA;
		if (r < 88) return BT_MID;
		return BT_LOW;
	}
	if (r < 16) return BT_PARK;
	if (r < 24) return BT_PLAZA;
	if (r < 32) return BT_TOWER;
	if (r < 54) return BT_MID;
	return BT_LOW;
}

static void layoutBlock(int i, int j) {
	const float bx = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)i, bz = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)j;
	unsigned st = hash2(i * 131 + 5, j * 71 + 9);
	const float L = LOT_H;
	float dt = dtFactor(bx, bz);
	addP(bx - BLK_H, bz - BLK_H, bx + BLK_H, bz + BLK_H, 0.04f, 0.47f, 0.47f, 0.49f); // sidewalk all around
	switch (blkType[i][j]) {
	case BT_TOWER: {
		static const float glass[5][3] = {{0.55f, 0.68f, 0.84f}, {0.40f, 0.48f, 0.60f}, {0.70f, 0.74f, 0.80f}, {0.36f, 0.46f, 0.58f}, {0.62f, 0.68f, 0.66f}};
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.55f, 0.55f, 0.53f);
		float w = 11.0f + (float)(lrnd(&st) % 5), d = 11.0f + (float)(lrnd(&st) % 5);
		float ox = ((int)(lrnd(&st) % 5) - 2) * 0.7f, oz = ((int)(lrnd(&st) % 5) - 2) * 0.7f;
		int nf = 9 + (int)(dt * 16.0f) + (int)(lrnd(&st) % 8);
		if (nf > 30) nf = 30;
		const float* c = glass[lrnd(&st) % 5];
		Building* b = addB(0, bx + ox - w * 0.5f, bz + oz - d * 0.5f, bx + ox + w * 0.5f, bz + oz + d * 0.5f, (float)nf * FLOOR_H, c[0], c[1], c[2], st);
		if (b) b->deco = 1 | (nf >= 24 ? 2 : 0);
		for (int k = 0; k < 4; k++)
			if (lrnd(&st) % 100 < 50)
				addTree(bx + ((k & 1) ? 10.6f : -10.6f) + (float)((int)(lrnd(&st) % 3) - 1) * 0.4f,
				        bz + ((k & 2) ? 10.6f : -10.6f) + (float)((int)(lrnd(&st) % 3) - 1) * 0.4f, lrnd(&st));
		break;
	}
	case BT_LOW: {
		static const float brick[5][3] = {{0.68f, 0.38f, 0.30f}, {0.72f, 0.56f, 0.40f}, {0.60f, 0.54f, 0.50f}, {0.64f, 0.30f, 0.26f}, {0.78f, 0.70f, 0.56f}};
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.40f, 0.40f, 0.41f);
		for (int s = 0; s < 4; s++) {
			float sx = bx + ((s & 1) ? 6.3f : -6.3f), sz = bz + ((s & 2) ? 6.3f : -6.3f);
			if (lrnd(&st) % 100 < 78) {
				float w = 8.0f + (float)(lrnd(&st) % 3), d = 8.0f + (float)(lrnd(&st) % 3);
				float jx = ((int)(lrnd(&st) % 3) - 1) * 0.5f, jz = ((int)(lrnd(&st) % 3) - 1) * 0.5f;
				int nf = 2 + (int)(lrnd(&st) % 4) + (int)(dt * 3.0f);
				const float* c = brick[lrnd(&st) % 5];
				Building* b = addB(0, sx + jx - w * 0.5f, sz + jz - d * 0.5f, sx + jx + w * 0.5f, sz + jz + d * 0.5f, (float)nf * FLOOR_H, c[0], c[1], c[2], st);
				if (b && lrnd(&st) % 3 == 0) b->deco = 1;
			} else {
				addTree(sx, sz, lrnd(&st));
			}
		}
		break;
	}
	case BT_MID: {
		static const float stone[4][3] = {{0.76f, 0.72f, 0.64f}, {0.66f, 0.62f, 0.58f}, {0.58f, 0.60f, 0.66f}, {0.72f, 0.60f, 0.50f}};
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.42f, 0.42f, 0.43f);
		int horiz = (int)(lrnd(&st) & 1);
		float len = 20.0f + (float)(lrnd(&st) % 3), dep = 8.0f + (float)(lrnd(&st) % 3);
		int nf = 4 + (int)(dt * 6.0f) + (int)(lrnd(&st) % 5);
		const float* c = stone[lrnd(&st) % 4];
		float sgn = (lrnd(&st) & 1) ? 1.0f : -1.0f;      // which side of the block the building hugs
		float a0 = sgn * (L - 0.5f), a1 = sgn * (L - 0.5f - dep);
		float lo = a0 < a1 ? a0 : a1, hi = a0 < a1 ? a1 : a0;
		Building* b;
		if (horiz) b = addB(0, bx - len * 0.5f, bz + lo, bx + len * 0.5f, bz + hi, (float)nf * FLOOR_H, c[0], c[1], c[2], st);
		else       b = addB(0, bx + lo, bz - len * 0.5f, bx + hi, bz + len * 0.5f, (float)nf * FLOOR_H, c[0], c[1], c[2], st);
		if (b) b->deco = 1;
		// the other half of the lot: a car park with a few trees
		float p0 = -sgn * 1.5f, p1 = -sgn * (L - 0.5f);
		float plo = p0 < p1 ? p0 : p1, phi = p0 < p1 ? p1 : p0;
		if (horiz) addP(bx - L + 1.0f, bz + plo, bx + L - 1.0f, bz + phi, 0.08f, 0.17f, 0.17f, 0.19f);
		else       addP(bx + plo, bz - L + 1.0f, bx + phi, bz + L - 1.0f, 0.08f, 0.17f, 0.17f, 0.19f);
		for (int k = 0; k < 2; k++) {
			float t = (-6.0f + 12.0f * (float)k), n = -sgn * (L - 1.5f);
			addTree(horiz ? bx + t : bx + n, horiz ? bz + n : bz + t, lrnd(&st));
		}
		break;
	}
	case BT_PARK: {
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.20f, 0.40f, 0.20f);
		int pond = (lrnd(&st) % 3 == 0);
		if (pond) addP(bx - 5.5f, bz - 4.0f, bx + 5.5f, bz + 4.0f, 0.07f, 0.14f, 0.32f, 0.50f);
		addP(bx - 1.0f, bz - L, bx + 1.0f, bz + L, 0.08f, 0.60f, 0.56f, 0.48f); // paths cross in the middle (a bridge over the pond)
		addP(bx - L, bz - 1.0f, bx + L, bz + 1.0f, 0.08f, 0.60f, 0.56f, 0.48f);
		float pvx = 0.0f, pvz = 0.0f;
		int pav = (lrnd(&st) % 4 == 0 && !pond);
		if (pav) {
			pvx = bx + ((lrnd(&st) & 1) ? 7.5f : -7.5f); pvz = bz + ((lrnd(&st) & 1) ? 7.5f : -7.5f);
			addB(0, pvx - 2.2f, pvz - 2.2f, pvx + 2.2f, pvz + 2.2f, 3.4f, 0.86f, 0.84f, 0.78f, st);
		}
		int placed = 0;
		for (int tries = 0; tries < 40 && placed < 6; tries++) {
			float x = bx + (float)((int)(lrnd(&st) % 23) - 11), z = bz + (float)((int)(lrnd(&st) % 23) - 11);
			if (fabsf(x - bx) < 2.8f || fabsf(z - bz) < 2.8f) continue;
			if (pond && fabsf(x - bx) < 7.0f && fabsf(z - bz) < 5.5f) continue;
			if (pav && fabsf(x - pvx) < 4.0f && fabsf(z - pvz) < 4.0f) continue;
			addTree(x, z, lrnd(&st));
			placed++;
		}
		break;
	}
	default: { // BT_PLAZA
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.62f, 0.60f, 0.56f);
		addP(bx - 8.0f, bz - 8.0f, bx + 8.0f, bz + 8.0f, 0.07f, 0.50f, 0.48f, 0.46f);
		Building* f = addB(2, bx - 3.2f, bz - 3.2f, bx + 3.2f, bz + 3.2f, 0.8f, 0.45f, 0.62f, 0.78f, st);
		if (f) f->deco = 4;
		for (int k = 0; k < 4; k++)
			if (k == 0 || k == 3 || (lrnd(&st) & 1))
				addTree(bx + ((k & 1) ? 10.4f : -10.4f), bz + ((k & 2) ? 10.4f : -10.4f), lrnd(&st));
		if (lrnd(&st) & 1) addB(0, bx - 2.2f, bz + 8.5f, bx + 2.2f, bz + 11.0f, 3.2f, 0.80f, 0.74f, 0.62f, st); // kiosk
		break;
	}
	}
}

// Thin post drawn as two crossed quads (12 vertices instead of a 30-vertex box).
static void addPost(float cx, float cz, float hw, float y0, float y1, float r, float g, float b) {
	float a0[3] = {cx - hw, y0, cz}, a1[3] = {cx + hw, y0, cz}, a2[3] = {cx + hw, y1, cz}, a3[3] = {cx - hw, y1, cz};
	quad(a0, a1, a2, a3, r, g, b);
	float c0[3] = {cx, y0, cz - hw}, c1[3] = {cx, y0, cz + hw}, c2[3] = {cx, y1, cz + hw}, c3[3] = {cx, y1, cz - hw};
	quad(c0, c1, c2, c3, r * 0.8f, g * 0.8f, b * 0.8f);
}

static void tri(const float a[3], const float b[3], const float c[3], float r, float g, float bl) {
	addV(a[0], a[1], a[2], r, g, bl); addV(b[0], b[1], b[2], r, g, bl); addV(c[0], c[1], c[2], r, g, bl);
}

// Low-poly tree: square trunk plus a cone (or a diamond) crown.
static void addTreeGeom(const Building* t) {
	float cx = (t->x0 + t->x1) * 0.5f, cz = (t->z0 + t->z1) * 0.5f, top = t->h;
	unsigned hh = hash2((int)(cx * 10.0f), (int)(cz * 10.0f));
	float rad = 1.3f + (float)(hh % 7) * 0.12f, base = 1.9f;
	addPost(cx, cz, 0.22f, 0.0f, base + 0.6f, 0.36f, 0.26f, 0.18f);
	float px[4][3] = {{cx - rad, base, cz}, {cx, base, cz - rad}, {cx + rad, base, cz}, {cx, base, cz + rad}};
	const float shade[4] = {0.62f, 0.60f, 0.95f, 0.80f};
	float apex[3] = {cx, top, cz};
	if (t->deco & 8) { // diamond crown: widest point part-way up, a point below as well
		float mid = base + (top - base) * 0.45f, low[3] = {cx, base - 0.15f, cz};
		for (int k = 0; k < 4; k++) {
			float a[3] = {px[k][0], mid, px[k][2]}, b[3] = {px[(k + 1) & 3][0], mid, px[(k + 1) & 3][2]};
			tri(a, b, apex, t->r * shade[k], t->g * shade[k], t->b * shade[k]);
			tri(a, b, low, t->r * shade[k] * 0.8f, t->g * shade[k] * 0.8f, t->b * shade[k] * 0.8f);
		}
	} else {
		for (int k = 0; k < 4; k++)
			tri(px[k], px[(k + 1) & 3], apex, t->r * shade[k] * 0.9f, t->g * shade[k] * 0.9f, t->b * shade[k] * 0.9f);
	}
}

static void buildCity(void) {
	nBld = 0; nPatch = 0;
	for (int i = 0; i < GRID; i++) for (int j = 0; j < GRID; j++) blkType[i][j] = pickBlock(i, j);
	for (int i = 0; i < GRID; i++) for (int j = 0; j < GRID; j++) layoutBlock(i, j);

	// ground outside the city, in 40 m tiles (fog is computed per vertex, so big quads would look washed out).
	// East of the city is Lake Michigan.
	const float T = 40.0f, GE = 320.0f, GF = 1200.0f;
	for (float x = -GE; x < GE - 0.1f; x += T) {
		for (float z = -GE; z < GE - 0.1f; z += T) {
			float cx = x + T * 0.5f, cz = z + T * 0.5f;
			if (fabsf(cx) < CITY_H && fabsf(cz) < CITY_H) continue; // the city itself covers this
			int water = cx > CITY_H + 40.0f;
			float r = water ? 0.12f : 0.34f, g = water ? 0.30f : 0.35f, b = water ? 0.50f : 0.36f;
			float a0[3] = {x, 0, z}, a1[3] = {x + T, 0, z}, a2[3] = {x + T, 0, z + T}, a3[3] = {x, 0, z + T};
			quad(a0, a3, a2, a1, r, g, b);
		}
	}
	{ // far away, completely fogged: four big slabs around the tiled area
		float s0[3] = {-GF, 0, -GF}, s1[3] = {GF, 0, -GF}, s2[3] = {GF, 0, -GE}, s3[3] = {-GF, 0, -GE};
		quad(s0, s3, s2, s1, 0.34f, 0.35f, 0.36f);
		float n0[3] = {-GF, 0, GE}, n1[3] = {GF, 0, GE}, n2[3] = {GF, 0, GF}, n3[3] = {-GF, 0, GF};
		quad(n0, n3, n2, n1, 0.34f, 0.35f, 0.36f);
		float w0[3] = {-GF, 0, -GE}, w1[3] = {-GE, 0, -GE}, w2[3] = {-GE, 0, GE}, w3[3] = {-GF, 0, GE};
		quad(w0, w3, w2, w1, 0.34f, 0.35f, 0.36f);
		float e0[3] = {GE, 0, -GE}, e1[3] = {GF, 0, -GE}, e2[3] = {GF, 0, GE}, e3[3] = {GE, 0, GE};
		quad(e0, e3, e2, e1, 0.12f, 0.30f, 0.50f);
	}
	// asphalt, in 20 m pieces for the same reason
	for (int k = 0; k < ROADS; k++) {
		float c0 = ROAD_C(k);
		for (float s = -ROAD_END; s < ROAD_END - 0.1f; s += 20.0f) {
			float v0[3] = {c0 - ROAD_HW, 0.02f, s}, v1[3] = {c0 + ROAD_HW, 0.02f, s};
			float v2[3] = {c0 + ROAD_HW, 0.02f, s + 20.0f}, v3[3] = {c0 - ROAD_HW, 0.02f, s + 20.0f};
			quad(v0, v3, v2, v1, 0.17f, 0.18f, 0.20f);
			float h0[3] = {s, 0.02f, c0 - ROAD_HW}, h1[3] = {s, 0.02f, c0 + ROAD_HW};
			float h2[3] = {s + 20.0f, 0.02f, c0 + ROAD_HW}, h3[3] = {s + 20.0f, 0.02f, c0 - ROAD_HW};
			quad(h0, h3, h2, h1, 0.17f, 0.18f, 0.20f);
		}
	}

	// distant skyline ring (never reachable, heavily fogged); none over the lake
	for (int i = 0; i < 56; i++) {
		unsigned h = hash2(i, 31337);
		float ang = (float)i * 6.2832f / 56.0f + (float)(h & 15) * 0.004f;
		float rad = 235.0f + (float)((h >> 4) % 60);
		float w   = 9.0f + (float)((h >> 10) % 11);
		float hh  = 25.0f + (float)((h >> 14) % 85);
		float cx = sinf(ang) * rad, cz = cosf(ang) * rad;
		if (cx > 150.0f) continue;
		float tone = 0.42f + (float)((h >> 20) % 20) / 100.0f;
		addBoxG(cx - w, cz - w, cx + w, cz + w, 0.0f, hh, tone * 0.95f, tone, tone * 1.08f, 0.5f, 1.0f);
	}
	globalEnd = vcount;

	// per district: sidewalks and lots, roofs, trees, road markings, crossings, signal poles
	for (int c = 0; c < CELLS; c++) {
		cellAStart[c] = vcount;
		for (int n = 0; n < nPatch; n++) {
			const Patch* p = &patch[n];
			if (cellOf((p->x0 + p->x1) * 0.5f, (p->z0 + p->z1) * 0.5f) != c) continue;
			float s0[3] = {p->x0, p->y, p->z0}, s1[3] = {p->x1, p->y, p->z0};
			float s2[3] = {p->x1, p->y, p->z1}, s3[3] = {p->x0, p->y, p->z1};
			quad(s0, s3, s2, s1, p->r, p->g, p->b);
		}
		for (int n = 0; n < nBld; n++) {
			const Building* b = &bldg[n];
			float cx = (b->x0 + b->x1) * 0.5f, cz = (b->z0 + b->z1) * 0.5f;
			if (cellOf(cx, cz) != c) continue;
			if (b->kind == 1) { addTreeGeom(b); continue; }
			if (b->kind == 2) { // fountain: basin with a spout
				addBoxY(b->x0, b->z0, b->x1, b->z1, 0.0f, b->h, b->r, b->g, b->b);
				addBoxY(cx - 0.3f, cz - 0.3f, cx + 0.3f, cz + 0.3f, b->h, b->h + 2.6f, 0.75f, 0.88f, 1.0f);
				continue;
			}
			float rr = b->r * 0.42f, rg = b->g * 0.42f, rb = b->b * 0.42f;
			float r0[3] = {b->x0, b->h, b->z0}, r1[3] = {b->x1, b->h, b->z0};
			float r2[3] = {b->x1, b->h, b->z1}, r3[3] = {b->x0, b->h, b->z1};
			quad(r0, r1, r2, r3, rr + 0.08f, rg + 0.08f, rb + 0.08f);
			float hw = (b->x1 - b->x0) * 0.5f, hd = (b->z1 - b->z0) * 0.5f;
			unsigned h = hash2((int)(cx * 3.0f), (int)(cz * 3.0f));
			if (b->deco & 1) {
				float uw = hw < hd ? hw : hd;
				uw = uw * 0.22f; if (uw > 1.5f) uw = 1.5f;
				float ux = cx + ((int)((h >> 3) % 9) - 4) * hw * 0.09f, uz = cz + ((int)((h >> 7) % 9) - 4) * hd * 0.09f;
				addBoxG(ux - uw, uz - uw, ux + uw, uz + uw, b->h, b->h + 1.2f, 0.52f, 0.54f, 0.57f, 0.8f, 1.0f);
			}
			if (b->deco & 2) { // antenna mast on the tallest towers
				float ah = 7.0f + (float)((h >> 12) % 6);
				addBoxG(cx - 0.18f, cz - 0.18f, cx + 0.18f, cz + 0.18f, b->h + 1.0f, b->h + 1.0f + ah, 0.30f, 0.30f, 0.33f, 0.9f, 1.0f);
			}
		}
		for (int k = 0; k < ROADS; k++) {
			float c0 = ROAD_C(k);
			for (float s = -CITY_H; s < CITY_H; s += 7.0f) {
				float m = s + 1.5f;
				int skip = 0;
				for (int q = 0; q < ROADS; q++) if (fabsf(m - ROAD_C(q)) < XW + 2.2f) skip = 1;
				if (skip) continue;
				if (cellOf(c0, m) == c) { // dash on the road running along z
					float a[3] = {c0 - 0.14f, 0.05f, s}, b[3] = {c0 + 0.14f, 0.05f, s};
					float cc[3] = {c0 + 0.14f, 0.05f, s + 3.0f}, d[3] = {c0 - 0.14f, 0.05f, s + 3.0f};
					quad(a, b, cc, d, 0.85f, 0.72f, 0.16f);
				}
				if (cellOf(m, c0) == c) { // dash on the road running along x
					float e[3] = {s, 0.05f, c0 - 0.14f}, f[3] = {s, 0.05f, c0 + 0.14f};
					float g[3] = {s + 3.0f, 0.05f, c0 + 0.14f}, hh[3] = {s + 3.0f, 0.05f, c0 - 0.14f};
					quad(e, f, g, hh, 0.85f, 0.72f, 0.16f);
				}
			}
		}
		for (int a = 0; a < ROADS; a++) {
			for (int b = 0; b < ROADS; b++) {
				float xc = ROAD_C(a), zc = ROAD_C(b);
				if (cellOf(xc, zc) != c) continue;
				for (int arm = 0; arm < 4; arm++) {
					float sg = (arm & 1) ? 1.0f : -1.0f;
					for (int st = -1; st <= 1; st++) {
						float off = st * 3.6f;
						float p0[3], p1[3], p2[3], p3[3];
						if (arm < 2) { // crosswalk over the vertical road: bars run along z
							float zz = zc + sg * XW, xx = xc + off;
							p0[0] = xx - 0.8f; p0[1] = 0.05f; p0[2] = zz - 1.2f;
							p1[0] = xx + 0.8f; p1[1] = 0.05f; p1[2] = zz - 1.2f;
							p2[0] = xx + 0.8f; p2[1] = 0.05f; p2[2] = zz + 1.2f;
							p3[0] = xx - 0.8f; p3[1] = 0.05f; p3[2] = zz + 1.2f;
						} else {       // crosswalk over the horizontal road: bars run along x
							float xx = xc + sg * XW, zz = zc + off;
							p0[0] = xx - 1.2f; p0[1] = 0.05f; p0[2] = zz - 0.8f;
							p1[0] = xx + 1.2f; p1[1] = 0.05f; p1[2] = zz - 0.8f;
							p2[0] = xx + 1.2f; p2[1] = 0.05f; p2[2] = zz + 0.8f;
							p3[0] = xx - 1.2f; p3[1] = 0.05f; p3[2] = zz + 0.8f;
						}
						quad(p0, p1, p2, p3, 0.78f, 0.78f, 0.76f);
					}
				}
				float x = xc + SW_OFF + 0.7f, z = zc + SW_OFF + 0.7f;
				addPost(x, z, 0.16f, 0.0f, 4.0f, 0.2f, 0.2f, 0.22f);
			}
		}
		cellACount[c] = vcount - cellAStart[c];
	}

	// textured facades go last (per district) so they can be drawn, and re-drawn for night glow, as ranges
	wallStart = vcount;
	for (int c = 0; c < CELLS; c++) {
		cellWStart[c] = vcount;
		for (int n = 0; n < nBld; n++) {
			const Building* b = &bldg[n];
			if (b->kind != 0) continue;
			if (cellOf((b->x0 + b->x1) * 0.5f, (b->z0 + b->z1) * 0.5f) != c) continue;
			addWalls(b->x0, b->z0, b->x1, b->z1, b->h, b->r, b->g, b->b, b->uo, b->vo);
		}
		cellWCount[c] = vcount - cellWStart[c];
	}
	wallEnd = vcount;
}

// ---- audio ------------------------------------------------------------------
// Sound effects and ambience loops are synthesised at start-up (no sample files). The title music is
// streamed from romfs:/title.pcm (32 kHz stereo) by a background thread so it never stalls a frame.
// Needs the DSP firmware dump at sdmc:/3ds/dspfirm.cdc (dump it once with the DSP1 homebrew);
// without it the game simply stays silent.
#define SFX_RATE 22050
#define MUS_RATE 32000
#define PI_F     3.14159265f
enum { SFX_PISTOL, SFX_SMG, SFX_SHOTGUN, SFX_ROCKET, SFX_EXPLODE, SFX_HIT, SFX_CLICK, SFX_SWITCH,
       SFX_PICKUP, SFX_HORN, SFX_STEP, SFX_SIREN, SFX_COUNT };
typedef struct { s16* data; int n; } Snd;
static Snd sfx[SFX_COUNT], ambSnd[4]; // ambience: 0 city, 1 rain, 2 night crickets, 3 daytime birds
static int sndOK = 0;
static ndspWaveBuf sfxWB[8], ambWB[4];
static int sfxNext = 0;
static float ambSet[4] = {-1.0f, -1.0f, -1.0f, -1.0f};

static float nrand(unsigned* st) {
	*st = *st * 1664525u + 1013904223u;
	return (float)((*st >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

static s16 pcm(float v) {
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	return (s16)(v * 30000.0f);
}

static Snd newSnd(int n) {
	Snd s;
	s.n = n;
	s.data = (s16*)linearAlloc((size_t)n * 2);
	if (!s.data) s.n = 0;
	return s;
}

static Snd synthSfx(int id) {
	static const float dur[SFX_COUNT] = {0.22f, 0.13f, 0.55f, 0.90f, 1.40f, 0.12f, 0.05f, 0.12f, 0.25f, 0.55f, 0.09f, 3.60f};
	int n = (int)(dur[id] * SFX_RATE);
	Snd s = newSnd(n);
	if (!s.data) return s;
	unsigned st = 12345u + (unsigned)id * 977u;
	float lp = 0.0f, lp2 = 0.0f, ph = 0.0f;
	for (int i = 0; i < n; i++) {
		float t = (float)i / SFX_RATE, v = 0.0f, w = nrand(&st);
		lp  += (w - lp)  * 0.30f;
		lp2 += (w - lp2) * 0.06f;
		switch (id) {
		case SFX_PISTOL:
			v = 0.7f * w * expf(-t * 38.0f) + 0.5f * lp * expf(-t * 15.0f) + 0.6f * sinf(2.0f * PI_F * 140.0f * t) * expf(-t * 26.0f);
			break;
		case SFX_SMG:
			v = 0.8f * w * expf(-t * 50.0f) + 0.5f * sinf(2.0f * PI_F * 190.0f * t) * expf(-t * 42.0f);
			break;
		case SFX_SHOTGUN:
			v = 0.9f * lp * expf(-t * 9.0f) + 0.5f * w * expf(-t * 30.0f) + 0.8f * sinf(2.0f * PI_F * 68.0f * t) * expf(-t * 8.0f);
			break;
		case SFX_ROCKET: {
			float e = sinf(PI_F * t / dur[id]);
			v = (0.8f * lp2 + 0.25f * lp) * powf(e, 0.7f) * 2.0f;
			break;
		}
		case SFX_EXPLODE:
			v = lp2 * 3.0f * expf(-t * 2.2f) + 0.4f * lp * expf(-t * 6.0f) + 0.9f * sinf(2.0f * PI_F * 46.0f * t) * expf(-t * 2.6f);
			break;
		case SFX_HIT:
			v = 0.9f * sinf(2.0f * PI_F * 95.0f * t) * expf(-t * 30.0f) + 0.4f * w * expf(-t * 60.0f);
			break;
		case SFX_CLICK:
			v = 0.7f * w * expf(-t * 130.0f) + 0.4f * sinf(2.0f * PI_F * 1800.0f * t) * expf(-t * 90.0f);
			break;
		case SFX_SWITCH: {
			float t2 = t - 0.06f;
			v = 0.6f * w * expf(-t * 130.0f) + (t2 > 0.0f ? 0.6f * w * expf(-t2 * 110.0f) : 0.0f);
			break;
		}
		case SFX_PICKUP:
			v = 0.5f * sinf(2.0f * PI_F * (t < 0.1f ? 660.0f : 990.0f) * t) * expf(-fmodf(t, 0.1f) * 14.0f);
			break;
		case SFX_HORN: {
			float e = fminf(1.0f, t * 40.0f) * fminf(1.0f, (dur[id] - t) * 25.0f);
			v = e * 0.30f * (sinf(2.0f * PI_F * 410.0f * t) + sinf(2.0f * PI_F * 512.0f * t) + 0.5f * sinf(2.0f * PI_F * 820.0f * t));
			break;
		}
		case SFX_STEP:
			v = 2.4f * lp2 * expf(-t * 45.0f) + 0.4f * sinf(2.0f * PI_F * 70.0f * t) * expf(-t * 35.0f);
			break;
		case SFX_SIREN: {
			float f = 900.0f + 280.0f * sinf(2.0f * PI_F * t / 1.5f);
			ph += 2.0f * PI_F * f / SFX_RATE;
			float e = powf(sinf(PI_F * t / dur[id]), 1.5f);
			v = e * 0.35f * (sinf(ph) + 0.3f * sinf(2.0f * ph));
			break;
		}
		}
		s.data[i] = pcm(v * 0.7f); // headroom so the shots don't clip
	}
	DSP_FlushDataCache(s.data, (u32)s.n * 2);
	return s;
}

// Seamless ambience loop: render a little extra and cross-fade the tail into the head.
static Snd synthLoop(int kind, int seconds) {
	int L = seconds * SFX_RATE, X = 4096, n = L + X;
	Snd s = newSnd(L);
	float* tmp = (float*)malloc((size_t)n * sizeof(float));
	if (!s.data || !tmp) { free(tmp); return s; }
	static const float birdT[7]  = {0.30f, 0.90f, 1.55f, 2.20f, 2.50f, 3.30f, 3.70f};
	static const float birdF[7]  = {3400.0f, 2900.0f, 3800.0f, 3100.0f, 4200.0f, 3500.0f, 3000.0f};
	unsigned st = 777u + (unsigned)kind * 31u;
	float b = 0.0f, lp = 0.0f, lp2 = 0.0f;
	for (int i = 0; i < n; i++) {
		float t = (float)i / SFX_RATE, v = 0.0f, w = nrand(&st);
		lp  += (w - lp)  * 0.25f;
		lp2 += (w - lp2) * 0.03f;
		b = b * 0.985f + w * 0.04f;
		if (kind == 0) { // traffic rumble, engine drone and the odd car passing
			float drone = 0.10f * sinf(2.0f * PI_F * 55.0f * t) + 0.06f * sinf(2.0f * PI_F * 82.5f * t) * (0.6f + 0.4f * sinf(2.0f * PI_F * 0.25f * t));
			float pass = expf(-powf((t - 1.4f) / 0.35f, 2.0f)) + 0.7f * expf(-powf((t - 3.1f) / 0.30f, 2.0f));
			v = b * 1.6f + drone + pass * 0.35f * lp2 * 4.0f;
		} else if (kind == 1) { // steady rain hiss
			v = 0.55f * (w - lp * 0.9f) * (0.85f + 0.15f * sinf(2.0f * PI_F * 0.7f * t));
		} else if (kind == 2) { // crickets
			for (int k = 0; k < 3; k++) {
				float per = 0.46f + 0.05f * (float)k, tp = fmodf(t + 0.13f * (float)k, per), e = 0.0f;
				if (tp < 0.12f) {
					float tq = fmodf(tp, 0.034f);
					if (tq < 0.017f) e = sinf(PI_F * tq / 0.017f);
				}
				v += 0.10f * e * sinf(2.0f * PI_F * (4300.0f + 380.0f * (float)k) * t);
			}
			v += 0.03f * lp2;
		} else { // birdsong over a little wind
			for (int k = 0; k < 7; k++) {
				float tau = t - birdT[k];
				if (tau >= 0.0f && tau < 0.14f) {
					float u = tau / 0.14f;
					v += 0.18f * sinf(2.0f * PI_F * (birdF[k] * tau + 1400.0f * tau * tau * tau / (3.0f * 0.14f * 0.14f))) * sinf(PI_F * u);
				}
			}
			v += 0.10f * lp2;
		}
		tmp[i] = v;
	}
	for (int i = 0; i < X; i++) {
		float wgt = (float)i / (float)X;
		tmp[i] = tmp[i] * wgt + tmp[L + i] * (1.0f - wgt);
	}
	for (int i = 0; i < L; i++) s.data[i] = pcm(tmp[i]);
	free(tmp);
	DSP_FlushDataCache(s.data, (u32)s.n * 2);
	return s;
}

static void playSfx(int id, float vol) {
	if (!sndOK || !sfx[id].data) return;
	int ch = 5 + sfxNext;
	sfxNext = (sfxNext + 1) & 7;
	ndspChnWaveBufClear(ch);
	float mix[12];
	memset(mix, 0, sizeof mix);
	mix[0] = vol; mix[1] = vol;
	ndspChnSetMix(ch, mix);
	ndspWaveBuf* wb = &sfxWB[ch - 5];
	memset(wb, 0, sizeof *wb);
	wb->data_vaddr = sfx[id].data;
	wb->nsamples = (u32)sfx[id].n;
	wb->looping = false;
	ndspChnWaveBufAdd(ch, wb);
}

static void ambSetVol(int i, float v) {
	if (!sndOK || fabsf(v - ambSet[i]) < 0.01f) return;
	ambSet[i] = v;
	float mix[12];
	memset(mix, 0, sizeof mix);
	mix[0] = v; mix[1] = v;
	ndspChnSetMix(1 + i, mix);
}

// ---- title music streaming thread ----
static volatile int musicWant = 0, musicQuit = 0, musicFail = 0;
static Thread musicThread = NULL;
// what the title screen reports about the music: 0 not started, 1 streaming title.pcm,
// 2 built-in synthesized theme (title.pcm missing), 3 no audio at all (DSP firmware missing)
static int gMusicMode = 0;
static Snd themeSnd;
static ndspWaveBuf themeWB;
static float themeGain = -1.0f;
#define THEME_CH 13

// title.pcm is looked for inside the app (romfs) first, then on the SD card.
static FILE* openMusicFile(void) {
	static const char* paths[] = {
		"romfs:/title.pcm", "sdmc:/3ds/Chicago3DS/title.pcm", "sdmc:/3ds/chicago3ds/title.pcm", "sdmc:/Chicago3DS/title.pcm"
	};
	for (int i = 0; i < 4; i++) {
		FILE* f = fopen(paths[i], "rb");
		if (!f) continue;
		u8 probe[4];
		if (fread(probe, 1, 4, f) == 4) { fseek(f, 0, SEEK_SET); return f; }
		fclose(f);
	}
	return NULL;
}

// ---- built-in title theme (used when title.pcm cannot be found) ----------------------------
// A slow synth piece in A minor: pad chords, a plucked arpeggio, bass and a soft beat. About 20 s, looped.
static float sineTab[1025];

static float tabSin(float cycles) {
	cycles -= floorf(cycles);
	float x = cycles * 1024.0f;
	int i = (int)x;
	float f = x - (float)i;
	return sineTab[i] * (1.0f - f) + sineTab[i + 1] * f;
}

static float midiHz(int m) { return 440.0f * powf(2.0f, (float)(m - 69) / 12.0f); }

static Snd synthTitleTheme(void) {
	static const int chord[8][3] = {{57, 60, 64}, {53, 57, 60}, {48, 52, 55}, {55, 59, 62}, {57, 60, 64}, {53, 57, 60}, {50, 53, 57}, {52, 56, 59}};
	static const int bassN[8]    = {33, 29, 36, 31, 33, 29, 26, 28};
	static const int arpPat[8]   = {0, 1, 2, 1, 2, 1, 0, 1};
	for (int i = 0; i <= 1024; i++) sineTab[i] = sinf(2.0f * PI_F * (float)i / 1024.0f);
	const float beat = 60.0f / 96.0f, barLen = 4.0f * beat;
	int n = (int)(barLen * 8.0f * (float)SFX_RATE);
	Snd s = newSnd(n);
	if (!s.data) return s;
	float padPh[3][2] = {{0, 0}, {0, 0}, {0, 0}}, bassPh = 0.0f, arpPh = 0.0f, kickPh = 0.0f, lp = 0.0f;
	unsigned st = 4242u;
	for (int i = 0; i < n; i++) {
		float t = (float)i / (float)SFX_RATE;
		int ci = (int)(t / barLen);
		if (ci > 7) ci = 7;
		float ct = t - (float)ci * barLen, v = 0.0f;

		// pad: slow swell in, soft release out (silent at the bar lines, so the loop point is seamless)
		float padEnv = fminf(1.0f, ct / 0.7f) * fminf(1.0f, (barLen - ct) / 0.6f);
		for (int k = 0; k < 3; k++) {
			float hz = midiHz(chord[ci][k]);
			padPh[k][0] += hz / SFX_RATE;
			padPh[k][1] += hz * 1.004f / SFX_RATE;
			v += 0.065f * padEnv * (tabSin(padPh[k][0]) + tabSin(padPh[k][1]));
		}
		// bass: plucked on beats 1 and 3
		float bt = fmodf(ct, 2.0f * beat);
		bassPh += midiHz(bassN[ci]) / SFX_RATE;
		v += 0.30f * expf(-bt * 2.4f) * (tabSin(bassPh) + 0.30f * tabSin(bassPh * 2.0f));
		// arpeggio: eighth notes, one octave and a half above the pad
		float eighth = beat * 0.5f;
		int step = (int)(ct / eighth);
		if (step > 7) step = 7;
		float at = ct - (float)step * eighth;
		arpPh += midiHz(chord[ci][arpPat[step]] + 24) / SFX_RATE;
		v += 0.16f * expf(-at * 8.0f) * (tabSin(arpPh) + 0.25f * tabSin(arpPh * 3.0f));
		// soft kick on beats 1 and 3, hat on the off-beats
		kickPh += (46.0f + 70.0f * expf(-bt * 28.0f)) / SFX_RATE;
		v += 0.30f * expf(-bt * 11.0f) * tabSin(kickPh);
		float w = nrand(&st);
		lp += (w - lp) * 0.35f;
		v += 0.05f * (w - lp) * expf(-fmodf(ct + eighth, beat) * 55.0f) * ((step & 1) ? 1.0f : 0.0f);
		s.data[i] = pcm(v * 0.85f);
	}
	DSP_FlushDataCache(s.data, (u32)s.n * 2);
	return s;
}

static void startTheme(void) {
	if (themeSnd.data) return;
	themeSnd = synthTitleTheme();
	if (!themeSnd.data) return;
	memset(&themeWB, 0, sizeof themeWB);
	themeWB.data_vaddr = themeSnd.data;
	themeWB.nsamples = (u32)themeSnd.n;
	themeWB.looping = true;
	float mix[12];
	memset(mix, 0, sizeof mix);
	ndspChnSetMix(THEME_CH, mix);
	ndspChnWaveBufAdd(THEME_CH, &themeWB);
	themeGain = 0.0f;
	gMusicMode = 2;
}

static void musicSetMix(float g) {
	float mix[12];
	memset(mix, 0, sizeof mix);
	mix[0] = g; mix[1] = g;
	ndspChnSetMix(0, mix);
}

static void fillMusic(FILE* f, s16* buf, int frames) {
	int need = frames * 4, got = 0, retry = 0;
	while (got < need) {
		size_t r = fread((u8*)buf + got, 1, (size_t)(need - got), f);
		got += (int)r;
		if (got < need) {
			if (r == 0 && ++retry > 2) break;
			fseek(f, 0, SEEK_SET); // end of the track: loop
		}
	}
	if (got < need) memset((u8*)buf + got, 0, (size_t)(need - got));
	DSP_FlushDataCache(buf, (u32)need);
}

static void musicThreadFn(void* arg) {
	(void)arg;
	const int CH = 8192; // stereo frames per buffer, about 0.26 s
	s16* bufs[3];
	ndspWaveBuf wbs[3];
	FILE* f = NULL;
	int playing = 0;
	float gain = 0.0f;
	for (int i = 0; i < 3; i++) bufs[i] = (s16*)linearAlloc((size_t)CH * 4);
	memset(wbs, 0, sizeof wbs);
	while (!musicQuit) {
		if (musicWant && !playing) {
			f = openMusicFile();
			if (f && bufs[0] && bufs[1] && bufs[2]) {
				ndspChnWaveBufClear(0);
				memset(wbs, 0, sizeof wbs);
				gain = 0.0f;
				musicSetMix(0.0f);
				for (int i = 0; i < 3; i++) {
					fillMusic(f, bufs[i], CH);
					wbs[i].data_vaddr = bufs[i];
					wbs[i].nsamples = (u32)CH;
					ndspChnWaveBufAdd(0, &wbs[i]);
				}
				playing = 1;
			} else {
				if (f) { fclose(f); f = NULL; }
				musicWant = 0; // no music file found: the main thread switches to the built-in theme
				musicFail = 1;
			}
		}
		if (playing) {
			for (int i = 0; i < 3; i++) {
				if (wbs[i].status == NDSP_WBUF_DONE) {
					fillMusic(f, bufs[i], CH);
					wbs[i].nsamples = (u32)CH;
					ndspChnWaveBufAdd(0, &wbs[i]);
				}
			}
			gain += ((musicWant ? 1.0f : 0.0f) - gain) * 0.06f;
			musicSetMix(gain * 0.85f);
			if (!musicWant && gain < 0.01f) {
				ndspChnWaveBufClear(0);
				fclose(f); f = NULL;
				playing = 0;
			}
		}
		svcSleepThread(20000000LL);
	}
	ndspChnWaveBufClear(0);
	if (f) fclose(f);
	for (int i = 0; i < 3; i++) if (bufs[i]) linearFree(bufs[i]);
}

static void initAudio(void) {
	if (R_FAILED(ndspInit())) { gMusicMode = 3; return; } // dspfirm.cdc missing: the whole game stays silent
	sndOK = 1;
	ndspSetOutputMode(NDSP_OUTPUT_STEREO);
	for (int ch = 0; ch < 14; ch++) {
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	}
	ndspChnSetRate(0, (float)MUS_RATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	for (int ch = 1; ch < 14; ch++) {
		ndspChnSetRate(ch, (float)SFX_RATE);
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	}
	for (int i = 0; i < SFX_COUNT; i++) sfx[i] = synthSfx(i);
	for (int i = 0; i < 4; i++) {
		ambSnd[i] = synthLoop(i, 4);
		if (!ambSnd[i].data) continue;
		memset(&ambWB[i], 0, sizeof ambWB[i]);
		ambWB[i].data_vaddr = ambSnd[i].data;
		ambWB[i].nsamples = (u32)ambSnd[i].n;
		ambWB[i].looping = true;
		ambSetVol(i, 0.0f);
		ndspChnWaveBufAdd(1 + i, &ambWB[i]);
	}
	// the real title music if title.pcm can be found, otherwise the built-in theme
	FILE* probe = openMusicFile();
	if (probe) {
		fclose(probe);
		s32 prio = 0;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		musicThread = threadCreate(musicThreadFn, NULL, 64 * 1024, prio - 1, -1, false);
		if (musicThread) gMusicMode = 1;
	}
	if (gMusicMode != 1) startTheme();
}

static void shutdownAudio(void) {
	if (!sndOK) return;
	musicQuit = 1;
	if (musicThread) { threadJoin(musicThread, U64_MAX); threadFree(musicThread); }
	ndspExit();
}

// ---- traffic -------------------------------------------------------------
typedef struct { int axis, road, dir, brake; float s, speed, r, g, b; } Car;
static Car cars[N_CARS];
static int hackA = -1, hackB = -1, hackTimer = 0;

// 0 = green, 1 = yellow, 2 = red. axis 0 = traffic moving along x, axis 1 = along z.
static int lightState(int a, int b, int axis, int frame) {
	if (hackTimer > 0 && a == hackA && b == hackB) return 2;
	int t = (frame + (a * 7 + b * 13) * 37) % 600;
	int ew = (t < 260) ? 0 : (t < 300) ? 1 : 2;
	int ns = (t < 300) ? 2 : (t < 560) ? 0 : 1;
	return axis == 0 ? ew : ns;
}

static void initCars(void) {
	static const float pal[6][3] = {
		{0.85f, 0.15f, 0.15f}, {0.15f, 0.35f, 0.85f}, {0.90f, 0.80f, 0.15f},
		{0.85f, 0.85f, 0.88f}, {0.12f, 0.12f, 0.14f}, {0.20f, 0.65f, 0.30f}
	};
	for (int i = 0; i < N_CARS; i++) {
		unsigned h = hash2(i, 991);
		Car* c = &cars[i];
		c->axis  = i & 1;
		c->road  = (h >> 3) % ROADS;
		c->dir   = ((h >> 9) & 1) ? 1 : -1;
		c->s     = -CITY_H + (float)((h >> 12) % (int)(2.0f * CITY_H));
		c->speed = 0.14f + (float)((h >> 20) % 10) * 0.012f;
		c->brake = 0;
		const float* p = pal[(h >> 5) % 6];
		c->r = p[0]; c->g = p[1]; c->b = p[2];
	}
}

static void updateCars(int frame, float px, float pz) {
	if (hackTimer > 0) hackTimer--;
	for (int i = 0; i < N_CARS; i++) {
		Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		int stop = 0;

		// red/yellow light just ahead
		for (int k = 0; k < ROADS; k++) {
			float gap = (ROAD_C(k) - c->s) * c->dir;
			if (gap > ROAD_HW + 4.2f && gap < ROAD_HW + 6.2f) {
				int a = (c->axis == 0) ? k : c->road;
				int b = (c->axis == 0) ? c->road : k;
				if (lightState(a, b, c->axis, frame) != 0) stop = 1;
			}
		}
		// car ahead in the same lane
		for (int j = 0; j < N_CARS && !stop; j++) {
			if (j == i) continue;
			const Car* o = &cars[j];
			if (o->axis == c->axis && o->road == c->road && o->dir == c->dir) {
				float gap = (o->s - c->s) * c->dir;
				if (gap > 0.0f && gap < 7.0f) stop = 1;
			}
		}
		// player standing in the way
		float ppar = (c->axis == 0) ? px : pz, plat = (c->axis == 0) ? pz : px;
		float pgap = (ppar - c->s) * c->dir;
		if (pgap > 0.0f && pgap < 4.5f && fabsf(plat - lane) < 1.8f) stop = 1;

		c->brake = stop;
		if (!stop) c->s += c->dir * c->speed;
		if (c->s >  CITY_H) c->s = -CITY_H;
		if (c->s < -CITY_H) c->s =  CITY_H;
	}
}

static int blocked(float x, float z) {
	for (int i = 0; i < nBld; i++) {
		const Building* b = &bldg[i];
		if (x > b->x0 - PLAYER_R && x < b->x1 + PLAYER_R &&
		    z > b->z0 - PLAYER_R && z < b->z1 + PLAYER_R) return 1;
	}
	float lim = CITY_H + 20.0f;
	return (x < -lim || x > lim || z < -lim || z > lim);
}

// ---- characters ------------------------------------------------------------
// Blocky low-poly person built from rotated boxes. Body space: feet at the origin,
// facing -Z. Each part swings about its joint (X axis), then the whole body leans
// and turns to its heading.
static float gPx, gPz, gFace, gLean, gBob;
static float gSx = 1.0f, gSy = 1.0f; // body scale: width, height (pedestrians come in different builds)

static void addPart(float jx, float jy, float jz, float ox, float oy, float oz,
                    float hx, float hy, float hz, float swing, float r, float g, float b) {
	float cs = cosf(swing), sn = sinf(swing);
	float cl = cosf(gLean), sl = sinf(gLean);
	float cf = cosf(gFace), sf = sinf(gFace);
	float P[8][3];
	for (int i = 0; i < 8; i++) {
		float x = ox + ((i & 1) ? hx : -hx);
		float y = oy + ((i & 2) ? hy : -hy);
		float z = oz + ((i & 4) ? hz : -hz);
		float y1 = y * cs - z * sn, z1 = y * sn + z * cs;      // joint swing
		x = (x + jx) * gSx; y1 = (y1 + jy) * gSy; z1 = (z1 + jz) * gSy;
		float y2 = y1 * cl - z1 * sl, z2 = y1 * sl + z1 * cl;  // body lean
		P[i][0] = gPx + x * cf + z2 * sf;
		P[i][1] = y2 + gBob;
		P[i][2] = gPz - x * sf + z2 * cf;
	}
	quad(P[0], P[1], P[3], P[2], r * 0.70f, g * 0.70f, b * 0.70f);
	quad(P[4], P[5], P[7], P[6], r * 0.70f, g * 0.70f, b * 0.70f);
	quad(P[0], P[4], P[6], P[2], r * 0.85f, g * 0.85f, b * 0.85f);
	quad(P[1], P[5], P[7], P[3], r * 0.85f, g * 0.85f, b * 0.85f);
	quad(P[2], P[3], P[7], P[6], r, g, b);
	quad(P[0], P[1], P[5], P[4], r * 0.50f, g * 0.50f, b * 0.50f);
}

static void addShadow(float x, float z, float hx, float hz, float a) {
	float p0[3] = {x - hx, 0.10f, z - hz}, p1[3] = {x + hx, 0.10f, z - hz};
	float p2[3] = {x + hx, 0.10f, z + hz}, p3[3] = {x - hx, 0.10f, z + hz};
	gAlpha = a;
	quad(p0, p1, p2, p3, 0.0f, 0.0f, 0.0f);
	gAlpha = 1.0f;
}

// ---- pedestrians -------------------------------------------------------------
// They walk the sidewalks and wait at the kerb until the cross traffic has a red light.
// Each one gets its own look: 10 skin tones, 9 hair colours, 9 hair styles / head coverings, 8 outfits,
// accessories, and a different height and build.
enum { HS_BALD, HS_SHORT, HS_LONG, HS_AFRO, HS_BUN, HS_CAP, HS_BEANIE, HS_SCARF };
enum { OF_TEE, OF_LONG, OF_SHORTS, OF_DRESS, OF_HOODIE, OF_SUIT, OF_COAT, OF_TANK };
enum { AC_PACK = 1, AC_BAG = 2, AC_SHADES = 4, AC_BEARD = 8 };
typedef struct {
	int axis, road, side, dir, walk, hp, dead, deadT, panic, hair, outfit, acc;
	float s, speed, ph, off, scale, width;
	float shirt[3], pants[3], skin[3], hairC[3], shoe[3], trim[3];
} Ped;
static Ped peds[N_PEDS];

static void styleNpc(Ped* p, unsigned h) {
	static const float skinPal[10][3] = {
		{0.96f, 0.80f, 0.69f}, {0.92f, 0.74f, 0.60f}, {0.86f, 0.66f, 0.50f}, {0.78f, 0.58f, 0.40f}, {0.70f, 0.50f, 0.34f},
		{0.60f, 0.42f, 0.28f}, {0.50f, 0.34f, 0.23f}, {0.40f, 0.27f, 0.19f}, {0.31f, 0.20f, 0.14f}, {0.82f, 0.62f, 0.48f}
	};
	static const float hairPal[9][3] = {
		{0.07f, 0.06f, 0.06f}, {0.20f, 0.12f, 0.08f}, {0.34f, 0.22f, 0.12f}, {0.56f, 0.40f, 0.22f}, {0.82f, 0.68f, 0.36f},
		{0.58f, 0.24f, 0.12f}, {0.62f, 0.62f, 0.64f}, {0.88f, 0.88f, 0.88f}, {0.65f, 0.22f, 0.55f}
	};
	static const float shirtPal[16][3] = {
		{0.80f, 0.20f, 0.20f}, {0.20f, 0.45f, 0.80f}, {0.90f, 0.75f, 0.20f}, {0.88f, 0.88f, 0.86f}, {0.25f, 0.65f, 0.40f}, {0.65f, 0.30f, 0.70f},
		{0.92f, 0.52f, 0.15f}, {0.15f, 0.60f, 0.62f}, {0.92f, 0.55f, 0.70f}, {0.14f, 0.20f, 0.42f}, {0.52f, 0.54f, 0.58f}, {0.10f, 0.10f, 0.12f},
		{0.50f, 0.14f, 0.20f}, {0.44f, 0.48f, 0.24f}, {0.55f, 0.75f, 0.92f}, {0.94f, 0.88f, 0.72f}
	};
	static const float pantsPal[10][3] = {
		{0.18f, 0.26f, 0.45f}, {0.10f, 0.13f, 0.25f}, {0.08f, 0.08f, 0.09f}, {0.58f, 0.50f, 0.36f}, {0.35f, 0.35f, 0.38f},
		{0.30f, 0.34f, 0.20f}, {0.30f, 0.20f, 0.12f}, {0.82f, 0.82f, 0.80f}, {0.40f, 0.12f, 0.18f}, {0.22f, 0.30f, 0.50f}
	};
	static const float shoePal[6][3] = {
		{0.08f, 0.08f, 0.09f}, {0.90f, 0.90f, 0.90f}, {0.75f, 0.15f, 0.15f}, {0.35f, 0.22f, 0.12f}, {0.14f, 0.18f, 0.36f}, {0.45f, 0.45f, 0.47f}
	};
	static const float suitPal[3][3] = {{0.12f, 0.14f, 0.26f}, {0.09f, 0.09f, 0.10f}, {0.34f, 0.35f, 0.38f}};
	static const int styles[16]  = {HS_SHORT, HS_SHORT, HS_SHORT, HS_LONG, HS_LONG, HS_AFRO, HS_BUN, HS_CAP, HS_CAP, HS_BEANIE, HS_SCARF, HS_BALD, HS_SHORT, HS_LONG, HS_AFRO, HS_BALD};
	static const int outfits[12] = {OF_TEE, OF_TEE, OF_LONG, OF_LONG, OF_SHORTS, OF_DRESS, OF_HOODIE, OF_HOODIE, OF_SUIT, OF_COAT, OF_TANK, OF_TEE};
	unsigned a = hash2((int)h, 1001), b = hash2((int)h, 2002), c = hash2((int)h, 3003);
	int sk = (int)(a % 10), hc = (int)((a >> 8) % 9);
	if (sk >= 5 && hc >= 3 && hc <= 5) hc -= 3; // blond / red hair is rarer with the darker skin tones
	p->hair   = styles[b % 16];
	p->outfit = outfits[(b >> 4) % 12];
	for (int q = 0; q < 3; q++) {
		p->skin[q]  = skinPal[sk][q];
		p->hairC[q] = hairPal[hc][q];
		p->shirt[q] = shirtPal[(b >> 8) % 16][q];
		p->pants[q] = pantsPal[(b >> 12) % 10][q];
		p->shoe[q]  = shoePal[(b >> 16) % 6][q];
		p->trim[q]  = shirtPal[c % 16][q];
	}
	if (p->outfit == OF_SUIT) for (int q = 0; q < 3; q++) { p->shirt[q] = suitPal[(c >> 4) % 3][q]; p->pants[q] = p->shirt[q]; }
	p->acc = 0;
	if ((c >> 8) % 5 == 0) p->acc |= AC_PACK; else if ((c >> 8) % 5 == 1) p->acc |= AC_BAG;
	if ((c >> 12) % 6 == 0) p->acc |= AC_SHADES;
	if ((c >> 16) % 7 == 0 && (p->hair == HS_SHORT || p->hair == HS_BALD || p->hair == HS_CAP) && p->outfit != OF_DRESS) p->acc |= AC_BEARD;
	p->scale = 0.90f + (float)((c >> 20) % 17) * 0.01f;
	p->width = 0.88f + (float)((c >> 24) % 28) * 0.01f;
}

static void initPeds(void) {
	for (int i = 0; i < N_PEDS; i++) {
		unsigned h = hash2(i, 777);
		Ped* p = &peds[i];
		p->axis  = i & 1;
		p->road  = (h >> 3) % ROADS;
		p->side  = ((h >> 8) & 1) ? 1 : -1;
		p->dir   = ((h >> 9) & 1) ? 1 : -1;
		p->s     = -CITY_H + (float)((h >> 12) % (int)(2.0f * CITY_H));
		p->speed = 0.035f + (float)((h >> 20) % 5) * 0.006f;
		p->ph    = (float)(h % 100) * 0.1f;
		p->off   = ((float)((h >> 24) % 19) - 9.0f) * 0.1f; // not everybody walks in a straight line along the kerb
		p->walk  = 1; p->hp = 4; p->dead = 0; p->deadT = 0; p->panic = 0;
		styleNpc(p, h);
	}
}

// A killed pedestrian is replaced by a fresh one (with a new look) somewhere else after a few seconds.
static void respawnPed(Ped* p, int salt) {
	unsigned h = hash2(salt, 9001 + salt * 7);
	p->axis = (int)((h >> 11) & 1);
	p->road = (int)(h % ROADS);
	p->side = ((h >> 8) & 1) ? 1 : -1;
	p->dir  = ((h >> 9) & 1) ? 1 : -1;
	p->s    = -CITY_H + (float)((h >> 12) % (int)(2.0f * CITY_H));
	p->off  = ((float)((h >> 24) % 19) - 9.0f) * 0.1f;
	p->hp = 4; p->dead = 0; p->deadT = 0; p->panic = 0; p->walk = 1;
	styleNpc(p, h);
}

static void updatePeds(int frame) {
	static int respawns = 0;
	for (int i = 0; i < N_PEDS; i++) {
		Ped* p = &peds[i];
		if (p->dead) {
			if (++p->deadT > 360) respawnPed(p, ++respawns + i);
			continue;
		}
		int wait = 0;
		if (p->panic <= 0) { // a frightened pedestrian ignores the signals
			for (int m = 0; m < ROADS; m++) {
				float gap = (ROAD_C(m) - p->s) * p->dir;
				if (gap > ROAD_HW + 0.4f && gap < ROAD_HW + 1.0f) {
					int st = (p->axis == 0) ? lightState(m, p->road, 1, frame) : lightState(p->road, m, 0, frame);
					if (st != 2) wait = 1;
				}
			}
		} else p->panic--;
		float sp = p->speed * (p->panic > 0 ? 2.4f : 1.0f);
		p->walk = !wait;
		if (!wait) { p->s += p->dir * sp; p->ph += sp * 1.3f + 0.02f; }
		if (p->s >  CITY_H) p->s = -CITY_H;
		if (p->s < -CITY_H) p->s =  CITY_H;
	}
}

static void pedPos(const Ped* p, float* x, float* z) {
	float lat = ROAD_C(p->road) + p->side * (SW_OFF + p->off);
	*x = (p->axis == 0) ? p->s : lat;
	*z = (p->axis == 0) ? lat : p->s;
}

static void drawPed(const Ped* p) {
	float x, z;
	pedPos(p, &x, &z);
	float mx = (p->axis == 0) ? (float)p->dir : 0.0f, mz = (p->axis == 0) ? 0.0f : (float)p->dir;
	gPx = x; gPz = z; gFace = atan2f(-mx, -mz);
	float s;
	if (p->dead) { // topples over backwards and stays down
		float fall = (float)p->deadT * 0.09f;
		gLean = fall < 1.45f ? fall : 1.45f;
		gBob = 0.15f * (gLean / 1.45f);
		s = 0.0f;
	} else {
		gLean = 0.0f;
		gBob = p->walk ? fabsf(cosf(p->ph)) * 0.03f : 0.0f;
		s = p->walk ? sinf(p->ph) * (p->panic > 0 ? 0.8f : 0.5f) : 0.0f;
	}
	gSy = p->scale; gSx = p->scale * p->width;
	const float* sh = p->shirt; const float* pa = p->pants; const float* sk = p->skin;
	const float* hr = p->hairC; const float* so = p->shoe; const float* tr = p->trim;
	float dx = x - gCamX, dz = z - gCamZ;
	int near = (dx * dx + dz * dz) < 45.0f * 45.0f; // small details are skipped on far-away people
	int of = p->outfit;
	int longLegs = (of == OF_TEE || of == OF_LONG || of == OF_HOODIE || of == OF_SUIT || of == OF_COAT);
	int longSleeves = (of == OF_LONG || of == OF_HOODIE || of == OF_SUIT || of == OF_COAT);
	int shortSleeves = (of == OF_TEE || of == OF_SHORTS || of == OF_DRESS);

	// legs, shoes
	for (int side = -1; side <= 1; side += 2) {
		float sw = (side < 0) ? s : -s, jx = 0.15f * (float)side;
		if (longLegs) addPart(jx, 0.90f, 0.0f, 0.0f, -0.45f, 0.0f, 0.09f, 0.45f, 0.10f, sw, pa[0], pa[1], pa[2]);
		else          addPart(jx, 0.90f, 0.0f, 0.0f, -0.45f, 0.0f, 0.09f, 0.45f, 0.10f, sw, sk[0], sk[1], sk[2]);
		if (of == OF_SHORTS || of == OF_TANK)
			addPart(jx, 0.90f, 0.0f, 0.0f, -0.19f, 0.0f, 0.10f, 0.21f, 0.11f, sw, pa[0], pa[1], pa[2]);
		if (near) addPart(jx, 0.90f, 0.0f, 0.0f, -0.86f, -0.03f, 0.095f, 0.05f, 0.13f, sw, so[0], so[1], so[2]);
	}
	if (of == OF_DRESS) addPart(0.0f, 0.90f, 0.0f, 0.0f, -0.17f, 0.0f, 0.25f, 0.20f, 0.15f, 0.0f, sh[0] * 0.85f, sh[1] * 0.85f, sh[2] * 0.85f);

	// torso
	addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.36f, 0.0f, 0.27f, 0.36f, 0.14f, 0.0f, sh[0], sh[1], sh[2]);
	if (of == OF_COAT) addPart(0.0f, 0.90f, 0.0f, 0.0f, -0.10f, 0.0f, 0.28f, 0.30f, 0.15f, 0.0f, sh[0], sh[1], sh[2]);
	if (near && of == OF_SUIT) addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.45f, -0.141f, 0.07f, 0.30f, 0.005f, 0.0f, 0.92f, 0.92f, 0.92f);
	if (near && of == OF_HOODIE) addPart(0.0f, 1.62f, 0.0f, 0.0f, -0.10f, 0.12f, 0.15f, 0.10f, 0.07f, 0.0f, sh[0], sh[1], sh[2]);
	if (near && (p->acc & AC_PACK)) addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.40f, 0.22f, 0.19f, 0.27f, 0.09f, 0.0f, tr[0], tr[1], tr[2]);
	if (near && (p->acc & AC_BAG))  addPart(0.0f, 0.90f, 0.0f, 0.34f, 0.08f, 0.0f, 0.06f, 0.13f, 0.14f, 0.0f, tr[0], tr[1], tr[2]);

	// arms
	for (int side = -1; side <= 1; side += 2) {
		float sw = (side < 0) ? -s : s, jx = 0.36f * (float)side;
		if (longSleeves) {
			addPart(jx, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.065f, 0.30f, 0.065f, sw, sh[0], sh[1], sh[2]);
			if (near) addPart(jx, 1.50f, 0.0f, 0.0f, -0.58f, 0.0f, 0.07f, 0.05f, 0.07f, sw, sk[0], sk[1], sk[2]);
		} else {
			addPart(jx, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.065f, 0.30f, 0.065f, sw, sk[0], sk[1], sk[2]);
			if (shortSleeves) addPart(jx, 1.50f, 0.0f, 0.0f, -0.10f, 0.0f, 0.075f, 0.14f, 0.075f, sw, sh[0], sh[1], sh[2]);
		}
	}

	// head, hair or head covering, face details
	addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.00f, 0.0f, 0.13f, 0.14f, 0.13f, 0.0f, sk[0], sk[1], sk[2]);
	switch (p->hair) {
	case HS_SHORT: addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.05f, 0.04f, 0.14f, 0.10f, 0.11f, 0.0f, hr[0], hr[1], hr[2]); break;
	case HS_LONG:  addPart(0.0f, 1.62f, 0.0f, 0.0f, -0.02f, 0.045f, 0.145f, 0.17f, 0.105f, 0.0f, hr[0], hr[1], hr[2]); break;
	case HS_AFRO:  addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.07f, 0.05f, 0.20f, 0.15f, 0.17f, 0.0f, hr[0], hr[1], hr[2]); break;
	case HS_BUN:
		addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.05f, 0.04f, 0.14f, 0.10f, 0.11f, 0.0f, hr[0], hr[1], hr[2]);
		if (near) addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.22f, 0.06f, 0.06f, 0.06f, 0.06f, 0.0f, hr[0], hr[1], hr[2]);
		break;
	case HS_CAP:
		addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.13f, 0.0f, 0.15f, 0.045f, 0.15f, 0.0f, tr[0], tr[1], tr[2]);
		if (near) addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.10f, -0.17f, 0.11f, 0.015f, 0.06f, 0.0f, tr[0] * 0.8f, tr[1] * 0.8f, tr[2] * 0.8f);
		break;
	case HS_BEANIE: addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.10f, 0.0f, 0.15f, 0.08f, 0.15f, 0.0f, tr[0], tr[1], tr[2]); break;
	case HS_SCARF:
		addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.0f, 0.04f, 0.155f, 0.17f, 0.11f, 0.0f, tr[0], tr[1], tr[2]);
		if (near) addPart(0.0f, 1.62f, 0.0f, 0.0f, -0.20f, 0.02f, 0.20f, 0.07f, 0.14f, 0.0f, tr[0], tr[1], tr[2]);
		break;
	default: break;
	}
	if (near && (p->acc & AC_BEARD))  addPart(0.0f, 1.62f, 0.0f, 0.0f, -0.09f, -0.115f, 0.10f, 0.055f, 0.03f, 0.0f, hr[0], hr[1], hr[2]);
	if (near && (p->acc & AC_SHADES)) addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.03f, -0.13f, 0.115f, 0.028f, 0.012f, 0.0f, 0.04f, 0.04f, 0.05f);
	gSx = 1.0f; gSy = 1.0f;
}

// Rebuilds the dynamic tail of the vertex buffer: shadows, cars, pedestrians (only those near the camera).
static void drawDynamic(void) {
	vcount = staticCount;
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float cx = (c->axis == 0) ? c->s : lane, cz = (c->axis == 0) ? lane : c->s;
		if (!inView(cx, cz)) continue;
		addShadow(cx, cz, (c->axis == 0) ? 2.4f : 1.3f, (c->axis == 0) ? 1.3f : 2.4f, 0.38f);
	}
	for (int i = 0; i < N_PEDS; i++) {
		float x, z;
		pedPos(&peds[i], &x, &z);
		if (inView(x, z)) addShadow(x, z, 0.38f, 0.30f, 0.34f);
	}
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float hx = (c->axis == 0) ? 2.1f : 1.0f, hz = (c->axis == 0) ? 1.0f : 2.1f;
		float cx = (c->axis == 0) ? c->s : lane, cz = (c->axis == 0) ? lane : c->s;
		if (!inView(cx, cz)) continue;
		float gx = hx * 0.58f, gz = hz * 0.58f;
		addBoxG(cx - hx, cz - hz, cx + hx, cz + hz, 0.15f, 1.05f, c->r, c->g, c->b, 0.7f, 1.0f);   // body
		addBoxG(cx - gx, cz - gz, cx + gx, cz + gz, 1.05f, 1.55f, 0.07f, 0.09f, 0.13f, 0.9f, 1.0f); // glass
		addBoxG(cx - gx, cz - gz, cx + gx, cz + gz, 1.55f, 1.66f, c->r * 0.9f, c->g * 0.9f, c->b * 0.9f, 1.0f, 1.0f); // roof
	}
	for (int i = 0; i < N_PEDS; i++) {
		float x, z;
		pedPos(&peds[i], &x, &z);
		if (inView(x, z)) drawPed(&peds[i]);
	}
	GSPGPU_FlushDataCache(&vbo[staticCount], (vcount - staticCount) * sizeof(Vertex));
}

// Emissive geometry (signal heads, head/tail lights): drawn with its own tint so it stays bright at night.
static void drawLights(int frame, float night) {
	int start = vcount;
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float wx = (c->axis == 0) ? c->s : lane, wz = (c->axis == 0) ? lane : c->s;
		if (!inView(wx, wz)) continue;
		float ax = (c->axis == 0) ? (float)c->dir : 0.0f, az = (c->axis == 0) ? 0.0f : (float)c->dir;
		float ox = (c->axis == 0) ? 0.0f : 0.6f, oz = (c->axis == 0) ? 0.6f : 0.0f;
		for (int s = -1; s <= 1; s += 2) {
			float tx = wx - ax * 2.2f + s * ox, tz = wz - az * 2.2f + s * oz; // tail lights
			float k = c->brake ? 1.0f : (night > 0.3f ? 0.75f : 0.45f);
			addBoxY(tx - 0.18f, tz - 0.18f, tx + 0.18f, tz + 0.18f, 0.5f, 0.8f, k, 0.06f * k, 0.05f * k);
			if (night > 0.3f) { // headlights
				float hx = wx + ax * 2.2f + s * ox, hz = wz + az * 2.2f + s * oz;
				addBoxY(hx - 0.18f, hz - 0.18f, hx + 0.18f, hz + 0.18f, 0.4f, 0.75f, 1.0f, 0.95f, 0.7f);
			}
		}
	}
	for (int a = 0; a < ROADS; a++) {
		for (int b = 0; b < ROADS; b++) {
			float x = ROAD_C(a) + SW_OFF + 0.7f, z = ROAD_C(b) + SW_OFF + 0.7f;
			if (!inView(x, z)) continue;
			int hacked = (hackTimer > 0 && a == hackA && b == hackB);
			for (int axis = 0; axis < 2; axis++) {
				int st = lightState(a, b, axis, frame);
				float r, g, bl;
				if (hacked)       { r = 1.0f; g = 0.2f;  bl = 1.0f; }
				else if (st == 0) { r = 0.1f; g = 1.0f;  bl = 0.2f; }
				else if (st == 1) { r = 1.0f; g = 0.85f; bl = 0.1f; }
				else              { r = 1.0f; g = 0.1f;  bl = 0.1f; }
				float y0 = 4.0f + axis * 0.7f;
				addBoxY(x - 0.3f, z - 0.3f, x + 0.3f, z + 0.3f, y0, y0 + 0.6f, r, g, bl);
			}
		}
	}
	GSPGPU_FlushDataCache(&vbo[start], (vcount - start) * sizeof(Vertex));
}

static void addBillboard(float X, float Y, float Z, float hw, float hh, float rx, float rz,
                         float r, float g, float b, float a);

// ---- weapons ---------------------------------------------------------------
typedef struct {
	const char* name;
	int startAmmo, pickupAmmo, cooldown, autoFire;
	float len, thick, r, g, b;
} WDef;
enum { WP_FISTS, WP_PISTOL, WP_SMG, WP_SHOTGUN, WP_ROCKET, WP_COUNT };
static const WDef wdef[WP_COUNT] = {
	{"FISTS",    0,  0,  0, 0, 0.00f, 0.00f, 0.00f, 0.00f, 0.00f},
	{"PISTOL",  48, 24, 14, 0, 0.30f, 0.05f, 0.14f, 0.14f, 0.16f},
	{"SMG",    120, 60,  4, 1, 0.50f, 0.06f, 0.10f, 0.10f, 0.12f},
	{"SHOTGUN", 16,  8, 38, 0, 0.85f, 0.06f, 0.30f, 0.20f, 0.12f},
	{"ROCKET",   4,  2, 72, 0, 0.95f, 0.12f, 0.22f, 0.30f, 0.18f}
};
static int   ammo[WP_COUNT], curWeapon = WP_PISTOL, fireCool = 0;
static float kick = 0.0f, shake = 0.0f, recoilAnim = 0.0f;
static int   hitMark = 0, killMark = 0;
static int   camStyle = 0; // 0 default, 1 Watch Dogs, 2 first person

static unsigned gRng = 20251018u;
static float rnd(void) { return nrand(&gRng); }

static void drawPlayer(float px, float pz, float face, float phase, float moveAmt, float phoneE, float aim) {
	int start = vcount;
	float walk = moveAmt < 1.0f ? moveAmt : 1.0f;
	float run  = moveAmt > 1.0f ? moveAmt - 1.0f : 0.0f;
	addShadow(px, pz, 0.5f, 0.42f, 0.42f);
	gPx = px; gPz = pz; gFace = face;
	gLean = -(0.04f * walk + 0.18f * run);               // lean into a run
	gBob  = fabsf(cosf(phase)) * 0.05f * moveAmt;        // bounce while stepping
	float s = sinf(phase) * (0.45f * walk + 0.45f * run); // leg / arm swing

	// legs
	addPart(-0.15f, 0.90f, 0.0f, 0.0f, -0.45f, 0.0f, 0.09f, 0.45f, 0.10f,  s, 0.13f, 0.15f, 0.28f);
	addPart( 0.15f, 0.90f, 0.0f, 0.0f, -0.45f, 0.0f, 0.09f, 0.45f, 0.10f, -s, 0.13f, 0.15f, 0.28f);
	// torso (dark jacket), head, cap
	addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.36f, 0.0f, 0.27f, 0.36f, 0.14f, 0.0f, 0.10f, 0.10f, 0.12f);
	addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.00f, 0.0f, 0.13f, 0.14f, 0.13f, 0.0f, 0.85f, 0.66f, 0.52f);
	addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.14f, -0.02f, 0.15f, 0.05f, 0.16f, 0.0f, 0.08f, 0.08f, 0.09f);

	int armed = (curWeapon != WP_FISTS) && phoneE < 0.05f;
	float lSwing = -s * 0.9f;
	float rSwing = s * 0.9f * (1.0f - phoneE) + 1.65f * phoneE;
	if (armed) { // gun low at the hip, raised towards the crosshair while aiming
		rSwing = 0.35f + (1.55f - 0.35f) * aim + recoilAnim * 0.25f;
		lSwing = 0.25f + 1.20f * aim;
	}
	addPart(-0.36f, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, lSwing, 0.10f, 0.10f, 0.12f);
	addPart( 0.36f, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, rSwing, 0.10f, 0.10f, 0.12f);
	if (armed) {
		const WDef* w = &wdef[curWeapon];
		addPart(0.36f, 1.50f, 0.0f, 0.0f, -0.58f - w->len * 0.35f, 0.0f, w->thick * 0.5f, w->len * 0.5f, w->thick * 0.9f,
		        rSwing, w->r, w->g, w->b);
	}
	if (phoneE > 0.05f) // glowing phone in the raised hand
		addPart(0.36f, 1.50f, 0.0f, 0.0f, -0.66f, 0.0f, 0.07f, 0.03f, 0.11f, rSwing, 0.20f, 0.90f, 1.00f);

	GSPGPU_FlushDataCache(&vbo[start], (vcount - start) * sizeof(Vertex));
}

static void camForward(float yaw, float pitch, float f[3]) {
	f[0] = -cosf(pitch) * sinf(yaw);
	f[1] = -sinf(pitch);
	f[2] = -cosf(pitch) * cosf(yaw);
}

// First-person view model: the weapon is glued to the camera (lower right of the screen).
static void drawViewModel(float cx, float cy, float cz, float cyaw, float cpitch) {
	if (curWeapon == WP_FISTS) return;
	int start = vcount;
	const WDef* w = &wdef[curWeapon];
	float f[3];
	camForward(cyaw, cpitch, f);
	float rx = cosf(cyaw), rz = -sinf(cyaw);
	float ux = -sinf(cpitch) * sinf(cyaw), uy = cosf(cpitch), uz = -sinf(cpitch) * cosf(cyaw);
	float back = 0.58f - recoilAnim * 0.10f;
	gPx = cx + f[0] * back + rx * 0.27f - ux * 0.24f;
	gBob = cy + f[1] * back - uy * 0.24f;
	gPz = cz + f[2] * back + rz * 0.27f - uz * 0.24f;
	gFace = cyaw; gLean = -cpitch;
	addPart(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -w->len * 0.5f, w->thick * 0.5f, w->thick * 0.9f, w->len * 0.5f, 0.0f, w->r, w->g, w->b);
	addPart(0.0f, 0.0f, 0.0f, 0.0f, -0.09f, 0.02f, 0.05f, 0.08f, 0.05f, 0.0f, 0.85f, 0.66f, 0.52f); // hand on the grip
	GSPGPU_FlushDataCache(&vbo[start], (vcount - start) * sizeof(Vertex));
}

// ---- combat ------------------------------------------------------------------
// Bullets are hitscans along the crosshair ray. A pedestrian dies after 4 hits (a shotgun blast counts as
// one hit however many pellets land); rocket explosions kill everyone nearby.
typedef struct { float x, y, z, size, grow, life, maxLife, r, g, b; } Fx;
#define N_FX 32
static Fx fxs[N_FX];
static int fxNext = 0;
typedef struct { float a[3], b[3]; int life; } Tracer;
#define N_TR 12
static Tracer trs[N_TR];
static int trNext = 0;
typedef struct { int alive, life; float p[3], v[3]; } Rocket;
static Rocket rocket;

static void addFx(float x, float y, float z, float size, float grow, float life, float r, float g, float b) {
	Fx* f = &fxs[fxNext];
	fxNext = (fxNext + 1) % N_FX;
	f->x = x; f->y = y; f->z = z; f->size = size; f->grow = grow;
	f->life = life; f->maxLife = life; f->r = r; f->g = g; f->b = b;
}

static void addTracer(const float a[3], const float b[3]) {
	Tracer* t = &trs[trNext];
	trNext = (trNext + 1) % N_TR;
	for (int i = 0; i < 3; i++) { t->a[i] = a[i]; t->b[i] = b[i]; }
	t->life = 4;
}

static float rayBox(const float o[3], const float d[3], float x0, float y0, float z0, float x1, float y1, float z1) {
	const float mn[3] = {x0, y0, z0}, mx[3] = {x1, y1, z1};
	float tmin = 0.0f, tmax = 1e9f;
	for (int a = 0; a < 3; a++) {
		if (fabsf(d[a]) < 1e-6f) {
			if (o[a] < mn[a] || o[a] > mx[a]) return -1.0f;
		} else {
			float t1 = (mn[a] - o[a]) / d[a], t2 = (mx[a] - o[a]) / d[a];
			if (t1 > t2) { float tt = t1; t1 = t2; t2 = tt; }
			if (t1 > tmin) tmin = t1;
			if (t2 < tmax) tmax = t2;
			if (tmin > tmax) return -1.0f;
		}
	}
	return tmin;
}

static float rayCyl(const float o[3], const float d[3], float cx, float cz, float r, float y0, float y1) {
	float a = d[0] * d[0] + d[2] * d[2];
	if (a < 1e-6f) return -1.0f;
	float ox = o[0] - cx, oz = o[2] - cz;
	float b = 2.0f * (ox * d[0] + oz * d[2]), c = ox * ox + oz * oz - r * r;
	float disc = b * b - 4.0f * a * c;
	if (disc < 0.0f) return -1.0f;
	float sq = sqrtf(disc), t = (-b - sq) / (2.0f * a);
	if (t < 0.0f) t = (-b + sq) / (2.0f * a);
	if (t < 0.0f) return -1.0f;
	float y = o[1] + d[1] * t;
	return (y < y0 || y > y1) ? -1.0f : t;
}

// Distance to the first thing a ray hits (ground, buildings, cars, living pedestrians), up to maxT.
// *ped is the pedestrian index when one is hit first, otherwise -1.
static float castRay(const float o[3], const float d[3], float maxT, int* ped) {
	float best = maxT;
	*ped = -1;
	if (d[1] < -1e-4f) {
		float t = -o[1] / d[1];
		if (t > 0.0f && t < best) best = t;
	}
	for (int i = 0; i < nBld; i++) {
		const Building* b = &bldg[i];
		float t = rayBox(o, d, b->x0, 0.0f, b->z0, b->x1, b->h, b->z1);
		if (t >= 0.0f && t < best) best = t;
	}
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float hx = (c->axis == 0) ? 2.1f : 1.0f, hz = (c->axis == 0) ? 1.0f : 2.1f;
		float cx = (c->axis == 0) ? c->s : lane, cz = (c->axis == 0) ? lane : c->s;
		float t = rayBox(o, d, cx - hx, 0.15f, cz - hz, cx + hx, 1.66f, cz + hz);
		if (t >= 0.0f && t < best) best = t;
	}
	for (int i = 0; i < N_PEDS; i++) {
		if (peds[i].dead) continue;
		float x, z;
		pedPos(&peds[i], &x, &z);
		float t = rayCyl(o, d, x, z, 0.45f, 0.0f, 1.8f);
		if (t >= 0.0f && t < best) { best = t; *ped = i; }
	}
	return best;
}

static void hurtPed(int i, int dmg) {
	Ped* p = &peds[i];
	if (p->dead) return;
	p->hp -= dmg;
	p->panic = 300;
	float x, z;
	pedPos(p, &x, &z);
	addFx(x, 1.2f, z, 0.45f, 0.02f, 8.0f, 0.8f, 0.05f, 0.05f);
	if (p->hp <= 0) { p->dead = 1; p->deadT = 0; killMark = 16; }
	else hitMark = 8;
	playSfx(SFX_HIT, 0.5f);
}

static void explode(float x, float y, float z) {
	addFx(x, y + 0.5f, z, 3.0f, 0.35f, 26.0f, 1.0f, 0.55f, 0.15f);
	addFx(x, y + 0.8f, z, 2.0f, 0.25f, 20.0f, 1.0f, 0.90f, 0.50f);
	addFx(x, y + 1.6f, z, 2.5f, 0.12f, 45.0f, 0.25f, 0.25f, 0.25f);
	float dx = x - gCamX, dz = z - gCamZ, dist = sqrtf(dx * dx + dz * dz);
	float sh = 1.4f / (1.0f + dist * 0.08f);
	if (sh > shake) shake = sh;
	playSfx(SFX_EXPLODE, 1.0f);
	for (int i = 0; i < N_PEDS; i++) {
		if (peds[i].dead) continue;
		float px, pz;
		pedPos(&peds[i], &px, &pz);
		float ex = px - x, ey = 0.9f - y, ez = pz - z;
		if (ex * ex + ey * ey + ez * ez < 7.5f * 7.5f) hurtPed(i, 99);
	}
}

static void normalize3(float v[3]) {
	float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l < 1e-6f) return;
	v[0] /= l; v[1] /= l; v[2] /= l;
}

// One trigger pull. cam/fwd = the crosshair ray, muz = where the barrel is.
static void fireWeapon(const float cam[3], const float fwd[3], const float muz[3]) {
	const int w = curWeapon;
	addFx(muz[0], muz[1], muz[2], 0.32f, 0.0f, 2.0f, 1.0f, 0.85f, 0.4f); // muzzle flash
	recoilAnim = 1.0f;
	if (w == WP_ROCKET) {
		int pi;
		float t = castRay(cam, fwd, 120.0f, &pi);
		float P[3] = {cam[0] + fwd[0] * t, cam[1] + fwd[1] * t, cam[2] + fwd[2] * t};
		float d[3] = {P[0] - muz[0], P[1] - muz[1], P[2] - muz[2]};
		normalize3(d);
		rocket.alive = 1; rocket.life = 160;
		for (int i = 0; i < 3; i++) { rocket.p[i] = muz[i]; rocket.v[i] = d[i] * 1.5f; }
		playSfx(SFX_ROCKET, 0.9f);
		kick += 0.07f;
		return;
	}
	int pellets = (w == WP_SHOTGUN) ? 8 : 1;
	float spread = (w == WP_PISTOL) ? 0.004f : (w == WP_SMG) ? 0.014f : 0.055f;
	float range = (w == WP_SHOTGUN) ? 35.0f : 90.0f;
	unsigned hitMask = 0;
	for (int k = 0; k < pellets; k++) {
		float d[3] = {fwd[0] + rnd() * spread, fwd[1] + rnd() * spread, fwd[2] + rnd() * spread};
		normalize3(d);
		int pi;
		float t = castRay(cam, d, range, &pi);
		float P[3] = {cam[0] + d[0] * t, cam[1] + d[1] * t, cam[2] + d[2] * t};
		if (k < 3) addTracer(muz, P);
		if (pi >= 0) hitMask |= 1u << pi;
		else if (t < range - 0.01f) addFx(P[0], P[1], P[2], 0.22f, 0.0f, 5.0f, 1.0f, 0.9f, 0.6f); // impact spark
	}
	for (int i = 0; i < N_PEDS; i++) if (hitMask & (1u << i)) hurtPed(i, 1);
	playSfx(w == WP_PISTOL ? SFX_PISTOL : (w == WP_SMG ? SFX_SMG : SFX_SHOTGUN), w == WP_SMG ? 0.55f : 0.8f);
	kick += (w == WP_PISTOL) ? 0.012f : (w == WP_SMG) ? 0.008f : 0.05f;
}

static void updateRocket(void) {
	if (!rocket.alive) return;
	float d[3] = {rocket.v[0], rocket.v[1], rocket.v[2]};
	float spd = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	normalize3(d);
	int pi;
	float t = castRay(rocket.p, d, spd, &pi);
	if (t < spd - 0.001f || rocket.p[1] < 0.1f || --rocket.life <= 0) {
		explode(rocket.p[0] + d[0] * t, rocket.p[1] + d[1] * t, rocket.p[2] + d[2] * t);
		rocket.alive = 0;
		return;
	}
	for (int i = 0; i < 3; i++) rocket.p[i] += rocket.v[i];
	addFx(rocket.p[0], rocket.p[1], rocket.p[2], 0.28f, 0.03f, 12.0f, 0.55f, 0.55f, 0.55f); // smoke trail
}

static void updateFx(void) {
	for (int i = 0; i < N_FX; i++) {
		if (fxs[i].life <= 0.0f) continue;
		fxs[i].life -= 1.0f;
		fxs[i].size += fxs[i].grow;
	}
	for (int i = 0; i < N_TR; i++) if (trs[i].life > 0) trs[i].life--;
	if (hitMark > 0) hitMark--;
	if (killMark > 0) killMark--;
	recoilAnim *= 0.82f;
	kick *= 0.86f;
	shake *= 0.9f;
}

// Blended sprites: muzzle flashes, sparks, blood, smoke and explosions.
static void drawFx(float cyaw) {
	float rx = cosf(cyaw), rz = -sinf(cyaw);
	for (int i = 0; i < N_FX; i++) {
		const Fx* f = &fxs[i];
		if (f->life <= 0.0f) continue;
		float a = f->life / f->maxLife;
		addBillboard(f->x, f->y, f->z, f->size, f->size, rx, rz, f->r, f->g, f->b, a * 0.9f);
	}
}

// Bright geometry for the emissive pass: tracer streaks and the rocket body.
static void drawTracers(void) {
	for (int i = 0; i < N_TR; i++) {
		const Tracer* t = &trs[i];
		if (t->life <= 0) continue;
		float d[3] = {t->b[0] - t->a[0], t->b[1] - t->a[1], t->b[2] - t->a[2]};
		float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (L < 0.5f) continue;
		if (L > 70.0f) { // keep long shots short on screen: start the streak near the target
			for (int k = 0; k < 3; k++) d[k] *= 70.0f / L;
			L = 70.0f;
		}
		for (int k = 0; k < 3; k++) d[k] /= L;
		float sx = -d[2], sz = d[0], sl = sqrtf(sx * sx + sz * sz);
		if (sl < 0.01f) { sx = 1.0f; sz = 0.0f; sl = 1.0f; }
		sx = sx / sl * 0.025f; sz = sz / sl * 0.025f;
		float ux = d[1] * sz, uy = d[2] * sx - d[0] * sz, uz = -d[1] * sx;
		float ul = sqrtf(ux * ux + uy * uy + uz * uz);
		if (ul > 1e-6f) { ux = ux / ul * 0.025f; uy = uy / ul * 0.025f; uz = uz / ul * 0.025f; }
		float e[3] = {t->a[0] + d[0] * L, t->a[1] + d[1] * L, t->a[2] + d[2] * L};
		gAlpha = 0.25f + 0.2f * (float)t->life;
		float a0[3] = {t->a[0] - sx, t->a[1], t->a[2] - sz}, a1[3] = {t->a[0] + sx, t->a[1], t->a[2] + sz};
		float b1[3] = {e[0] + sx, e[1], e[2] + sz}, b0[3] = {e[0] - sx, e[1], e[2] - sz};
		quad(a0, a1, b1, b0, 1.0f, 0.9f, 0.55f);
		float c0[3] = {t->a[0] - ux, t->a[1] - uy, t->a[2] - uz}, c1[3] = {t->a[0] + ux, t->a[1] + uy, t->a[2] + uz};
		float d1[3] = {e[0] + ux, e[1] + uy, e[2] + uz}, d0[3] = {e[0] - ux, e[1] - uy, e[2] - uz};
		quad(c0, c1, d1, d0, 1.0f, 0.9f, 0.55f);
		gAlpha = 1.0f;
	}
	if (rocket.alive) {
		const float* p = rocket.p;
		addBoxY(p[0] - 0.12f, p[2] - 0.12f, p[0] + 0.12f, p[2] + 0.12f, p[1] - 0.12f, p[1] + 0.12f, 0.35f, 0.35f, 0.30f);
		addBoxY(p[0] - 0.06f, p[2] - 0.06f, p[0] + 0.06f, p[2] + 0.06f, p[1] - 0.06f, p[1] + 0.06f, 1.0f, 0.6f, 0.2f);
	}
}

// ---- ammo crates -----------------------------------------------------------
typedef struct { float x, z; int active, respawn; } Crate;
#define N_CRATES 8
static Crate crates[N_CRATES];

static void initCrates(void) {
	for (int i = 0; i < N_CRATES; i++) {
		unsigned h = hash2(i, 4401);
		float lat = ROAD_C((int)(h % ROADS)) + (((h >> 8) & 1) ? SW_OFF : -SW_OFF);
		float s = -CITY_H + 10.0f + (float)((h >> 12) % (int)(2.0f * CITY_H - 20.0f));
		crates[i].x = ((h >> 20) & 1) ? lat : s;
		crates[i].z = ((h >> 20) & 1) ? s : lat;
		crates[i].active = 1;
		crates[i].respawn = 0;
	}
}

static void updateCrates(float px, float pz) {
	for (int i = 0; i < N_CRATES; i++) {
		Crate* c = &crates[i];
		if (!c->active) {
			if (--c->respawn <= 0) c->active = 1;
			continue;
		}
		float dx = px - c->x, dz = pz - c->z;
		if (dx * dx + dz * dz < 1.3f * 1.3f) {
			c->active = 0; c->respawn = 1800;
			for (int w = 1; w < WP_COUNT; w++) {
				ammo[w] += wdef[w].pickupAmmo;
				if (ammo[w] > 999) ammo[w] = 999;
			}
			playSfx(SFX_PICKUP, 0.7f);
		}
	}
}

static void drawCrates(int frame) {
	for (int i = 0; i < N_CRATES; i++) {
		const Crate* c = &crates[i];
		if (!c->active || !inView(c->x, c->z)) continue;
		float y = 0.55f + 0.10f * sinf((float)frame * 0.07f + (float)i);
		addBoxY(c->x - 0.25f, c->z - 0.25f, c->x + 0.25f, c->z + 0.25f, y - 0.25f, y + 0.25f, 1.0f, 0.85f, 0.20f);
		addBoxY(c->x - 0.04f, c->z - 0.04f, c->x + 0.04f, c->z + 0.04f, 0.0f, 3.0f, 1.0f, 0.9f, 0.3f);
	}
}

// ---- weather ----------------------------------------------------------------
// Sky colour, world tint and fog ease toward the chosen weather (about one second).
enum { W_SUNNY = 0, W_RAINY = 1, W_SUNSET = 2, W_NIGHT = 3 };
static int   weather = W_SUNNY;
static float wSky[3]  = {0.41f, 0.69f, 0.85f};
static float wTint[3] = {1.0f, 1.0f, 1.0f};
static float wRain = 0.0f, wNight = 0.0f, wDusk = 0.0f, wTime = 0.0f;
static float wFogS = 70.0f, wFogE = 300.0f;

static void updateWeather(void) {
	static const float sky[4][3]  = {{0.41f, 0.69f, 0.85f}, {0.36f, 0.40f, 0.45f}, {0.93f, 0.52f, 0.32f}, {0.03f, 0.04f, 0.10f}};
	static const float tint[4][3] = {{1.00f, 1.00f, 1.00f}, {0.70f, 0.76f, 0.84f}, {1.00f, 0.72f, 0.56f}, {0.30f, 0.34f, 0.52f}};
	static const float fogS[4] = {70.0f, 15.0f, 60.0f, 40.0f};
	static const float fogE[4] = {300.0f, 150.0f, 280.0f, 210.0f};
	for (int i = 0; i < 3; i++) {
		wSky[i]  += (sky[weather][i]  - wSky[i])  * 0.04f;
		wTint[i] += (tint[weather][i] - wTint[i]) * 0.04f;
	}
	wFogS += (fogS[weather] - wFogS) * 0.04f;
	wFogE += (fogE[weather] - wFogE) * 0.04f;
	wRain  += ((weather == W_RAINY)  ? 1.0f - wRain  : -wRain)  * 0.04f;
	wNight += ((weather == W_NIGHT)  ? 1.0f - wNight : -wNight) * 0.04f;
	wDusk  += ((weather == W_SUNSET) ? 1.0f - wDusk  : -wDusk)  * 0.04f;
	wTime  += 1.0f;
}

static u32 skyColor(float k) {
	u32 r = (u32)(wSky[0] * k * 255.0f), g = (u32)(wSky[1] * k * 255.0f), b = (u32)(wSky[2] * k * 255.0f);
	return (r << 24) | (g << 16) | (b << 8) | 0xFF;
}

static void addBillboard(float X, float Y, float Z, float hw, float hh, float rx, float rz,
                         float r, float g, float b, float a) {
	float p0[3] = {X - rx * hw, Y - hh, Z - rz * hw}, p1[3] = {X + rx * hw, Y - hh, Z + rz * hw};
	float p2[3] = {X + rx * hw, Y + hh, Z + rz * hw}, p3[3] = {X - rx * hw, Y + hh, Z - rz * hw};
	gAlpha = a;
	quad(p0, p1, p2, p3, r, g, b);
	gAlpha = 1.0f;
}

static float starOff[N_STARS][3];
static void initStars(void) {
	for (int i = 0; i < N_STARS; i++) {
		unsigned h = hash2(i, 5150);
		float az = (float)(h & 1023) / 1023.0f * 6.2832f;
		float el = 0.10f + (float)((h >> 10) & 1023) / 1023.0f * 1.1f;
		starOff[i][0] = 300.0f * cosf(el) * sinf(az);
		starOff[i][1] = 300.0f * sinf(el);
		starOff[i][2] = -300.0f * cosf(el) * cosf(az);
	}
}

// Sun by day (low and orange at sunset), moon and stars at night. Far away, blended.
static void drawSkyBodies(float cx, float cy, float cz, float cyaw) {
	float rx = cosf(cyaw), rz = -sinf(cyaw);
	float day = (1.0f - wNight) * (1.0f - wRain);
	if (day > 0.02f) {
		float sy = 112.0f - 72.0f * wDusk, sc = 1.0f + 0.5f * wDusk;
		addBillboard(cx + 95.0f, cy + sy, cz - 262.0f, 46.0f * sc, 46.0f * sc, rx, rz,
		             1.0f, 0.95f - 0.35f * wDusk, 0.70f - 0.40f * wDusk, 0.22f * day);
		addBillboard(cx + 95.0f, cy + sy, cz - 262.0f, 22.0f * sc, 22.0f * sc, rx, rz,
		             1.0f, 0.97f - 0.40f * wDusk, 0.80f - 0.55f * wDusk, day);
	}
	if (wNight > 0.02f) {
		addBillboard(cx - 120.0f, cy + 120.0f, cz - 250.0f, 15.0f, 15.0f, rx, rz, 0.90f, 0.93f, 1.0f, wNight);
		for (int i = 0; i < N_STARS; i++) {
			float tw = 0.55f + 0.45f * sinf(wTime * 0.05f + (float)i);
			addBillboard(cx + starOff[i][0], cy + starOff[i][1], cz + starOff[i][2],
			             1.8f, 1.8f, rx, rz, 1.0f, 1.0f, 0.95f, wNight * 0.9f * tw);
		}
	}
}

// Thin falling streaks in a box around the camera, tiled so they stay put in the world.
static void drawRain(float cx, float cz, float cyaw) {
	int n = (int)((float)RAIN_N * wRain);
	if (n <= 0) return;
	float rx = cosf(cyaw) * 0.02f, rz = -sinf(cyaw) * 0.02f;
	gAlpha = 0.38f;
	for (int i = 0; i < n; i++) {
		unsigned h = hash2(i, 4242);
		float dx = fmodf((float)(h & 1023) / 1023.0f * 30.0f - cx, 30.0f);
		float dz = fmodf((float)((h >> 10) & 1023) / 1023.0f * 30.0f - cz, 30.0f);
		if (dx < 0.0f) dx += 30.0f;
		if (dz < 0.0f) dz += 30.0f;
		float x = cx - 15.0f + dx, z = cz - 15.0f + dz;
		float ph = (float)((h >> 20) & 1023) / 1023.0f * 16.0f;
		float y = 16.0f - fmodf(ph + wTime * 0.55f, 16.0f);
		float a[3] = {x - rx, y, z - rz},               b[3] = {x + rx, y, z + rz};
		float c[3] = {x + rx + 0.08f, y + 0.5f, z + rz}, d[3] = {x - rx + 0.08f, y + 0.5f, z - rz};
		quad(a, b, c, d, 0.78f, 0.88f, 1.0f);
	}
	gAlpha = 1.0f;
}

static void addDisc(float x, float y, float r, float cr, float cg, float cb, float a) {
	gAlpha = a;
	for (int k = 0; k < 6; k++) {
		float a0 = (float)k * 1.0472f, a1 = (float)(k + 1) * 1.0472f;
		addV(x, y, 0.0f, cr, cg, cb);
		addV(x + r * cosf(a0), y + r * sinf(a0), 0.0f, cr, cg, cb);
		addV(x + r * cosf(a1), y + r * sinf(a1), 0.0f, cr, cg, cb);
	}
	gAlpha = 1.0f;
}

// A few faint droplets on the "lens" (top screen, pixel coordinates). Each fades in, creeps
// down a little, fades out, then reappears somewhere else.
static void drawDroplets(void) {
	if (wRain < 0.03f) return;
	for (int i = 0; i < N_DROPS; i++) {
		float len = 260.0f + (float)(hash2(i, 11) % 200);
		float ph  = wTime + (float)(hash2(i, 12) % 400);
		int   cyc = (int)(ph / len);
		float u   = (ph - (float)cyc * len) / len;
		unsigned h = hash2(i * 131 + cyc, 77);
		float x = 20.0f + (float)(h & 1023) / 1023.0f * 360.0f;
		float y = 15.0f + (float)((h >> 10) & 1023) / 1023.0f * 190.0f + u * u * 28.0f;
		float r = 2.0f + (float)((h >> 20) & 7) * 0.45f;
		float a = sinf(3.14159f * u) * 0.30f * wRain;
		addDisc(x, y, r, 0.78f, 0.90f, 1.0f, a);
		addDisc(x - r * 0.3f, y - r * 0.35f, r * 0.35f, 1.0f, 1.0f, 1.0f, a * 1.6f);
	}
}

// Ambience follows the weather and time of day; occasional car horns and distant sirens.
static void updateAudio(int inGame, int paused) {
	static float g = 0.0f;
	static int hornT = 400, sirenT = 1800;
	if (!sndOK) return;
	if (musicFail) { musicFail = 0; if (gMusicMode == 1) startTheme(); } // title.pcm turned out to be unreadable
	if (themeSnd.data) {
		themeGain += ((musicWant ? 1.0f : 0.0f) - themeGain) * 0.06f;
		float mix[12];
		memset(mix, 0, sizeof mix);
		mix[0] = mix[1] = themeGain * 0.80f;
		ndspChnSetMix(THEME_CH, mix);
	}
	g += ((inGame ? (paused ? 0.35f : 1.0f) : 0.0f) - g) * 0.05f;
	ambSetVol(0, 0.30f * g);
	ambSetVol(1, wRain * 0.50f * g);
	ambSetVol(2, wNight * 0.30f * g);
	ambSetVol(3, (1.0f - wNight) * (1.0f - wRain) * (1.0f - 0.5f * wDusk) * 0.26f * g);
	if (inGame && !paused) {
		if (--hornT <= 0)  { playSfx(SFX_HORN, 0.16f);  hornT  = 420  + (int)(hash2(hornT, 5) % 900); }
		if (--sirenT <= 0) { playSfx(SFX_SIREN, 0.20f); sirenT = 2600 + (int)(hash2(sirenT, 9) % 2400); }
	}
}

static void addLine2D(float x0, float y0, float x1, float y1, float w, float r, float g, float b, float a) {
	float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
	if (l < 0.01f) return;
	float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
	float p0[3] = {x0 + nx, y0 + ny, 0.0f}, p1[3] = {x1 + nx, y1 + ny, 0.0f};
	float p2[3] = {x1 - nx, y1 - ny, 0.0f}, p3[3] = {x0 - nx, y0 - ny, 0.0f};
	gAlpha = a;
	quad(p0, p1, p2, p3, r, g, b);
	gAlpha = 1.0f;
}

// Top-screen overlay in pixel coordinates: crosshair, hit / kill markers, fade-from-black.
static void drawHud2D(float aim, int armed, float fade) {
	const float cx = 200.0f, cy = 120.0f;
	if (armed && aim > 0.05f) {
		float a = aim * 0.9f, gap = 6.0f + kick * 150.0f;
		addLine2D(cx - gap - 9.0f, cy, cx - gap, cy, 2.0f, 1.0f, 1.0f, 1.0f, a);
		addLine2D(cx + gap, cy, cx + gap + 9.0f, cy, 2.0f, 1.0f, 1.0f, 1.0f, a);
		addLine2D(cx, cy - gap - 9.0f, cx, cy - gap, 2.0f, 1.0f, 1.0f, 1.0f, a);
		addLine2D(cx, cy + gap, cx, cy + gap + 9.0f, 2.0f, 1.0f, 1.0f, 1.0f, a);
		addLine2D(cx - 1.0f, cy, cx + 1.0f, cy, 2.0f, 1.0f, 0.3f, 0.3f, a);
	} else if (armed) {
		addLine2D(cx - 1.5f, cy, cx + 1.5f, cy, 3.0f, 1.0f, 1.0f, 1.0f, 0.5f);
	}
	if (hitMark > 0 || killMark > 0) {
		int kill = killMark > 0;
		float a = (float)(kill ? killMark : hitMark) / (kill ? 16.0f : 8.0f);
		float r = kill ? 1.0f : 1.0f, g = kill ? 0.15f : 1.0f, b = kill ? 0.15f : 1.0f;
		for (int sx = -1; sx <= 1; sx += 2) {
			for (int sy = -1; sy <= 1; sy += 2) {
				addLine2D(cx + sx * 5.0f, cy + sy * 5.0f, cx + sx * 11.0f, cy + sy * 11.0f, 2.0f, r, g, b, a);
			}
		}
	}
	if (fade > 0.002f) {
		float p0[3] = {0.0f, 0.0f, 0.0f}, p1[3] = {400.0f, 0.0f, 0.0f}, p2[3] = {400.0f, 240.0f, 0.0f}, p3[3] = {0.0f, 240.0f, 0.0f};
		gAlpha = fade;
		quad(p0, p1, p2, p3, 0.0f, 0.0f, 0.0f);
		gAlpha = 1.0f;
	}
}

// ---- bottom screen: street map ----------------------------------------------
// North is up (-Z). The map scrolls so the player circle always stays in the centre.
#define MAP_S   2.4f    // pixels per world unit
#define MAP_CX  160.0f
#define MAP_CY  88.0f
#define MAP_H   176.0f  // map viewport height; buttons and the city text sit below it
#define BTN_Y0  178
#define BTN_Y1  204

// Vertical streets, west to east (x = ROAD_C(k)); horizontal streets, north to south (z = ROAD_C(k)).
static const char* vNames[ROADS] = {
	"Western Ave", "Damen Ave", "Ashland Ave", "Halsted St", "Clark St", "State St", "Michigan Ave"
};
static const char* hNames[ROADS] = {
	"Belmont Ave", "Diversey Pkwy", "Fullerton Ave", "Armitage Ave", "North Ave", "Division St", "Chicago Ave"
};
// Chicago neighborhoods, a 4x4 layout over the city (rows north to south, columns west to east).
static const char* hoods[4][4] = {
	{ "Ravenswood",     "Lakeview",          "Wrigleyville", "Uptown"        },
	{ "Roscoe Village", "Lincoln Park",      "Old Town",     "Gold Coast"    },
	{ "Bucktown",       "Wicker Park",       "River North",  "Streeterville" },
	{ "West Town",      "Ukrainian Village", "The Loop",     "Grant Park"    }
};

#define N_MENU_TEXT 19
static C2D_TextBuf gStatic, gDyn;
static C2D_Text vLabel[ROADS], hLabel[ROADS], btnHack, btnBlackout;
static C2D_Text txtMenu[N_MENU_TEXT], txtActive, txtPaused, txtWeatherT, txtGfxT, txtCamT, txtSub, txtFooter, txtHint;
static C2D_Text txtWp[WP_COUNT], txtWheelT, txtRelease;

static void initMapText(void) {
	gStatic = C2D_TextBufNew(1024);
	gDyn    = C2D_TextBufNew(1024);
	for (int k = 0; k < ROADS; k++) {
		C2D_TextParse(&vLabel[k], gStatic, vNames[k]);
		C2D_TextOptimize(&vLabel[k]);
		C2D_TextParse(&hLabel[k], gStatic, hNames[k]);
		C2D_TextOptimize(&hLabel[k]);
	}
	C2D_TextParse(&btnHack, gStatic, "HACK LIGHTS");
	C2D_TextOptimize(&btnHack);
	C2D_TextParse(&btnBlackout, gStatic, "BLACKOUT");
	C2D_TextOptimize(&btnBlackout);

	// pause menu labels: main 0-4, weather 5-9, graphics toggles 10-15, camera styles 16-18
	static const char* menuNames[N_MENU_TEXT] = {
		"RESUME", "WEATHER", "GRAPHICS", "CAMERA", "TITLE",
		"SUNNY", "RAINY", "SUNSET", "NIGHT", "BACK",
		"FOG: ON", "FOG: OFF", "WINDOWS: ON", "WINDOWS: OFF", "FPS: ON", "FPS: OFF",
		"DEFAULT", "WATCH DOGS", "FIRST PERSON"
	};
	for (int i = 0; i < N_MENU_TEXT; i++) {
		C2D_TextParse(&txtMenu[i], gStatic, menuNames[i]);
		C2D_TextOptimize(&txtMenu[i]);
	}
	C2D_TextParse(&txtActive,   gStatic, "ACTIVE");
	C2D_TextParse(&txtPaused,   gStatic, "P A U S E D");
	C2D_TextParse(&txtWeatherT, gStatic, "W E A T H E R");
	C2D_TextParse(&txtGfxT,     gStatic, "G R A P H I C S");
	C2D_TextParse(&txtCamT,     gStatic, "C A M E R A");
	C2D_TextParse(&txtWheelT,   gStatic, "W E A P O N   W H E E L");
	C2D_TextParse(&txtRelease,  gStatic, "RELEASE X TO EQUIP");
	for (int i = 0; i < WP_COUNT; i++) { C2D_TextParse(&txtWp[i], gStatic, wdef[i].name); C2D_TextOptimize(&txtWp[i]); }
	C2D_TextParse(&txtSub,      gStatic, "// CHICAGO GRID NET");
	C2D_TextParse(&txtFooter,   gStatic, "B: RESUME     A: SELECT     D-PAD: MOVE");
	C2D_TextParse(&txtHint,     gStatic, "SELECT: PAUSE");
	C2D_TextOptimize(&txtActive);
	C2D_TextOptimize(&txtPaused);
	C2D_TextOptimize(&txtWeatherT);
	C2D_TextOptimize(&txtGfxT);
	C2D_TextOptimize(&txtCamT);
	C2D_TextOptimize(&txtWheelT);
	C2D_TextOptimize(&txtRelease);
	C2D_TextOptimize(&txtSub);
	C2D_TextOptimize(&txtFooter);
	C2D_TextOptimize(&txtHint);
}

static void fillClipped(float x0, float y0, float x1, float y1, u32 clr) {
	if (x0 < 0.0f) x0 = 0.0f;
	if (y0 < 0.0f) y0 = 0.0f;
	if (x1 > 320.0f) x1 = 320.0f;
	if (y1 > MAP_H) y1 = MAP_H;
	if (x1 > x0 && y1 > y0) C2D_DrawRectSolid(x0, y0, 0.5f, x1 - x0, y1 - y0, clr);
}

static void drawCentered(const C2D_Text* t, float cx, float y, float scale, u32 clr) {
	float w, h;
	C2D_TextGetDimensions(t, scale, scale, &w, &h);
	C2D_DrawText(t, C2D_WithColor, cx - w * 0.5f, y, 0.5f, scale, scale, clr);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int roadIdx(float v) { return clampi((int)floorf((v - ROAD_C(0)) / ROAD_SP + 0.5f), 0, ROADS - 1); }

static void drawMap(float px, float pz, int frame, int phoneTimer, float face) {
	const u32 cBg    = C2D_Color32(0x12, 0x15, 0x1d, 0xFF);
	const u32 cRoad  = C2D_Color32(0x4a, 0x51, 0x66, 0xFF);
	const u32 cMid   = C2D_Color32(0x6b, 0x73, 0x8c, 0xFF);
	const u32 cBlock = C2D_Color32(0x1c, 0x21, 0x2e, 0xFF);
	const u32 cPill  = C2D_Color32(0x00, 0x00, 0x00, 0xB4);
	const u32 cText  = C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF);
	const u32 cPink  = C2D_Color32(0xE0, 0x30, 0xE0, 0xFF);
	const u32 cWhite = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, MAP_H, cBg);

	// roads (the whole city ground), then the city blocks on top
	float ext = CITY_H + 12.0f;
	const u32 cPark  = C2D_Color32(0x1d, 0x4a, 0x2c, 0xFF);
	const u32 cPlaza = C2D_Color32(0x3a, 0x3f, 0x4c, 0xFF);
	const u32 cBld   = C2D_Color32(0x2f, 0x37, 0x4b, 0xFF);
	const u32 cTree  = C2D_Color32(0x2a, 0x7a, 0x3c, 0xFF);
	fillClipped(MAP_CX + (-ext - px) * MAP_S, MAP_CY + (-ext - pz) * MAP_S,
	            MAP_CX + ( ext - px) * MAP_S, MAP_CY + ( ext - pz) * MAP_S, cRoad);
	for (int i = 0; i < GRID; i++) {
		for (int j = 0; j < GRID; j++) {
			float bx = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)i, bz = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)j;
			int t = blkType[i][j];
			fillClipped(MAP_CX + (bx - BLK_H - px) * MAP_S, MAP_CY + (bz - BLK_H - pz) * MAP_S,
			            MAP_CX + (bx + BLK_H - px) * MAP_S, MAP_CY + (bz + BLK_H - pz) * MAP_S,
			            t == BT_PARK ? cPark : (t == BT_PLAZA ? cPlaza : cBlock));
		}
	}
	for (int i = 0; i < nBld; i++) {
		const Building* b = &bldg[i];
		if (b->kind == 1) {
			float cx = MAP_CX + ((b->x0 + b->x1) * 0.5f - px) * MAP_S, cy = MAP_CY + ((b->z0 + b->z1) * 0.5f - pz) * MAP_S;
			if (cx > 3.0f && cx < 317.0f && cy > 3.0f && cy < MAP_H - 3.0f)
				C2D_DrawRectSolid(cx - 2.0f, cy - 2.0f, 0.5f, 4.0f, 4.0f, cTree);
			continue;
		}
		fillClipped(MAP_CX + (b->x0 - px) * MAP_S, MAP_CY + (b->z0 - pz) * MAP_S,
		            MAP_CX + (b->x1 - px) * MAP_S, MAP_CY + (b->z1 - pz) * MAP_S, b->kind == 2 ? cPark : cBld);
	}
	// road centre lines
	for (int k = 0; k < ROADS; k++) {
		float sx = MAP_CX + (ROAD_C(k) - px) * MAP_S, sy = MAP_CY + (ROAD_C(k) - pz) * MAP_S;
		fillClipped(sx - 0.5f, MAP_CY + (-ext - pz) * MAP_S, sx + 0.5f, MAP_CY + (ext - pz) * MAP_S, cMid);
		fillClipped(MAP_CX + (-ext - px) * MAP_S, sy - 0.5f, MAP_CX + (ext - px) * MAP_S, sy + 0.5f, cMid);
	}

	// traffic
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float wx = (c->axis == 0) ? c->s : lane, wz = (c->axis == 0) ? lane : c->s;
		float sx = MAP_CX + (wx - px) * MAP_S, sy = MAP_CY + (wz - pz) * MAP_S;
		if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f)
			C2D_DrawRectSolid(sx - 1.5f, sy - 1.5f, 0.5f, 3.0f, 3.0f, C2D_Color32f(c->r, c->g, c->b, 1.0f));
	}
	// pedestrians
	for (int i = 0; i < N_PEDS; i++) {
		float wx, wz;
		pedPos(&peds[i], &wx, &wz);
		float sx = MAP_CX + (wx - px) * MAP_S, sy = MAP_CY + (wz - pz) * MAP_S;
		if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f)
			C2D_DrawRectSolid(sx - 1.0f, sy - 1.0f, 0.5f, 2.0f, 2.0f, cText);
	}

	// ammo crates
	for (int i = 0; i < N_CRATES; i++) {
		if (!crates[i].active) continue;
		float sx = MAP_CX + (crates[i].x - px) * MAP_S, sy = MAP_CY + (crates[i].z - pz) * MAP_S;
		if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f)
			C2D_DrawRectSolid(sx - 2.0f, sy - 2.0f, 0.5f, 4.0f, 4.0f, C2D_Color32(0xFF, 0xD8, 0x30, 0xFF));
	}

	// hacked traffic light marker
	if (hackTimer > 0) {
		float sx = MAP_CX + (ROAD_C(hackA) - px) * MAP_S, sy = MAP_CY + (ROAD_C(hackB) - pz) * MAP_S;
		if (sx > 18.0f && sx < 302.0f && sy > 18.0f && sy < MAP_H - 18.0f) {
			float r = 8.0f + (float)(frame % 30) * 0.2f;
			C2D_DrawCircleSolid(sx, sy, 0.5f, r, cPink);
			C2D_DrawCircleSolid(sx, sy, 0.5f, r - 3.0f, cRoad);
			C2D_DrawCircleSolid(sx, sy, 0.5f, 3.0f, cPink);
		}
	}

	// street names: vertical streets along the top edge, horizontal streets along the left edge
	for (int k = 0; k < ROADS; k++) {
		float w, h;
		float sx = MAP_CX + (ROAD_C(k) - px) * MAP_S;
		C2D_TextGetDimensions(&vLabel[k], 0.38f, 0.38f, &w, &h);
		float lx = sx - w * 0.5f;
		if (lx > 2.0f && lx + w < 318.0f) {
			C2D_DrawRectSolid(lx - 3.0f, 2.0f, 0.5f, w + 6.0f, h + 3.0f, cPill);
			C2D_DrawText(&vLabel[k], C2D_WithColor, lx, 3.0f, 0.5f, 0.38f, 0.38f, cText);
		}
		float sy = MAP_CY + (ROAD_C(k) - pz) * MAP_S;
		C2D_TextGetDimensions(&hLabel[k], 0.38f, 0.38f, &w, &h);
		if (sy > 22.0f && sy < MAP_H - 10.0f) {
			C2D_DrawRectSolid(3.0f, sy - h * 0.5f - 1.0f, 0.5f, w + 6.0f, h + 2.0f, cPill);
			C2D_DrawText(&hLabel[k], C2D_WithColor, 6.0f, sy - h * 0.5f, 0.5f, 0.38f, 0.38f, cText);
		}
	}

	// current weapon and ammo
	if (curWeapon != WP_FISTS) {
		char ab[16];
		C2D_Text ta;
		snprintf(ab, sizeof ab, "%d", ammo[curWeapon]);
		C2D_TextParse(&ta, gDyn, ab);
		C2D_TextOptimize(&ta);
		C2D_DrawRectSolid(206.0f, 22.0f, 0.5f, 110.0f, 31.0f, cPill);
		C2D_DrawText(&txtWp[curWeapon], C2D_WithColor, 212.0f, 24.0f, 0.5f, 0.36f, 0.36f, cText);
		C2D_DrawText(&ta, C2D_WithColor, 212.0f, 34.0f, 0.5f, 0.5f, 0.5f,
		             ammo[curWeapon] > 0 ? cText : C2D_Color32(0xFF, 0x50, 0x50, 0xFF));
	}

	// pause hint
	{
		float w, h;
		C2D_TextGetDimensions(&txtHint, 0.34f, 0.34f, &w, &h);
		C2D_DrawRectSolid(317.0f - w - 8.0f, MAP_H - h - 7.0f, 0.5f, w + 8.0f, h + 4.0f, cPill);
		C2D_DrawText(&txtHint, C2D_WithColor, 317.0f - w - 4.0f, MAP_H - h - 5.0f, 0.5f, 0.34f, 0.34f, cText);
	}
	// frame-rate readout (pause menu > GRAPHICS > FPS)
	if (gFps) {
		char fb[16];
		C2D_Text tf;
		snprintf(fb, sizeof fb, "%d FPS", gFpsValue);
		C2D_TextParse(&tf, gDyn, fb);
		C2D_TextOptimize(&tf);
		float w, h;
		C2D_TextGetDimensions(&tf, 0.34f, 0.34f, &w, &h);
		C2D_DrawRectSolid(3.0f, MAP_H - h - 7.0f, 0.5f, w + 8.0f, h + 4.0f, cPill);
		C2D_DrawText(&tf, C2D_WithColor, 7.0f, MAP_H - h - 5.0f, 0.5f, 0.34f, 0.34f,
		             gFpsValue >= 55 ? C2D_Color32(0x70, 0xFF, 0x90, 0xFF) : C2D_Color32(0xFF, 0xC0, 0x50, 0xFF));
	}

	// the player: blue circle with a white outline and a heading arrow, always at the map centre
	{
		float dx = -sinf(face), dy = -cosf(face), nx = -dy, ny = dx;
		C2D_DrawTriangle(MAP_CX + dx * 15.0f, MAP_CY + dy * 15.0f, cWhite,
		                 MAP_CX + dx * 6.0f + nx * 6.0f, MAP_CY + dy * 6.0f + ny * 6.0f, cWhite,
		                 MAP_CX + dx * 6.0f - nx * 6.0f, MAP_CY + dy * 6.0f - ny * 6.0f, cWhite, 0.5f);
	}
	C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 8.5f, cWhite);
	C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 6.5f, C2D_Color32(0x1E, 0x7B, 0xFF, 0xFF));

	// hack buttons (greyed out while the phone animation plays)
	int busy = phoneTimer > 0;
	C2D_DrawRectSolid(0.0f, MAP_H, 0.5f, 320.0f, 240.0f - MAP_H, C2D_Color32(0x0b, 0x0d, 0x12, 0xFF));
	C2D_DrawRectSolid(6.0f,   (float)BTN_Y0, 0.5f, 150.0f, (float)(BTN_Y1 - BTN_Y0 - 2),
	                  busy ? C2D_Color32(0x33, 0x36, 0x40, 0xFF) : C2D_Color32(0x1f, 0x6f, 0x8f, 0xFF));
	C2D_DrawRectSolid(164.0f, (float)BTN_Y0, 0.5f, 150.0f, (float)(BTN_Y1 - BTN_Y0 - 2),
	                  busy ? C2D_Color32(0x33, 0x36, 0x40, 0xFF) : C2D_Color32(0x5a, 0x2a, 0x7a, 0xFF));
	drawCentered(&btnHack,     81.0f,  (float)BTN_Y0 + 3.0f, 0.45f, cText);
	drawCentered(&btnBlackout, 239.0f, (float)BTN_Y0 + 3.0f, 0.45f, cText);

	// where in Chicago the player is
	int ni = clampi((int)floorf((px + CITY_H) / CELL_W), 0, 3);
	int nj = clampi((int)floorf((pz + CITY_H) / CELL_W), 0, 3);
	int a  = roadIdx(px);
	int b  = roadIdx(pz);
	char line1[64], line2[96];
	snprintf(line1, sizeof line1, "%s, Chicago, IL", hoods[nj][ni]);
	snprintf(line2, sizeof line2, "Near %s & %s", vNames[a], hNames[b]);
	C2D_Text t1, t2;
	C2D_TextParse(&t1, gDyn, line1);
	C2D_TextOptimize(&t1);
	C2D_TextParse(&t2, gDyn, line2);
	C2D_TextOptimize(&t2);
	drawCentered(&t1, 160.0f, (float)BTN_Y1 + 1.0f,  0.55f, cText);
	drawCentered(&t2, 160.0f, (float)BTN_Y1 + 19.0f, 0.40f, C2D_Color32(0x9a, 0xa3, 0xb8, 0xFF));
}

static void drawTitleBottom(int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0x0b, 0x0d, 0x14, 0xFF));
	C2D_Text t1, t2, t3, t4, t5, t6;
	{
		const char* msg = gMusicMode == 1 ? "MUSIC: title.pcm" :
		                  gMusicMode == 2 ? "MUSIC: built-in theme  (title.pcm not found)" :
		                  gMusicMode == 3 ? "NO SOUND: put dspfirm.cdc in SD:/3ds/" : "MUSIC: starting...";
		C2D_TextParse(&t6, gDyn, msg);
		C2D_TextOptimize(&t6);
		drawCentered(&t6, 160.0f, 6.0f, 0.34f, gMusicMode == 3 ? C2D_Color32(0xFF, 0x80, 0x70, 0xFF) : C2D_Color32(0x6b, 0x73, 0x8c, 0xFF));
	}
	C2D_TextParse(&t1, gDyn, "CHICAGO 3DS");
	C2D_TextParse(&t2, gDyn, "a low-poly hacking sandbox");
	C2D_TextParse(&t3, gDyn, "PRESS A or TAP TO START");
	C2D_TextParse(&t4, gDyn, "START = quit");
	C2D_TextParse(&t5, gDyn, "SELECT = pause menu in game");
	C2D_TextOptimize(&t1); C2D_TextOptimize(&t2); C2D_TextOptimize(&t3); C2D_TextOptimize(&t4); C2D_TextOptimize(&t5);
	drawCentered(&t1, 160.0f, 62.0f,  1.2f, C2D_Color32(0x4a, 0x9d, 0xff, 0xFF));
	drawCentered(&t2, 160.0f, 112.0f, 0.5f, C2D_Color32(0x9a, 0xa3, 0xb8, 0xFF));
	if ((frame / 30) % 2 == 0)
		drawCentered(&t3, 160.0f, 160.0f, 0.6f, C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF));
	drawCentered(&t5, 160.0f, 196.0f, 0.4f, C2D_Color32(0x6b, 0x73, 0x8c, 0xFF));
	drawCentered(&t4, 160.0f, 214.0f, 0.4f, C2D_Color32(0x6b, 0x73, 0x8c, 0xFF));
}

// ---- weapon wheel (bottom screen, hold X) -----------------------------------
#define WH_W 76.0f
#define WH_H 36.0f
static void wheelTile(int i, float* x, float* y) {
	float a = (-90.0f + 72.0f * (float)i) * 0.0174533f;
	*x = 160.0f + 78.0f * cosf(a);
	*y = 112.0f + 78.0f * sinf(a);
}

// ---- pause menu (bottom screen only) ------------------------------------------
// Menus: 0 main (RESUME, WEATHER, GRAPHICS, TITLE), 1 weather (SUNNY, RAINY, SUNSET, NIGHT, BACK),
// 2 graphics (FOG, WINDOWS, FPS, BACK). B always resumes.
static int menuCount(int state) { return (state == 0 || state == 1) ? 5 : 4; }

static void menuRect(int state, int i, float* y0, float* h) {
	float sp = (menuCount(state) <= 4) ? 36.0f : 30.0f;
	*y0 = 60.0f + (float)i * sp;
	*h  = sp - 6.0f;
}

static const C2D_Text* menuText(int state, int i) {
	if (state == 0) return &txtMenu[i];                 // RESUME WEATHER GRAPHICS CAMERA TITLE
	if (state == 1) return &txtMenu[5 + i];             // SUNNY RAINY SUNSET NIGHT BACK
	if (state == 2) {
		if (i == 0) return gFog ? &txtMenu[10] : &txtMenu[11];
		if (i == 1) return gWin ? &txtMenu[12] : &txtMenu[13];
		if (i == 2) return gFps ? &txtMenu[14] : &txtMenu[15];
		return &txtMenu[9];
	}
	return i < 3 ? &txtMenu[16 + i] : &txtMenu[9];      // DEFAULT WATCH DOGS FIRST PERSON BACK
}

static u32 neon(int r, int g, int b, float a) {
	if (a < 0.0f) a = 0.0f;
	if (a > 1.0f) a = 1.0f;
	return C2D_Color32(r, g, b, (int)(a * 255.0f));
}

static void frameRect(float x, float y, float w, float h, float th, u32 clr) {
	C2D_DrawRectSolid(x, y, 0.5f, w, th, clr);
	C2D_DrawRectSolid(x, y + h - th, 0.5f, w, th, clr);
	C2D_DrawRectSolid(x, y, 0.5f, th, h, clr);
	C2D_DrawRectSolid(x + w - th, y, 0.5f, th, h, clr);
}

static void brackets(float x, float y, float w, float h, float len, float th, u32 clr) {
	C2D_DrawRectSolid(x, y, 0.5f, len, th, clr);                 C2D_DrawRectSolid(x, y, 0.5f, th, len, clr);
	C2D_DrawRectSolid(x + w - len, y, 0.5f, len, th, clr);       C2D_DrawRectSolid(x + w - th, y, 0.5f, th, len, clr);
	C2D_DrawRectSolid(x, y + h - th, 0.5f, len, th, clr);        C2D_DrawRectSolid(x, y + h - len, 0.5f, th, len, clr);
	C2D_DrawRectSolid(x + w - len, y + h - th, 0.5f, len, th, clr);
	C2D_DrawRectSolid(x + w - th, y + h - len, 0.5f, th, len, clr);
}

// dim: 0 = no menu, 1 = fully open. It dims the map underneath and fades the menu in and out.
static void drawPause(float dim, int state, int sel, int active, int frame) {
	if (dim < 0.01f) return;
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(2, 6, 14, 0.80f * dim));
	for (float y = 2.0f; y < 240.0f; y += 6.0f) // scanlines
		C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.05f * dim));
	float by = fmodf((float)frame * 1.6f, 270.0f) - 15.0f; // sweeping scan bar
	C2D_DrawRectSolid(0.0f, by, 0.5f, 320.0f, 3.0f, neon(0, 229, 255, 0.09f * dim));
	brackets(8.0f, 8.0f, 304.0f, 224.0f, 20.0f, 2.0f, neon(0, 229, 255, 0.95f * dim));
	brackets(14.0f, 14.0f, 292.0f, 212.0f, 8.0f, 1.0f, neon(255, 46, 209, 0.7f * dim));

	drawCentered(state == 0 ? &txtPaused : (state == 1 ? &txtWeatherT : (state == 2 ? &txtGfxT : &txtCamT)), 160.0f, 20.0f, 0.8f, neon(0, 229, 255, dim));
	C2D_DrawRectSolid(50.0f, 52.0f, 0.5f, 220.0f, 1.0f, neon(0, 229, 255, 0.6f * dim));
	C2D_DrawRectSolid(50.0f, 51.0f, 0.5f, 44.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));
	if (state == 0) drawCentered(&txtSub, 160.0f, 55.0f, 0.30f, neon(255, 46, 209, 0.9f * dim));

	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.12f);
	int n = menuCount(state);
	for (int i = 0; i < n; i++) {
		float y0, h, x0 = 50.0f, w = 220.0f;
		menuRect(state, i, &y0, &h);
		int on = (i == sel);
		C2D_DrawRectSolid(x0, y0, 0.5f, w, h, on ? neon(0, 229, 255, (0.18f + 0.07f * pulse) * dim)
		                                         : neon(8, 24, 40, 0.80f * dim));
		frameRect(x0, y0, w, h, 1.0f, on ? neon(0, 229, 255, dim) : neon(0, 150, 180, 0.6f * dim));
		C2D_DrawRectSolid(x0, y0, 0.5f, on ? 6.0f : 2.0f, h, on ? neon(255, 46, 209, dim) : neon(0, 229, 255, 0.7f * dim));
		if (on) C2D_DrawRectSolid(x0 + w - 18.0f, y0 + h - 3.0f, 0.5f, 18.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));

		const C2D_Text* t = menuText(state, i);
		float tw, th;
		C2D_TextGetDimensions(t, 0.55f, 0.55f, &tw, &th);
		C2D_DrawText(t, C2D_WithColor, x0 + 20.0f, y0 + (h - th) * 0.5f, 0.5f, 0.55f, 0.55f,
		             on ? neon(255, 255, 255, dim) : neon(120, 220, 235, 0.9f * dim));

		if (((state == 1 && i < 4) || (state == 3 && i < 3)) && i == active) { // current weather / camera tag
			float aw, ah;
			C2D_TextGetDimensions(&txtActive, 0.38f, 0.38f, &aw, &ah);
			C2D_DrawText(&txtActive, C2D_WithColor, x0 + w - aw - 12.0f, y0 + (h - ah) * 0.5f, 0.5f, 0.38f, 0.38f,
			             neon(255, 46, 209, dim));
		}
	}
	drawCentered(&txtFooter, 160.0f, 213.0f, 0.34f, neon(120, 220, 235, 0.85f * dim));
}

static void drawWheel(float amt, int sel, int cur, int frame) {
	if (amt < 0.01f) return;
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(2, 6, 14, 0.82f * amt));
	for (float y = 2.0f; y < 240.0f; y += 6.0f)
		C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.05f * amt));
	drawCentered(&txtWheelT, 160.0f, 1.0f, 0.34f, neon(0, 229, 255, amt));
	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.12f);
	for (int i = 0; i < WP_COUNT; i++) {
		float x, y;
		wheelTile(i, &x, &y);
		float x0 = x - WH_W * 0.5f, y0 = y - WH_H * 0.5f;
		int on = (i == sel);
		C2D_DrawRectSolid(x0, y0, 0.5f, WH_W, WH_H, on ? neon(0, 229, 255, (0.22f + 0.08f * pulse) * amt) : neon(8, 24, 40, 0.88f * amt));
		frameRect(x0, y0, WH_W, WH_H, 1.0f, on ? neon(0, 229, 255, amt) : neon(0, 150, 180, 0.6f * amt));
		if (i == cur) C2D_DrawRectSolid(x0, y0, 0.5f, 4.0f, WH_H, neon(255, 46, 209, amt));
		C2D_DrawText(&txtWp[i], C2D_WithColor, x0 + 8.0f, y0 + 3.0f, 0.5f, 0.38f, 0.38f,
		             on ? neon(255, 255, 255, amt) : neon(120, 220, 235, 0.9f * amt));
		char ab[16];
		C2D_Text ta;
		if (i == WP_FISTS) snprintf(ab, sizeof ab, "-"); else snprintf(ab, sizeof ab, "%d", ammo[i]);
		C2D_TextParse(&ta, gDyn, ab);
		C2D_TextOptimize(&ta);
		C2D_DrawText(&ta, C2D_WithColor, x0 + 8.0f, y0 + 15.0f, 0.5f, 0.52f, 0.52f,
		             (i != WP_FISTS && ammo[i] == 0) ? neon(255, 80, 80, amt) : neon(255, 255, 255, 0.95f * amt));
	}
	drawCentered(&txtRelease, 160.0f, 104.0f, 0.30f, neon(255, 46, 209, 0.9f * amt));
}

// ---- boot: developer logo sequence ---------------------------------------------
// Each logo fades in, holds, then fades slowly to black before the next one. These are original
// placeholder studios - change the names / tag lines below to your own. A or START skips.
#define LOGO_COUNT 3
static const char* logoName[LOGO_COUNT] = {"NORTHBRANCH GAMES", "GRIDLINE STUDIOS", "CHICAGO 3DS"};
static const char* logoTag[LOGO_COUNT]  = {"DEVELOPER", "TECHNOLOGY", "A HOMEBREW PRODUCTION"};

static float smooth01(float x) {
	x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
	return x * x * (3.0f - 2.0f * x);
}

static void drawEmblem(int idx, float cx, float cy, float a) {
	if (idx == 0) { // ring with a diagonal slash
		C2D_DrawCircleSolid(cx, cy, 0.5f, 36.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a));
		C2D_DrawCircleSolid(cx, cy, 0.5f, 28.0f, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
		C2D_DrawTriangle(cx - 26.0f, cy + 22.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a),
		                 cx - 14.0f, cy + 30.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a),
		                 cx + 26.0f, cy - 22.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a), 0.5f);
		C2D_DrawTriangle(cx + 26.0f, cy - 22.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a),
		                 cx + 14.0f, cy - 30.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a),
		                 cx - 14.0f, cy + 30.0f, C2D_Color32f(0.95f, 0.45f, 0.15f, a), 0.5f);
	} else if (idx == 1) { // 3x3 grid
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++) {
				float k = ((r + c) % 2) ? 1.0f : 0.55f;
				C2D_DrawRectSolid(cx - 33.0f + c * 24.0f, cy - 33.0f + r * 24.0f, 0.5f, 18.0f, 18.0f,
				                  C2D_Color32f(0.25f * k, 0.75f * k, 0.95f * k, a));
			}
	} else { // skyline
		static const float hgt[7] = {26.0f, 44.0f, 34.0f, 62.0f, 40.0f, 50.0f, 24.0f};
		for (int i = 0; i < 7; i++)
			C2D_DrawRectSolid(cx - 42.0f + i * 12.5f, cy + 32.0f - hgt[i], 0.5f, 10.0f, hgt[i], C2D_Color32f(0.85f, 0.88f, 0.95f, a));
	}
}

static void runSplash(C3D_RenderTarget* top, C3D_RenderTarget* bottom) {
	const float T_IN = 0.9f, T_HOLD = 1.7f, T_OUT = 1.1f, T_GAP = 0.5f, T_LOGO = T_IN + T_HOLD + T_OUT + T_GAP;
	C2D_TextBuf tb = C2D_TextBufNew(512);
	u64 t0 = osGetTime();
	while (aptMainLoop()) {
		hidScanInput();
		if (hidKeysDown() & (KEY_A | KEY_START)) break;
		float el = (float)(osGetTime() - t0) / 1000.0f;
		int idx = (int)(el / T_LOGO);
		if (idx >= LOGO_COUNT) break;
		float lt = el - (float)idx * T_LOGO;
		float a = lt < T_IN ? smooth01(lt / T_IN)
		        : lt < T_IN + T_HOLD ? 1.0f
		        : lt < T_IN + T_HOLD + T_OUT ? 1.0f - smooth01((lt - T_IN - T_HOLD) / T_OUT) : 0.0f;
		u8 a8 = (u8)(a * 255.0f);

		C2D_TextBufClear(tb);
		C2D_Text tn, tt, tl, th;
		char dots[24];
		int nd = (int)(el * 2.5f) % 4;
		snprintf(dots, sizeof dots, "LOADING%.*s", nd, "...");
		C2D_TextParse(&tn, tb, logoName[idx]);
		C2D_TextParse(&tt, tb, logoTag[idx]);
		C2D_TextParse(&tl, tb, dots);
		C2D_TextParse(&th, tb, "A / START: SKIP");
		C2D_TextOptimize(&tn); C2D_TextOptimize(&tt); C2D_TextOptimize(&tl); C2D_TextOptimize(&th);

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_Prepare();
		C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
		C2D_SceneBegin(top);
		drawEmblem(idx, 200.0f, 84.0f, a);
		drawCentered(&tn, 200.0f, 132.0f, 0.85f, C2D_Color32(255, 255, 255, a8));
		drawCentered(&tt, 200.0f, 170.0f, 0.42f, C2D_Color32(150, 160, 185, a8));

		C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 0xFF));
		C2D_SceneBegin(bottom);
		float frac = el / ((float)LOGO_COUNT * T_LOGO);
		if (frac > 1.0f) frac = 1.0f;
		C2D_DrawRectSolid(60.0f, 122.0f, 0.5f, 200.0f, 4.0f, C2D_Color32(40, 44, 56, 0xFF));
		C2D_DrawRectSolid(60.0f, 122.0f, 0.5f, 200.0f * frac, 4.0f, C2D_Color32(0, 229, 255, 0xFF));
		drawCentered(&tl, 160.0f, 96.0f, 0.5f, C2D_Color32(200, 210, 230, 0xFF));
		drawCentered(&th, 160.0f, 214.0f, 0.34f, C2D_Color32(90, 96, 112, 0xFF));
		C2D_Flush();
		C3D_FrameEnd(0);
	}
	C2D_TextBufDelete(tb);
}

static void envPlain(void) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
}

static void envTextured(void) {
	C3D_TexEnv* env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
}

static void drawRange(int first, int count) {
	if (count > 0) C3D_DrawArrays(GPU_TRIANGLES, first, count);
}

// Draws the visible districts; neighbouring visible districts are merged into one draw call.
static void drawCells(const int* st, const int* cnt, const unsigned char* vis) {
	int rs = 0, rc = 0;
	for (int c = 0; c < CELLS; c++) {
		if (!vis[c] || cnt[c] == 0) continue;
		if (rc > 0 && rs + rc == st[c]) rc += cnt[c];
		else { drawRange(rs, rc); rs = st[c]; rc = cnt[c]; }
	}
	drawRange(rs, rc);
}

int main(int argc, char** argv) {
	gfxInitDefault();
	osSetSpeedupEnable(true); // New 3DS: 804 MHz CPU + extra cache (ignored on an Old 3DS)
	romfsInit();              // title music lives in romfs:/title.pcm
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	initMapText();

	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	DVLB_s* dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgram_s program;
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	int uProj   = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	int uMV     = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
	int uTint   = shaderInstanceGetUniformLocation(program.vertexShader, "tint");
	int uFog    = shaderInstanceGetUniformLocation(program.vertexShader, "fog");
	int uFogClr = shaderInstanceGetUniformLocation(program.vertexShader, "fogClr");

	// kept in our own structs: citro2d replaces the GPU's current attribute/buffer state
	C3D_AttrInfo attr;
	AttrInfo_Init(&attr);
	AttrInfo_AddLoader(&attr, 0, GPU_FLOAT, 3); // position
	AttrInfo_AddLoader(&attr, 1, GPU_FLOAT, 4); // colour
	AttrInfo_AddLoader(&attr, 2, GPU_FLOAT, 2); // texture coordinates

	vbo = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_VERTS);
	texOK = initWindowTex();
	if (!texOK) gWin = 0;
	initStars();
	buildCity();
	staticCount = vcount;
	GSPGPU_FlushDataCache(vbo, staticCount * sizeof(Vertex));
	initCars();
	initPeds();
	initCrates();
	for (int w = 0; w < WP_COUNT; w++) ammo[w] = wdef[w].startAmmo;
	initAudio();

	C3D_BufInfo buf;
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, vbo, sizeof(Vertex), 3, 0x210);

	C3D_Mtx proj, ortho, ident;
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true); // lens droplets, HUD: pixel coordinates
	Mtx_Identity(&ident);

	runSplash(top, bottom); // developer logos, each fading to black

	// start on a road intersection
	float px = 0.0f, pz = 0.0f, yaw = 0.0f, pitch = 0.3f;
	int   hackFrames = 0, frame = 0;
	int   title = 1;
	float titleFade = 0.0f; // title screen fades in from black
	float orbit = 0.0f;
	float face = 0.0f, phase = 0.0f, moveAmt = 0.0f; // character heading, step cycle, 0 idle / 1 walk / 2 run
	int   phoneTimer = 0, pending = 0;               // phone animation frames left, queued hack (1 lights, 2 blackout)
	int   lastTouchX = -1, lastTouchY = -1;
	int   paused = 0, menuState = 0, sel = 0, sim = 0; // pause menu; sim = simulation clock (stops while paused)
	float pauseDim = 0.0f;
	float aimAmt = 0.0f, wheelAmt = 0.0f;              // aim camera blend, weapon wheel fade
	int   wheelSel = curWeapon, wheelWas = 0, lastStep = 0;
	int   fpsFrames = 0;
	u64   fpsT0 = osGetTime();
	musicWant = 1;                                     // title music starts now

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (title && (down & KEY_START)) break; // START quits from the title screen

		circlePosition cp;
		hidCircleRead(&cp);
		circlePosition cs; // C-stick (New 3DS); reads zero on an Old 3DS
		hidCstickRead(&cs);
		touchPosition tp;
		hidTouchRead(&tp);

		if (title) {
			if (down & (KEY_A | KEY_TOUCH)) {
				title = 0; musicWant = 0; // the music fades out, the city ambience fades in
				px = 0.0f; pz = 0.0f; yaw = 0.0f; pitch = 0.3f;
				face = 0.0f; phoneTimer = 0; pending = 0;
				hackFrames = 0; frame = 0; aimAmt = 0.0f;
			}
			held = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; // ignore game input on the title screen
		}

		// pause menu: SELECT toggles, D-Pad / touch select, A confirm, B resume
		if (!title && (down & KEY_SELECT)) {
			if (paused) paused = 0;
			else { paused = 1; menuState = 0; sel = 0; }
		}
		if (paused) {
			int n = menuCount(menuState), choose = (down & KEY_A) ? 1 : 0;
			if (down & KEY_B) paused = 0;
			if (down & KEY_DDOWN) sel = (sel + 1) % n;
			if (down & KEY_DUP)   sel = (sel + n - 1) % n;
			if (down & KEY_TOUCH) {
				for (int i = 0; i < n; i++) {
					float y0, h;
					menuRect(menuState, i, &y0, &h);
					if (tp.px >= 50 && tp.px <= 270 && tp.py >= y0 && tp.py < y0 + h) { sel = i; choose = 1; }
				}
			}
			if (choose && paused) {
				if (menuState == 0) {
					if (sel == 0)      paused = 0;
					else if (sel == 1) { menuState = 1; sel = weather; }
					else if (sel == 2) { menuState = 2; sel = 0; }
					else if (sel == 3) { menuState = 3; sel = camStyle; }
					else { title = 1; paused = 0; menuState = 0; musicWant = 1; titleFade = 0.0f; wheelAmt = 0.0f; } // TITLE
				} else if (menuState == 1) {
					if (sel < 4) weather = sel;                  // SUNNY / RAINY / SUNSET / NIGHT
					else         { menuState = 0; sel = 1; }     // BACK
				} else if (menuState == 2) {
					if (sel == 0)      gFog = !gFog;
					else if (sel == 1) { if (texOK) gWin = !gWin; }
					else if (sel == 2) gFps = !gFps;
					else               { menuState = 0; sel = 2; } // BACK
				} else {
					if (sel < 3) camStyle = sel;                 // DEFAULT / WATCH DOGS / FIRST PERSON
					else         { menuState = 0; sel = 3; }     // BACK
				}
			}
			held = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; // game input is frozen while paused
		}
		pauseDim += ((paused ? 1.0f : 0.0f) - pauseDim) * 0.25f;
		if (pauseDim < 0.01f) pauseDim = 0.0f;
		updateWeather();
		updateAudio(!title, paused);
		if (title) titleFade += (1.0f - titleFade) * 0.04f;

		if (!title && !paused && (down & KEY_Y)) camStyle = (camStyle + 1) % 3; // quick camera switch

		// weapon wheel: hold X, pick with the Circle Pad or by touch, release to equip
		int wheelOpen = (!title && !paused && (held & KEY_X)) ? 1 : 0;
		if (wheelOpen && !wheelWas) wheelSel = curWeapon;
		if (!wheelOpen && wheelWas && wheelSel != curWeapon) {
			curWeapon = wheelSel; fireCool = 12;
			playSfx(SFX_SWITCH, 0.6f);
		}
		wheelWas = wheelOpen;
		wheelAmt += ((wheelOpen ? 1.0f : 0.0f) - wheelAmt) * 0.3f;
		if (wheelAmt < 0.01f) wheelAmt = 0.0f;
		if (wheelOpen) {
			float mx = (float)cp.dx, my = -(float)cp.dy; // screen coordinates, y down
			if (mx * mx + my * my > 55.0f * 55.0f) {
				float bd = -1e9f;
				for (int i = 0; i < WP_COUNT; i++) {
					float a = (-90.0f + 72.0f * (float)i) * 0.0174533f, dd = mx * cosf(a) + my * sinf(a);
					if (dd > bd) { bd = dd; wheelSel = i; }
				}
			}
			if (down & KEY_TOUCH) {
				for (int i = 0; i < WP_COUNT; i++) {
					float x, y;
					wheelTile(i, &x, &y);
					if (fabsf((float)tp.px - x) < WH_W * 0.5f && fabsf((float)tp.py - y) < WH_H * 0.5f) wheelSel = i;
				}
			}
			cp.dx = 0; cp.dy = 0; // the stick picks a weapon instead of moving the character
		}

		// aiming: hold L / ZL, or just fire - the camera goes over the shoulder and the crosshair appears
		int armed = (curWeapon != WP_FISTS);
		int wantAim = (!title && !paused && !wheelOpen && armed && (held & (KEY_L | KEY_ZL | KEY_R | KEY_ZR))) ? 1 : 0;
		aimAmt += ((wantAim ? 1.0f : 0.0f) - aimAmt) * 0.22f;
		if (aimAmt < 0.01f) aimAmt = 0.0f;
		float lookScale = 1.0f - 0.45f * aimAmt;

		if (held & KEY_DLEFT)  yaw += 0.04f * lookScale;
		if (held & KEY_DRIGHT) yaw -= 0.04f * lookScale;
		if (held & KEY_DUP)    pitch -= 0.03f * lookScale;
		if (held & KEY_DDOWN)  pitch += 0.03f * lookScale;
		if (abs(cs.dx) > 15) yaw   -= cs.dx / 156.0f * 0.05f * lookScale;
		if (abs(cs.dy) > 15) pitch -= cs.dy / 156.0f * 0.04f * lookScale;

		// touch: bottom strip = HACK button, elsewhere = drag to look
		if ((held & KEY_TOUCH) && !wheelOpen) {
			if (tp.py >= BTN_Y0) {
				if (down & KEY_TOUCH) {
					if (phoneTimer == 0 && tp.py < BTN_Y1) {
						pending = (tp.px < 160) ? 1 : 2; // 1 = hack lights, 2 = blackout
						phoneTimer = 120;                // character pulls out the phone
					}
				}
			} else if (lastTouchX >= 0) {
				yaw   -= (tp.px - lastTouchX) * 0.01f * lookScale;
				pitch += (tp.py - lastTouchY) * 0.01f * lookScale;
			}
			lastTouchX = tp.px; lastTouchY = tp.py;
		} else {
			lastTouchX = lastTouchY = -1;
		}
		float pMin = (camStyle == 2) ? -1.3f : -0.3f, pMax = (camStyle == 2) ? 1.3f : 1.2f;
		if (pitch > pMax) pitch = pMax;
		if (pitch < pMin) pitch = pMin;

		float fx = (cp.dy / 156.0f), sx = (cp.dx / 156.0f);
		if (fabsf(fx) < 0.1f) fx = 0;
		if (fabsf(sx) < 0.1f) sx = 0;
		float len = sqrtf(fx * fx + sx * sx);
		if (len > 1.0f) { fx /= len; sx /= len; len = 1.0f; }
		if (phoneTimer > 0) { fx = 0.0f; sx = 0.0f; len = 0.0f; } // standing still while on the phone
		int aimFace = (aimAmt > 0.5f) || (camStyle == 2);        // aiming / first person: body follows the camera
		int running = (held & KEY_B) && len > 0.5f && aimAmt < 0.3f;
		float speed = running ? 0.17f : (aimAmt > 0.3f ? 0.045f : 0.07f);
		float dirx = -sinf(yaw), dirz = -cosf(yaw);
		float rgtx =  cosf(yaw), rgtz = -sinf(yaw);
		float nx = px + (dirx * fx + rgtx * sx) * speed;
		float nz = pz + (dirz * fx + rgtz * sx) * speed;
		if (!blocked(nx, pz)) px = nx;
		if (!blocked(px, nz)) pz = nz;

		// walking / running animation state
		if (aimFace) {
			float dlt = yaw - face;
			while (dlt >  3.14159f) dlt -= 6.28318f;
			while (dlt < -3.14159f) dlt += 6.28318f;
			face += (camStyle == 2) ? dlt : dlt * 0.35f;
		} else if (len > 0.0f) {
			float mx = dirx * fx + rgtx * sx, mz = dirz * fx + rgtz * sx;
			float dlt = atan2f(-mx, -mz) - face;
			while (dlt >  3.14159f) dlt -= 6.28318f;
			while (dlt < -3.14159f) dlt += 6.28318f;
			face += dlt * 0.25f;
		}
		if (len > 0.0f) {
			phase += speed * 0.6f + 0.06f;
			int stepNo = (int)floorf(phase / 3.14159f);
			if (stepNo != lastStep) { lastStep = stepNo; playSfx(SFX_STEP, running ? 0.35f : 0.20f); }
		}
		moveAmt += ((len > 0.0f ? (running ? 2.0f : 1.0f) : 0.0f) - moveAmt) * 0.15f;

		// phone animation: the hack fires once the phone is raised
		if (phoneTimer > 0 && !paused) {
			phoneTimer--;
			if (phoneTimer == 80) {
				if (pending == 1) {
					hackA = roadIdx(px);
					hackB = roadIdx(pz);
					hackTimer = 600;
				} else if (pending == 2) {
					hackFrames = 180;
				}
				pending = 0;
			}
		}

		if (!paused) {
			if (hackFrames > 0) hackFrames--;
			if (!wheelOpen || (sim & 3) == 0) { // the world slows to a crawl while the wheel is open
				updateCars(sim, title ? 1e6f : px, title ? 1e6f : pz);
				updatePeds(sim);
			}
			sim++;
		}

		// ---- camera ----
		static const float baseDist[3] = {6.5f, 3.9f, 0.0f}, aimDist[3] = {2.8f, 2.4f, 0.0f};
		static const float baseSh[3]   = {0.0f, 0.75f, 0.0f}, aimSh[3]   = {0.8f, 0.95f, 0.0f};
		static const float baseH[3]    = {1.6f, 1.9f, 1.65f},  aimH[3]    = {1.75f, 1.85f, 1.65f};
		float cyaw = yaw, cpitch = pitch - kick;
		float cx, cy, cz;
		{
			float dist0 = baseDist[camStyle] + (aimDist[camStyle] - baseDist[camStyle]) * aimAmt;
			float sh    = baseSh[camStyle]   + (aimSh[camStyle]   - baseSh[camStyle])   * aimAmt;
			float hh    = baseH[camStyle]    + (aimH[camStyle]    - baseH[camStyle])    * aimAmt;
			float flat = cosf(cpitch), sn = sinf(yaw), cs2 = cosf(yaw), dist = dist0;
			for (;;) { // pull in if the camera would end up inside a building
				cx = px + flat * sn * dist + cs2 * sh;
				cz = pz + flat * cs2 * dist - sn * sh;
				if (dist <= 1.0f || !blocked(cx, cz)) break;
				dist -= 0.4f;
			}
			if (camStyle == 2 || blocked(cx, cz)) { cx = px; cz = pz; dist = 0.0f; }
			cy = (camStyle == 2) ? 1.62f + fabsf(cosf(phase)) * 0.035f * moveAmt : hh + sinf(cpitch) * dist;
			if (cy < 0.4f) cy = 0.4f;
		}
		if (title) { // slow cinematic orbit around the city
			orbit += 0.006f;
			cyaw = orbit; cpitch = 0.6f; cy = 140.0f;
			cx = sinf(orbit) * 200.0f; cz = cosf(orbit) * 200.0f;
		}
		if (shake > 0.01f) { cx += rnd() * shake * 0.15f; cy += rnd() * shake * 0.15f; cz += rnd() * shake * 0.15f; }

		float fov = ((camStyle == 2) ? 72.0f : 65.0f) - 16.0f * aimAmt;
		Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(fov), C3D_AspectRatioTop, 0.2f, 400.0f, false);
		C3D_Mtx mv;
		Mtx_Identity(&mv);
		Mtx_RotateX(&mv, cpitch, true);
		Mtx_RotateY(&mv, -cyaw, true);
		Mtx_Translate(&mv, -cx, -cy, -cz, true);

		gCamX = cx; gCamZ = cz;
		gCullR = title ? 400.0f : 140.0f;
		gFwdX = -sinf(cyaw); gFwdZ = -cosf(cyaw);
		gCone = !title;
		unsigned char vis[CELLS]; // which districts the camera can see (the title orbit sees them all)
		for (int c = 0; c < CELLS; c++) {
			float dx = ((float)(c & 3) - 1.5f) * CELL_W - cx, dz = ((float)(c >> 2) - 1.5f) * CELL_W - cz;
			float d = sqrtf(dx * dx + dz * dz), dot = dx * gFwdX + dz * gFwdZ;
			vis[c] = (title || d < 100.0f || dot + 70.0f > 0.62f * d) ? 1 : 0;
		}

		// ---- shooting (the crosshair ray starts at the camera; the bullet leaves the barrel) ----
		if (!title && !paused) {
			float fwd[3], cam[3] = {cx, cy, cz}, muz[3];
			camForward(cyaw, cpitch, fwd);
			if (camStyle == 2) {
				float rx = cosf(cyaw), rz = -sinf(cyaw);
				muz[0] = cx + fwd[0] * 0.9f + rx * 0.25f; muz[1] = cy + fwd[1] * 0.9f - 0.2f; muz[2] = cz + fwd[2] * 0.9f + rz * 0.25f;
			} else {
				float hx = -sinf(face), hz = -cosf(face), rx = cosf(face), rz = -sinf(face), L = wdef[curWeapon].len;
				muz[0] = px + hx * (0.6f + L) + rx * 0.36f; muz[1] = 1.2f; muz[2] = pz + hz * (0.6f + L) + rz * 0.36f;
			}
			if (fireCool > 0) fireCool--;
			if (!wheelOpen && phoneTimer == 0 && armed && (held & (KEY_R | KEY_ZR)) && fireCool == 0) {
				const WDef* w = &wdef[curWeapon];
				int fresh = (down & (KEY_R | KEY_ZR)) ? 1 : 0;
				if (w->autoFire || fresh) {
					if (ammo[curWeapon] > 0) { ammo[curWeapon]--; fireCool = w->cooldown; fireWeapon(cam, fwd, muz); }
					else if (fresh) { playSfx(SFX_CLICK, 0.6f); fireCool = 10; }
				}
			}
			updateRocket();
			updateCrates(px, pz);
			updateFx();
		}

		// ---- build this frame's dynamic geometry ----
		drawDynamic();
		if (!title) {
			if (camStyle == 2) {
				drawViewModel(cx, cy, cz, cyaw, cpitch);
			} else {
				float phoneE = 0.0f; // 0 = phone away, 1 = phone raised (eased)
				if (phoneTimer > 0) {
					float up = (120 - phoneTimer) / 25.0f, dn = phoneTimer / 25.0f;
					phoneE = up < dn ? up : dn;
					if (phoneE > 1.0f) phoneE = 1.0f;
					phoneE = phoneE * phoneE * (3.0f - 2.0f * phoneE);
				}
				drawPlayer(px, pz, face, phase, moveAmt, phoneE, aimAmt);
			}
		}
		int litStart = vcount;            // emissive: signal heads, lights, ammo crates, tracers, rocket
		drawLights(sim, wNight);
		drawCrates(sim);
		drawTracers();
		GSPGPU_FlushDataCache(&vbo[litStart], (vcount - litStart) * sizeof(Vertex));
		int skyStart = vcount;            // blended: sun / moon / stars, rain, muzzle flashes, smoke, explosions
		drawSkyBodies(cx, cy, cz, cyaw);
		drawRain(cx, cz, cyaw);
		drawFx(cyaw);
		int dropStart = vcount;           // lens droplets + HUD (pixel coordinates, own pass)
		drawDroplets();
		float fadeA = title ? 1.0f - titleFade : 0.0f;
		drawHud2D(aimAmt, !title && !paused && armed, fadeA);
		GSPGPU_FlushDataCache(&vbo[skyStart], (vcount - skyStart) * sizeof(Vertex));

		float t = (hackFrames > 0) ? 0.25f : 1.0f;
		float fogC[3] = { wSky[0], wSky[1], wSky[2] };
		if (hackFrames > 0) { fogC[0] = 0.03f; fogC[1] = 0.03f; fogC[2] = 0.06f; }
		float fS = title ? wFogS + 140.0f : wFogS, fE = title ? wFogE + 260.0f : wFogE; // the title orbit is far above the city
		float fogK = 1.0f / (fE - fS);
		float fogA = gFog ? -fogK : 0.0f, fogB = gFog ? -fS * fogK : 0.0f;
		float glow = wNight + 0.35f * wDusk + 0.12f * wRain;
		if (glow > 1.0f) glow = 1.0f;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, hackFrames > 0 ? 0x080810FF : skyColor(1.0f), 0);
		C3D_FrameDrawOn(top);
		// citro2d changes the GPU state while drawing the bottom screen, so set ours up every frame
		C3D_BindProgram(&program);
		C3D_SetAttrInfo(&attr);
		C3D_SetBufInfo(&buf);
		for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
		C3D_CullFace(GPU_CULL_NONE);
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
		               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, &proj);
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMV, &mv);

		// 1) opaque world: weather-tinted, fogged toward the sky colour
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, fogA, fogB, 0.0f, 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uFogClr, fogC[0], fogC[1], fogC[2], 1.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, wTint[0] * t, wTint[1] * t, wTint[2] * t, 1.0f);
		envPlain();
		drawRange(0, globalEnd);                         // ground + skyline
		drawCells(cellAStart, cellACount, vis);          // roofs, road markings, crossings, signal poles
		if (gWin) { envTextured(); C3D_TexBind(0, &texDay); }
		drawCells(cellWStart, cellWCount, vis);          // textured facades
		envPlain();
		drawRange(staticCount, litStart - staticCount);  // shadows, cars, pedestrians, player

		// 2) emissive lights stay bright at night (still fogged)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, t, t, t, 1.0f);
		drawRange(litStart, skyStart - litStart);

		// 3) lit windows: the night texture added on top of the facades
		if (gWin && glow > 0.03f) {
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
			C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_COLOR);
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uFogClr, 0.0f, 0.0f, 0.0f, 1.0f); // distant windows fade to black
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, glow * t * 1.8f, glow * t * 1.8f, glow * t * 1.8f, 1.0f);
			envTextured();
			C3D_TexBind(0, &texNight);
			drawCells(cellWStart, cellWCount, vis);
			envPlain();
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
			               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		}

		// 4) blended sky, rain, flashes and smoke: depth-tested but not depth-written, no fog
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, 0.0f, 0.0f, 0.0f, 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, t, t, t, 1.0f);
		drawRange(skyStart, dropStart - skyStart);

		// 5) lens droplets, crosshair and fade, 2D overlay
		if (vcount > dropStart) {
			C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, &ortho);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMV, &ident);
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, 1.0f, 1.0f, 1.0f, 1.0f);
			drawRange(dropStart, vcount - dropStart);
		}

		// bottom screen: street map (or the title card)
		C2D_Prepare();
		C2D_TextBufClear(gDyn);
		C2D_TargetClear(bottom, C2D_Color32(0x12, 0x15, 0x1d, 0xFF));
		C2D_SceneBegin(bottom);
		if (title) drawTitleBottom(frame);
		else {
			drawMap(px, pz, frame, phoneTimer, face);
			drawWheel(wheelAmt, wheelSel, curWeapon, frame);
			drawPause(pauseDim, menuState, sel, menuState == 3 ? camStyle : weather, frame);
		}
		if (fadeA > 0.01f) C2D_DrawRectSolid(0.0f, 0.0f, 0.9f, 320.0f, 240.0f, C2D_Color32(0, 0, 0, (u8)(fadeA * 255.0f)));
		C2D_Flush();
		C3D_FrameEnd(0);

		frame++;
		if (++fpsFrames >= 30) { // frame-rate readout, refreshed twice a second
			u64 now = osGetTime();
			if (now > fpsT0) gFpsValue = (int)(30000ULL / (now - fpsT0));
			if (gFpsValue > 60) gFpsValue = 60;
			fpsT0 = now; fpsFrames = 0;
		}
	}

	shutdownAudio();
	if (texOK) { C3D_TexDelete(&texDay); C3D_TexDelete(&texNight); }
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C2D_TextBufDelete(gStatic);
	C2D_TextBufDelete(gDyn);
	C2D_Fini();
	C3D_Fini();
	romfsExit();
	gfxExit();
	return 0;
}
