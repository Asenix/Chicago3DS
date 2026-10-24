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

#define GRID      14         // 14 x 14 city blocks (560 m x 560 m)
#define ROAD_SP   40.0f      // distance between road centre lines
#define ROAD_HW   5.5f       // half width of the asphalt
#define SW_W      2.5f       // sidewalk width
#define SW_OFF    (ROAD_HW + SW_W * 0.5f) // pedestrians walk this far from the road centre line
#define XW        (ROAD_HW + 1.2f)        // crosswalks sit this far from the junction centre
#define LANE      2.8f       // car lane offset from the road centre line
#define BLK_H     (ROAD_SP * 0.5f - ROAD_HW)  // half size of a block (sidewalk included)
#define LOT_H     (BLK_H - SW_W)              // half size of the buildable lot
#define CITY_H    (GRID * ROAD_SP * 0.5f)     // half size of the whole city (280)
#define ROAD_END  (CITY_H + 10.0f)            // traffic and pedestrians wrap here
#define FLOOR_H   3.5f
#define MAX_VERTS 229998   // a multiple of 3, so a full buffer never ends in half a triangle
#define MAX_BLD   3600
#define MAX_PATCH 2400
#define MAX_FEAT  11000
#define RAIN_N    140
#define N_DROPS   11
#define N_CARS    36
#define N_PEDS    30
static int gTraffic = 1;   // cars and pedestrians exist in Free Roam only
#define NCARS (gTraffic ? N_CARS : 0)
#define NPEDS (gTraffic ? N_PEDS : 0)
#define N_STARS   70
#define ROADS     13
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
enum { BT_TOWER, BT_LOW, BT_MID, BT_PARK, BT_PLAZA, BT_HOUSES, BT_MIXED, BT_COMM, BT_INDUS, BT_DOCK, BT_LOT, BT_COURT, BT_RAIL, BT_FWY };
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

static unsigned gVtxOver = 0; // vertices refused because the buffer was full (should stay 0; the FPS readout shows it if it does not)

static void addVT(float x, float y, float z, float r, float g, float b, float u, float v) {
	if (vcount >= MAX_VERTS) { gVtxOver++; return; }
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
// Roads run along x = ROAD_C(k) and z = ROAD_C(k), k = 0..12 (40 m apart). Intersections are (a, b).
// Between the roads lie 14 x 14 city blocks. Every block is one district type: glass towers downtown,
// brick houses and two-flats, shops with flats above, strip malls and diners, warehouse yards, docks on the
// lake, vacant lots, courts and parks. One block row is an elevated railway (an embankment with a tunnel
// for every cross street) and another is an elevated expressway with a car park underneath.
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

#define CN       (GRID / 2)  // districts of 2x2 blocks; each gets its own vertex ranges so it can be skipped when out of view
#define CELLS    (CN * CN)
#define CELL_W   (ROAD_SP * 2.0f)
static int cellAStart[CELLS], cellACount[CELLS], cellWStart[CELLS], cellWCount[CELLS];
static int globalEnd = 0; // ground + skyline, always drawn

static int cellOf(float x, float z) {
	int ix = (int)floorf((x + CITY_H) / CELL_W), iz = (int)floorf((z + CITY_H) / CELL_W);
	ix = ix < 0 ? 0 : (ix > CN - 1 ? CN - 1 : ix);
	iz = iz < 0 ? 0 : (iz > CN - 1 ? CN - 1 : iz);
	return iz * CN + ix;
}

static unsigned lrnd(unsigned* s) { // tiny LCG for layout decisions
	*s = *s * 1664525u + 1013904223u;
	return *s >> 8;
}

// ---- world data: ground patches, solid buildings, small features, doors, lamps ----------------
enum { RM_STORE, RM_DINER, RM_LAUNDRY, RM_APT, RM_WARE, RM_STATION, RM_N };
static const float roomClr[RM_N][3] = {
	{0.85f, 0.18f, 0.18f}, {0.95f, 0.35f, 0.65f}, {0.15f, 0.72f, 0.74f}, {0.55f, 0.34f, 0.20f}, {0.95f, 0.60f, 0.12f}, {0.20f, 0.45f, 0.90f}
};
static const char* const roomName[RM_N] = {"QUICK STOP", "LAKESIDE DINER", "WASH & FOLD", "APARTMENT", "WAREHOUSE", "L STATION"};

// small features, built into the per-district vertex ranges
enum { FT_BOX, FT_BOXU, FT_POST, FT_CAR, FT_BUSH, FT_PRISM, FT_WALLQ, FT_SLOPE, FT_SIDEZ, FT_FLAT };
typedef struct { short type, aux; float x0, z0, x1, z1, y0, y1, r, g, b; } Feat;
static Feat feat[MAX_FEAT];
static int  nFeat = 0, gDropped = 0;

typedef struct { float x, z, fx, fz; int room; } Door; // x,z = where the player stands to use it, (fx,fz) = outward direction
#define MAX_DOOR 64
static Door doors[MAX_DOOR];
static int  nDoor = 0;

typedef struct { float x, y, z, w; } Lamp; // emissive light fixtures (street lights, tunnel lights)
#define MAX_LAMP 700
static Lamp lamps[MAX_LAMP];
static int  nLamp = 0;

static void addP(float x0, float z0, float x1, float z1, float y, float r, float g, float b) {
	if (nPatch >= MAX_PATCH) { gDropped++; return; }
	Patch* p = &patch[nPatch++];
	p->x0 = x0; p->z0 = z0; p->x1 = x1; p->z1 = z1; p->y = y; p->r = r; p->g = g; p->b = b;
}

static Building* addB(int kind, float x0, float z0, float x1, float z1, float h, float r, float g, float b, unsigned hs) {
	if (nBld >= MAX_BLD) { gDropped++; return NULL; }
	Building* p = &bldg[nBld++];
	p->x0 = x0; p->z0 = z0; p->x1 = x1; p->z1 = z1; p->h = h; p->kind = kind; p->deco = 0;
	p->r = r; p->g = g; p->b = b;
	p->uo = (float)((hs >> 20) & 3) * 0.25f; p->vo = (float)((hs >> 24) & 3) * 0.25f;
	return p;
}
static void addSolid(float x0, float z0, float x1, float z1, float h) { addB(3, x0, z0, x1, z1, h, 0.5f, 0.5f, 0.5f, 0); } // collision only

static void addF(int type, int aux, float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) {
	if (nFeat >= MAX_FEAT) { gDropped++; return; }
	Feat* f = &feat[nFeat++];
	f->type = (short)type; f->aux = (short)aux;
	f->x0 = x0; f->z0 = z0; f->x1 = x1; f->z1 = z1; f->y0 = y0; f->y1 = y1; f->r = r; f->g = g; f->b = b;
}
static void fBox(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) { addF(FT_BOX, 0, x0, z0, x1, z1, y0, y1, r, g, b); }
static void fBoxU(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) { addF(FT_BOXU, 0, x0, z0, x1, z1, y0, y1, r, g, b); }
static void fPost(float x, float z, float hw, float y0, float y1, float r, float g, float b) { addF(FT_POST, 0, x - hw, z - hw, x + hw, z + hw, y0, y1, r, g, b); }
static void fBush(float x, float z, float rad, float h, float r, float g, float b) { addF(FT_BUSH, 0, x - rad, z - rad, x + rad, z + rad, 0.0f, h, r, g, b); }
static void fFlat(float x0, float z0, float x1, float z1, float y, float r, float g, float b) { addF(FT_FLAT, 0, x0, z0, x1, z1, y, y, r, g, b); }
static void fSlope(float x0, float x1, float z0, float z1, float ya, float yb, float r, float g, float b) { addF(FT_SLOPE, 0, x0, z0, x1, z1, ya, yb, r, g, b); }
static void fSideZ(float z, float x0, float x1, float ya, float yb, float r, float g, float b) { addF(FT_SIDEZ, 0, x0, z, x1, z, ya, yb, r, g, b); }
static void fWall(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) { addF(FT_WALLQ, 0, x0, z0, x1, z1, y0, y1, r, g, b); }
static void fPrism(float x0, float z0, float x1, float z1, float y0, float y1, int ridgeAlongZ, float r, float g, float b) { addF(FT_PRISM, ridgeAlongZ, x0, z0, x1, z1, y0, y1, r, g, b); }
static void addLamp(float x, float y, float z, float w) { if (nLamp < MAX_LAMP) { lamps[nLamp].x = x; lamps[nLamp].y = y; lamps[nLamp].z = z; lamps[nLamp].w = w; nLamp++; } else gDropped++; }

static const float carPal[8][3] = {
	{0.80f, 0.14f, 0.14f}, {0.16f, 0.33f, 0.80f}, {0.88f, 0.78f, 0.16f}, {0.82f, 0.82f, 0.86f},
	{0.12f, 0.12f, 0.14f}, {0.20f, 0.60f, 0.30f}, {0.55f, 0.56f, 0.60f}, {0.50f, 0.24f, 0.16f}
};
// parked car: 4.2 x 1.9. axis 0 = nose along x, 1 = along z. solid = also blocks the player.
static void fCar(float x, float z, int axis, unsigned h, int solid) {
	const float* c = carPal[h & 7];
	float hx = axis == 0 ? 2.1f : 0.95f, hz = axis == 0 ? 0.95f : 2.1f;
	addF(FT_CAR, axis, x - hx, z - hz, x + hx, z + hz, 0.0f, 0.0f, c[0], c[1], c[2]);
	if (solid) addSolid(x - hx, z - hz, x + hx, z + hz, 1.6f);
}

static const float flowerPal[5][3] = {{0.92f, 0.25f, 0.30f}, {0.95f, 0.82f, 0.25f}, {0.70f, 0.35f, 0.85f}, {0.95f, 0.95f, 0.92f}, {0.95f, 0.55f, 0.20f}};
static void flowerBed(float x0, float z0, float x1, float z1, unsigned* st) {
	addP(x0, z0, x1, z1, 0.09f, 0.28f, 0.20f, 0.13f);
	int n = 4 + (int)(lrnd(st) % 3);
	for (int k = 0; k < n; k++) {
		float x = x0 + 0.3f + (x1 - x0 - 0.6f) * (float)(lrnd(st) % 100) / 100.0f, z = z0 + 0.3f + (z1 - z0 - 0.6f) * (float)(lrnd(st) % 100) / 100.0f;
		const float* c = flowerPal[lrnd(st) % 5];
		fPost(x, z, 0.16f, 0.0f, 0.5f, c[0], c[1], c[2]);
	}
	fBush((x0 + x1) * 0.5f, (z0 + z1) * 0.5f, 0.5f, 0.7f, 0.18f, 0.40f, 0.20f);
}
static void weeds(float x0, float z0, float x1, float z1, int n, unsigned* st) {
	for (int k = 0; k < n; k++) {
		float x = x0 + (x1 - x0) * (float)(lrnd(st) % 100) / 100.0f, z = z0 + (z1 - z0) * (float)(lrnd(st) % 100) / 100.0f;
		float g = 0.34f + (float)(lrnd(st) % 20) / 100.0f;
		fPost(x, z, 0.22f, 0.0f, 0.5f + (float)(lrnd(st) % 6) * 0.1f, 0.40f, g + 0.12f, 0.20f);
	}
}
static void bench(float x, float z, int alongX) { // 1.8 m park bench
	if (alongX) { fBox(x - 0.9f, z - 0.25f, x + 0.9f, z + 0.25f, 0.0f, 0.45f, 0.45f, 0.30f, 0.18f); fBox(x - 0.9f, z + 0.18f, x + 0.9f, z + 0.28f, 0.45f, 0.95f, 0.40f, 0.27f, 0.16f); }
	else         { fBox(x - 0.25f, z - 0.9f, x + 0.25f, z + 0.9f, 0.0f, 0.45f, 0.45f, 0.30f, 0.18f); fBox(x + 0.18f, z - 0.9f, x + 0.28f, z + 0.9f, 0.45f, 0.95f, 0.40f, 0.27f, 0.16f); }
}
static void dumpster(float x, float z, int alongX, unsigned h) {
	float hx = alongX ? 1.0f : 0.65f, hz = alongX ? 0.65f : 1.0f;
	const float* c = (h & 1) ? roomClr[RM_LAUNDRY] : roomClr[RM_STATION];
	fBox(x - hx, z - hz, x + hx, z + hz, 0.0f, 1.25f, c[0] * 0.55f, c[1] * 0.55f, c[2] * 0.55f);
	addSolid(x - hx, z - hz, x + hx, z + hz, 1.25f);
}
static void trashCan(float x, float z) { fPost(x, z, 0.28f, 0.0f, 0.9f, 0.16f, 0.30f, 0.20f); }
static void streetLamp(float x, float z, float dx, float dz) { // pole with an arm reaching (dx,dz) unit direction
	fPost(x, z, 0.09f, 0.0f, 5.6f, 0.22f, 0.23f, 0.26f);
	float ex = x + dx * 1.5f, ez = z + dz * 1.5f;
	fBox(fminf(x, ex) - 0.07f, fminf(z, ez) - 0.07f, fmaxf(x, ex) + 0.07f, fmaxf(z, ez) + 0.07f, 5.5f, 5.65f, 0.22f, 0.23f, 0.26f);
	addLamp(ex, 5.45f, ez, 0.55f);
}

// A vertical quad just in front of a building wall. side: 0 = -x face, 1 = +x face, 2 = -z face, 3 = +z face.
// a0..a1 is the span along the wall.
static void facade(float x0, float z0, float x1, float z1, int side, float a0, float a1, float y0, float y1, float r, float g, float b) {
	const float o = 0.05f;
	switch (side) {
	case 0:  fWall(x0 - o, a0, x0 - o, a1, y0, y1, r, g, b); break;
	case 1:  fWall(x1 + o, a0, x1 + o, a1, y0, y1, r, g, b); break;
	case 2:  fWall(a0, z0 - o, a1, z0 - o, y0, y1, r, g, b); break;
	default: fWall(a0, z1 + o, a1, z1 + o, y0, y1, r, g, b); break;
	}
}
static void awning(float x0, float z0, float x1, float z1, int side, float a0, float a1, float y, float depth, float r, float g, float b) {
	switch (side) {
	case 0:  fBox(x0 - depth, a0, x0, a1, y, y + 0.25f, r, g, b); break;
	case 1:  fBox(x1, a0, x1 + depth, a1, y, y + 0.25f, r, g, b); break;
	case 2:  fBox(a0, z0 - depth, a1, z0, y, y + 0.25f, r, g, b); break;
	default: fBox(a0, z1, a1, z1 + depth, y, y + 0.25f, r, g, b); break;
	}
}
// Enterable door in a facade. Adds the door, a sign band over it and a Door record (the spot outside it).
static void addDoor(float x0, float z0, float x1, float z1, int side, float a, int room) {
	const float* c = roomClr[room];
	facade(x0, z0, x1, z1, side, a - 0.8f, a + 0.8f, 0.0f, 2.4f, c[0] * 0.35f, c[1] * 0.35f, c[2] * 0.35f);
	facade(x0, z0, x1, z1, side, a - 2.2f, a + 2.2f, 2.7f, 3.5f, c[0], c[1], c[2]);
	if (nDoor >= MAX_DOOR) { gDropped++; return; }
	Door* d = &doors[nDoor++];
	d->room = room;
	switch (side) {
	case 0:  d->fx = -1.0f; d->fz = 0.0f; d->x = x0 - 1.3f; d->z = a; break;
	case 1:  d->fx = 1.0f;  d->fz = 0.0f; d->x = x1 + 1.3f; d->z = a; break;
	case 2:  d->fx = 0.0f;  d->fz = -1.0f; d->x = a; d->z = z0 - 1.3f; break;
	default: d->fx = 0.0f;  d->fz = 1.0f; d->x = a; d->z = z1 + 1.3f; break;
	}
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

// ---- layout of the city: a rail row, a freeway row and districts ---------------------------------------------
#define BLK_C(i)  (-CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)(i))
#define RAIL_J    3
#define FWY_J     10
#define RAIL_Z    BLK_C(RAIL_J)           // centre of the rail block row (z)
#define RAIL_H    5.2f                    // top of the embankment
#define RAIL_HW   7.0f
#define TRK_Z     (RAIL_Z - 3.5f)         // track centre line
#define PLAT_Z0   (RAIL_Z - 1.0f)         // station platform: from here to the south edge of the embankment
#define FWY_Z     BLK_C(FWY_J)            // centre of the freeway
#define FWY_HW    10.0f
#define FWY_Y     7.5f                    // top of the deck
#define FWY_T     1.3f
#define TUN_HW    8.0f                    // half width of tunnels / underpasses (road + sidewalks)
#define RAMP_X    (-CITY_H)               // the west end of the freeway comes down to the ground here...
#define RAMP_L    60.0f                   // ...over this length
#define FWY_X0    (RAMP_X - RAMP_L)
#define FWY_X1    (CITY_H + 160.0f)       // the east end runs out over the lake
static const int kStationI[2] = {4, 10};

static float dtFactor(float bx, float bz) { // 1 in the Loop, fading to 0 far away: taller buildings downtown
	float dx = bx - 30.0f, dz = bz + 10.0f, v = 1.0f - sqrtf(dx * dx + dz * dz) / 230.0f;
	return v < 0.0f ? 0.0f : v;
}

static int pickBlock(int i, int j) {
	float bx = BLK_C(i), bz = BLK_C(j);
	int r = (int)(hash2(i * 31 + 7, j * 17 + 3) % 100);
	if (j == RAIL_J) return BT_RAIL;
	if (j == FWY_J)  return BT_FWY;
	if (i >= GRID - 2 && j < FWY_J) return r < 66 ? BT_PARK : (r < 82 ? BT_PLAZA : BT_COURT); // Grant Park by the lake
	if (j > FWY_J) {                                                                           // south of the expressway
		if (i >= GRID - 2) return BT_DOCK;
		if (i >= 8) return r < 26 ? BT_COMM : (r < 44 ? BT_LOT : BT_INDUS);
		return r < 14 ? BT_LOT : BT_INDUS;
	}
	if (i >= 2 && i <= 3 && j >= 1 && j <= 2) return r < 80 ? BT_PARK : BT_COURT;              // Lincoln Park
	float dt = dtFactor(bx, bz);
	if (dt > 0.45f) {                                                                          // downtown
		if (r < 60) return BT_TOWER;
		if (r < 68) return BT_PLAZA;
		if (r < 86) return BT_MID;
		return BT_LOW;
	}
	if (j < RAIL_J) {                                                                          // north of the railway
		if (i <= 4 || i >= 10) return r < 8 ? BT_PARK : (r < 15 ? BT_COURT : (r < 21 ? BT_LOT : BT_HOUSES));
		return r < 50 ? BT_MIXED : (r < 74 ? BT_COMM : (r < 90 ? BT_MID : BT_LOW));
	}
	if (i <= 3) return r < 8 ? BT_COURT : (r < 14 ? BT_PARK : (r < 20 ? BT_LOT : BT_HOUSES)); // west neighbourhoods
	if (i <= 5) return r < 44 ? BT_HOUSES : (r < 78 ? BT_MIXED : (r < 86 ? BT_PARK : BT_LOT));
	return r < 40 ? BT_MIXED : (r < 62 ? BT_COMM : (r < 80 ? BT_MID : (r < 90 ? BT_LOW : BT_PARK)));
}

static const float kBrick[6][3] = {{0.68f, 0.38f, 0.30f}, {0.72f, 0.56f, 0.40f}, {0.60f, 0.54f, 0.50f}, {0.64f, 0.30f, 0.26f}, {0.78f, 0.70f, 0.56f}, {0.52f, 0.40f, 0.34f}};
static const float kPaint[6][3] = {{0.86f, 0.84f, 0.74f}, {0.60f, 0.72f, 0.80f}, {0.78f, 0.62f, 0.58f}, {0.62f, 0.72f, 0.58f}, {0.88f, 0.80f, 0.60f}, {0.70f, 0.68f, 0.74f}};

// Brick houses, two-flats and bungalows facing the streets either side of a back alley.
static void layoutHouses(float bx, float bz, unsigned st) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.27f, 0.42f, 0.23f);   // lawns
	addP(bx - 2.0f, bz - L, bx + 2.0f, bz + L, 0.07f, 0.30f, 0.30f, 0.31f); // the alley
	for (int side = 0; side < 2; side++) {
		float sg = side ? 1.0f : -1.0f;
		int face = side ? 1 : 0; // which wall faces the street
		for (int k = 0; k < 3; k++) {
			float cz = bz + ((float)k - 1.0f) * 8.0f;
			int roll = (int)(lrnd(&st) % 100);
			float w = 6.6f + (float)(lrnd(&st) % 3) * 0.4f, dep = 6.2f + (float)(lrnd(&st) % 3) * 0.4f;
			float xf = bx + sg * (L - 2.4f), xb = bx + sg * (L - 2.4f - dep);
			float xa = xf < xb ? xf : xb, xc = xf < xb ? xb : xf;
			float yardX = bx + sg * (L - 1.2f);
			if (roll < 12) { // an empty lot: a tree and shrubs
				addTree(bx + sg * 7.0f, cz, lrnd(&st));
				fBush(yardX, cz + 2.6f, 0.8f, 1.0f, 0.20f, 0.42f, 0.20f);
				unsigned q = st; flowerBed(yardX - 0.6f, cz - 3.0f, yardX + 0.6f, cz - 1.0f, &q);
				continue;
			}
			int type = roll < 45 ? 0 : (roll < 78 ? 1 : 2); // 0 two-flat, 1 greystone, 2 bungalow
			float h = type == 0 ? 2.0f * FLOOR_H : (type == 1 ? 3.0f * FLOOR_H : 1.2f * FLOOR_H);
			const float* c = (lrnd(&st) % 3) ? kBrick[lrnd(&st) % 6] : kPaint[lrnd(&st) % 6];
			Building* b = addB(0, xa, cz - w * 0.5f, xc, cz + w * 0.5f, h, c[0], c[1], c[2], st);
			if (!b) continue;
			if (type == 2) { // pitched roof
				const float* rc = kBrick[(lrnd(&st) % 2) ? 5 : 2];
				fPrism(xa - 0.2f, cz - w * 0.5f - 0.2f, xc + 0.2f, cz + w * 0.5f + 0.2f, h, h + 1.7f, 0, rc[0] * 0.7f, rc[1] * 0.7f, rc[2] * 0.7f);
			}
			float fx0 = xa, fx1 = xc;
			if (lrnd(&st) % 100 < 60) { // front porch with a canopy
				float p0 = sg < 0 ? xa - 1.5f : xc, p1 = sg < 0 ? xa : xc + 1.5f;
				fBox(p0, cz - 1.1f, p1, cz + 1.1f, 0.0f, 0.5f, 0.62f, 0.60f, 0.56f);
				fBox(p0, cz - 1.1f, p1, cz + 1.1f, 2.4f, 2.6f, 0.82f, 0.80f, 0.74f);
			}
			facade(fx0, 0, fx1, 0, face, cz - 0.5f, cz + 0.5f, 0.5f, 2.3f, 0.30f, 0.20f, 0.14f); // front door
			if (lrnd(&st) % 100 < 35) { // low front fence
				float fxx = bx + sg * (L - 0.15f);
				fBox(fxx - 0.05f, cz - w * 0.5f, fxx + 0.05f, cz + w * 0.5f, 0.0f, 0.85f, 0.86f, 0.86f, 0.84f);
			}
			if (lrnd(&st) % 100 < 60) fBush(yardX, cz + (lrnd(&st) % 2 ? 2.4f : -2.4f), 0.6f, 0.9f, 0.20f, 0.40f + (float)(lrnd(&st) % 10) * 0.01f, 0.20f);
			if (type == 1 && (lrnd(&st) % 2)) addTree(yardX, cz + (lrnd(&st) % 2 ? 3.2f : -3.2f), lrnd(&st));
		}
	}
	// alley: dumpsters, bins, a utility pole, maybe a parked car
	dumpster(bx - 0.6f, bz + (float)((int)(lrnd(&st) % 17) - 8), 0, lrnd(&st));
	dumpster(bx + 0.8f, bz + (float)((int)(lrnd(&st) % 17) - 8), 0, lrnd(&st));
	trashCan(bx - 1.5f, bz + (float)((int)(lrnd(&st) % 21) - 10));
	trashCan(bx + 1.5f, bz + (float)((int)(lrnd(&st) % 21) - 10));
	{
		float pz = bz + (float)((int)(lrnd(&st) % 13) - 6);
		fPost(bx + 1.7f, pz, 0.12f, 0.0f, 7.0f, 0.30f, 0.22f, 0.16f);
		fBox(bx + 0.9f, pz - 0.08f, bx + 2.5f, pz + 0.08f, 6.4f, 6.6f, 0.30f, 0.22f, 0.16f);
	}
	if (lrnd(&st) % 100 < 40) fCar(bx, bz + (float)((int)(lrnd(&st) % 17) - 8) * 0.9f, 1, lrnd(&st), 1);
	weeds(bx - 2.0f, bz - L, bx + 2.0f, bz + L, 4, &st);
}

// Shops with apartments above: two rows of 3 units, a courtyard in between.
static void layoutMixed(float bx, float bz, unsigned st, float dt) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.38f, 0.38f, 0.40f);
	int ent = (int)(lrnd(&st) % 100);
	int entRow = (int)(lrnd(&st) & 1), entU = (int)(lrnd(&st) % 3);
	for (int row = 0; row < 2; row++) {
		float sg = row ? 1.0f : -1.0f;
		int side = row ? 3 : 2;
		float za = bz + sg * L, zb = bz + sg * (L - 8.5f);
		float z0 = za < zb ? za : zb, z1 = za < zb ? zb : za;
		for (int u = 0; u < 3; u++) {
			float x0 = bx - L + (float)u * 8.0f, x1 = x0 + 7.8f;
			int nf = 3 + (int)(lrnd(&st) % 3) + (int)(dt * 3.0f);
			const float* c = (lrnd(&st) % 3) ? kBrick[lrnd(&st) % 6] : kPaint[lrnd(&st) % 6];
			Building* b = addB(0, x0, z0, x1, z1, (float)nf * FLOOR_H, c[0], c[1], c[2], st);
			if (!b) continue;
			if (lrnd(&st) % 3 == 0) b->deco = 1;
			int special = (row == entRow && u == entU);
			float xm = (x0 + x1) * 0.5f;
			const float* ac = flowerPal[lrnd(&st) % 5];
			facade(x0, z0, x1, z1, side, x0 + 0.6f, x1 - 0.6f, 0.35f, 2.6f, 0.15f, 0.22f, 0.28f);          // shop window
			awning(x0, z0, x1, z1, side, x0 + 0.3f, x1 - 0.3f, 2.7f, 1.1f, ac[0] * 0.8f, ac[1] * 0.8f, ac[2] * 0.8f);
			if (special && ent < 22)      addDoor(x0, z0, x1, z1, side, xm, RM_STORE);
			else if (special && ent < 36) addDoor(x0, z0, x1, z1, side, xm, RM_LAUNDRY);
			else if (special && ent < 52) addDoor(x0, z0, x1, z1, side, xm, RM_APT);
			else facade(x0, z0, x1, z1, side, xm - 0.5f, xm + 0.5f, 0.0f, 2.2f, 0.25f, 0.20f, 0.18f);
		}
	}
	// courtyard
	for (int k = 0; k < 3; k++) {
		float t = bx - 7.0f + (float)k * 7.0f;
		if (lrnd(&st) % 100 < 50) addTree(t, bz + (float)((int)(lrnd(&st) % 3) - 1) * 0.6f, lrnd(&st));
		else dumpster(t, bz, 1, lrnd(&st));
	}
	if (lrnd(&st) % 100 < 50) fCar(bx + (lrnd(&st) % 2 ? 4.0f : -4.0f), bz + 1.0f, 0, lrnd(&st), 1);
	trashCan(bx + L - 1.0f, bz - 2.0f);
}

// Strip mall, diner or big-box store with a car park.
static void layoutComm(float bx, float bz, unsigned st) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.30f, 0.30f, 0.32f);
	int variant = (int)(lrnd(&st) % 3);
	float bz0 = bz - L + 0.5f;
	// the car park: bays across the south half
	float pz0 = bz - 2.0f, pz1 = bz + L - 0.8f;
	addP(bx - L + 0.5f, pz0, bx + L - 0.5f, pz1, 0.08f, 0.16f, 0.16f, 0.18f);
	for (int k = 0; k <= 8; k++) addP(bx - 10.1f + (float)k * 2.5f, pz0 + 0.3f, bx - 9.9f + (float)k * 2.5f, pz0 + 4.6f, 0.10f, 0.85f, 0.85f, 0.82f);
	for (int k = 0; k < 8; k++) if (lrnd(&st) % 100 < 45) fCar(bx - 8.75f + (float)k * 2.5f, pz0 + 2.5f, 1, lrnd(&st), 1);
	streetLamp(bx - L + 1.0f, pz1 - 0.5f, 1.0f, 0.0f);
	streetLamp(bx + L - 1.0f, pz1 - 0.5f, -1.0f, 0.0f);
	if (variant == 0) { // strip mall: three shops
		for (int u = 0; u < 3; u++) {
			float x0 = bx - 10.5f + (float)u * 7.0f, x1 = x0 + 6.9f, z0 = bz0, z1 = bz0 + 7.0f;
			const float* c = kPaint[lrnd(&st) % 6];
			addSolid(x0, z0, x1, z1, 4.4f);
			fBox(x0, z0, x1, z1, 0.0f, 4.4f, c[0], c[1], c[2]);
			facade(x0, z0, x1, z1, 3, x0 + 0.6f, x1 - 0.6f, 0.4f, 2.7f, 0.14f, 0.22f, 0.30f);
			const float* ac = flowerPal[lrnd(&st) % 5];
			facade(x0, z0, x1, z1, 3, x0 + 0.3f, x1 - 0.3f, 3.2f, 4.1f, ac[0], ac[1], ac[2]);
			awning(x0, z0, x1, z1, 3, x0 + 0.3f, x1 - 0.3f, 2.9f, 1.2f, ac[0] * 0.7f, ac[1] * 0.7f, ac[2] * 0.7f);
			if (u == 1 && (lrnd(&st) % 100) < 70) addDoor(x0, z0, x1, z1, 3, (x0 + x1) * 0.5f, RM_STORE);
		}
	} else if (variant == 1) { // diner
		float x0 = bx - 6.5f, x1 = bx + 6.5f, z0 = bz0 + 1.0f, z1 = bz0 + 7.5f;
		addSolid(x0, z0, x1, z1, 3.8f);
		fBox(x0, z0, x1, z1, 0.0f, 3.8f, 0.80f, 0.82f, 0.86f);
		facade(x0, z0, x1, z1, 3, x0 + 0.5f, x1 - 0.5f, 0.9f, 2.7f, 0.18f, 0.28f, 0.36f);
		facade(x0, z0, x1, z1, 3, x0, x1, 3.0f, 3.5f, 0.85f, 0.15f, 0.25f);
		facade(x0, z0, x1, z1, 1, z0 + 0.5f, z1 - 0.5f, 0.9f, 2.7f, 0.18f, 0.28f, 0.36f);
		facade(x0, z0, x1, z1, 0, z0 + 0.5f, z1 - 0.5f, 0.9f, 2.7f, 0.18f, 0.28f, 0.36f);
		fBox(bx - 0.8f, z1 + 4.6f, bx + 0.8f, z1 + 4.9f, 6.0f, 7.4f, 0.95f, 0.35f, 0.65f); // sign on a pole... (flat plate)
		fPost(bx, z1 + 4.75f, 0.1f, 0.0f, 6.0f, 0.3f, 0.3f, 0.32f);
		addDoor(x0, z0, x1, z1, 3, bx, RM_DINER);
	} else { // big-box store
		float x0 = bx - L + 1.0f, x1 = bx + L - 1.0f, z0 = bz0, z1 = bz0 + 11.0f;
		addSolid(x0, z0, x1, z1, 6.5f);
		fBox(x0, z0, x1, z1, 0.0f, 6.5f, 0.52f, 0.56f, 0.62f);
		facade(x0, z0, x1, z1, 3, bx - 4.0f, bx + 4.0f, 0.2f, 3.0f, 0.14f, 0.22f, 0.30f);
		facade(x0, z0, x1, z1, 3, x0 + 1.0f, x1 - 1.0f, 4.2f, 5.8f, 0.18f, 0.35f, 0.80f);
		awning(x0, z0, x1, z1, 3, bx - 4.0f, bx + 4.0f, 3.3f, 2.0f, 0.18f, 0.35f, 0.80f);
	}
	for (int k = 0; k < 3; k++) fBush(bx - 8.0f + (float)k * 8.0f, pz1 + 0.2f, 0.7f, 0.9f, 0.20f, 0.40f, 0.20f);
}

// Warehouse yard: big sheds, shipping containers, tanks, a chimney, a chain-link fence.
static void layoutIndus(float bx, float bz, unsigned st) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.36f, 0.35f, 0.33f);
	static const float shed[4][3] = {{0.42f, 0.50f, 0.60f}, {0.62f, 0.56f, 0.45f}, {0.38f, 0.52f, 0.45f}, {0.55f, 0.58f, 0.62f}};
	const float* c = shed[lrnd(&st) % 4];
	float sg = (lrnd(&st) & 1) ? 1.0f : -1.0f;
	float zf = bz + sg * (L - 0.5f), zbk = bz + sg * (L - 13.5f);
	float z0 = zf < zbk ? zf : zbk, z1 = zf < zbk ? zbk : zf;
	float x0 = bx - L + 1.0f, x1 = bx + L - 1.0f;
	float h = 7.0f + (float)(lrnd(&st) % 3);
	addSolid(x0, z0, x1, z1, h);
	fBox(x0, z0, x1, z1, 0.0f, h, c[0], c[1], c[2]);
	int side = sg > 0 ? 3 : 2; // face towards the street... the yard is on the other side, so doors face the yard
	int ys = sg > 0 ? 2 : 3;
	(void)side;
	facade(x0, z0, x1, z1, ys, x0 + 1.0f, x1 - 1.0f, h - 1.4f, h - 0.7f, c[0] * 0.6f, c[1] * 0.6f, c[2] * 0.6f); // stripe
	for (int k = 0; k < 3; k++) facade(x0, z0, x1, z1, ys, x0 + 2.0f + (float)k * 6.5f, x0 + 5.5f + (float)k * 6.5f, 0.0f, 4.0f, 0.62f, 0.64f, 0.66f); // roller doors
	if (lrnd(&st) % 100 < 55) addDoor(x0, z0, x1, z1, ys, x1 - 1.4f, RM_WARE);
	// the yard: containers and a trailer
	float yz0 = sg > 0 ? bz - L + 1.0f : zbk + 0.5f, yz1 = sg > 0 ? zbk - 0.5f : bz + L - 1.0f;
	(void)yz0; (void)yz1;
	float yardZ = bz - sg * (L - 4.0f);
	static const float conC[5][3] = {{0.70f, 0.20f, 0.16f}, {0.16f, 0.36f, 0.66f}, {0.85f, 0.62f, 0.14f}, {0.22f, 0.52f, 0.36f}, {0.60f, 0.60f, 0.62f}};
	for (int k = 0; k < 4; k++) {
		float cx = bx - 9.0f + (float)k * 6.0f + (float)((int)(lrnd(&st) % 3) - 1) * 0.3f;
		if (lrnd(&st) % 100 < 25) continue;
		const float* cc = conC[lrnd(&st) % 5];
		fBox(cx - 1.2f, yardZ - 3.0f, cx + 1.2f, yardZ + 3.0f, 0.0f, 2.6f, cc[0], cc[1], cc[2]);
		addSolid(cx - 1.2f, yardZ - 3.0f, cx + 1.2f, yardZ + 3.0f, 2.6f);
		if (lrnd(&st) % 100 < 40) { const float* c2 = conC[lrnd(&st) % 5]; fBox(cx - 1.2f, yardZ - 3.0f, cx + 1.2f, yardZ + 3.0f, 2.6f, 5.2f, c2[0], c2[1], c2[2]); }
	}
	if (lrnd(&st) % 100 < 40) { // storage tank
		float tx = bx + (lrnd(&st) % 2 ? 7.0f : -7.0f), tz = bz - sg * 1.0f;
		fBox(tx - 1.8f, tz - 1.8f, tx + 1.8f, tz + 1.8f, 0.0f, 6.0f, 0.82f, 0.82f, 0.80f);
		addSolid(tx - 1.8f, tz - 1.8f, tx + 1.8f, tz + 1.8f, 6.0f);
	}
	if (lrnd(&st) % 100 < 35) { // chimney
		float tx = bx + (lrnd(&st) % 2 ? 9.0f : -9.0f), tz = (z0 + z1) * 0.5f;
		fBox(tx - 0.8f, tz - 0.8f, tx + 0.8f, tz + 0.8f, h, h + 12.0f, 0.60f, 0.38f, 0.32f);
	}
	weeds(bx - L, bz - L, bx + L, bz + L, 5, &st);
	// fence along the street side of the yard
	float fz = bz - sg * (L - 0.2f);
	fBox(bx - L, fz - 0.05f, bx + L, fz + 0.05f, 1.6f, 1.7f, 0.40f, 0.42f, 0.44f);
	for (int k = 0; k <= 4; k++) fPost(bx - L + (float)k * 6.0f, fz, 0.07f, 0.0f, 1.7f, 0.40f, 0.42f, 0.44f);
}

// Docks beside the lake: apron, container stacks, a crane and a wooden pier.
static void layoutDock(float bx, float bz, unsigned st, int lakeSide) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.40f, 0.39f, 0.37f);
	static const float conC[5][3] = {{0.70f, 0.20f, 0.16f}, {0.16f, 0.36f, 0.66f}, {0.85f, 0.62f, 0.14f}, {0.22f, 0.52f, 0.36f}, {0.60f, 0.60f, 0.62f}};
	for (int k = 0; k < 4; k++) for (int m = 0; m < 2; m++) {
		if (lrnd(&st) % 100 < 22) continue;
		float cx = bx - 9.0f + (float)k * 6.0f, cz = bz - 6.0f + (float)m * 9.0f;
		const float* cc = conC[lrnd(&st) % 5];
		fBox(cx - 1.2f, cz - 3.0f, cx + 1.2f, cz + 3.0f, 0.0f, 2.6f, cc[0], cc[1], cc[2]);
		addSolid(cx - 1.2f, cz - 3.0f, cx + 1.2f, cz + 3.0f, 2.6f);
		if (lrnd(&st) % 100 < 50) { const float* c2 = conC[lrnd(&st) % 5]; fBox(cx - 1.2f, cz - 3.0f, cx + 1.2f, cz + 3.0f, 2.6f, 5.2f, c2[0], c2[1], c2[2]); }
	}
	if (lakeSide) { // pier, crane
		float px0 = bx + L, px1 = bx + L + 80.0f;
		fBox(px0, bz - 3.5f, px1, bz + 3.5f, 0.0f, 0.8f, 0.45f, 0.32f, 0.20f);
		for (int k = 0; k < 8; k++) { fPost(px0 + 8.0f + (float)k * 10.0f, bz - 3.2f, 0.25f, -0.5f, 1.4f, 0.30f, 0.22f, 0.15f); fPost(px0 + 8.0f + (float)k * 10.0f, bz + 3.2f, 0.25f, -0.5f, 1.4f, 0.30f, 0.22f, 0.15f); }
		float cx = bx + L - 1.5f;
		fBox(cx - 0.6f, bz - 6.0f, cx + 0.6f, bz - 4.8f, 0.0f, 17.0f, 0.90f, 0.70f, 0.10f);
		fBox(cx - 0.6f, bz + 4.8f, cx + 0.6f, bz + 6.0f, 0.0f, 17.0f, 0.90f, 0.70f, 0.10f);
		fBox(cx - 22.0f, bz - 6.0f, cx + 40.0f, bz + 6.0f, 17.0f, 18.2f, 0.90f, 0.70f, 0.10f);
		addSolid(cx - 0.6f, bz - 6.0f, cx + 0.6f, bz - 4.8f, 17.0f);
		addSolid(cx - 0.6f, bz + 4.8f, cx + 0.6f, bz + 6.0f, 17.0f);
	}
	weeds(bx - L, bz - L, bx + L, bz + L, 4, &st);
}

// Vacant lot: weeds, rubble, an old car, a billboard and a chain-link fence with a gap.
static void layoutLot(float bx, float bz, unsigned st) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.36f, 0.38f, 0.26f);
	addP(bx - 6.0f, bz - 5.0f, bx + 4.0f, bz + 6.0f, 0.07f, 0.40f, 0.34f, 0.26f);
	weeds(bx - L, bz - L, bx + L, bz + L, 16, &st);
	for (int k = 0; k < 4; k++) {
		float x = bx - 9.0f + (float)(lrnd(&st) % 18), z = bz - 9.0f + (float)(lrnd(&st) % 18);
		fBox(x - 0.6f, z - 0.5f, x + 0.6f, z + 0.5f, 0.0f, 0.5f + (float)(lrnd(&st) % 5) * 0.1f, 0.50f, 0.46f, 0.42f);
	}
	if (lrnd(&st) % 100 < 50) fCar(bx + 5.0f, bz + 3.0f, (int)(lrnd(&st) & 1), 6, 1);
	for (int k = 0; k < 2; k++) { // fences on two sides
		float e = (k ? 1.0f : -1.0f) * (L - 0.3f);
		fBox(bx - L, bz + e - 0.04f, bx + L, bz + e + 0.04f, 1.55f, 1.65f, 0.45f, 0.47f, 0.48f);
		for (int p = 0; p <= 4; p++) fPost(bx - L + (float)p * 6.0f, bz + e, 0.06f, 0.0f, 1.65f, 0.45f, 0.47f, 0.48f);
	}
	// graffiti panel on a low wall + billboard
	fBox(bx - 6.0f, bz - L + 0.6f, bx + 3.0f, bz - L + 0.9f, 0.0f, 2.6f, 0.62f, 0.60f, 0.56f);
	const float* g1 = flowerPal[lrnd(&st) % 5]; const float* g2 = flowerPal[lrnd(&st) % 5];
	fWall(bx - 5.0f, bz - L + 0.95f, bx - 1.0f, bz - L + 0.95f, 0.5f, 2.2f, g1[0], g1[1], g1[2]);
	fWall(bx - 0.5f, bz - L + 0.95f, bx + 2.5f, bz - L + 0.95f, 0.8f, 2.0f, g2[0], g2[1], g2[2]);
	fPost(bx - 3.0f, bz + 8.0f, 0.12f, 0.0f, 6.5f, 0.35f, 0.35f, 0.38f); fPost(bx + 3.0f, bz + 8.0f, 0.12f, 0.0f, 6.5f, 0.35f, 0.35f, 0.38f);
	fBox(bx - 4.5f, bz + 7.85f, bx + 4.5f, bz + 8.15f, 4.5f, 7.5f, 0.85f, 0.85f, 0.82f);
}

// Basketball court and a playground.
static void layoutCourt(float bx, float bz, unsigned st) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.24f, 0.44f, 0.24f);
	float cx0 = bx - 6.5f, cx1 = bx + 6.5f, cz0 = bz - 12.0f, cz1 = bz - 0.5f + 1.0f;
	cz0 = bz - 11.0f; cz1 = bz + 1.0f;
	addP(cx0, cz0, cx1, cz1, 0.08f, 0.52f, 0.26f, 0.20f);
	addP(cx0, cz0, cx1, cz0 + 0.12f, 0.10f, 0.92f, 0.92f, 0.90f); addP(cx0, cz1 - 0.12f, cx1, cz1, 0.10f, 0.92f, 0.92f, 0.90f);
	addP(cx0, cz0, cx0 + 0.12f, cz1, 0.10f, 0.92f, 0.92f, 0.90f); addP(cx1 - 0.12f, cz0, cx1, cz1, 0.10f, 0.92f, 0.92f, 0.90f);
	addP(cx0, (cz0 + cz1) * 0.5f - 0.06f, cx1, (cz0 + cz1) * 0.5f + 0.06f, 0.10f, 0.92f, 0.92f, 0.90f);
	addP(bx - 2.4f, cz0, bx + 2.4f, cz0 + 3.6f, 0.09f, 0.42f, 0.20f, 0.16f); addP(bx - 2.4f, cz1 - 3.6f, bx + 2.4f, cz1, 0.09f, 0.42f, 0.20f, 0.16f);
	for (int e = 0; e < 2; e++) { // hoops
		float z = e ? cz1 - 0.3f : cz0 + 0.3f;
		fPost(bx, z, 0.08f, 0.0f, 3.2f, 0.30f, 0.30f, 0.33f);
		fBox(bx - 0.9f, z + (e ? -0.4f : 0.4f) - 0.04f, bx + 0.9f, z + (e ? -0.4f : 0.4f) + 0.04f, 2.7f, 3.5f, 0.92f, 0.92f, 0.92f);
		fBox(bx - 0.25f, z + (e ? -0.8f : 0.8f) - 0.25f, bx + 0.25f, z + (e ? -0.8f : 0.8f) + 0.25f, 2.95f, 3.0f, 0.95f, 0.45f, 0.10f);
	}
	// playground: swing frame, slide, sandbox
	float pz = bz + 7.5f;
	fPost(bx - 6.0f, pz - 1.4f, 0.1f, 0.0f, 2.6f, 0.80f, 0.30f, 0.20f); fPost(bx - 6.0f, pz + 1.4f, 0.1f, 0.0f, 2.6f, 0.80f, 0.30f, 0.20f);
	fBox(bx - 6.1f, pz - 1.5f, bx - 5.9f, pz + 1.5f, 2.55f, 2.7f, 0.80f, 0.30f, 0.20f);
	fBox(bx - 6.1f, pz - 0.9f, bx - 5.9f, pz - 0.3f, 0.7f, 0.8f, 0.15f, 0.15f, 0.15f); fBox(bx - 6.1f, pz + 0.3f, bx - 5.9f, pz + 0.9f, 0.7f, 0.8f, 0.15f, 0.15f, 0.15f);
	fBox(bx + 1.0f, pz - 1.0f, bx + 3.0f, pz + 1.0f, 0.0f, 1.8f, 0.20f, 0.45f, 0.85f);
	addF(FT_SLOPE, 0, bx + 3.0f, pz - 0.5f, bx + 6.0f, pz + 0.5f, 1.8f, 0.2f, 0.95f, 0.80f, 0.15f);
	addP(bx - 3.0f, pz + 2.2f, bx + 0.5f, pz + 4.2f, 0.09f, 0.80f, 0.72f, 0.50f);
	addSolid(bx + 1.0f, pz - 1.0f, bx + 3.0f, pz + 1.0f, 1.8f);
	bench(bx - 9.0f, bz - 4.0f, 0); bench(bx + 9.0f, bz - 4.0f, 0);
	addTree(bx + 9.5f, bz + 9.5f, lrnd(&st)); addTree(bx - 9.5f, bz + 9.5f, lrnd(&st));
	// perimeter fence
	fBox(bx - L, bz - L + 0.05f, bx + L, bz - L + 0.15f, 1.5f, 1.6f, 0.30f, 0.32f, 0.34f);
}

// Under the elevated expressway: a car park between the pillars (the deck itself is built in layoutFreeway).
static void layoutFwyBlock(float bx, float bz, unsigned st) {
	const float L = LOT_H;
	addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.20f, 0.20f, 0.22f);
	for (int side = 0; side < 2; side++) {
		float sg = side ? 1.0f : -1.0f;
		for (int k = 0; k <= 8; k++) {
			float z = bz - 10.0f + (float)k * 2.5f;
			float xa = bx + sg * 3.0f, xb = bx + sg * 7.6f;
			fFlat(fminf(xa, xb), z - 0.06f, fmaxf(xa, xb), z + 0.06f, 0.10f, 0.88f, 0.88f, 0.84f);
		}
		for (int k = 0; k < 8; k++) if (lrnd(&st) % 100 < 40) fCar(bx + sg * 5.3f, bz - 8.75f + (float)k * 2.5f, 0, lrnd(&st), 1);
		addLamp(bx + sg * 6.0f, FWY_Y - FWY_T - 0.25f, bz - 5.0f, 0.5f);
		addLamp(bx + sg * 6.0f, FWY_Y - FWY_T - 0.25f, bz + 5.0f, 0.5f);
	}
	weeds(bx - L, bz - L, bx + L, bz - 10.5f, 4, &st);
	weeds(bx - L, bz + 10.5f, bx + L, bz + L, 4, &st);
	dumpster(bx + 9.5f, bz + 10.0f, 1, lrnd(&st));
}

static void layoutBlock(int i, int j) {
	const float bx = BLK_C(i), bz = BLK_C(j);
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
		bench(bx - 8.0f, bz + 10.5f, 1); trashCan(bx + 8.0f, bz + 10.5f);
		if (lrnd(&st) % 100 < 50) flowerBed(bx - 11.0f, bz - 3.0f, bx - 9.0f, bz + 3.0f, &st);
		break;
	}
	case BT_LOW: {
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.40f, 0.40f, 0.41f);
		for (int s = 0; s < 4; s++) {
			float sx = bx + ((s & 1) ? 6.3f : -6.3f), sz = bz + ((s & 2) ? 6.3f : -6.3f);
			if (lrnd(&st) % 100 < 78) {
				float w = 8.0f + (float)(lrnd(&st) % 3), d = 8.0f + (float)(lrnd(&st) % 3);
				float jx = ((int)(lrnd(&st) % 3) - 1) * 0.5f, jz = ((int)(lrnd(&st) % 3) - 1) * 0.5f;
				int nf = 2 + (int)(lrnd(&st) % 4) + (int)(dt * 3.0f);
				const float* c = kBrick[lrnd(&st) % 5];
				Building* b = addB(0, sx + jx - w * 0.5f, sz + jz - d * 0.5f, sx + jx + w * 0.5f, sz + jz + d * 0.5f, (float)nf * FLOOR_H, c[0], c[1], c[2], st);
				if (b && lrnd(&st) % 3 == 0) b->deco = 1;
				if (b && nf >= 3 && lrnd(&st) % 100 < 14) addDoor(b->x0, b->z0, b->x1, b->z1, (s & 1) ? 1 : 0, sz + jz, RM_APT);
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
		// the other half of the lot: a car park with a few trees and cars
		float p0 = -sgn * 1.5f, p1 = -sgn * (L - 0.5f);
		float plo = p0 < p1 ? p0 : p1, phi = p0 < p1 ? p1 : p0;
		if (horiz) addP(bx - L + 1.0f, bz + plo, bx + L - 1.0f, bz + phi, 0.08f, 0.17f, 0.17f, 0.19f);
		else       addP(bx + plo, bz - L + 1.0f, bx + phi, bz + L - 1.0f, 0.08f, 0.17f, 0.17f, 0.19f);
		for (int k = 0; k < 2; k++) {
			float t = (-6.0f + 12.0f * (float)k), n = -sgn * (L - 1.5f);
			addTree(horiz ? bx + t : bx + n, horiz ? bz + n : bz + t, lrnd(&st));
		}
		{
			float n = -sgn * 5.5f;
			if (lrnd(&st) % 100 < 60) fCar(horiz ? bx - 3.0f : bx + n, horiz ? bz + n : bz - 3.0f, horiz ? 0 : 1, lrnd(&st), 1);
			if (lrnd(&st) % 100 < 50) fCar(horiz ? bx + 4.0f : bx + n, horiz ? bz + n : bz + 4.0f, horiz ? 0 : 1, lrnd(&st), 1);
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
		float tpx[6], tpz[6];
		for (int tries = 0; tries < 40 && placed < 6; tries++) {
			float x = bx + (float)((int)(lrnd(&st) % 23) - 11), z = bz + (float)((int)(lrnd(&st) % 23) - 11);
			if (fabsf(x - bx) < 2.8f || fabsf(z - bz) < 2.8f) continue;
			if (pond && fabsf(x - bx) < 7.0f && fabsf(z - bz) < 5.5f) continue;
			if (pav && fabsf(x - pvx) < 4.0f && fabsf(z - pvz) < 4.0f) continue;
			int crowded = 0;                                   // two trees on the same spot are one tree drawn twice
			for (int k = 0; k < placed; k++) if (fabsf(x - tpx[k]) < 2.0f && fabsf(z - tpz[k]) < 2.0f) crowded = 1;
			if (crowded) continue;
			tpx[placed] = x; tpz[placed] = z;
			addTree(x, z, lrnd(&st));
			placed++;
		}
		bench(bx + 2.2f, bz - 6.0f, 0); bench(bx - 2.2f, bz + 6.0f, 0); bench(bx + 6.0f, bz + 2.2f, 1);
		for (int k = 0; k < 3; k++) fBush(bx + (float)((int)(lrnd(&st) % 21) - 10), bz + (float)((int)(lrnd(&st) % 21) - 10), 0.8f, 1.1f, 0.18f, 0.38f, 0.18f);
		unsigned q = st;
		if (!pond) flowerBed(bx + 4.0f, bz - 10.5f, bx + 8.0f, bz - 8.5f, &q);
		flowerBed(bx - 9.0f, bz + 8.5f, bx - 5.0f, bz + 10.5f, &q);
		streetLamp(bx - 2.0f, bz - 2.0f, 1.0f, 0.0f);
		weeds(bx - L, bz - L, bx + L, bz + L, 6, &st);
		break;
	}
	case BT_HOUSES: layoutHouses(bx, bz, st); break;
	case BT_MIXED:  layoutMixed(bx, bz, st, dt); break;
	case BT_COMM:   layoutComm(bx, bz, st); break;
	case BT_INDUS:  layoutIndus(bx, bz, st); break;
	case BT_DOCK:   layoutDock(bx, bz, st, i == GRID - 1); break;
	case BT_LOT:    layoutLot(bx, bz, st); break;
	case BT_COURT:  layoutCourt(bx, bz, st); break;
	case BT_FWY: layoutFwyBlock(bx, bz, st); break;
	case BT_RAIL: { // gravel strips either side of the embankment; stations at kStationI
		addP(bx - L, bz - L, bx + L, bz + L, 0.06f, 0.36f, 0.35f, 0.32f);
		weeds(bx - L, RAIL_Z - L, bx + L, RAIL_Z - RAIL_HW, 8, &st);
		weeds(bx - L, RAIL_Z + RAIL_HW, bx + L, RAIL_Z + L, 8, &st);
		if (i != kStationI[0] && i != kStationI[1]) {
			dumpster(bx + 4.0f, RAIL_Z + 9.0f, 1, lrnd(&st));
			if (lrnd(&st) % 100 < 60) fCar(bx - 4.0f, RAIL_Z - 9.6f, 0, 6, 1);
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
		bench(bx - 6.0f, bz - 6.0f, 1); bench(bx + 6.0f, bz - 6.0f, 1); bench(bx, bz + 6.0f, 1);
		fBush(bx - 6.0f, bz + 6.0f, 0.9f, 1.0f, 0.18f, 0.40f, 0.18f); fBush(bx + 6.0f, bz + 6.0f, 0.9f, 1.0f, 0.18f, 0.40f, 0.18f);
		{ unsigned q = st; flowerBed(bx - 11.5f, bz - 3.0f, bx - 9.5f, bz + 3.0f, &q); }
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

// ---- feature geometry ------------------------------------------------------------------------------
static void emitFeat(const Feat* f) {
	const float cx = (f->x0 + f->x1) * 0.5f, cz = (f->z0 + f->z1) * 0.5f;
	const float R = f->r, G = f->g, B = f->b;
	switch (f->type) {
	case FT_BOX:
		addBoxG(f->x0, f->z0, f->x1, f->z1, f->y0, f->y1, R, G, B, f->aux ? 0.5f : 0.82f, 1.0f);
		break;
	case FT_BOXU: { // box with a visible underside (ceilings, decks)
		addBoxG(f->x0, f->z0, f->x1, f->z1, f->y0, f->y1, R, G, B, 0.82f, 1.0f);
		float a[3] = {f->x0, f->y0, f->z0}, b[3] = {f->x1, f->y0, f->z0}, c[3] = {f->x1, f->y0, f->z1}, d[3] = {f->x0, f->y0, f->z1};
		quad(a, b, c, d, R * 0.42f, G * 0.42f, B * 0.42f);
		break;
	}
	case FT_POST:
		addPost(cx, cz, (f->x1 - f->x0) * 0.5f, f->y0, f->y1, R, G, B);
		break;
	case FT_CAR: { // parked car: body plus a cabin that doubles as glass and roof
		int ax = f->aux;
		float hx = (f->x1 - f->x0) * 0.5f, hz = (f->z1 - f->z0) * 0.5f;
		addBoxG(cx - hx, cz - hz, cx + hx, cz + hz, 0.15f, 1.05f, R, G, B, 0.7f, 1.0f);
		float gx = ax == 0 ? hx * 0.55f : hx * 0.85f, gz = ax == 0 ? hz * 0.85f : hz * 0.55f;
		addBoxG(cx - gx, cz - gz, cx + gx, cz + gz, 1.05f, 1.6f, 0.10f + 0.35f * R, 0.12f + 0.35f * G, 0.16f + 0.35f * B, 0.9f, 1.0f);
		break;
	}
	case FT_BUSH: { // squat pyramid
		float rad = (f->x1 - f->x0) * 0.5f;
		float p[4][3] = {{cx - rad, f->y0, cz}, {cx, f->y0, cz - rad}, {cx + rad, f->y0, cz}, {cx, f->y0, cz + rad}};
		float apex[3] = {cx, f->y1, cz};
		const float sh[4] = {0.62f, 0.60f, 0.95f, 0.80f};
		for (int k = 0; k < 4; k++) tri(p[k], p[(k + 1) & 3], apex, R * sh[k], G * sh[k], B * sh[k]);
		break;
	}
	case FT_PRISM: { // gable roof; aux 0 = ridge along x, 1 = along z
		float x0 = f->x0, x1 = f->x1, z0 = f->z0, z1 = f->z1, ya = f->y0, yb = f->y1;
		if (f->aux == 0) {
			float zm = cz;
			float a[3] = {x0, ya, z0}, b[3] = {x1, ya, z0}, c[3] = {x1, yb, zm}, d[3] = {x0, yb, zm};
			quad(a, b, c, d, R * 0.7f, G * 0.7f, B * 0.7f);
			float e[3] = {x0, ya, z1}, g[3] = {x1, ya, z1};
			quad(e, g, c, d, R, G, B);
			float t0[3] = {x0, yb, zm}, t1[3] = {x1, yb, zm};
			tri(a, e, t0, R * 0.55f, G * 0.55f, B * 0.55f);
			tri(b, g, t1, R * 0.8f, G * 0.8f, B * 0.8f);
		} else {
			float xm = cx;
			float a[3] = {x0, ya, z0}, b[3] = {x0, ya, z1}, c[3] = {xm, yb, z1}, d[3] = {xm, yb, z0};
			quad(a, b, c, d, R * 0.7f, G * 0.7f, B * 0.7f);
			float e[3] = {x1, ya, z0}, g[3] = {x1, ya, z1};
			quad(e, g, c, d, R, G, B);
			float t0[3] = {xm, yb, z0}, t1[3] = {xm, yb, z1};
			tri(a, e, t0, R * 0.55f, G * 0.55f, B * 0.55f);
			tri(b, g, t1, R * 0.8f, G * 0.8f, B * 0.8f);
		}
		break;
	}
	case FT_WALLQ: { // vertical quad along the line (x0,z0) - (x1,z1)
		float a[3] = {f->x0, f->y0, f->z0}, b[3] = {f->x1, f->y0, f->z1}, c[3] = {f->x1, f->y1, f->z1}, d[3] = {f->x0, f->y1, f->z0};
		quad(a, b, c, d, R, G, B);
		break;
	}
	case FT_FLAT: {
		float a[3] = {f->x0, f->y0, f->z0}, b[3] = {f->x1, f->y0, f->z0}, c[3] = {f->x1, f->y0, f->z1}, d[3] = {f->x0, f->y0, f->z1};
		quad(a, d, c, b, R, G, B);
		break;
	}
	case FT_SLOPE: { // surface from (x0, y0) to (x1, y1) across z0..z1
		float a[3] = {f->x0, f->y0, f->z0}, b[3] = {f->x1, f->y1, f->z0}, c[3] = {f->x1, f->y1, f->z1}, d[3] = {f->x0, f->y0, f->z1};
		quad(a, d, c, b, R, G, B);
		break;
	}
	case FT_SIDEZ: { // vertical side at z = z0, x0..x1, from the ground up to y0 (at x0) / y1 (at x1)
		float a[3] = {f->x0, 0.0f, f->z0}, b[3] = {f->x1, 0.0f, f->z0}, c[3] = {f->x1, f->y1, f->z0}, d[3] = {f->x0, f->y0, f->z0};
		quad(a, b, c, d, R * 0.85f, G * 0.85f, B * 0.85f);
		break;
	}
	}
}

// ---- street furniture at every junction -----------------------------------------------------------------------
static void layoutIntersections(void) {
	for (int a = 0; a < ROADS; a++) {
		for (int b = 0; b < ROADS; b++) {
			float xc = ROAD_C(a), zc = ROAD_C(b);
			unsigned h = hash2(a * 53 + 1, b * 29 + 7);
			float sx = xc - SW_OFF - 0.7f, sz = zc - SW_OFF - 0.7f; // street name sign, north-west corner
			fPost(sx, sz, 0.06f, 0.0f, 2.9f, 0.25f, 0.25f, 0.28f);
			fWall(sx - 0.65f, sz + 0.08f, sx + 0.65f, sz + 0.08f, 2.55f, 2.95f, 0.08f, 0.42f, 0.26f);
			fWall(sx + 0.08f, sz - 0.65f, sx + 0.08f, sz + 0.65f, 2.15f, 2.5f, 0.08f, 0.42f, 0.26f);
			fPost(xc + SW_OFF + 0.7f, zc - SW_OFF - 0.7f, 0.17f, 0.0f, 0.75f, 0.80f, 0.10f, 0.10f); // hydrant, north-east
			streetLamp(xc - SW_OFF - 0.7f, zc + SW_OFF + 0.7f, 0.0f, -1.0f);                         // street light, south-west
			if ((h >> 4) % 100 < 45) trashCan(xc + SW_OFF + 0.7f, zc - SW_OFF + 0.4f);
		}
	}
}

// ---- railway: an embankment with tunnels for the cross streets, an elevated track and two stations ----------
static void layoutRail(void) {
	const float xs = -CITY_H - 90.0f, xe = CITY_H + 90.0f;
	float a = xs;
	for (int k = 0; k <= ROADS; k++) {
		float b = (k < ROADS) ? ROAD_C(k) - TUN_HW : xe;
		if (b > a + 0.5f) { // solid embankment between two tunnels
			addSolid(a, RAIL_Z - RAIL_HW, b, RAIL_Z + RAIL_HW, RAIL_H);
			for (float x = a; x < b - 0.01f; x += 20.0f) {
				float x1 = fminf(x + 20.0f, b);
				fBox(x, RAIL_Z - RAIL_HW, x1, RAIL_Z + RAIL_HW, 0.0f, RAIL_H, 0.56f, 0.55f, 0.52f);
				fSideZ(RAIL_Z - RAIL_HW - 0.02f, x, x1, 0.55f, 0.55f, 0.18f, 0.18f, 0.20f); // dark base band
			}
		}
		a = (k < ROADS) ? ROAD_C(k) + TUN_HW : xe;
	}
	// ballast, rails and sleepers along the whole line
	for (float x = xs; x < xe - 0.01f; x += 20.0f) {
		fFlat(x, RAIL_Z - RAIL_HW + 0.3f, x + 20.0f, RAIL_Z + RAIL_HW - 0.3f, RAIL_H + 0.03f, 0.29f, 0.28f, 0.27f);
		fFlat(x, TRK_Z - 0.78f, x + 20.0f, TRK_Z - 0.64f, RAIL_H + 0.09f, 0.62f, 0.62f, 0.66f);
		fFlat(x, TRK_Z + 0.64f, x + 20.0f, TRK_Z + 0.78f, RAIL_H + 0.09f, 0.62f, 0.62f, 0.66f);
		for (int k = 0; k < 8; k++) fFlat(x + 1.0f + (float)k * 2.5f, TRK_Z - 1.3f, x + 1.5f + (float)k * 2.5f, TRK_Z + 1.3f, RAIL_H + 0.06f, 0.30f, 0.22f, 0.15f);
	}
	// tunnels under the line: roof slab, pipes, wall stripes, lights
	for (int k = 0; k < ROADS; k++) {
		float rc = ROAD_C(k);
		fBoxU(rc - TUN_HW, RAIL_Z - RAIL_HW, rc + TUN_HW, RAIL_Z + RAIL_HW, 4.4f, RAIL_H, 0.52f, 0.52f, 0.50f);
		fBox(rc - 6.4f, RAIL_Z - RAIL_HW, rc - 6.0f, RAIL_Z + RAIL_HW, 3.9f, 4.3f, 0.36f, 0.40f, 0.46f);
		fBox(rc + 6.0f, RAIL_Z - RAIL_HW, rc + 6.4f, RAIL_Z + RAIL_HW, 3.9f, 4.3f, 0.58f, 0.30f, 0.22f);
		for (int s = -1; s <= 1; s += 2) {
			float wx = rc + (float)s * (TUN_HW - 0.04f);
			fWall(wx, RAIL_Z - RAIL_HW, wx, RAIL_Z + RAIL_HW, 0.0f, 0.55f, 0.16f, 0.16f, 0.18f);
			fWall(wx, RAIL_Z - RAIL_HW, wx, RAIL_Z + RAIL_HW, 1.1f, 1.35f, 0.90f, 0.75f, 0.14f);
			addLamp(rc + (float)s * 4.2f, 4.3f, RAIL_Z - 3.8f, 0.5f);
			addLamp(rc + (float)s * 4.2f, 4.3f, RAIL_Z + 3.8f, 0.5f);
			unsigned h = hash2(k * 7 + s, 4242);
			if (h % 100 < 35) { const float* g = flowerPal[(h >> 8) % 5]; fWall(wx, RAIL_Z - 3.0f + (float)((h >> 12) % 3), wx, RAIL_Z + 1.5f + (float)((h >> 12) % 3), 0.7f, 2.5f, g[0], g[1], g[2]); }
		}
	}
	// stations: a platform with a canopy on top of the embankment and a ticket hall at street level
	for (int s = 0; s < 2; s++) {
		float bx = BLK_C(kStationI[s]);
		float px0 = bx - 30.0f, px1 = bx + 30.0f, pz0 = PLAT_Z0, pz1 = RAIL_Z + RAIL_HW - 0.2f;
		fBox(px0, pz0, px1, pz1, RAIL_H, RAIL_H + 0.5f, 0.72f, 0.72f, 0.70f);
		fFlat(px0, pz0 + 0.2f, px1, pz0 + 0.55f, RAIL_H + 0.53f, 0.95f, 0.80f, 0.10f);          // yellow safety line
		fBox(px0 + 2.0f, pz0 + 0.8f, px1 - 2.0f, pz1 - 0.3f, 8.9f, 9.2f, 0.30f, 0.34f, 0.40f);  // canopy
		for (int k = 0; k < 6; k++) fPost(px0 + 6.0f + (float)k * 9.6f, pz1 - 0.6f, 0.15f, RAIL_H + 0.5f, 8.9f, 0.30f, 0.32f, 0.36f);
		fBox(px0 + 6.0f, pz1 - 1.0f, px0 + 9.0f, pz1 - 0.2f, RAIL_H + 0.5f, RAIL_H + 1.6f, 0.20f, 0.45f, 0.90f); // information board
		for (int k = 0; k < 3; k++) addLamp(bx - 18.0f + (float)k * 18.0f, 8.8f, (pz0 + pz1) * 0.5f, 0.6f);
		float z0 = RAIL_Z + RAIL_HW + 0.3f, z1 = z0 + 4.5f;
		float x0 = bx - 9.0f, x1 = bx + 9.0f;
		addSolid(x0, z0, x1, z1, 4.4f);
		fBox(x0, z0, x1, z1, 0.0f, 4.4f, 0.62f, 0.62f, 0.60f);
		facade(x0, z0, x1, z1, 3, x0 + 1.0f, x1 - 1.0f, 0.4f, 2.8f, 0.14f, 0.24f, 0.34f);
		facade(x0, z0, x1, z1, 3, x0, x1, 3.4f, 4.1f, 0.20f, 0.45f, 0.90f);
		addDoor(x0, z0, x1, z1, 3, bx, RM_STATION);
	}
}

// ---- elevated expressway: ramp at the west end, deck over the city, a bridge over the lake ------------------------
static void layoutFreeway(void) {
	const float Z = FWY_Z, HW = FWY_HW;
	const float top = FWY_Y, under = FWY_Y - FWY_T;
	int n = 0;
	for (float x = RAMP_X; x < FWY_X1 - 0.01f; x += 20.0f, n++) {
		float x1 = x + 20.0f;
		fBoxU(x, Z - HW, x1, Z + HW, under, top, 0.58f, 0.58f, 0.56f);
		fFlat(x, Z - HW + 1.0f, x1, Z + HW - 1.0f, top + 0.03f, 0.14f, 0.14f, 0.16f);
		fFlat(x, Z - HW + 1.3f, x1, Z - HW + 1.46f, top + 0.05f, 0.92f, 0.92f, 0.88f);
		fFlat(x, Z + HW - 1.46f, x1, Z + HW - 1.3f, top + 0.05f, 0.92f, 0.92f, 0.88f);
		for (int k = 0; k < 3; k++) {
			fFlat(x + 1.0f + (float)k * 6.0f, Z - 4.7f, x + 4.0f + (float)k * 6.0f, Z - 4.5f, top + 0.05f, 0.92f, 0.92f, 0.88f);
			fFlat(x + 1.0f + (float)k * 6.0f, Z + 4.5f, x + 4.0f + (float)k * 6.0f, Z + 4.7f, top + 0.05f, 0.92f, 0.92f, 0.88f);
		}
		fBox(x, Z - 0.4f, x1, Z + 0.4f, top, top + 0.9f, 0.64f, 0.64f, 0.62f);                // median barrier
		fBox(x, Z - HW, x1, Z - HW + 0.5f, top, top + 1.0f, 0.68f, 0.68f, 0.66f);              // edge barriers
		fBox(x, Z + HW - 0.5f, x1, Z + HW, top, top + 1.0f, 0.68f, 0.68f, 0.66f);
		if ((n & 1) == 0) { // street light on the edge, every 40 m, alternating sides
			float sg = ((n >> 1) & 1) ? 1.0f : -1.0f, lx = x + 10.0f, lz = Z + sg * (HW - 0.25f);
			fPost(lx, lz, 0.1f, top, top + 7.0f, 0.28f, 0.29f, 0.32f);
			fBox(lx - 0.06f, lz - sg * 2.2f, lx + 0.06f, lz, top + 6.9f, top + 7.05f, 0.28f, 0.29f, 0.32f);
			addLamp(lx, top + 6.8f, lz - sg * 2.2f, 0.7f);
		}
	}
	// pillars under the deck (and out in the water): a pair of columns and a cap
	for (float xp = BLK_C(0) - 40.0f * 2.0f; xp < FWY_X1 - 5.0f; xp += 40.0f) {
		if (xp < RAMP_X + 4.0f) continue;
		for (int s = -1; s <= 1; s += 2) {
			fBox(xp - 0.8f, Z + (float)s * 5.5f - 0.8f, xp + 0.8f, Z + (float)s * 5.5f + 0.8f, 0.0f, under, 0.60f, 0.60f, 0.58f);
			addSolid(xp - 0.8f, Z + (float)s * 5.5f - 0.8f, xp + 0.8f, Z + (float)s * 5.5f + 0.8f, top);
		}
		fBox(xp - 1.2f, Z - 8.0f, xp + 1.2f, Z + 8.0f, under - 1.0f, under, 0.56f, 0.56f, 0.54f);
	}
	// the west end: the road comes down to the ground
	const int nr = 4;
	for (int m = 0; m < nr; m++) {
		float xa = FWY_X0 + RAMP_L / (float)nr * (float)m, xb = xa + RAMP_L / (float)nr;
		float ya = 0.1f + (top - 0.1f) * (float)m / (float)nr, yb = 0.1f + (top - 0.1f) * (float)(m + 1) / (float)nr;
		fSlope(xa, xb, Z - HW + 0.5f, Z + HW - 0.5f, ya, yb, 0.14f, 0.14f, 0.16f);
		fSlope(xa, xb, Z - 0.1f, Z + 0.1f, ya + 0.03f, yb + 0.03f, 0.88f, 0.72f, 0.14f);
		fSideZ(Z - HW, xa, xb, ya + 1.0f, yb + 1.0f, 0.62f, 0.62f, 0.60f);
		fSideZ(Z + HW, xa, xb, ya + 1.0f, yb + 1.0f, 0.62f, 0.62f, 0.60f);
	}
	addSolid(FWY_X0, Z - HW, RAMP_X + 2.0f, Z + HW, top);
	// overhead gantry signs
	static const float gx[2] = {-190.0f, 110.0f};
	for (int g = 0; g < 2; g++) {
		float x = gx[g];
		fBox(x - 0.3f, Z - HW, x + 0.3f, Z - HW + 0.6f, top, top + 8.2f, 0.34f, 0.35f, 0.38f);
		fBox(x - 0.3f, Z + HW - 0.6f, x + 0.3f, Z + HW, top, top + 8.2f, 0.34f, 0.35f, 0.38f);
		fBox(x - 0.25f, Z - HW + 0.6f, x + 0.25f, Z + HW - 0.6f, top + 5.8f, top + 8.2f, 0.05f, 0.40f, 0.20f);
		for (int k = 0; k < 3; k++) for (int s = -1; s <= 1; s += 2)
			fWall(x + (float)s * 0.3f, Z - 7.0f + (float)k * 5.0f, x + (float)s * 0.3f, Z - 3.6f + (float)k * 5.0f, top + 6.5f + (float)(k & 1) * 0.4f, top + 7.0f + (float)(k & 1) * 0.4f, 0.95f, 0.95f, 0.92f);
	}
}

// ---- collision acceleration: a coarse grid of building lists -------------------------------------------------------
#define BK_C 16.0f
#define BK_O (-CITY_H - 100.0f)
#define BK_N 48
#define MAX_BKL 40000
static unsigned short bkStart[BK_N * BK_N + 1];
static unsigned short bkList[MAX_BKL];
static int bkOverflow = 0;
static unsigned short bkStamp[MAX_BLD];
static unsigned short bkGen = 0;

static int bkIdx(float v) { int i = (int)floorf((v - BK_O) / BK_C); return i < 0 ? 0 : (i >= BK_N ? BK_N - 1 : i); }

static void buildBuckets(void) {
	static unsigned short cnt[BK_N * BK_N + 1];
	memset(cnt, 0, sizeof cnt);
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1) {
			unsigned s = 0;
			for (int k = 0; k < BK_N * BK_N; k++) { bkStart[k] = (unsigned short)s; s += cnt[k]; cnt[k] = 0; }
			bkStart[BK_N * BK_N] = (unsigned short)s;
		}
		int total = 0;
		for (int i = 0; i < nBld; i++) {
			const Building* b = &bldg[i];
			int x0 = bkIdx(b->x0 - 2.0f), x1 = bkIdx(b->x1 + 2.0f), z0 = bkIdx(b->z0 - 2.0f), z1 = bkIdx(b->z1 + 2.0f);
			for (int z = z0; z <= z1; z++) for (int x = x0; x <= x1; x++) {
				int k = z * BK_N + x;
				if (pass == 0) cnt[k]++;
				else if (bkStart[k] + cnt[k] < MAX_BKL) { bkList[bkStart[k] + cnt[k]] = (unsigned short)i; cnt[k]++; }
				total++;
			}
		}
		if (pass == 0 && total >= MAX_BKL) { bkOverflow = 1; }
	}
}

// Gathers the indices of every building near the box (x0,z0)-(x1,z1), each once.
static int bkGather(float x0, float z0, float x1, float z1, unsigned short* out, int maxN) {
	int n = 0;
	if (++bkGen == 0) { memset(bkStamp, 0, sizeof bkStamp); bkGen = 1; }
	int ix0 = bkIdx(x0), ix1 = bkIdx(x1), iz0 = bkIdx(z0), iz1 = bkIdx(z1);
	for (int z = iz0; z <= iz1; z++) for (int x = ix0; x <= ix1; x++) {
		int k = z * BK_N + x;
		for (int q = bkStart[k]; q < bkStart[k + 1]; q++) {
			int id = bkList[q];
			if (bkStamp[id] == bkGen) continue;
			bkStamp[id] = bkGen;
			if (n < maxN) out[n++] = (unsigned short)id;
		}
	}
	return n;
}

// Height of whatever roof (freeway deck, tunnel slab) is above this spot; the camera stays below it.
static float ceilingAt(float x, float z) {
	float c = 99.0f;
	if (fabsf(z - FWY_Z) < FWY_HW && x >= RAMP_X && x <= FWY_X1) c = FWY_Y - FWY_T - 1.0f;
	if (fabsf(z - RAIL_Z) < RAIL_HW) {
		int k = (int)floorf((x - ROAD_C(0)) / ROAD_SP + 0.5f);
		if (k < 0) k = 0;
		if (k > ROADS - 1) k = ROADS - 1;
		if (fabsf(x - ROAD_C(k)) < TUN_HW) c = 4.3f;
	}
	return c;
}

static int gRoom = -1;                       // >= 0 while the player is inside a building
static const float kJacketClr[4][3] = {{0.10f, 0.10f, 0.12f}, {0.80f, 0.15f, 0.15f}, {0.15f, 0.35f, 0.85f}, {0.20f, 0.62f, 0.32f}};
static int gShades = 0, gCap = 0, gJacketIx = 0;      // what the player bought (sunglasses, cap, jacket colour)
// ---- interiors ---------------------------------------------------------------------------------------------------
// Six rooms, each built once into the vertex buffer far away from the city (room k is centred on x = 2000 + 100 k).
// A Room records where the player may walk, the furniture collision boxes, the shopkeeper and the service spot.
#define RM_X(k)  (2000.0f + 100.0f * (float)(k))
#define MAX_RSOL 56
typedef struct {
	float x0, z0, x1, z1, h;       // walkable bounds and ceiling height
	float spX, spZ;                // where the player appears (just inside the door)
	float exX, exZ;                // stand within 2 m of this spot to leave
	int   kHas;                    // shopkeeper?
	float kX, kZ, kFace;
	float aX, aZ;                  // service spot: counter, bed, ticket machine
	int   nSol;
	float sol[MAX_RSOL][4];
	int   nCrate;                  // loose crates to carry for cash (warehouse)
	float crate[6][2];
	int   start, count;
} Room;
static Room  rooms[RM_N];
static Room* rb;                   // the room being built

static void rSolid(float x0, float z0, float x1, float z1) {
	if (rb->nSol < MAX_RSOL) { float* s = rb->sol[rb->nSol++]; s[0] = x0; s[1] = z0; s[2] = x1; s[3] = z1; } else gDropped++;
}
static void rBox(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b, int solid) {
	addBoxG(x0, z0, x1, z1, y0, y1, r, g, b, 0.80f, 1.0f);
	if (solid) rSolid(x0, z0, x1, z1);
}
static void rWallQ(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) { // vertical quad
	float a[3] = {x0, y0, z0}, c[3] = {x1, y0, z1}, d[3] = {x1, y1, z1}, e[3] = {x0, y1, z0};
	quad(a, c, d, e, r, g, b);
}
static void rFlat(float x0, float z0, float x1, float z1, float y, float r, float g, float b) {
	float a[3] = {x0, y, z0}, c[3] = {x1, y, z0}, d[3] = {x1, y, z1}, e[3] = {x0, y, z1};
	quad(a, e, d, c, r, g, b);
}
static void rLight(float x, float z, float hw, float hd, float h) { rFlat(x - hw, z - hd, x + hw, z + hd, h - 0.03f, 1.0f, 1.0f, 0.93f); }

// floor (optionally a 2 m chequer), ceiling, four walls with a skirting board and an accent stripe
static void rShell(float x0, float z0, float x1, float z1, float h, const float wl[3], const float f1[3], const float f2[3], int tile, const float ac[3]) {
	if (tile) {
		int ni = 0;
		for (float x = x0; x < x1 - 0.01f; x += 2.0f, ni++) {
			int nj = 0;
			for (float z = z0; z < z1 - 0.01f; z += 2.0f, nj++) {
				const float* c = ((ni + nj) & 1) ? f2 : f1;
				rFlat(x, z, fminf(x + 2.0f, x1), fminf(z + 2.0f, z1), 0.02f, c[0], c[1], c[2]);
			}
		}
	} else rFlat(x0, z0, x1, z1, 0.02f, f1[0], f1[1], f1[2]);
	rFlat(x0, z0, x1, z1, h, fminf(1.0f, wl[0] * 1.05f), fminf(1.0f, wl[1] * 1.05f), fminf(1.0f, wl[2] * 1.05f));
	const float k[4] = {0.92f, 0.80f, 0.86f, 0.88f};
	rWallQ(x0, z0, x1, z0, 0.0f, h, wl[0] * k[0], wl[1] * k[0], wl[2] * k[0]);
	rWallQ(x0, z1, x1, z1, 0.0f, h, wl[0] * k[1], wl[1] * k[1], wl[2] * k[1]);
	rWallQ(x0, z0, x0, z1, 0.0f, h, wl[0] * k[2], wl[1] * k[2], wl[2] * k[2]);
	rWallQ(x1, z0, x1, z1, 0.0f, h, wl[0] * k[3], wl[1] * k[3], wl[2] * k[3]);
	const float o = 0.02f;
	for (int w = 0; w < 4; w++) { // skirting (dark) and stripe (accent colour) just in front of each wall
		float ax = w == 2 ? x0 + o : (w == 3 ? x1 - o : x0), bx = w == 2 ? x0 + o : (w == 3 ? x1 - o : x1);
		float az = w == 0 ? z0 + o : (w == 1 ? z1 - o : z0), bz = w == 0 ? z0 + o : (w == 1 ? z1 - o : z1);
		rWallQ(ax, az, bx, bz, 0.0f, 0.22f, wl[0] * 0.45f, wl[1] * 0.45f, wl[2] * 0.45f);
		rWallQ(ax, az, bx, bz, 1.15f, 1.35f, ac[0], ac[1], ac[2]);
	}
}
// door in the south wall (z = zw): frame, glass, an EXIT sign and a mat
static void rDoor(float x, float zw) {
	rWallQ(x - 1.0f, zw - 0.03f, x + 1.0f, zw - 0.03f, 0.0f, 2.5f, 0.20f, 0.20f, 0.23f);
	rWallQ(x - 0.8f, zw - 0.04f, x + 0.8f, zw - 0.04f, 0.0f, 2.3f, 0.52f, 0.74f, 0.88f);
	rWallQ(x - 0.04f, zw - 0.05f, x + 0.04f, zw - 0.05f, 0.0f, 2.3f, 0.20f, 0.20f, 0.23f);
	rBox(x - 0.5f, zw - 0.9f, x + 0.5f, zw - 0.8f, 2.65f, 2.95f, 0.10f, 0.85f, 0.30f, 0);
	rFlat(x - 1.2f, zw - 1.6f, x + 1.2f, zw - 0.4f, 0.03f, 0.22f, 0.22f, 0.25f);
}
// window with a white frame in a wall along x (z = zw, glass faces +dz) or along z (x = xw)
static void rWinX(float x0, float x1, float zw, float dz, float y0, float y1) {
	rWallQ(x0 - 0.08f, zw + dz * 0.02f, x1 + 0.08f, zw + dz * 0.02f, y0 - 0.08f, y1 + 0.08f, 0.92f, 0.92f, 0.90f);
	rWallQ(x0, zw + dz * 0.03f, x1, zw + dz * 0.03f, y0, y1, 0.55f, 0.76f, 0.94f);
	rWallQ((x0 + x1) * 0.5f - 0.03f, zw + dz * 0.04f, (x0 + x1) * 0.5f + 0.03f, zw + dz * 0.04f, y0, y1, 0.92f, 0.92f, 0.90f);
}
static void rWinZ(float z0, float z1, float xw, float dx, float y0, float y1) {
	rWallQ(xw + dx * 0.02f, z0 - 0.08f, xw + dx * 0.02f, z1 + 0.08f, y0 - 0.08f, y1 + 0.08f, 0.92f, 0.92f, 0.90f);
	rWallQ(xw + dx * 0.03f, z0, xw + dx * 0.03f, z1, y0, y1, 0.55f, 0.76f, 0.94f);
	rWallQ(xw + dx * 0.04f, (z0 + z1) * 0.5f - 0.03f, xw + dx * 0.04f, (z0 + z1) * 0.5f + 0.03f, y0, y1, 0.92f, 0.92f, 0.90f);
}
static void rSetup(Room* r, float x0, float z0, float x1, float z1, float h, float doorX) {
	rb = r;
	memset(r, 0, sizeof *r);
	r->x0 = x0 + 0.45f; r->x1 = x1 - 0.45f; r->z0 = z0 + 0.45f; r->z1 = z1 - 0.45f; r->h = h;
	r->spX = doorX; r->spZ = z1 - 1.6f; r->exX = doorX; r->exZ = z1 - 1.2f;
	r->start = vcount;
}

static void buildStore(void) {
	const float cx = RM_X(RM_STORE);
	const float wl[3] = {0.93f, 0.93f, 0.89f}, f1[3] = {0.88f, 0.88f, 0.85f}, f2[3] = {0.66f, 0.70f, 0.76f}, ac[3] = {0.85f, 0.16f, 0.16f};
	rSetup(&rooms[RM_STORE], cx - 6.0f, -5.0f, cx + 6.0f, 5.0f, 3.4f, cx + 1.0f);
	rShell(cx - 6.0f, -5.0f, cx + 6.0f, 5.0f, 3.4f, wl, f1, f2, 1, ac);
	rDoor(cx + 1.0f, 5.0f);
	for (int i = 0; i < 3; i++) rLight(cx - 3.5f + 3.5f * (float)i, -1.0f, 0.8f, 0.3f, 3.4f), rLight(cx - 3.5f + 3.5f * (float)i, 2.5f, 0.8f, 0.3f, 3.4f);
	rWinX(cx - 5.0f, cx - 2.0f, 5.0f, -1.0f, 1.0f, 2.6f);
	// drinks fridges along the back wall
	for (int i = 0; i < 4; i++) {
		float x = cx - 5.6f + 1.35f * (float)i;
		rBox(x, -5.0f, x + 1.3f, -4.15f, 0.0f, 2.2f, 0.78f, 0.86f, 0.92f, 0);
		rWallQ(x + 0.1f, -4.14f, x + 1.2f, -4.14f, 0.25f, 2.0f, 0.10f, 0.22f, 0.30f);
		rWallQ(x + 0.1f, -4.13f, x + 1.2f, -4.13f, 0.45f, 0.75f, 0.90f, 0.30f, 0.20f);
		rWallQ(x + 0.1f, -4.13f, x + 1.2f, -4.13f, 0.95f, 1.25f, 0.20f, 0.60f, 0.90f);
		rWallQ(x + 0.1f, -4.13f, x + 1.2f, -4.13f, 1.45f, 1.75f, 0.95f, 0.80f, 0.20f);
	}
	rSolid(cx - 5.6f, -5.0f, cx - 0.2f, -4.1f);
	// two aisles of shelves
	for (int row = 0; row < 2; row++) {
		float z = -1.7f + 2.2f * (float)row;
		for (int u = 0; u < 3; u++) {
			float x = cx - 5.3f + 1.45f * (float)u;
			rBox(x, z, x + 1.3f, z + 0.8f, 0.0f, 1.55f, 0.74f, 0.74f, 0.78f, 1);
			static const float pc[3][3] = {{0.95f, 0.78f, 0.20f}, {0.85f, 0.25f, 0.25f}, {0.25f, 0.60f, 0.85f}};
			for (int s = 0; s < 3; s++) {
				const float* c = pc[(u + s + row) % 3];
				rBox(x + 0.05f, z - 0.04f, x + 1.25f, z, 0.25f + 0.45f * (float)s, 0.55f + 0.45f * (float)s, c[0], c[1], c[2], 0);
				rBox(x + 0.05f, z + 0.8f, x + 1.25f, z + 0.84f, 0.25f + 0.45f * (float)s, 0.55f + 0.45f * (float)s, c[2], c[0], c[1], 0);
			}
		}
	}
	// counter with a till, and the staff-only end of it
	rBox(cx, -3.9f, cx + 5.8f, -2.8f, 0.0f, 1.05f, 0.85f, 0.20f, 0.20f, 1);
	rBox(cx - 0.05f, -3.95f, cx + 5.85f, -2.75f, 1.05f, 1.12f, 0.93f, 0.93f, 0.90f, 0);
	rBox(cx + 2.8f, -3.5f, cx + 3.5f, -3.0f, 1.12f, 1.45f, 0.14f, 0.14f, 0.17f, 0);
	rBox(cx + 4.4f, -3.6f, cx + 5.4f, -3.0f, 1.12f, 1.50f, 0.95f, 0.75f, 0.25f, 0);
	rBox(cx + 1.0f, -4.95f, cx + 5.5f, -4.9f, 2.3f, 2.9f, 0.90f, 0.18f, 0.18f, 0); // sign over the counter
	rBox(cx + 1.2f, -4.93f, cx + 5.3f, -4.88f, 2.45f, 2.75f, 0.98f, 0.98f, 0.95f, 0);
	// ice-cream freezer and a snack rack on the east wall
	rBox(cx + 4.4f, 1.2f, cx + 5.6f, 3.4f, 0.0f, 0.95f, 0.80f, 0.88f, 0.95f, 1);
	rBox(cx + 4.45f, 1.25f, cx + 5.55f, 3.35f, 0.95f, 1.0f, 0.30f, 0.50f, 0.85f, 0);
	rBox(cx + 5.0f, -1.8f, cx + 5.6f, 0.6f, 0.0f, 1.7f, 0.95f, 0.55f, 0.15f, 1);
	rb->kHas = 1; rb->kX = cx + 3.4f; rb->kZ = -4.5f; rb->kFace = 3.14159f;
	rb->aX = cx + 3.2f; rb->aZ = -2.0f;
	rb->count = vcount - rb->start;
}

static void buildDiner(void) {
	const float cx = RM_X(RM_DINER);
	const float wl[3] = {0.95f, 0.86f, 0.88f}, f1[3] = {0.90f, 0.90f, 0.90f}, f2[3] = {0.14f, 0.14f, 0.16f}, ac[3] = {0.92f, 0.28f, 0.60f};
	rSetup(&rooms[RM_DINER], cx - 7.0f, -5.0f, cx + 7.0f, 5.0f, 3.4f, cx + 3.5f);
	rShell(cx - 7.0f, -5.0f, cx + 7.0f, 5.0f, 3.4f, wl, f1, f2, 1, ac);
	rDoor(cx + 3.5f, 5.0f);
	for (int i = 0; i < 4; i++) rLight(cx - 5.0f + 3.4f * (float)i, 0.5f, 0.8f, 0.3f, 3.4f);
	rWinX(cx - 6.0f, cx - 3.0f, 5.0f, -1.0f, 0.9f, 2.5f);
	rWinX(cx + 0.0f, cx + 2.2f, 5.0f, -1.0f, 0.9f, 2.5f);
	rWinZ(-3.0f, 0.0f, cx - 7.0f, 1.0f, 0.9f, 2.5f);
	// neon strip over the kitchen pass
	rBox(cx - 4.0f, -4.95f, cx + 4.0f, -4.9f, 2.5f, 2.75f, 1.0f, 0.35f, 0.70f, 0);
	rBox(cx - 4.0f, -4.95f, cx + 4.0f, -4.9f, 2.0f, 2.2f, 0.30f, 0.85f, 0.95f, 0);
	// counter with stools, grill and fridge behind it
	rBox(cx - 5.0f, -2.9f, cx + 5.0f, -1.9f, 0.0f, 1.05f, 0.72f, 0.74f, 0.78f, 1);
	rBox(cx - 5.05f, -2.95f, cx + 5.05f, -1.85f, 1.05f, 1.12f, 0.20f, 0.20f, 0.23f, 0);
	for (int i = 0; i < 6; i++) {
		float x = cx - 4.2f + 1.65f * (float)i;
		rBox(x - 0.04f, -1.34f, x + 0.04f, -1.26f, 0.0f, 0.62f, 0.60f, 0.60f, 0.64f, 0);
		rBox(x - 0.28f, -1.58f, x + 0.28f, -1.02f, 0.62f, 0.76f, 0.85f, 0.15f, 0.20f, 0);
	}
	rBox(cx - 5.0f, -4.6f, cx - 2.6f, -3.8f, 0.0f, 1.0f, 0.30f, 0.30f, 0.34f, 0);   // grill
	rBox(cx + 3.2f, -4.7f, cx + 5.0f, -3.9f, 0.0f, 2.0f, 0.82f, 0.84f, 0.88f, 0);   // fridge
	rSolid(cx - 5.0f, -4.6f, cx + 5.0f, -1.9f);
	// booths along the west wall and tables in the middle
	for (int k = 0; k < 3; k++) {
		float z = 0.2f + 1.5f * (float)k * 1.0f + (k > 0 ? 0.3f : 0.0f);
		float x = cx - 6.3f;
		rBox(x - 0.1f, z - 0.55f, x + 0.55f, z + 0.55f, 0.0f, 0.5f, 0.82f, 0.15f, 0.22f, 1);
		rBox(x - 0.1f, z - 0.55f, x + 0.12f, z + 0.55f, 0.5f, 1.2f, 0.82f, 0.15f, 0.22f, 0);
		rBox(x + 1.0f, z - 0.5f, x + 1.9f, z + 0.5f, 0.72f, 0.80f, 0.92f, 0.92f, 0.88f, 1);
		rBox(x + 1.4f, z - 0.08f, x + 1.5f, z + 0.08f, 0.0f, 0.72f, 0.50f, 0.50f, 0.54f, 0);
		rBox(x + 2.35f, z - 0.55f, x + 3.0f, z + 0.55f, 0.0f, 0.5f, 0.82f, 0.15f, 0.22f, 1);
		rBox(x + 2.8f, z - 0.55f, x + 3.02f, z + 0.55f, 0.5f, 1.2f, 0.82f, 0.15f, 0.22f, 0);
	}
	for (int k = 0; k < 2; k++) {
		float x = cx - 1.0f + 2.6f * (float)k, z = 1.6f;   // the lane in front of the door (x = cx + 3.5) stays clear
		rBox(x - 0.55f, z - 0.55f, x + 0.55f, z + 0.55f, 0.72f, 0.80f, 0.92f, 0.92f, 0.88f, 1);
		rBox(x - 0.06f, z - 0.06f, x + 0.06f, z + 0.06f, 0.0f, 0.72f, 0.50f, 0.50f, 0.54f, 0);
		rBox(x - 0.45f, z - 1.25f, x + 0.45f, z - 0.75f, 0.0f, 0.5f, 0.15f, 0.40f, 0.80f, 0);
		rBox(x - 0.45f, z + 0.75f, x + 0.45f, z + 1.25f, 0.0f, 0.5f, 0.15f, 0.40f, 0.80f, 0);
		rSolid(x - 0.55f, z - 1.25f, x + 0.55f, z + 1.25f);
	}
	rBox(cx + 6.1f, -1.2f, cx + 6.55f, 0.2f, 0.0f, 1.7f, 0.95f, 0.30f, 0.65f, 1); // jukebox
	rBox(cx + 6.12f, -1.0f, cx + 6.5f, 0.0f, 1.1f, 1.5f, 0.98f, 0.85f, 0.30f, 0);
	rb->kHas = 1; rb->kX = cx + 0.8f; rb->kZ = -3.9f; rb->kFace = 3.14159f;
	rb->aX = cx + 0.8f; rb->aZ = -0.7f;
	rb->count = vcount - rb->start;
}

static void buildLaundry(void) {
	const float cx = RM_X(RM_LAUNDRY);
	const float wl[3] = {0.80f, 0.92f, 0.94f}, f1[3] = {0.92f, 0.95f, 0.96f}, f2[3] = {0.62f, 0.84f, 0.88f}, ac[3] = {0.12f, 0.62f, 0.66f};
	rSetup(&rooms[RM_LAUNDRY], cx - 6.0f, -4.5f, cx + 6.0f, 4.5f, 3.2f, cx - 1.5f);
	rShell(cx - 6.0f, -4.5f, cx + 6.0f, 4.5f, 3.2f, wl, f1, f2, 1, ac);
	rDoor(cx - 1.5f, 4.5f);
	for (int i = 0; i < 3; i++) rLight(cx - 3.5f + 3.5f * (float)i, 0.0f, 0.8f, 0.3f, 3.2f);
	rWinX(cx + 1.5f, cx + 4.5f, 4.5f, -1.0f, 0.9f, 2.4f);
	// washers on both side walls: white drum with a dark glass door
	for (int side = 0; side < 2; side++) {
		for (int k = 0; k < 5; k++) {
			float z = -3.4f + 1.05f * (float)k;
			float x0 = side ? cx + 4.95f : cx - 5.55f, x1 = side ? cx + 5.55f : cx - 4.95f;
			rBox(x0, z, x1, z + 0.95f, 0.0f, 1.05f, 0.92f, 0.93f, 0.95f, 1);
			float fx = side ? x0 - 0.03f : x1 + 0.03f;
			rWallQ(fx, z + 0.22f, fx, z + 0.72f, 0.28f, 0.78f, 0.20f, 0.30f, 0.42f);
			rWallQ(fx, z + 0.30f, fx, z + 0.64f, 0.36f, 0.70f, 0.40f, 0.60f, 0.78f);
			rBox(x0 + 0.05f, z + 0.05f, x1 - 0.05f, z + 0.9f, 1.05f, 1.12f, 0.70f, 0.72f, 0.76f, 0);
		}
	}
	// stacked dryers on the back wall, a folding table, a bench
	for (int k = 0; k < 4; k++) {
		float x = cx - 4.3f + 1.05f * (float)k;
		rBox(x, -4.5f, x + 0.95f, -3.6f, 0.0f, 2.1f, 0.88f, 0.89f, 0.92f, 0);
		rWallQ(x + 0.15f, -3.59f, x + 0.8f, -3.59f, 0.3f, 0.95f, 0.22f, 0.32f, 0.44f);
		rWallQ(x + 0.15f, -3.59f, x + 0.8f, -3.59f, 1.2f, 1.85f, 0.22f, 0.32f, 0.44f);
	}
	rSolid(cx - 4.3f, -4.5f, cx - 0.1f, -3.6f);
	rBox(cx - 1.6f, -0.9f, cx + 1.6f, 0.4f, 0.0f, 0.95f, 0.75f, 0.60f, 0.40f, 1);
	rBox(cx - 1.5f, -0.2f, cx - 0.7f, 0.2f, 0.95f, 1.15f, 0.95f, 0.55f, 0.65f, 0);
	rBox(cx + 0.2f, -0.7f, cx + 1.2f, -0.1f, 0.95f, 1.1f, 0.35f, 0.65f, 0.90f, 0);
	rBox(cx - 4.2f, 1.2f, cx - 2.6f, 1.8f, 0.0f, 0.5f, 0.30f, 0.34f, 0.40f, 1);   // bench: not in the lane in front of the door (x = cx - 1.5)
	// service counter with soap vending machine, attendant behind it
	rBox(cx + 1.0f, -3.9f, cx + 4.6f, -3.0f, 0.0f, 1.05f, 0.12f, 0.62f, 0.66f, 1);
	rBox(cx + 0.95f, -3.95f, cx + 4.65f, -2.95f, 1.05f, 1.12f, 0.93f, 0.93f, 0.90f, 0);
	rBox(cx + 4.7f, -4.2f, cx + 5.5f, -3.5f, 0.0f, 1.9f, 0.85f, 0.25f, 0.30f, 1); // soap machine
	rBox(cx + 4.8f, -3.5f, cx + 5.4f, -3.47f, 1.2f, 1.6f, 0.95f, 0.95f, 0.80f, 0);
	rSolid(cx + 1.0f, -4.2f, cx + 4.6f, -3.0f);
	rb->kHas = 1; rb->kX = cx + 2.8f; rb->kZ = -4.1f; rb->kFace = 3.14159f;
	rb->aX = cx + 2.8f; rb->aZ = -2.2f;
	rb->count = vcount - rb->start;
}

static void buildApt(void) {
	const float cx = RM_X(RM_APT);
	const float wl[3] = {0.86f, 0.80f, 0.70f}, f1[3] = {0.52f, 0.36f, 0.22f}, f2[3] = {0.48f, 0.32f, 0.20f}, ac[3] = {0.40f, 0.52f, 0.60f};
	rSetup(&rooms[RM_APT], cx - 5.0f, -4.0f, cx + 5.0f, 4.0f, 3.0f, cx);
	rShell(cx - 5.0f, -4.0f, cx + 5.0f, 4.0f, 3.0f, wl, f1, f2, 1, ac);
	rDoor(cx, 4.0f);
	rLight(cx - 1.5f, 0.0f, 0.7f, 0.3f, 3.0f); rLight(cx + 1.8f, 0.0f, 0.7f, 0.3f, 3.0f);
	rWinZ(-2.5f, 0.8f, cx - 5.0f, 1.0f, 0.8f, 2.4f);
	rWinX(cx + 0.5f, cx + 3.5f, -4.0f, 1.0f, 0.9f, 2.4f);
	rFlat(cx - 2.2f, -1.0f, cx + 2.2f, 2.4f, 0.04f, 0.60f, 0.22f, 0.22f); // rug
	// bed
	rBox(cx - 4.7f, -3.8f, cx - 2.3f, -1.4f, 0.0f, 0.5f, 0.45f, 0.30f, 0.20f, 1);
	rBox(cx - 4.65f, -3.75f, cx - 2.35f, -1.45f, 0.5f, 0.7f, 0.85f, 0.88f, 0.95f, 0);
	rBox(cx - 4.6f, -3.7f, cx - 3.4f, -3.0f, 0.7f, 0.85f, 0.95f, 0.95f, 0.98f, 0);
	rBox(cx - 4.6f, -2.4f, cx - 2.4f, -1.5f, 0.7f, 0.78f, 0.25f, 0.40f, 0.65f, 0);
	// sofa and TV
	rBox(cx + 0.8f, 1.2f, cx + 3.8f, 2.2f, 0.0f, 0.5f, 0.28f, 0.34f, 0.48f, 1);
	rBox(cx + 0.8f, 2.0f, cx + 3.8f, 2.3f, 0.5f, 1.1f, 0.24f, 0.30f, 0.44f, 0);
	rBox(cx + 1.4f, -3.9f, cx + 3.2f, -3.5f, 0.0f, 0.55f, 0.30f, 0.22f, 0.16f, 1);
	rBox(cx + 1.5f, -3.95f, cx + 3.1f, -3.88f, 0.9f, 1.8f, 0.05f, 0.05f, 0.07f, 0);
	rBox(cx + 1.6f, -3.88f, cx + 3.0f, -3.85f, 1.0f, 1.7f, 0.30f, 0.60f, 0.95f, 0);
	rBox(cx + 1.4f, 0.0f, cx + 3.2f, 0.7f, 0.0f, 0.42f, 0.55f, 0.42f, 0.30f, 1);
	// kitchenette on the east wall
	rBox(cx + 4.1f, -3.5f, cx + 4.9f, 0.0f, 0.0f, 0.95f, 0.90f, 0.90f, 0.88f, 1);
	rBox(cx + 4.1f, -3.5f, cx + 4.9f, 0.0f, 0.95f, 1.02f, 0.25f, 0.25f, 0.28f, 0);
	rBox(cx + 4.1f, -3.9f, cx + 4.9f, -3.5f, 0.0f, 1.9f, 0.84f, 0.86f, 0.90f, 1);
	rBox(cx + 4.1f, 0.6f, cx + 4.9f, 1.6f, 0.0f, 0.5f, 0.55f, 0.36f, 0.20f, 1);
	rb->aX = cx - 3.4f; rb->aZ = -0.9f; // the bed
	rb->count = vcount - rb->start;
}

static void buildWare(void) {
	const float cx = RM_X(RM_WARE);
	const float wl[3] = {0.60f, 0.62f, 0.64f}, f1[3] = {0.50f, 0.50f, 0.49f}, f2[3] = {0.46f, 0.46f, 0.45f}, ac[3] = {0.95f, 0.60f, 0.12f};
	rSetup(&rooms[RM_WARE], cx - 12.0f, -8.0f, cx + 12.0f, 8.0f, 7.5f, cx - 8.0f);
	rShell(cx - 12.0f, -8.0f, cx + 12.0f, 8.0f, 7.5f, wl, f1, f2, 0, ac);
	rDoor(cx - 8.0f, 8.0f);
	for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) rLight(cx - 9.0f + 6.0f * (float)i, -4.0f + 8.0f * (float)j, 1.0f, 0.4f, 7.5f);
	for (int k = 0; k < 4; k++) rFlat(cx - 10.0f + 6.0f * (float)k, -7.5f, cx - 9.8f + 6.0f * (float)k, 7.5f, 0.03f, 0.95f, 0.80f, 0.10f); // aisle lines
	// roll-up door on the east wall
	rWallQ(cx + 11.98f, -4.0f, cx + 11.98f, 0.5f, 0.0f, 4.2f, 0.70f, 0.72f, 0.76f);
	for (int k = 0; k < 8; k++) rWallQ(cx + 11.97f, -4.0f, cx + 11.97f, 0.5f, 0.5f + 0.5f * (float)k, 0.56f + 0.5f * (float)k, 0.55f, 0.57f, 0.62f);
	// pallet racking: two long rows with crates on them
	for (int row = 0; row < 2; row++) {
		float z = -6.9f + 3.4f * (float)row;
		rBox(cx - 10.8f, z, cx - 1.2f, z + 1.2f, 0.0f, 0.15f, 0.20f, 0.35f, 0.62f, 1);
		for (int u = 0; u < 5; u++) rBox(cx - 10.8f + 2.4f * (float)u, z, cx - 10.6f + 2.4f * (float)u, z + 1.2f, 0.0f, 5.0f, 0.20f, 0.35f, 0.62f, 0);
		for (int lv = 0; lv < 3; lv++) {
			rBox(cx - 10.8f, z, cx - 1.2f, z + 1.2f, 1.0f + 1.6f * (float)lv, 1.12f + 1.6f * (float)lv, 0.95f, 0.55f, 0.15f, 0);
			for (int u = 0; u < 4; u++) {
				float x = cx - 10.2f + 2.4f * (float)u;
				static const float cc[3][3] = {{0.72f, 0.54f, 0.34f}, {0.62f, 0.46f, 0.28f}, {0.78f, 0.62f, 0.40f}};
				const float* c = cc[(u + lv + row) % 3];
				rBox(x, z + 0.1f, x + 1.6f, z + 1.1f, 1.12f + 1.6f * (float)lv, 2.0f + 1.6f * (float)lv, c[0], c[1], c[2], 0);
			}
		}
		rSolid(cx - 10.8f, z, cx - 1.2f, z + 1.2f);
	}
	// pallets of cartons, a forklift
	rBox(cx + 1.0f, 3.5f, cx + 3.2f, 5.2f, 0.0f, 1.6f, 0.72f, 0.54f, 0.34f, 1);
	rBox(cx + 1.0f, 3.5f, cx + 3.2f, 5.2f, 0.0f, 0.18f, 0.55f, 0.42f, 0.26f, 0);
	rBox(cx - 3.0f, 3.0f, cx - 1.6f, 4.4f, 0.0f, 1.2f, 0.62f, 0.46f, 0.28f, 1);
	rBox(cx + 5.0f, 3.0f, cx + 7.0f, 5.5f, 0.2f, 1.2f, 0.95f, 0.78f, 0.10f, 1);
	rBox(cx + 5.1f, 3.2f, cx + 6.9f, 4.4f, 1.2f, 2.0f, 0.20f, 0.20f, 0.23f, 0);
	rBox(cx + 4.95f, 5.5f, cx + 5.1f, 6.9f, 0.05f, 0.15f, 0.55f, 0.56f, 0.60f, 0);
	rBox(cx + 6.9f, 5.5f, cx + 7.05f, 6.9f, 0.05f, 0.15f, 0.55f, 0.56f, 0.60f, 0);
	rBox(cx + 5.0f, 5.4f, cx + 7.0f, 5.6f, 0.0f, 3.0f, 0.30f, 0.30f, 0.33f, 0);
	// the dealer's cage in the north-east corner: counter, locker wall, gun rack
	rBox(cx + 6.0f, -2.7f, cx + 11.4f, -1.6f, 0.0f, 1.1f, 0.30f, 0.30f, 0.34f, 1);
	rBox(cx + 5.95f, -2.75f, cx + 11.45f, -1.55f, 1.1f, 1.18f, 0.70f, 0.72f, 0.76f, 0);
	rBox(cx + 6.0f, -7.5f, cx + 6.2f, -2.7f, 0.0f, 3.2f, 0.45f, 0.46f, 0.50f, 0);
	for (int k = 0; k < 4; k++) rBox(cx + 7.0f + 1.1f * (float)k, -7.45f, cx + 7.9f + 1.1f * (float)k, -7.0f, 0.0f, 2.2f, 0.22f, 0.30f, 0.26f, 0);
	rBox(cx + 7.2f, -7.4f, cx + 11.2f, -7.35f, 1.7f, 2.7f, 0.18f, 0.18f, 0.20f, 0);
	rSolid(cx + 6.0f, -7.5f, cx + 11.5f, -1.6f);
	rb->kHas = 1; rb->kX = cx + 8.6f; rb->kZ = -3.4f; rb->kFace = 3.14159f;
	rb->aX = cx + 8.6f; rb->aZ = -0.6f;
	// loose crates to carry out for cash
	rb->nCrate = 5;
	static const float cp[5][2] = {{-6.0f, 5.5f}, {-4.0f, 0.2f}, {1.5f, -3.0f}, {2.5f, 0.8f}, {-9.5f, 2.0f}};
	for (int i = 0; i < 5; i++) { rb->crate[i][0] = cx + cp[i][0]; rb->crate[i][1] = cp[i][1]; }
	rb->count = vcount - rb->start;
}

static void buildStation(void) {
	const float cx = RM_X(RM_STATION);
	const float wl[3] = {0.80f, 0.82f, 0.86f}, f1[3] = {0.74f, 0.74f, 0.72f}, f2[3] = {0.62f, 0.62f, 0.62f}, ac[3] = {0.16f, 0.36f, 0.78f};
	rSetup(&rooms[RM_STATION], cx - 9.0f, -11.0f, cx + 9.0f, 6.0f, 4.2f, cx);
	rooms[RM_STATION].z0 = -3.0f; // walkable: the ticket hall only
	rShell(cx - 9.0f, -11.0f, cx + 9.0f, 6.0f, 4.2f, wl, f1, f2, 1, ac);
	rDoor(cx, 6.0f);
	for (int i = 0; i < 3; i++) { rLight(cx - 5.0f + 5.0f * (float)i, 2.0f, 1.0f, 0.4f, 4.2f); rLight(cx - 5.0f + 5.0f * (float)i, -6.0f, 1.0f, 0.4f, 4.2f); }
	// the track bed behind the hall (floor lowered), platform edge, rails and sleepers
	rFlat(cx - 9.0f, -10.9f, cx + 9.0f, -6.3f, -0.80f, 0.26f, 0.25f, 0.24f);
	rFlat(cx - 9.0f, -3.7f, cx + 9.0f, -6.3f, 0.025f, 0.70f, 0.70f, 0.68f);
	rFlat(cx - 9.0f, -6.25f, cx + 9.0f, -5.95f, 0.04f, 0.95f, 0.80f, 0.10f);
	rWallQ(cx - 9.0f, -6.3f, cx + 9.0f, -6.3f, -0.8f, 0.0f, 0.45f, 0.45f, 0.46f);
	rFlat(cx - 9.0f, -9.2f, cx + 9.0f, -9.05f, -0.70f, 0.62f, 0.62f, 0.66f);
	rFlat(cx - 9.0f, -7.75f, cx + 9.0f, -7.6f, -0.70f, 0.62f, 0.62f, 0.66f);
	for (int k = 0; k < 14; k++) rFlat(cx - 8.5f + 1.3f * (float)k, -9.6f, cx - 8.2f + 1.3f * (float)k, -7.2f, -0.74f, 0.30f, 0.22f, 0.15f);
	// dark tunnel mouths in both side walls, where the train comes through
	rWallQ(cx - 8.98f, -9.9f, cx - 8.98f, -6.7f, -0.8f, 3.6f, 0.04f, 0.04f, 0.06f);
	rWallQ(cx + 8.98f, -9.9f, cx + 8.98f, -6.7f, -0.8f, 3.6f, 0.04f, 0.04f, 0.06f);
	// glass-style barrier between hall and platform: low wall, posts, yellow rail
	rBox(cx - 8.55f, -3.6f, cx + 8.55f, -3.4f, 0.0f, 0.9f, 0.55f, 0.58f, 0.64f, 1);
	for (int k = 0; k < 9; k++) rBox(cx - 8.4f + 2.1f * (float)k, -3.6f, cx - 8.25f + 2.1f * (float)k, -3.4f, 0.9f, 2.4f, 0.30f, 0.32f, 0.36f, 0);
	rBox(cx - 8.55f, -3.6f, cx + 8.55f, -3.4f, 2.3f, 2.45f, 0.95f, 0.80f, 0.10f, 0);
	// ticket machines, bench, information board
	for (int k = 0; k < 3; k++) {
		float x = cx + 2.4f + 1.5f * (float)k;
		rBox(x, -3.3f, x + 1.0f, -2.5f, 0.0f, 1.75f, 0.16f, 0.38f, 0.82f, 1);
		rBox(x + 0.12f, -2.52f, x + 0.88f, -2.49f, 1.1f, 1.6f, 0.50f, 0.92f, 0.70f, 0);
		rBox(x + 0.12f, -2.52f, x + 0.88f, -2.49f, 0.8f, 0.95f, 0.95f, 0.85f, 0.20f, 0);
	}
	rBox(cx - 7.5f, -0.6f, cx - 4.5f, 0.1f, 0.0f, 0.45f, 0.40f, 0.28f, 0.18f, 1);
	rBox(cx - 7.5f, 0.1f, cx - 4.5f, 0.2f, 0.45f, 1.0f, 0.38f, 0.26f, 0.16f, 0);
	rBox(cx - 7.0f, 1.8f, cx - 6.4f, 2.4f, 0.0f, 0.9f, 0.16f, 0.30f, 0.20f, 1); // bin
	rBox(cx - 3.0f, -3.35f, cx + 0.5f, -3.3f, 1.4f, 2.2f, 0.12f, 0.28f, 0.62f, 0); // route board
	rBox(cx - 2.8f, -3.34f, cx + 0.3f, -3.3f, 1.6f, 1.75f, 0.95f, 0.95f, 0.95f, 0);
	rBox(cx - 2.8f, -3.34f, cx + 0.3f, -3.3f, 1.9f, 2.0f, 0.95f, 0.30f, 0.25f, 0);
	rb->aX = cx + 3.9f; rb->aZ = -1.7f; // ticket machines
	rb->count = vcount - rb->start;
}

static void buildRooms(void) {
	buildStore(); buildDiner(); buildLaundry(); buildApt(); buildWare(); buildStation();
}

static int roomBlocked(float x, float z, float r) {
	const Room* m = &rooms[gRoom];
	if (x < m->x0 + r - 0.3f || x > m->x1 - r + 0.3f || z < m->z0 + r - 0.3f || z > m->z1 - r + 0.3f) return 1;
	for (int i = 0; i < m->nSol; i++) {
		const float* s = m->sol[i];
		if (x > s[0] - r && x < s[2] + r && z > s[1] - r && z < s[3] + r) return 1;
	}
	return 0;
}

// ---- build everything ---------------------------------------------------------------------------------------------
static void buildCity(void) {
	nBld = 0; nPatch = 0; nFeat = 0; nDoor = 0; nLamp = 0; gDropped = 0;
	for (int i = 0; i < GRID; i++) for (int j = 0; j < GRID; j++) blkType[i][j] = pickBlock(i, j);
	for (int i = 0; i < GRID; i++) for (int j = 0; j < GRID; j++) layoutBlock(i, j);
	layoutIntersections();
	layoutRail();
	layoutFreeway();
	// distant skyline ring: tall boxes, never reachable, heavily fogged; none over the lake
	for (int i = 0; i < 72; i++) {
		unsigned h = hash2(i, 31337);
		float ang = (float)i * 6.2832f / 72.0f + (float)(h & 15) * 0.004f;
		float rad = 380.0f + (float)((h >> 4) % 90);
		float w   = 9.0f + (float)((h >> 10) % 11);
		float hh  = 30.0f + (float)((h >> 14) % 100);
		float cx = sinf(ang) * rad, cz = cosf(ang) * rad;
		if (cx > CITY_H - 20.0f) continue;
		float tone = 0.42f + (float)((h >> 20) % 20) / 100.0f;
		addF(FT_BOX, 1, cx - w, cz - w, cx + w, cz + w, 0.0f, hh, tone * 0.95f, tone, tone * 1.08f);
	}

	// ground outside the city, in 40 m tiles (fog is computed per vertex, so big quads would look washed out).
	// East of the city is Lake Michigan.
	const float T = 40.0f, GE = 480.0f, GF = 1800.0f;
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
	globalEnd = vcount;

	// which district every item belongs to
	static unsigned char pCell[MAX_PATCH], bCell[MAX_BLD], fCell[MAX_FEAT];
	for (int n = 0; n < nPatch; n++) pCell[n] = (unsigned char)cellOf((patch[n].x0 + patch[n].x1) * 0.5f, (patch[n].z0 + patch[n].z1) * 0.5f);
	for (int n = 0; n < nBld; n++)   bCell[n] = (unsigned char)cellOf((bldg[n].x0 + bldg[n].x1) * 0.5f, (bldg[n].z0 + bldg[n].z1) * 0.5f);
	for (int n = 0; n < nFeat; n++)  fCell[n] = (unsigned char)cellOf((feat[n].x0 + feat[n].x1) * 0.5f, (feat[n].z0 + feat[n].z1) * 0.5f);

	// per district: asphalt, sidewalks and lots, roofs, trees, small features, road markings, crossings, signal poles
	for (int c = 0; c < CELLS; c++) {
		cellAStart[c] = vcount;
		for (int k = 0; k < ROADS; k++) { // asphalt, in 20 m pieces for the same reason
			float c0 = ROAD_C(k);
			for (float s = -ROAD_END; s < ROAD_END - 0.1f; s += 20.0f) {
				if (cellOf(c0, s + 10.0f) == c) {
					float v0[3] = {c0 - ROAD_HW, 0.02f, s}, v1[3] = {c0 + ROAD_HW, 0.02f, s};
					float v2[3] = {c0 + ROAD_HW, 0.02f, s + 20.0f}, v3[3] = {c0 - ROAD_HW, 0.02f, s + 20.0f};
					quad(v0, v3, v2, v1, 0.17f, 0.18f, 0.20f);
				}
				if (cellOf(s + 10.0f, c0) == c) {
					float h0[3] = {s, 0.02f, c0 - ROAD_HW}, h1[3] = {s, 0.02f, c0 + ROAD_HW};
					float h2[3] = {s + 20.0f, 0.02f, c0 + ROAD_HW}, h3[3] = {s + 20.0f, 0.02f, c0 - ROAD_HW};
					quad(h0, h3, h2, h1, 0.17f, 0.18f, 0.20f);
				}
			}
		}
		for (int n = 0; n < nPatch; n++) {
			if (pCell[n] != c) continue;
			const Patch* p = &patch[n];
			float s0[3] = {p->x0, p->y, p->z0}, s1[3] = {p->x1, p->y, p->z0};
			float s2[3] = {p->x1, p->y, p->z1}, s3[3] = {p->x0, p->y, p->z1};
			quad(s0, s3, s2, s1, p->r, p->g, p->b);
		}
		for (int n = 0; n < nBld; n++) {
			if (bCell[n] != c) continue;
			const Building* b = &bldg[n];
			float cx = (b->x0 + b->x1) * 0.5f, cz = (b->z0 + b->z1) * 0.5f;
			if (b->kind == 3) continue; // collision only
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
		for (int n = 0; n < nFeat; n++) if (fCell[n] == c) emitFeat(&feat[n]);
		for (int k = 0; k < ROADS; k++) {
			float c0 = ROAD_C(k);
			for (float s = -CITY_H; s < CITY_H; s += 8.0f) {
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
			if (b->kind != 0 || bCell[n] != c) continue;
			addWalls(b->x0, b->z0, b->x1, b->z1, b->h, b->r, b->g, b->b, b->uo, b->vo);
		}
		cellWCount[c] = vcount - cellWStart[c];
	}
	wallEnd = vcount;
	buildRooms();
	buildBuckets();
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
       SFX_PICKUP, SFX_HORN, SFX_STEP, SFX_SIREN, SFX_BEEP, SFX_GO, SFX_HURT, SFX_LEVEL, SFX_HEAL, SFX_COUNT };
typedef struct { s16* data; int n; } Snd;
// ambience loops: 0 city traffic, 1 rain, 2 night crickets, 3 daytime birds, 4 industrial hum, 5 tunnel drone,
// 6 expressway roar, 7 train rumble, 8 room tone. Channels: 1..4 for the first four, 14..18 for the rest.
#define AMB_COUNT 9
#define AMB_CH(i) ((i) < 4 ? 1 + (i) : 10 + (i))
static Snd sfx[SFX_COUNT], ambSnd[AMB_COUNT];
static int sndOK = 0;
static ndspWaveBuf sfxWB[8], ambWB[AMB_COUNT];
static int sfxNext = 0;
static float ambSet[AMB_COUNT] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};

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
	static const float dur[SFX_COUNT] = {0.22f, 0.13f, 0.55f, 0.90f, 1.40f, 0.12f, 0.05f, 0.12f, 0.25f, 0.55f, 0.09f, 3.60f, 0.22f, 0.65f, 0.16f, 0.90f, 0.60f};
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
		case SFX_BEEP: // countdown tick
			v = 0.6f * sinf(2.0f * PI_F * 880.0f * t) * fminf(1.0f, t * 200.0f) * expf(-t * 7.0f);
			break;
		case SFX_GO:   // countdown GO
			v = 0.6f * (sinf(2.0f * PI_F * 1320.0f * t) + 0.4f * sinf(2.0f * PI_F * 1980.0f * t)) * fminf(1.0f, t * 200.0f) * expf(-t * 4.5f);
			break;
		case SFX_HURT: // the player gets hit
			v = 0.9f * sinf(2.0f * PI_F * (150.0f - 220.0f * t) * t) * expf(-t * 18.0f) + 0.5f * lp * expf(-t * 25.0f);
			break;
		case SFX_LEVEL: { // rising arpeggio
			int k = (int)(t / 0.12f); if (k > 4) k = 4;
			static const float nf[5] = {392.0f, 494.0f, 587.0f, 784.0f, 988.0f};
			v = 0.5f * sinf(2.0f * PI_F * nf[k] * t) * expf(-fmodf(t, 0.12f) * 6.0f) * (k == 4 ? expf(-(t - 0.48f) * 3.5f) : 1.0f);
			break;
		}
		case SFX_HEAL:
			v = 0.45f * sinf(2.0f * PI_F * (440.0f + 500.0f * t) * t) * sinf(PI_F * t / dur[id]);
			break;
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
		} else if (kind == 4) { // industrial hum: mains hum, a distant machine pulse, the odd clank
			float am = 0.7f + 0.3f * sinf(2.0f * PI_F * 0.5f * t);
			float pulse = powf(fmaxf(0.0f, sinf(2.0f * PI_F * 2.0f * t)), 6.0f);
			float cl1 = expf(-fmodf(t + 0.0f, 4.0f) * 0.0f), clank = 0.0f;
			(void)cl1;
			float tc = t - 0.9f, td = t - 2.7f;
			if (tc > 0.0f) clank += expf(-tc * 9.0f) * (sinf(2.0f * PI_F * 520.0f * tc) + 0.6f * sinf(2.0f * PI_F * 1130.0f * tc));
			if (td > 0.0f) clank += 0.7f * expf(-td * 12.0f) * (sinf(2.0f * PI_F * 410.0f * td) + 0.5f * sinf(2.0f * PI_F * 980.0f * td));
			v = am * (0.07f * sinf(2.0f * PI_F * 60.0f * t) + 0.05f * sinf(2.0f * PI_F * 120.0f * t) + 0.03f * sinf(2.0f * PI_F * 180.0f * t))
			    + 0.5f * lp2 + 0.10f * pulse * sinf(2.0f * PI_F * 70.0f * t) + 0.07f * clank;
		} else if (kind == 5) { // tunnel / underpass: low rumble, slow beating, water drips
			float beat = 0.6f + 0.4f * sinf(2.0f * PI_F * 0.37f * t);
			float drip = 0.0f, t1 = t - 1.2f, t2 = t - 3.1f;
			if (t1 > 0.0f) drip += expf(-t1 * 22.0f) * sinf(2.0f * PI_F * 1750.0f * t1);
			if (t2 > 0.0f) drip += 0.8f * expf(-t2 * 22.0f) * sinf(2.0f * PI_F * 1450.0f * t2);
			v = beat * (0.09f * sinf(2.0f * PI_F * 38.0f * t) + 0.06f * sinf(2.0f * PI_F * 57.0f * t)) + 1.4f * b * 0.6f + 0.06f * drip;
		} else if (kind == 6) { // expressway: wide-band roar with cars passing over the joints
			float pass = expf(-powf((t - 0.7f) / 0.30f, 2.0f)) + 0.8f * expf(-powf((t - 1.9f) / 0.35f, 2.0f)) + 0.9f * expf(-powf((t - 3.0f) / 0.28f, 2.0f));
			float thump = powf(fmaxf(0.0f, sinf(2.0f * PI_F * 3.0f * t)), 12.0f);
			v = b * 1.5f + 0.35f * pass * (lp * 0.7f + lp2 * 2.5f) + 0.12f * thump * sinf(2.0f * PI_F * 48.0f * t);
		} else if (kind == 7) { // train: rail clatter over a low rumble
			float tau = fmodf(t, 0.25f), tau2 = fmodf(t + 0.0625f, 0.25f);
			float clack = expf(-tau * 38.0f) * sinf(2.0f * PI_F * 900.0f * tau) + 0.7f * expf(-tau2 * 45.0f) * sinf(2.0f * PI_F * 1250.0f * tau2);
			v = 0.16f * clack + 0.10f * sinf(2.0f * PI_F * 45.0f * t) + 0.6f * lp2 + 0.25f * b;
		} else if (kind == 8) { // room tone: fridge hum and air
			v = 0.05f * sinf(2.0f * PI_F * 50.0f * t) + 0.03f * sinf(2.0f * PI_F * 100.0f * t) + 0.25f * lp2;
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
	ndspChnSetMix(AMB_CH(i), mix);
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
	for (int ch = 0; ch < 19; ch++) {
		ndspChnReset(ch);
		ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	}
	ndspChnSetRate(0, (float)MUS_RATE);
	ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
	for (int ch = 1; ch < 19; ch++) {
		ndspChnSetRate(ch, (float)SFX_RATE);
		ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
	}
	for (int i = 0; i < SFX_COUNT; i++) sfx[i] = synthSfx(i);
	for (int i = 0; i < AMB_COUNT; i++) {
		ambSnd[i] = synthLoop(i, 4);
		if (!ambSnd[i].data) continue;
		memset(&ambWB[i], 0, sizeof ambWB[i]);
		ambWB[i].data_vaddr = ambSnd[i].data;
		ambWB[i].nsamples = (u32)ambSnd[i].n;
		ambWB[i].looping = true;
		ambSetVol(i, 0.0f);
		ndspChnWaveBufAdd(AMB_CH(i), &ambWB[i]);
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
	for (int i = 0; i < NCARS; i++) {
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
		for (int j = 0; j < NCARS && !stop; j++) {
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

static int roomBlocked(float x, float z, float r);
static int blockedR(float x, float z, float r) {
	if (gRoom >= 0) return roomBlocked(x, z, r);
	int k = bkIdx(z) * BK_N + bkIdx(x);       // every building within 2 m of this cell is listed in it
	for (int q = bkStart[k]; q < bkStart[k + 1]; q++) {
		const Building* b = &bldg[bkList[q]];
		if (x > b->x0 - r && x < b->x1 + r && z > b->z0 - r && z < b->z1 + r) return 1;
	}
	float lim = CITY_H + 20.0f;
	return (x < -lim || x > lim || z < -lim || z > lim);
}
static int blocked(float x, float z) { return blockedR(x, z, PLAYER_R); }

// ---- characters ------------------------------------------------------------
// Blocky low-poly person built from rotated boxes. Body space: feet at the origin,
// facing -Z. Each part swings about its joint (X axis), then the whole body leans
// and turns to its heading.
static float gPx, gPz, gFace, gLean, gBob;
static float gBaseY = 0.0f;                  // height of the floor under the bodies being drawn (target-practice room)
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
	float p0[3] = {x - hx, 0.10f + gBaseY, z - hz}, p1[3] = {x + hx, 0.10f + gBaseY, z - hz};
	float p2[3] = {x + hx, 0.10f + gBaseY, z + hz}, p3[3] = {x - hx, 0.10f + gBaseY, z + hz};
	gAlpha = a;
	quad(p0, p1, p2, p3, 0.0f, 0.0f, 0.0f);
	gAlpha = 1.0f;
}

// ---- weapon definitions ----
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
	for (int i = 0; i < NPEDS; i++) {
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

// Draws one person. gun = weapon id held (0 = none), aim = how far the gun is raised (0 hip .. 1 aiming).
static void drawBody(const Ped* p, float x, float z, float face, float ph, int walk, int panic, int dead, int deadT, int gun, float aim) {
	gPx = x; gPz = z; gFace = face;
	float s;
	if (dead) { // topples over backwards and stays down
		float fall = (float)deadT * 0.09f;
		gLean = fall < 1.45f ? fall : 1.45f;
		gBob = gBaseY + 0.15f * (gLean / 1.45f);
		s = 0.0f;
		gun = 0;
	} else {
		gLean = 0.0f;
		gBob = gBaseY + (walk ? fabsf(cosf(ph)) * 0.03f : 0.0f);
		s = walk ? sinf(ph) * (panic ? 0.8f : 0.5f) : 0.0f;
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
		if (gun > 0) sw = (side < 0) ? 0.25f + 1.20f * aim : 0.35f + 1.20f * aim;
		if (longSleeves) {
			addPart(jx, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.065f, 0.30f, 0.065f, sw, sh[0], sh[1], sh[2]);
			if (near) addPart(jx, 1.50f, 0.0f, 0.0f, -0.58f, 0.0f, 0.07f, 0.05f, 0.07f, sw, sk[0], sk[1], sk[2]);
		} else {
			addPart(jx, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.065f, 0.30f, 0.065f, sw, sk[0], sk[1], sk[2]);
			if (shortSleeves) addPart(jx, 1.50f, 0.0f, 0.0f, -0.10f, 0.0f, 0.075f, 0.14f, 0.075f, sw, sh[0], sh[1], sh[2]);
		}
	}

	if (gun > 0) { // the weapon in the right hand
		const WDef* w = &wdef[gun];
		float rs = 0.35f + 1.20f * aim;
		addPart(0.36f, 1.50f, 0.0f, 0.0f, -0.58f - w->len * 0.35f, 0.0f, w->thick * 0.5f, w->len * 0.5f, w->thick * 0.9f, rs, w->r, w->g, w->b);
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

static void drawPed(const Ped* p) {
	float x, z;
	pedPos(p, &x, &z);
	float mx = (p->axis == 0) ? (float)p->dir : 0.0f, mz = (p->axis == 0) ? 0.0f : (float)p->dir;
	drawBody(p, x, z, atan2f(-mx, -mz), p->ph, p->walk, p->panic > 0, p->dead, p->deadT, 0, 0.0f);
}

// Rebuilds the dynamic tail of the vertex buffer: shadows, cars, pedestrians (only those near the camera).
static void drawDynamic(void) {
	vcount = staticCount;
	for (int i = 0; i < NCARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float cx = (c->axis == 0) ? c->s : lane, cz = (c->axis == 0) ? lane : c->s;
		if (!inView(cx, cz)) continue;
		addShadow(cx, cz, (c->axis == 0) ? 2.4f : 1.3f, (c->axis == 0) ? 1.3f : 2.4f, 0.38f);
	}
	for (int i = 0; i < NPEDS; i++) {
		float x, z;
		pedPos(&peds[i], &x, &z);
		if (inView(x, z)) addShadow(x, z, 0.38f, 0.30f, 0.34f);
	}
	for (int i = 0; i < NCARS; i++) {
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
	for (int i = 0; i < NPEDS; i++) {
		float x, z;
		pedPos(&peds[i], &x, &z);
		if (inView(x, z)) drawPed(&peds[i]);
	}
	GSPGPU_FlushDataCache(&vbo[staticCount], (vcount - staticCount) * sizeof(Vertex));
}

// Emissive geometry (signal heads, head/tail lights): drawn with its own tint so it stays bright at night.
static void drawLights(int frame, float night) {
	int start = vcount;
	for (int i = 0; i < NCARS; i++) {
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

// ---- game modes: shared state ----------------------------------------------------------------
enum { M_FREE, M_DM, M_BR, M_TP };
enum { GS_COUNTDOWN, GS_PLAY, GS_OVER };
static int   gMode = M_FREE, gState = GS_PLAY, gCount = 0;
static float plHp = 100.0f;        // player health 0..100
static int   plSince = 999;        // frames since the player was last hurt
static int   plHeal = 0, plMedkits = 0, plFlash = 0;
static float plHitAng[4], plHitZ[4]; // where the last hits came from (x, z of the attacker)
static int   plHitLife[4];
static int   ownedW[WP_COUNT];     // weapons the player has
static float gPlX = 0.0f, gPlZ = 0.0f, gPlYaw = 0.0f, gCamYaw = 0.0f;
static int   gKills = 0, gLevel = 0, gTime = 0, gBanner = 0;

// ---- navigation grid (A* for the computer-controlled fighters) -------------------------------
#define NAV_C 2.25f
#define NAV_N 256
#define NAV_H (NAV_N * NAV_C * 0.5f)
static u8  navOpen[NAV_N * NAV_N];
static u16 navG[NAV_N * NAV_N], navStamp[NAV_N * NAV_N];
static u8  navPar[NAV_N * NAV_N];
static u32 navHeap[50000];
static int navHN = 0, navGoalX = 0, navGoalZ = 0;
static u16 navGen = 0;
static short navTmpX[600], navTmpZ[600];

static int navCell(float v) {
	int c = (int)floorf((v + NAV_H) / NAV_C);
	return c < 0 ? 0 : (c >= NAV_N ? NAV_N - 1 : c);
}
static float navWorld(int c) { return -NAV_H + ((float)c + 0.5f) * NAV_C; }
static int navIsOpen(float x, float z) { return navOpen[navCell(z) * NAV_N + navCell(x)]; }

static int navSnap(int* cx, int* cz);
static void initNav(void) {
	memset(navOpen, 1, sizeof navOpen);
	float lim = CITY_H + 6.0f;
	for (int cz = 0; cz < NAV_N; cz++)
		for (int cx = 0; cx < NAV_N; cx++)
			if (fabsf(navWorld(cx)) > lim || fabsf(navWorld(cz)) > lim) navOpen[cz * NAV_N + cx] = 0;
	for (int i = 0; i < nBld; i++) { // buildings, trees and fountains with a metre of clearance
		const Building* b = &bldg[i];
		const float m = 1.0f;
		int x0 = navCell(b->x0 - m), x1 = navCell(b->x1 + m), z0 = navCell(b->z0 - m), z1 = navCell(b->z1 + m);
		for (int cz = z0; cz <= z1; cz++)
			for (int cx = x0; cx <= x1; cx++) {
				float wx = navWorld(cx), wz = navWorld(cz);
				if (wx > b->x0 - m && wx < b->x1 + m && wz > b->z0 - m && wz < b->z1 + m) navOpen[cz * NAV_N + cx] = 0;
			}
	}
	// close off pockets that cannot be reached from the main streets (fenced courtyards etc.)
	static u8 seen[NAV_N * NAV_N];
	static u16 q[NAV_N * NAV_N];
	memset(seen, 0, sizeof seen);
	int s0 = navCell(0.0f) * NAV_N + navCell(0.0f), qh = 0, qt = 0;
	int sx = s0 % NAV_N, sz = s0 / NAV_N;
	navSnap(&sx, &sz);
	s0 = sz * NAV_N + sx;
	seen[s0] = 1; q[qt++] = (u16)s0;
	while (qh < qt) {
		int i = q[qh++], x = i % NAV_N, z = i / NAV_N;
		static const int dx[4] = {1, -1, 0, 0}, dz[4] = {0, 0, 1, -1};
		for (int d = 0; d < 4; d++) {
			int nx = x + dx[d], nz = z + dz[d];
			if (nx < 0 || nz < 0 || nx >= NAV_N || nz >= NAV_N) continue;
			int ni = nz * NAV_N + nx;
			if (navOpen[ni] && !seen[ni]) { seen[ni] = 1; q[qt++] = (u16)ni; }
		}
	}
	for (int i = 0; i < NAV_N * NAV_N; i++) if (!seen[i]) navOpen[i] = 0;
}

static int navSnap(int* cx, int* cz) { // nearest open cell
	if (navOpen[*cz * NAV_N + *cx]) return 1;
	for (int r = 1; r <= 8; r++)
		for (int dz = -r; dz <= r; dz++)
			for (int dx = -r; dx <= r; dx++) {
				if (abs(dx) != r && abs(dz) != r) continue;
				int x = *cx + dx, z = *cz + dz;
				if (x < 0 || z < 0 || x >= NAV_N || z >= NAV_N) continue;
				if (navOpen[z * NAV_N + x]) { *cx = x; *cz = z; return 1; }
			}
	return 0;
}

static void navPush(u32 k) {
	if (navHN >= 50000) return;
	int i = navHN++;
	while (i > 0) {
		int p = (i - 1) >> 1;
		if (navHeap[p] <= k) break;
		navHeap[i] = navHeap[p];
		i = p;
	}
	navHeap[i] = k;
}

static u32 navPop(void) {
	u32 top = navHeap[0], k = navHeap[--navHN];
	int i = 0;
	for (;;) {
		int c = 2 * i + 1;
		if (c >= navHN) break;
		if (c + 1 < navHN && navHeap[c + 1] < navHeap[c]) c++;
		if (navHeap[c] >= k) break;
		navHeap[i] = navHeap[c];
		i = c;
	}
	if (navHN > 0) navHeap[i] = k;
	return top;
}

static int navH(int cx, int cz) { // octile distance to the goal
	int dx = abs(cx - navGoalX), dz = abs(cz - navGoalZ);
	return 10 * (dx > dz ? dx : dz) + 4 * (dx < dz ? dx : dz);
}

// A* over the grid. Writes up to maxN cells (start first) and returns how many; 0 = no route.
static int navPath(float sx, float sz, float tx, float tz, short* oc, short* oz, int maxN) {
	static const signed char kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDz[8] = {0, 0, 1, -1, 1, -1, 1, -1};
	static const u8 kCost[8] = {10, 10, 10, 10, 14, 14, 14, 14};
	int scx = navCell(sx), scz = navCell(sz), tcx = navCell(tx), tcz = navCell(tz);
	if (!navSnap(&scx, &scz) || !navSnap(&tcx, &tcz)) return 0;
	int start = scz * NAV_N + scx, goal = tcz * NAV_N + tcx;
	if (start == goal) { oc[0] = (short)tcx; oz[0] = (short)tcz; return 1; }
	if (++navGen == 0) { memset(navStamp, 0, sizeof navStamp); navGen = 1; }
	navGoalX = tcx; navGoalZ = tcz;
	navHN = 0;
	navG[start] = 0; navStamp[start] = navGen; navPar[start] = 255;
	int bestIdx = start, bestH = navH(scx, scz), expanded = 0, found = 0;
	navPush(((u32)bestH << 16) | (u32)start);
	while (navHN > 0) {
		u32 key = navPop();
		int idx = (int)(key & 0xFFFFu), cx = idx % NAV_N, cz = idx / NAV_N, h = navH(cx, cz);
		if ((key >> 16) != (u32)(navG[idx] + h)) continue; // stale entry
		if (idx == goal) { found = 1; break; }
		if (h < bestH) { bestH = h; bestIdx = idx; }
		if (++expanded > 6000) break;
		for (int d = 0; d < 8; d++) {
			int nx = cx + kDx[d], nz = cz + kDz[d];
			if (nx < 0 || nz < 0 || nx >= NAV_N || nz >= NAV_N) continue;
			int nidx = nz * NAV_N + nx;
			if (!navOpen[nidx]) continue;
			if (d >= 4 && (!navOpen[cz * NAV_N + nx] || !navOpen[nz * NAV_N + cx])) continue; // no corner cutting
			int ng = navG[idx] + kCost[d];
			if (navStamp[nidx] != navGen || ng < navG[nidx]) {
				navStamp[nidx] = navGen; navG[nidx] = (u16)ng; navPar[nidx] = (u8)d;
				int f = ng + navH(nx, nz);
				if (f > 65535) continue;
				navPush(((u32)f << 16) | (u32)nidx);
			}
		}
	}
	int cur = found ? goal : bestIdx, L = 0;
	while (cur != start && L < 598) {
		navTmpX[L] = (short)(cur % NAV_N); navTmpZ[L] = (short)(cur / NAV_N); L++;
		int d = navPar[cur];
		cur = (cur / NAV_N - kDz[d]) * NAV_N + (cur % NAV_N - kDx[d]);
	}
	navTmpX[L] = (short)(start % NAV_N); navTmpZ[L] = (short)(start / NAV_N); L++;
	int n = 0;
	for (int i = L - 1; i >= 0 && n < maxN; i--) { oc[n] = navTmpX[i]; oz[n] = navTmpZ[i]; n++; }
	return n;
}

static int navSegClear(float ax, float az, float bx, float bz) { // straight line over open cells only
	float dx = bx - ax, dz = bz - az, L = sqrtf(dx * dx + dz * dz);
	int n = (int)L + 1;
	for (int i = 1; i <= n; i++) {
		float t = (float)i / (float)n;
		if (!navIsOpen(ax + dx * t, az + dz * t)) return 0;
	}
	return 1;
}

// ---- computer-controlled fighters ----------------------------------------------------------
#define MAX_BOTS 14
#define PATH_N   40
typedef struct {
	Ped look;                                   // how this one looks (skin, hair, outfit ...)
	int active, dead, deadT, gun, medkits, tgt, cool, burst, healT, thinkT, losT, los, pathN, pathI, repathT, strafeT, walk;
	float x, z, face, ph, hp, hpMax, goalX, goalZ, strafe, skill, aim, tdist;
	int own[WP_COUNT], ammoB[WP_COUNT];
	short pcx[PATH_N], pcz[PATH_N];
} Bot;
static Bot bots[MAX_BOTS];

// damage a computer-controlled fighter takes from one hit of each weapon, and how far each gun reaches
static const int   kDmg[WP_COUNT]   = {8, 22, 12, 11, 100};
static const float kRange[WP_COUNT] = {2.0f, 45.0f, 40.0f, 22.0f, 60.0f};
static int gExplSrc = -1;                           // who fired the rocket that is exploding: -1 player, 0.. a fighter
static void damageBot(int i, int dmg, int src);     // src: -1 player, >= 0 fighter, -3 storm
static void damagePlayer(int dmg, float fromX, float fromZ, int src);
static void splashDamage(float x, float z, int src);

// ---- weapons ---------------------------------------------------------------
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
	// torso (jacket), head, cap
	const float* jk = kJacketClr[gJacketIx];
	const float cap0 = gCap ? 0.12f : 0.08f, cap1 = gCap ? 0.28f : 0.08f, cap2 = gCap ? 0.70f : 0.09f;
	addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.36f, 0.0f, 0.27f, 0.36f, 0.14f, 0.0f, jk[0], jk[1], jk[2]);
	addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.00f, 0.0f, 0.13f, 0.14f, 0.13f, 0.0f, 0.85f, 0.66f, 0.52f);
	addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.14f, -0.02f, 0.15f, 0.05f, 0.16f, 0.0f, cap0, cap1, cap2);
	if (gCap) addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.11f, -0.18f, 0.11f, 0.015f, 0.06f, 0.0f, 0.85f, 0.15f, 0.15f);
	if (gShades) addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.03f, -0.13f, 0.115f, 0.028f, 0.012f, 0.0f, 0.03f, 0.03f, 0.04f);

	int armed = (curWeapon != WP_FISTS) && phoneE < 0.05f;
	float lSwing = -s * 0.9f;
	float rSwing = s * 0.9f * (1.0f - phoneE) + 1.65f * phoneE;
	if (armed) { // gun low at the hip, raised towards the crosshair while aiming
		rSwing = 0.35f + (1.55f - 0.35f) * aim + recoilAnim * 0.25f;
		lSwing = 0.25f + 1.20f * aim;
	}
	addPart(-0.36f, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, lSwing, jk[0], jk[1], jk[2]);
	addPart( 0.36f, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, rSwing, jk[0], jk[1], jk[2]);
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
#define N_TR 32
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
	if (gRoom < 0) {
		static unsigned short ids[MAX_BLD];
		float ex = o[0] + d[0] * best, ez = o[2] + d[2] * best;
		int nIds = bkGather(fminf(o[0], ex), fminf(o[2], ez), fmaxf(o[0], ex), fmaxf(o[2], ez), ids, MAX_BLD);
		for (int q = 0; q < nIds; q++) {
			const Building* b = &bldg[ids[q]];
			float t = rayBox(o, d, b->x0, 0.0f, b->z0, b->x1, b->h, b->z1);
			if (t >= 0.0f && t < best) best = t;
		}
	}
	for (int i = 0; i < NCARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float hx = (c->axis == 0) ? 2.1f : 1.0f, hz = (c->axis == 0) ? 1.0f : 2.1f;
		float cx = (c->axis == 0) ? c->s : lane, cz = (c->axis == 0) ? lane : c->s;
		float t = rayBox(o, d, cx - hx, 0.15f, cz - hz, cx + hx, 1.66f, cz + hz);
		if (t >= 0.0f && t < best) best = t;
	}
	for (int i = 0; i < NPEDS; i++) {
		if (peds[i].dead) continue;
		float x, z;
		pedPos(&peds[i], &x, &z);
		float t = rayCyl(o, d, x, z, 0.45f, 0.0f, 1.8f);
		if (t >= 0.0f && t < best) { best = t; *ped = i; }
	}
	for (int i = 0; i < MAX_BOTS; i++) { // computer-controlled fighters are reported as 100 + index
		const Bot* b = &bots[i];
		if (!b->active || b->dead) continue;
		float t = rayCyl(o, d, b->x, b->z, 0.45f, 0.0f, 1.8f);
		if (t >= 0.0f && t < best) { best = t; *ped = 100 + i; }
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
	for (int i = 0; i < NPEDS; i++) {
		if (peds[i].dead) continue;
		float px, pz;
		pedPos(&peds[i], &px, &pz);
		float ex = px - x, ey = 0.9f - y, ez = pz - z;
		if (ex * ex + ey * ey + ez * ez < 7.5f * 7.5f) hurtPed(i, 99);
	}	splashDamage(x, z, gExplSrc);
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
	float botDmg[MAX_BOTS] = {0};
	for (int k = 0; k < pellets; k++) {
		float d[3] = {fwd[0] + rnd() * spread, fwd[1] + rnd() * spread, fwd[2] + rnd() * spread};
		normalize3(d);
		int pi;
		float t = castRay(cam, d, range, &pi);
		float P[3] = {cam[0] + d[0] * t, cam[1] + d[1] * t, cam[2] + d[2] * t};
		if (k < 3) addTracer(muz, P);
		if (pi >= 100) { // a fighter: shotgun pellets lose punch with distance
			botDmg[pi - 100] += (float)kDmg[w] * (w == WP_SHOTGUN ? 1.0f - 0.7f * t / range : 1.0f);
			addFx(P[0], P[1], P[2], 0.3f, 0.0f, 5.0f, 0.9f, 0.1f, 0.1f);
		} else if (pi >= 0) hitMask |= 1u << pi;
		else if (t < range - 0.01f) addFx(P[0], P[1], P[2], 0.22f, 0.0f, 5.0f, 1.0f, 0.9f, 0.6f); // impact spark
	}
	for (int i = 0; i < N_PEDS; i++) if (hitMask & (1u << i)) hurtPed(i, 1);
	for (int i = 0; i < MAX_BOTS; i++) if (botDmg[i] > 0.0f) damageBot(i, (int)(botDmg[i] + 0.5f), -1);
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
		gExplSrc = -1;
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
// ---- game modes: fighters, loot, storm, Deathmatch and Battle Royale ---------------------------
static float rnd01(void) { return rnd() * 0.5f + 0.5f; }
static const char* kNames[16] = {"RILEY", "JAX", "MONA", "DEX", "SAGE", "KOA", "NOVA", "ZED", "LUCA", "IVY", "REX", "TESS", "ONYX", "WREN", "BLAZE", "CLEO"};

#define FEED_N 4
static char feedTxt[FEED_N][40];
static int  feedLife[FEED_N];
static void feedAdd(const char* a, const char* how, const char* b) {
	for (int i = FEED_N - 1; i > 0; i--) { memcpy(feedTxt[i], feedTxt[i - 1], 40); feedLife[i] = feedLife[i - 1]; }
	snprintf(feedTxt[0], 40, "%s %s %s", a, how, b);
	feedLife[0] = 420;
}
static int  gToast = 0;
static char gToastTxt[64];
static void toast(const char* s) { snprintf(gToastTxt, sizeof gToastTxt, "%s", s); gToast = 150; }

static int gOverT = 0, gPlace = 0, gWon = 0, gGo = 0, gLastMenu = 3, plUnseen = 0, plMoving = 0;
static int gCratesOn = 1;

// ---- loot ----
enum { LK_WEAPON, LK_AMMO, LK_MEDKIT };
typedef struct { float x, z; int kind, w, active; } Loot;
#define N_LOOT 110
static Loot loot[N_LOOT];

static int losClear(float ax, float az, float bx, float bz) {
	float dx = bx - ax, dz = bz - az, L = sqrtf(dx * dx + dz * dz);
	if (L < 0.5f) return 1;
	float o[3] = {ax, 1.3f, az}, d[3] = {dx / L, 0.0f, dz / L};
	float x0 = fminf(ax, bx) - 1.0f, x1 = fmaxf(ax, bx) + 1.0f, z0 = fminf(az, bz) - 1.0f, z1 = fmaxf(az, bz) + 1.0f;
	static unsigned short ids[MAX_BLD];
	int nIds = bkGather(x0, z0, x1, z1, ids, MAX_BLD);
	for (int q = 0; q < nIds; q++) {
		const Building* b = &bldg[ids[q]];
		if (b->x1 < x0 || b->x0 > x1 || b->z1 < z0 || b->z0 > z1) continue;
		float t = rayBox(o, d, b->x0, 0.0f, b->z0, b->x1, b->h, b->z1);
		if (t >= 0.0f && t < L) return 0;
	}
	return 1;
}

// A random open spot on the street grid, optionally far from / hidden from a point.
static int pickSpot(float* ox, float* oz, float fx, float fz, float minD, float maxD, int hidden) {
	for (int k = 0; k < 120; k++) {
		float spotR = gMode == M_FREE ? CITY_H - 6.0f : 215.0f; // the modes are played in the middle of the city
		float x = rnd() * spotR, z = rnd() * spotR;
		if (!navIsOpen(x, z) || !navIsOpen(x + 2.0f, z) || !navIsOpen(x - 2.0f, z) || !navIsOpen(x, z + 2.0f) || !navIsOpen(x, z - 2.0f)) continue;
		float d = sqrtf((x - fx) * (x - fx) + (z - fz) * (z - fz));
		if (d < minD || d > maxD) continue;
		if (hidden && losClear(x, z, fx, fz)) continue;
		*ox = x; *oz = z;
		return 1;
	}
	*ox = ROAD_C(4 + (int)(rnd01() * 4.99f)); *oz = ROAD_C(4 + (int)(rnd01() * 4.99f)); // any central junction
	return 0;
}

// ---- player health ----
static void plDie(int cause) {
	if (gState != GS_PLAY) return;
	gState = GS_OVER; gOverT = 0; gWon = 0;
	int alive = 0;
	for (int i = 0; i < MAX_BOTS; i++) if (bots[i].active && !bots[i].dead) alive++;
	gPlace = alive + 1;
	(void)cause;
}

static void damagePlayer(int dmg, float fx, float fz, int src) {
	if (gState != GS_PLAY || plHp <= 0.0f) return;
	plHp -= (float)dmg; plSince = 0; plFlash = 14; plHeal = 0;
	if (src != -3) { // remember where the shot came from for the red direction markers
		int slot = 0;
		for (int i = 1; i < 4; i++) if (plHitLife[i] < plHitLife[slot]) slot = i;
		plHitAng[slot] = fx; plHitZ[slot] = fz; plHitLife[slot] = 70;
	}
	playSfx(SFX_HURT, 0.8f);
	if (plHp <= 0.0f) { plHp = 0.0f; plDie(src); }
}

static void botName(int i, char* out, int n) { snprintf(out, (size_t)n, "%s", kNames[i & 15]); }

// ---- loot drops ----
static void dropLoot(float x, float z, int kind, int w) {
	for (int i = 0; i < N_LOOT; i++) if (!loot[i].active) {
		loot[i].x = x + rnd() * 0.8f; loot[i].z = z + rnd() * 0.8f; loot[i].kind = kind; loot[i].w = w; loot[i].active = 1;
		return;
	}
}

static const int dmLevelAt[6] = {0, 10, 25, 30, 45, 50}; // kills needed to reach levels 0..5
static int levelFor(int kills) { int l = 0; for (int i = 1; i < 6; i++) if (kills >= dmLevelAt[i]) l = i; return l; }
static const int   dmCool[6] = {46, 34, 26, 20, 15, 11};     // frames between shots
static const float dmAcc[6]  = {0.30f, 0.38f, 0.46f, 0.54f, 0.60f, 0.68f};
static const int   dmHp[6]   = {50, 60, 70, 80, 95, 110};
static const int   dmMax[6]  = {3, 4, 5, 6, 7, 8};
static const int   dmBurst[6] = {1, 2, 2, 3, 3, 4};
static const int   dmGun[6]  = {WP_PISTOL, WP_PISTOL, WP_SMG, WP_SMG, WP_SMG, WP_SMG};

static void damageBot(int i, int dmg, int src) {
	Bot* b = &bots[i];
	if (!b->active || b->dead || gState == GS_OVER) return;
	b->hp -= (float)dmg;
	if (src == -1) { hitMark = 8; playSfx(SFX_HIT, 0.5f); }
	addFx(b->x, 1.2f, b->z, 0.45f, 0.02f, 8.0f, 0.8f, 0.05f, 0.05f);
	if (b->hp > 0.0f) { if (gMode == M_BR) b->tdist = 0.0f; return; }
	b->dead = 1; b->deadT = 0; b->walk = 0; b->hp = 0.0f;
	char nm[16]; botName(i, nm, 16);
	if (src == -1) {
		gKills++; killMark = 16;
		if (gMode == M_BR) feedAdd("YOU", ">", nm);
		if (gMode == M_DM) {
			for (int w = 1; w < WP_COUNT; w++) { ammo[w] += (wdef[w].pickupAmmo + 1) / 2; if (ammo[w] > 999) ammo[w] = 999; }
			int nl = levelFor(gKills);
			if (nl > gLevel) { gLevel = nl; gBanner = 220; playSfx(SFX_LEVEL, 0.9f); }
		}
	} else if (gMode == M_BR) {
		if (src == -3) feedAdd(nm, "> STORM", "");
		else { char a[16]; botName(src, a, 16); feedAdd(a, ">", nm); bots[src].look.hp++; }
	}
	if (gMode == M_BR) { // drop what it was carrying
		for (int w = WP_COUNT - 1; w >= 1; w--) if (b->own[w]) { dropLoot(b->x, b->z, LK_WEAPON, w); break; }
		if (b->medkits > 0) dropLoot(b->x, b->z, LK_MEDKIT, 0);
		dropLoot(b->x, b->z, LK_AMMO, 0);
	}
}

static void splashDamage(float x, float z, int src) {
	for (int i = 0; i < MAX_BOTS; i++) {
		Bot* b = &bots[i];
		if (!b->active || b->dead || i == src) continue;
		float d = sqrtf((b->x - x) * (b->x - x) + (b->z - z) * (b->z - z));
		if (d < 6.5f) damageBot(i, (int)(100.0f * (1.0f - d / 7.5f)) + 8, src);
	}
	if (gState == GS_PLAY && gMode != M_FREE) {
		float d = sqrtf((gPlX - x) * (gPlX - x) + (gPlZ - z) * (gPlZ - z));
		if (d < (src == -1 ? 3.5f : 6.0f)) damagePlayer(src == -1 ? 35 : (gMode == M_DM ? 30 : 60), x, z, src);
	}
}

// ---- fighters ----
static int botsAlive(void) { int n = 0; for (int i = 0; i < MAX_BOTS; i++) if (bots[i].active && !bots[i].dead) n++; return n; }

static void botInit(Bot* b, int idx, float x, float z) {
	Ped keep = b->look;
	memset(b, 0, sizeof *b);
	b->look = keep;
	styleNpc(&b->look, hash2(idx * 31 + (int)(rnd01() * 1000.0f), 4242));
	b->look.hp = 0; // kills counter (battle royale)
	b->active = 1; b->x = x; b->z = z; b->face = rnd() * 3.14f;
	b->hp = b->hpMax = 100.0f; b->tgt = -2; b->skill = 0.5f;
	b->thinkT = idx * 3; b->strafe = 1.0f;
}

static void botSetPath(Bot* b, float tx, float tz) {
	b->pathN = navPath(b->x, b->z, tx, tz, b->pcx, b->pcz, PATH_N);
	b->pathI = (b->pathN > 1) ? 1 : 0;
	b->goalX = tx; b->goalZ = tz; b->repathT = 45;
}

// walk along the path; returns 1 while moving
static int botStep(Bot* b, float speed) {
	for (int k = 0; k < 3 && b->pathI + 1 < b->pathN; k++) { // cut corners when the straight line is clear
		float wx = navWorld(b->pcx[b->pathI + 1]), wz = navWorld(b->pcz[b->pathI + 1]);
		if (navSegClear(b->x, b->z, wx, wz)) b->pathI++; else break;
	}
	if (b->pathI >= b->pathN) return 0;
	float wx = navWorld(b->pcx[b->pathI]), wz = navWorld(b->pcz[b->pathI]);
	float dx = wx - b->x, dz = wz - b->z, d = sqrtf(dx * dx + dz * dz);
	if (d < 0.7f) { b->pathI++; return b->pathI < b->pathN; }
	float nx = b->x + dx / d * speed, nz = b->z + dz / d * speed;
	if (navIsOpen(nx, nz)) { b->x = nx; b->z = nz; }
	else if (navIsOpen(nx, b->z)) b->x = nx;
	else if (navIsOpen(b->x, nz)) b->z = nz;
	else { b->repathT = 0; return 0; }
	b->ph += speed * 1.3f + 0.02f;
	return 1;
}

static void turnTo(float* face, float want, float k) {
	float dl = want - *face;
	while (dl >  3.14159f) dl -= 6.28318f;
	while (dl < -3.14159f) dl += 6.28318f;
	*face += dl * k;
}

static int botBestGun(const Bot* b, float dist) {
	int best = 0, sc = -1;
	static const int rank[WP_COUNT] = {0, 1, 3, 2, 4};
	for (int w = 1; w < WP_COUNT; w++) {
		if (!b->own[w] || b->ammoB[w] <= 0) continue;
		if (dist > kRange[w]) continue;
		if (w == WP_ROCKET && dist < 9.0f) continue;
		if (rank[w] > sc) { sc = rank[w]; best = w; }
	}
	return best;
}

// One shot from fighter b at a target: tgt -1 = the player, >= 0 = another fighter.
static void botFire(int bi, int tgt, float tx, float tz, float dist, float baseAcc, int dmgOverride) {
	Bot* b = &bots[bi];
	int gun = b->gun;
	float mx = b->x - sinf(b->face) * 0.9f, mz = b->z - cosf(b->face) * 0.9f;
	float muz[3] = {mx, 1.25f, mz}, P[3] = {tx, 1.1f, tz};
	float acc = baseAcc * (1.0f - 0.45f * dist / kRange[gun]);
	if (tgt == -1 && plMoving) acc *= 0.8f;
	int hit = rnd01() < acc;
	if (!hit) { P[0] += rnd() * 1.6f; P[1] = 0.3f + rnd01() * 1.5f; P[2] += rnd() * 1.6f; }
	if (gun == WP_ROCKET) {
		gExplSrc = bi;
		explode(P[0], 0.5f, P[2]);
		gExplSrc = -1;
	} else {
		addTracer(muz, P);
		if (!hit) addFx(P[0], P[1], P[2], 0.22f, 0.0f, 5.0f, 1.0f, 0.9f, 0.6f);
		float dv = sqrtf((tx - gCamX) * (tx - gCamX) + (tz - gCamZ) * (tz - gCamZ));
		float vol = 0.9f / (1.0f + dv * 0.05f);
		playSfx(gun == WP_PISTOL ? SFX_PISTOL : (gun == WP_SMG ? SFX_SMG : SFX_SHOTGUN), vol * (gun == WP_SMG ? 0.6f : 0.9f));
		if (hit) {
			int dmg = dmgOverride > 0 ? dmgOverride : kDmg[gun];
			if (gun == WP_SHOTGUN && dmgOverride <= 0) dmg = (int)((float)kDmg[gun] * 4.0f * (1.0f - 0.8f * dist / kRange[gun]));
			if (tgt == -1) damagePlayer(dmg, b->x, b->z, bi);
			else damageBot(tgt, dmg, bi);
		}
	}
	addFx(muz[0], muz[1], muz[2], 0.3f, 0.0f, 2.0f, 1.0f, 0.85f, 0.4f);
	if (b->ammoB[gun] > 0 && gMode == M_BR) b->ammoB[gun]--;
}

// ---- Battle Royale storm ----
static float zCx, zCz, zR, zFx, zFz, zFr, zTx, zTz, zTr;
static int   zPhase, zT, zDmgT;
static const int   zWait[5]   = {1500, 1200, 900, 700, 500};
static const int   zShrink[5] = {1800, 1500, 1200, 900, 700};
static const float zRad[6]    = {235.0f, 120.0f, 70.0f, 38.0f, 18.0f, 6.0f};
static void stormStart(void) {
	zPhase = 0; zT = 0; zDmgT = 0;
	zCx = zFx = zTx = 0.0f; zCz = zFz = zTz = 0.0f; zR = zFr = zRad[0]; zTr = zRad[1];
	float a = rnd() * 3.1f, r = 40.0f * rnd01();
	zTx = sinf(a) * r; zTz = cosf(a) * r;
}
static int stormShrinking(void) { return zPhase < 5 && zT >= zWait[zPhase]; }
static float stormDps(void) { return zPhase >= 5 ? 9.0f : 1.5f + 1.5f * (float)zPhase; }
static void stormUpdate(void) {
	if (zPhase >= 5) return;
	zT++;
	if (zT >= zWait[zPhase]) {
		float k = (float)(zT - zWait[zPhase]) / (float)zShrink[zPhase];
		if (k >= 1.0f) {
			zPhase++; zT = 0; zCx = zFx = zTx; zCz = zFz = zTz; zR = zFr = zTr;
			if (zPhase < 5) {
				zTr = zRad[zPhase + 1];
				float a = rnd() * 6.28f, r = (zR - zTr) * 0.8f * rnd01();
				zTx = zCx + sinf(a) * r; zTz = zCz + cosf(a) * r;
			} else { zR = zRad[5]; }
		} else {
			zR = zFr + (zTr - zFr) * k; zCx = zFx + (zTx - zFx) * k; zCz = zFz + (zTz - zFz) * k;
		}
	}
}
static int outsideStorm(float x, float z) { float dx = x - zCx, dz = z - zCz; return dx * dx + dz * dz > zR * zR; }

// ---- loot pickups ----
static void giveWeaponBot(Bot* b, int w) { b->own[w] = 1; b->ammoB[w] += wdef[w].pickupAmmo + (w == WP_PISTOL ? 12 : 0); }
static void lootPickBot(Bot* b, Loot* l) {
	l->active = 0;
	if (l->kind == LK_WEAPON) giveWeaponBot(b, l->w);
	else if (l->kind == LK_AMMO) { for (int w = 1; w < WP_COUNT; w++) if (b->own[w]) b->ammoB[w] += wdef[w].pickupAmmo; }
	else if (b->medkits < 3) b->medkits++;
}
static void lootPickPlayer(Loot* l) {
	if (l->kind == LK_AMMO) {
		int any = 0;
		for (int w = 1; w < WP_COUNT; w++) if (ownedW[w]) any = 1;
		if (!any) return;
		for (int w = 1; w < WP_COUNT; w++) if (ownedW[w]) { ammo[w] += wdef[w].pickupAmmo; if (ammo[w] > 999) ammo[w] = 999; }
		toast("AMMO");
	} else if (l->kind == LK_MEDKIT) {
		if (plMedkits >= 4) return;
		plMedkits++; toast("MEDKIT ADDED TO WHEEL");
	} else {
		int w = l->w, had = ownedW[w];
		ownedW[w] = 1; ammo[w] += wdef[w].pickupAmmo + (w == WP_PISTOL ? 12 : 0);
		char t[40]; snprintf(t, sizeof t, "%s%s", had ? "AMMO: " : "FOUND ", wdef[w].name); toast(t);
		if (curWeapon == WP_FISTS) curWeapon = w;
	}
	l->active = 0;
	playSfx(SFX_PICKUP, 0.7f);
}

// ---- fighter thinking ----
static void botUseMedkit(Bot* b) { b->medkits--; b->healT = 130; }

static void botBR(int bi) {
	Bot* b = &bots[bi];
	float speed = 0.075f;
	// choose a target: nearest fighter (or the player) that can be seen
	if (b->thinkT-- <= 0) {
		b->thinkT = 9 + (bi & 3);
		int best = -2; float bd = 1e9f;
		for (int k = -1; k < MAX_BOTS; k++) {
			float tx, tz;
			if (k == -1) { if (gState != GS_PLAY || plHp <= 0.0f) continue; tx = gPlX; tz = gPlZ; }
			else { if (k == bi || !bots[k].active || bots[k].dead) continue; tx = bots[k].x; tz = bots[k].z; }
			float d = sqrtf((tx - b->x) * (tx - b->x) + (tz - b->z) * (tz - b->z));
			if (d < bd && d < 48.0f && losClear(b->x, b->z, tx, tz)) { bd = d; best = k; }
		}
		if (best != b->tgt) b->aim = 0.0f;
		b->tgt = best; b->tdist = bd;
	}
	if (b->healT > 0) { // standing still, using a medkit
		b->healT--; b->hp += 0.4f; if (b->hp > b->hpMax) b->hp = b->hpMax; b->walk = 0; return;
	}
	float tx = 0, tz = 0;
	int seen = 0;
	if (b->tgt >= -1) {
		if (b->tgt == -1) { tx = gPlX; tz = gPlZ; seen = (gState == GS_PLAY && plHp > 0.0f); }
		else if (bots[b->tgt].active && !bots[b->tgt].dead) { tx = bots[b->tgt].x; tz = bots[b->tgt].z; seen = 1; }
		if (!seen) b->tgt = -2;
	}
	float dist = seen ? sqrtf((tx - b->x) * (tx - b->x) + (tz - b->z) * (tz - b->z)) : 0.0f;
	if (b->medkits > 0 && b->hp < 55.0f && (!seen || dist > 22.0f)) { botUseMedkit(b); return; }
	int gun = seen ? botBestGun(b, dist) : 0;
	if (!gun) { // keep the best gun it has for show
		for (int w = WP_COUNT - 1; w >= 1; w--) if (b->own[w] && b->ammoB[w] > 0) { gun = w; break; }
	}
	b->gun = gun;
	int canShoot = seen && gun && botBestGun(b, dist) != 0;
	b->walk = 0;
	if (canShoot) {
		b->face = atan2f(-(tx - b->x), -(tz - b->z));
		if (b->aim < 1.0f) b->aim += 0.03f;
		if (b->cool > 0) b->cool--;
		else if (b->aim > 0.8f) {
			botFire(bi, b->tgt, tx, tz, dist, 0.35f + 0.25f * b->skill, 0);
			b->cool = 22 + (int)(rnd01() * 18.0f) + (gun == WP_SHOTGUN ? 30 : 0) + (gun == WP_ROCKET ? 70 : 0) - (gun == WP_SMG ? 14 : 0);
		}
		float want = gun == WP_SHOTGUN ? 8.0f : 16.0f;
		if (dist > want + 6.0f) { // close in
			if (b->repathT-- <= 0 || b->pathI >= b->pathN) botSetPath(b, tx, tz);
			b->walk = botStep(b, speed);
		} else { // sidestep
			if (b->strafeT-- <= 0) { b->strafeT = 40 + (int)(rnd01() * 50.0f); b->strafe = -b->strafe; }
			float sx = cosf(b->face) * b->strafe * speed * 0.6f, sz = -sinf(b->face) * b->strafe * speed * 0.6f;
			if (navIsOpen(b->x + sx, b->z + sz)) { b->x += sx; b->z += sz; b->walk = 1; b->ph += 0.1f; }
			else b->strafe = -b->strafe;
		}
		return;
	}
	if (b->aim > 0.0f) b->aim -= 0.05f;
	if (b->aim < 0.0f) b->aim = 0.0f;
	// no fight: head for the storm's safe zone, otherwise pick up loot, otherwise roam
	float gx = 0, gz = 0;
	int have = 0, anyGun = 0;
	for (int w = 1; w < WP_COUNT; w++) if (b->own[w] && b->ammoB[w] > 0) anyGun = 1;
	int lowAmmo = 0;
	for (int w = 1; w < WP_COUNT; w++) if (b->own[w] && b->ammoB[w] < 8 && w != WP_ROCKET) lowAmmo = 1;
	{
		float cxs = stormShrinking() ? zTx : zCx, czs = stormShrinking() ? zTz : zCz, rs = stormShrinking() ? zTr : zR;
		if (outsideStorm(b->x, b->z) || (b->x - cxs) * (b->x - cxs) + (b->z - czs) * (b->z - czs) > rs * rs * 0.8f) { gx = cxs; gz = czs; have = 1; }
	}
	float bestD = have ? 15.0f : 1e9f; int bl = -1;
	for (int i = 0; i < N_LOOT; i++) {
		const Loot* l = &loot[i];
		if (!l->active) continue;
		int want = 0;
		if (l->kind == LK_WEAPON) want = !anyGun || !b->own[l->w] || l->w == WP_ROCKET;
		else if (l->kind == LK_AMMO) want = anyGun && lowAmmo;
		else want = b->medkits < 2 && (b->hp < 90.0f || b->medkits == 0);
		if (!want || outsideStorm(l->x, l->z)) continue;
		float d = sqrtf((l->x - b->x) * (l->x - b->x) + (l->z - b->z) * (l->z - b->z));
		if (!anyGun && l->kind == LK_WEAPON) d *= 0.5f;
		if (d < bestD) { bestD = d; bl = i; }
	}
	if (bl >= 0) { gx = loot[bl].x; gz = loot[bl].z; have = 1; }
	if (!have) { // roam towards the middle of the next safe circle
		if (b->pathI >= b->pathN || b->repathT <= -200) { float a = rnd() * 6.28f, r = rnd01() * (zR > 30.0f ? 30.0f : zR); gx = zCx + sinf(a) * r; gz = zCz + cosf(a) * r; have = 1; }
		else { b->repathT--; b->walk = botStep(b, speed); return; }
	}
	if (b->repathT-- <= 0 || b->pathI >= b->pathN || fabsf(gx - b->goalX) > 3.0f || fabsf(gz - b->goalZ) > 3.0f) {
		if (b->thinkT == 0 || b->pathI >= b->pathN || bl >= 0) botSetPath(b, gx, gz);
	}
	b->walk = botStep(b, speed);
	if (b->walk) turnTo(&b->face, atan2f(-(navWorld(b->pcx[b->pathI < b->pathN ? b->pathI : b->pathN - 1]) - b->x), -(navWorld(b->pcz[b->pathI < b->pathN ? b->pathI : b->pathN - 1]) - b->z)), 0.2f);
	for (int i = 0; i < N_LOOT; i++) { // pick up what it is standing on
		Loot* l = &loot[i];
		if (!l->active) continue;
		float dx = l->x - b->x, dz = l->z - b->z;
		if (dx * dx + dz * dz < 1.4f * 1.4f) {
			int w = (l->kind == LK_WEAPON) ? (!anyGun || !b->own[l->w] || l->w == WP_ROCKET) : (l->kind == LK_AMMO ? anyGun : b->medkits < 3);
			if (w) { lootPickBot(b, l); break; }
		}
	}
}

static void botDM(int bi) {
	Bot* b = &bots[bi];
	int L = gLevel;
	float speed = 0.085f + 0.008f * (float)L;
	if (b->thinkT-- <= 0) {
		b->thinkT = 8 + (bi & 3);
		int was = b->los;
		float dx = gPlX - b->x, dz = gPlZ - b->z;
		b->tdist = sqrtf(dx * dx + dz * dz);
		b->los = b->tdist < 52.0f && losClear(b->x, b->z, gPlX, gPlZ);
		if (b->los && !was) b->aim = 0.0f;
	}
	b->gun = dmGun[L];
	float dist = b->tdist;
	b->walk = 0;
	if (b->los) {
		b->face = atan2f(-(gPlX - b->x), -(gPlZ - b->z));
		if (b->aim < 1.0f) b->aim += 0.025f + 0.004f * (float)L;
		if (b->cool > 0) b->cool--;
		else if (b->aim > 0.75f && gState == GS_PLAY) {
			botFire(bi, -1, gPlX, gPlZ, dist, dmAcc[L], 10);
			if (--b->burst > 0) b->cool = dmCool[L] / 3 + 2;
			else { b->burst = dmBurst[L]; b->cool = dmCool[L] * 2 + (int)(rnd01() * 20.0f); }
		}
		if (dist > 20.0f) {
			if (b->repathT-- <= 0 || b->pathI >= b->pathN) botSetPath(b, gPlX, gPlZ);
			b->walk = botStep(b, speed);
		} else {
			if (b->strafeT-- <= 0) { b->strafeT = 30 + (int)(rnd01() * 40.0f); b->strafe = -b->strafe; }
			float sx = cosf(b->face) * b->strafe * speed * 0.7f, sz = -sinf(b->face) * b->strafe * speed * 0.7f;
			if (navIsOpen(b->x + sx, b->z + sz)) { b->x += sx; b->z += sz; b->walk = 1; b->ph += 0.1f; }
			else b->strafe = -b->strafe;
		}
	} else {
		if (b->aim > 0.0f) b->aim -= 0.04f;
		if (b->aim < 0.0f) b->aim = 0.0f;
		if (b->repathT-- <= 0 || b->pathI >= b->pathN) botSetPath(b, gPlX, gPlZ);
		b->walk = botStep(b, speed * 1.2f);
		if (b->walk && b->pathI < b->pathN) {
			float wx = navWorld(b->pcx[b->pathI]), wz = navWorld(b->pcz[b->pathI]);
			turnTo(&b->face, atan2f(-(wx - b->x), -(wz - b->z)), 0.2f);
		}
	}
}

static void spawnDmBot(void) {
	for (int i = 0; i < MAX_BOTS; i++) if (!bots[i].active) {
		float x, z;
		if (!pickSpot(&x, &z, gPlX, gPlZ, 26.0f, 60.0f, 1)) pickSpot(&x, &z, gPlX, gPlZ, 20.0f, 80.0f, 0);
		botInit(&bots[i], i, x, z);
		bots[i].hp = bots[i].hpMax = (float)dmHp[gLevel];
		bots[i].gun = dmGun[gLevel]; bots[i].burst = dmBurst[gLevel]; bots[i].cool = 70;
		return;
	}
}

static void healPlayerStep(void) {
	if (plHeal > 0) { float h = plHeal > 0 ? 0.4f : 0.0f; plHp += h; plHeal -= 1; if (plHp > 100.0f) plHp = 100.0f; if (plHeal < 0) plHeal = 0; }
}
static void useMedkit(void) {
	if (plMedkits > 0 && plHp < 100.0f && plHeal == 0 && gState == GS_PLAY) {
		plMedkits--; plHeal = 125; playSfx(SFX_HEAL, 0.8f);
	}
}

// ---- Target Practice: a darts-style shooting range ---------------------------------------------
#define TP_RX 1000.0f          // the range sits far outside the city
#define TP_RZ 0.0f
static const float tpZ[4] = {2.0f, 9.0f, 2.0f, 2.0f}; // where each shooter stands (the player stands furthest back so the rivals are in view)
#define TP_BZ (-8.06f)         // the boards hang on this plane
#define TP_BY 2.0f             // board centre height
#define TP_BR 1.8f             // board radius
#define TP_RING 0.18f          // width of one scoring ring
#define TP_PL 1                // the player's lane
#define TP_ROUNDS 5
static const float tpLane[4] = {-6.6f, -2.2f, 2.2f, 6.6f};
static const float tpSigma[4] = {0.50f, 0.0f, 0.42f, 0.34f};
static const char* tpName[4] = {"RILEY", "YOU", "MONA", "JAX"};
typedef struct { float x, y; int s; } Mark;
static Mark tpMarks[4][16];
static int  tpMarkN[4], tpScore[4], tpRound, tpTurn, tpPhase, tpTimer, tpShotsLeft, tpLastShot = -1, tpLastLane = 0, tpShotNo, tpRoundScore[4][TP_ROUNDS], gDraw;
static float tpAim[4];

static int tpRegister(int lane, float lx, float ly) { // offset from the board centre in metres -> score
	float d = sqrtf(lx * lx + ly * ly);
	int s = d < TP_BR ? 10 - (int)(d / TP_RING) : 0;
	if (s < 0) s = 0;
	if (tpMarkN[lane] < 16) { tpMarks[lane][tpMarkN[lane]].x = lx; tpMarks[lane][tpMarkN[lane]].y = ly; tpMarks[lane][tpMarkN[lane]].s = s; tpMarkN[lane]++; }
	tpScore[lane] += s; tpRoundScore[lane][tpRound] += s;
	tpLastShot = s; tpLastLane = lane;
	return s;
}

static void tpPlayerShot(const float cam[3], const float fwd[3], const float muz[3]) {
	float P[3] = {cam[0] + fwd[0] * 40.0f, cam[1] + fwd[1] * 40.0f, cam[2] + fwd[2] * 40.0f};
	int s = 0;
	if (fwd[2] < -1e-3f) {
		float t = (TP_BZ - cam[2]) / fwd[2];
		if (t > 0.0f) {
			P[0] = cam[0] + fwd[0] * t; P[1] = cam[1] + fwd[1] * t; P[2] = TP_BZ;
			float lx = P[0] - (TP_RX + tpLane[TP_PL]), ly = P[1] - TP_BY;
			if (lx * lx + ly * ly < 2.6f * 2.6f && P[1] > 0.0f) s = tpRegister(TP_PL, lx, ly);
			else { tpLastShot = 0; tpLastLane = TP_PL; }
		}
	}
	addTracer(muz, P);
	addFx(muz[0], muz[1], muz[2], 0.3f, 0.0f, 2.0f, 1.0f, 0.85f, 0.4f);
	addFx(P[0], P[1], P[2], 0.25f, 0.0f, 6.0f, 1.0f, 0.9f, 0.6f);
	playSfx(SFX_PISTOL, 0.8f);
	recoilAnim = 1.0f; kick += 0.012f;
	tpShotsLeft--; tpShotNo++;
	char t[40]; snprintf(t, sizeof t, s >= 10 ? "BULLSEYE! +10" : (s > 0 ? "+%d" : "MISS"), s); toast(t);
	if (s >= 10) playSfx(SFX_PICKUP, 0.7f);
}

static float gauss(void) { return rnd() + rnd() + rnd(); }

static void tpNpcShot(int lane) {
	float lx = gauss() * tpSigma[lane], ly = gauss() * tpSigma[lane];
	int s = tpRegister(lane, lx, ly);
	float muz[3] = {TP_RX + tpLane[lane] + 0.3f, 1.25f, tpZ[lane] - 0.9f}, P[3] = {TP_RX + tpLane[lane] + lx, TP_BY + ly, TP_BZ};
	addTracer(muz, P);
	addFx(muz[0], muz[1], muz[2], 0.3f, 0.0f, 2.0f, 1.0f, 0.85f, 0.4f);
	addFx(P[0], P[1], P[2], 0.25f, 0.0f, 6.0f, 1.0f, 0.9f, 0.6f);
	playSfx(SFX_PISTOL, 0.6f);
	(void)s;
}

static void tpFinish(void) {
	gState = GS_OVER; gOverT = 0;
	int better = 0, same = 0;
	for (int i = 0; i < 4; i++) if (i != TP_PL) { if (tpScore[i] > tpScore[TP_PL]) better++; else if (tpScore[i] == tpScore[TP_PL]) same++; }
	gPlace = 1 + better;
	gWon = (better == 0 && same == 0);
	gDraw = (better == 0 && same > 0);
}

static void updateTp(void) {
	for (int i = 0; i < 4; i++) {
		float want = (tpPhase == 1 && i == tpTurn && i != TP_PL) || (tpPhase == 0 && i == tpTurn && i != TP_PL && tpTimer < 40) ? 1.0f : 0.0f;
		tpAim[i] += (want - tpAim[i]) * 0.12f;
	}
	if (tpPhase == 0) {
		if (--tpTimer <= 0) { tpPhase = 1; tpShotsLeft = 3; tpTimer = (tpTurn == TP_PL) ? 900 : 70; fireCool = 20; tpShotNo = 0; }
	} else if (tpPhase == 1) {
		if (tpTurn == TP_PL) {
			if (--tpTimer <= 0 || tpShotsLeft <= 0) { tpPhase = 2; tpTimer = 60; }
		} else if (--tpTimer <= 0) {
			tpNpcShot(tpTurn);
			tpShotsLeft--;
			tpTimer = 55 + (int)(rnd01() * 30.0f);
			if (tpShotsLeft <= 0) { tpPhase = 2; tpTimer = 70; }
		}
	} else if (--tpTimer <= 0) {
		if (++tpTurn >= 4) { tpTurn = 0; if (++tpRound >= TP_ROUNDS) { tpRound = TP_ROUNDS - 1; tpFinish(); return; } }
		tpPhase = 0; tpTimer = 80;
		tpLastShot = -1;
	}
}

static void addBoardBand(float cx, float cy, float z, float r0, float r1, float r, float g, float b) {
	const int N = 18;
	for (int k = 0; k < N; k++) {
		float a0 = (float)k * 6.2832f / N, a1 = (float)(k + 1) * 6.2832f / N;
		float p0[3] = {cx + r0 * cosf(a0), cy + r0 * sinf(a0), z}, p1[3] = {cx + r1 * cosf(a0), cy + r1 * sinf(a0), z};
		float p2[3] = {cx + r1 * cosf(a1), cy + r1 * sinf(a1), z}, p3[3] = {cx + r0 * cosf(a1), cy + r0 * sinf(a1), z};
		quad(p0, p1, p2, p3, r, g, b);
	}
}

static void addMarkQuad(float x, float y, float z, float h, float r, float g, float b) {
	float p0[3] = {x - h, y - h, z}, p1[3] = {x + h, y - h, z}, p2[3] = {x + h, y + h, z}, p3[3] = {x - h, y + h, z};
	quad(p0, p1, p2, p3, r, g, b);
}

static void drawTpRoom(int frame) {
	int start = vcount;
	const float X = TP_RX, Z = TP_RZ;
	addBoxY(X - 11.0f, Z - 9.0f, X + 11.0f, Z + 12.5f, -0.4f, 0.0f, 0.13f, 0.15f, 0.20f);                 // floor
	addBoxY(X - 11.4f, Z - 9.0f, X - 11.0f, Z + 12.5f, 0.0f, 7.0f, 0.16f, 0.18f, 0.26f);                  // side walls
	addBoxY(X + 11.0f, Z - 9.0f, X + 11.4f, Z + 12.5f, 0.0f, 7.0f, 0.16f, 0.18f, 0.26f);
	addBoxY(X - 11.4f, Z - 9.0f, X + 11.4f, Z - 8.1f, 0.0f, 7.0f, 0.20f, 0.22f, 0.30f);                  // board wall
	addBoxY(X - 11.4f, Z + 12.5f, X + 11.4f, Z + 12.9f, 0.0f, 7.0f, 0.16f, 0.18f, 0.26f);                // back wall
	addBoxY(X - 11.4f, Z - 9.0f, X + 11.4f, Z + 12.9f, 7.0f, 7.4f, 0.12f, 0.14f, 0.20f);                 // ceiling
	for (int k = 0; k < 4; k++) // ceiling light panels
		for (int m = 0; m < 3; m++) {
			float lx = X - 8.25f + 5.5f * (float)k, lz = Z - 4.0f + 7.0f * (float)m;
			addBoxY(lx - 1.4f, lz - 1.2f, lx + 1.4f, lz + 1.2f, 6.93f, 7.0f, 0.95f, 0.97f, 1.0f);
		}
	addBoxY(X - 11.0f, Z - 8.1f, X + 11.0f, Z - 8.0f, 0.0f, 0.12f, 0.0f, 0.75f, 0.85f);                  // neon skirting under the boards
	for (int k = 0; k < 4; k++) { // lanes: floor pad, guide line to the board, booth divider
		int act = gState == GS_PLAY && k == tpTurn;
		float cx = X + tpLane[k], sz = Z + tpZ[k];
		float cr = k == TP_PL ? 0.0f : 0.9f, cg = k == TP_PL ? 0.9f : 0.4f, cb = k == TP_PL ? 1.0f : 0.8f;
		float br = act ? 0.75f : 0.22f;
		addBoxY(cx - 1.0f, sz - 1.0f, cx + 1.0f, sz + 1.2f, 0.0f, 0.03f, cr * br, cg * br, cb * br);
		addBoxY(cx - 0.03f, Z + TP_BZ + 0.3f, cx + 0.03f, sz - 1.0f, 0.0f, 0.015f, cr * 0.35f, cg * 0.35f, cb * 0.35f);
		// board: backing plate and rings
		addBoxY(cx - 2.1f, Z - 8.1f, cx + 2.1f, Z - 8.075f, TP_BY - 2.1f, TP_BY + 2.1f, 0.05f, 0.05f, 0.07f);
		float by = TP_BY, bz = Z + TP_BZ;
		addBoardBand(cx, by, bz, 0.0f, TP_RING, 1.0f, 0.82f, 0.15f);
		static const float rr[6] = {1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 10.0f};
		for (int q = 0; q < 5; q++) addBoardBand(cx, by, bz, rr[q] * TP_RING, rr[q + 1] * TP_RING, (q & 1) ? 0.92f : 0.85f, (q & 1) ? 0.92f : 0.12f, (q & 1) ? 0.92f : 0.12f);
		for (int m = 0; m < tpMarkN[k]; m++) {
			const Mark* mk = &tpMarks[k][m];
			float r = k == TP_PL ? 0.0f : (k == 0 ? 1.0f : (k == 2 ? 0.2f : 0.9f));
			float g = k == TP_PL ? 0.9f : (k == 0 ? 0.55f : (k == 2 ? 1.0f : 0.2f));
			float b = k == TP_PL ? 1.0f : (k == 0 ? 0.0f : (k == 2 ? 0.3f : 0.9f));
			if (mk->s == 0) { r = g = b = 0.5f; }
			addMarkQuad(cx + mk->x, by + mk->y, bz + 0.02f, 0.10f, 0.02f, 0.02f, 0.02f);
			addMarkQuad(cx + mk->x, by + mk->y, bz + 0.03f, 0.07f, r, g, b);
		}
		if (k != TP_PL) { // the rival with its gun levelled at its board
			int ni = k < TP_PL ? k : k - 1;
			gBaseY = 0.0f;
			drawBody(&bots[ni].look, cx, sz, 0.0f, 0.0f, 0, 0, 0, 0, WP_PISTOL, tpAim[k]);
			addShadow(cx, sz, 0.4f, 0.32f, 0.35f);
		}
		if (gState == GS_PLAY && k == tpTurn) { // marker over whoever is up
			float bob = 0.15f * sinf((float)frame * 0.12f);
			addBoxY(cx - 0.18f, sz - 0.18f, cx + 0.18f, sz + 0.18f, 2.55f + bob, 2.9f + bob, cr, cg, cb);
		}
	}
	GSPGPU_FlushDataCache(&vbo[start], (vcount - start) * sizeof(Vertex));
}

// ---- drawing the fighters, loot and the storm ----------------------------------------------------
static void drawBots(void) {
	int start = vcount;
	for (int i = 0; i < MAX_BOTS; i++) {
		const Bot* b = &bots[i];
		if (!b->active || !inView(b->x, b->z)) continue;
		if (!b->dead) addShadow(b->x, b->z, 0.38f, 0.30f, 0.34f);
		drawBody(&b->look, b->x, b->z, b->face, b->ph, b->walk, 0, b->dead, b->deadT, b->dead ? 0 : (b->gun ? b->gun : (gMode == M_DM ? dmGun[gLevel] : 0)), b->aim);
	}
	GSPGPU_FlushDataCache(&vbo[start], (vcount - start) * sizeof(Vertex));
}

static void drawLoot(int frame) {
	if (gMode != M_BR) return;
	for (int i = 0; i < N_LOOT; i++) {
		const Loot* l = &loot[i];
		if (!l->active || !inView(l->x, l->z)) continue;
		float y = 0.6f + 0.12f * sinf((float)frame * 0.08f + (float)i), r, g, b;
		if (l->kind == LK_WEAPON) { r = 0.30f; g = 0.60f; b = 1.0f; if (l->w == WP_SHOTGUN) { r = 1.0f; g = 0.55f; b = 0.1f; } if (l->w == WP_ROCKET) { r = 1.0f; g = 0.15f; b = 0.15f; } if (l->w == WP_PISTOL) { r = 0.8f; g = 0.8f; b = 0.85f; }
			addBoxY(l->x - 0.45f, l->z - 0.12f, l->x + 0.45f, l->z + 0.12f, y - 0.1f, y + 0.1f, r, g, b);
			addBoxY(l->x - 0.1f, l->z - 0.12f, l->x + 0.1f, l->z + 0.12f, y - 0.3f, y - 0.1f, r * 0.6f, g * 0.6f, b * 0.6f); }
		else if (l->kind == LK_AMMO) { r = 1.0f; g = 0.85f; b = 0.2f; addBoxY(l->x - 0.28f, l->z - 0.28f, l->x + 0.28f, l->z + 0.28f, y - 0.25f, y + 0.25f, r, g, b); }
		else { r = 0.2f; g = 1.0f; b = 0.45f;
			addBoxY(l->x - 0.32f, l->z - 0.32f, l->x + 0.32f, l->z + 0.32f, y - 0.25f, y + 0.25f, 0.95f, 0.95f, 0.95f);
			addBoxY(l->x - 0.34f, l->z - 0.07f, l->x + 0.34f, l->z + 0.07f, y - 0.05f, y + 0.05f, 0.9f, 0.1f, 0.1f);
			addBoxY(l->x - 0.07f, l->z - 0.34f, l->x + 0.07f, l->z + 0.34f, y - 0.05f, y + 0.05f, 0.9f, 0.1f, 0.1f); }
		addBoxY(l->x - 0.03f, l->z - 0.03f, l->x + 0.03f, l->z + 0.03f, 0.0f, 5.0f, r, g, b);
	}
}

static void drawStorm(void) {
	if (gMode != M_BR) return;
	const int N = 56;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 1 && zPhase >= 5) break;
		float cx = pass ? zTx : zCx, cz = pass ? zTz : zCz, R = pass ? zTr : zR;
		for (int k = 0; k < N; k++) {
			float a0 = (float)k * 6.2832f / N, a1 = (float)(k + 1) * 6.2832f / N;
			float x0 = cx + R * sinf(a0), z0 = cz + R * cosf(a0), x1 = cx + R * sinf(a1), z1 = cz + R * cosf(a1);
			float mx = (x0 + x1) * 0.5f - gCamX, mz = (z0 + z1) * 0.5f - gCamZ;
			if (mx * mx + mz * mz > 130.0f * 130.0f) continue;
			float p0[3] = {x0, 0.0f, z0}, p1[3] = {x1, 0.0f, z1}, p2[3] = {x1, 45.0f, z1}, p3[3] = {x0, 45.0f, z0};
			gAlpha = pass ? 0.22f : 0.38f;
			if (pass) quad(p0, p1, p2, p3, 0.9f, 0.95f, 1.0f); else quad(p0, p1, p2, p3, 0.65f, 0.15f, 0.95f);
			gAlpha = 1.0f;
		}
	}
}

// ---- starting, running and leaving a mode ----------------------------------------------------------
static int gSavedWeather = 0, gSavedCam = 0, gHasSaved = 0, gSpawnT = 0;

static void restoreUserSettings(void) {
	if (gHasSaved) { weather = gSavedWeather; camStyle = gSavedCam; gHasSaved = 0; }
}

static void worldReset(void);
static void setupMode(int mode) {
	worldReset();
	restoreUserSettings();
	gRng ^= (unsigned)osGetTime() * 2654435761u;
	gMode = mode; gState = GS_COUNTDOWN; gCount = 239; gGo = 0; gOverT = 0; gWon = 0; gDraw = 0; gPlace = 0;
	gKills = 0; gLevel = 0; gTime = 0; gBanner = 0; gSpawnT = 60; gToast = 0;
	memset(bots, 0, sizeof bots); memset(loot, 0, sizeof loot);
	memset(feedLife, 0, sizeof feedLife); memset(plHitLife, 0, sizeof plHitLife);
	memset(fxs, 0, sizeof fxs); memset(trs, 0, sizeof trs);
	rocket.alive = 0; hitMark = killMark = 0; shake = 0.0f; kick = 0.0f;
	plHp = 100.0f; plSince = 999; plHeal = 0; plMedkits = 0; plFlash = 0; plUnseen = 0; plMoving = 0;
	gTraffic = (mode == M_FREE);
	gCratesOn = (mode == M_FREE || mode == M_DM);
	for (int w = 0; w < WP_COUNT; w++) { ownedW[w] = 1; ammo[w] = wdef[w].startAmmo; }
	curWeapon = WP_PISTOL; fireCool = 0;
	gPlYaw = 0.0f;
	if (mode == M_FREE) { gPlX = 0.0f; gPlZ = 0.0f; initCrates(); }
	else if (mode == M_DM) {
		pickSpot(&gPlX, &gPlZ, 0.0f, 0.0f, 0.0f, 999.0f, 0);
		gPlYaw = rnd() * 3.14f;
		initCrates();
		for (int i = 0; i < dmMax[0]; i++) spawnDmBot();
	} else if (mode == M_BR) {
		for (int w = 0; w < WP_COUNT; w++) { ownedW[w] = 0; ammo[w] = 0; }
		curWeapon = WP_FISTS; ownedW[WP_FISTS] = 1;
		pickSpot(&gPlX, &gPlZ, 0.0f, 0.0f, 0.0f, 999.0f, 0);
		gPlYaw = rnd() * 3.14f;
		int nl = 0;
		for (int k = 0; k < 64 && nl < 66; k++) { // 24 guns, 26 ammo drops, 16 medkits, spread out
			float x, z;
			pickSpot(&x, &z, 0.0f, 0.0f, 0.0f, 999.0f, 0);
			int ok = 1;
			for (int j = 0; j < nl; j++) if (fabsf(loot[j].x - x) < 7.0f && fabsf(loot[j].z - z) < 7.0f) ok = 0;
			if (!ok) continue;
			loot[nl].x = x; loot[nl].z = z; loot[nl].active = 1;
			int t = nl % 11;
			if (t < 4) { loot[nl].kind = LK_WEAPON; float r = rnd01(); loot[nl].w = r < 0.40f ? WP_PISTOL : (r < 0.72f ? WP_SMG : (r < 0.92f ? WP_SHOTGUN : WP_ROCKET)); }
			else if (t < 8) loot[nl].kind = LK_AMMO;
			else loot[nl].kind = LK_MEDKIT;
			nl++;
		}
		float mx[MAX_BOTS], mz[MAX_BOTS];
		for (int i = 0; i < 11; i++) {
			float x = 0, z = 0;
			for (int tries = 0; tries < 12; tries++) {
				pickSpot(&x, &z, gPlX, gPlZ, 28.0f, 999.0f, 0);
				int ok = 1;
				for (int j = 0; j < i; j++) if (fabsf(mx[j] - x) + fabsf(mz[j] - z) < 22.0f) ok = 0;
				if (ok) break;
			}
			mx[i] = x; mz[i] = z;
			botInit(&bots[i], i, x, z);
			bots[i].skill = 0.3f + 0.6f * rnd01();
		}
		stormStart();
	} else if (mode == M_TP) {
		memset(tpMarkN, 0, sizeof tpMarkN); memset(tpScore, 0, sizeof tpScore); memset(tpRoundScore, 0, sizeof tpRoundScore); memset(tpAim, 0, sizeof tpAim);
		tpRound = 0; tpTurn = 0; tpPhase = 0; tpTimer = 90; tpShotsLeft = 0; tpLastShot = -1; tpShotNo = 0;
		for (int i = 0; i < 3; i++) { botInit(&bots[i], i + 5, 0.0f, 0.0f); bots[i].active = 0; }
		gPlX = TP_RX + tpLane[TP_PL]; gPlZ = TP_RZ + tpZ[TP_PL]; gPlYaw = 0.0f;
		gSavedWeather = weather; gSavedCam = camStyle; gHasSaved = 1;
		weather = W_SUNNY; camStyle = 2;
		ammo[WP_PISTOL] = 999; curWeapon = WP_PISTOL;
	}
}

static void exitMode(void) { // back to the menus: everything returns to Free Roam defaults
	worldReset();
	restoreUserSettings();
	gMode = M_FREE; gState = GS_PLAY; gTraffic = 1; gCratesOn = 1;
	memset(bots, 0, sizeof bots);
	rocket.alive = 0;
	for (int w = 0; w < WP_COUNT; w++) { ownedW[w] = 1; ammo[w] = wdef[w].startAmmo; }
	curWeapon = WP_PISTOL;
}

static void updateMode(float px, float pz, int moving) {
	gPlX = px; gPlZ = pz; plMoving = moving;
	for (int i = 0; i < 4; i++) if (plHitLife[i] > 0) plHitLife[i]--;
	for (int i = 0; i < FEED_N; i++) if (feedLife[i] > 0) feedLife[i]--;
	if (plFlash > 0) plFlash--;
	if (gBanner > 0) gBanner--;
	if (gToast > 0) gToast--;
	if (gGo > 0) gGo--;
	if (gState == GS_COUNTDOWN) {
		if (gCount % 60 == 59 && gCount > 60) playSfx(SFX_BEEP, 0.7f);
		if (--gCount < 60) { gState = GS_PLAY; gGo = 60; playSfx(SFX_GO, 0.8f); }
		return;
	}
	if (gState == GS_OVER) { gOverT++; return; }
	if (gMode == M_FREE) return;
	gTime++;
	if (gMode == M_TP) { updateTp(); return; }
	healPlayerStep();
	if (gMode == M_DM) {
		int seen = 0;
		for (int i = 0; i < MAX_BOTS; i++) if (bots[i].active && !bots[i].dead && bots[i].los) seen = 1;
		plUnseen = seen ? 0 : plUnseen + 1;
		plSince++;
		if (plHp < 50.0f && plSince > 150 && plUnseen > 120) { plHp += 0.12f; if (plHp > 50.0f) plHp = 50.0f; } // hiding heals up to half
		if (botsAlive() < dmMax[gLevel] && --gSpawnT <= 0) { spawnDmBot(); gSpawnT = 100 - 8 * gLevel; }
		for (int i = 0; i < MAX_BOTS; i++) {
			Bot* b = &bots[i];
			if (!b->active) continue;
			if (b->dead) { if (++b->deadT > 200) b->active = 0; continue; }
			botDM(i);
		}
	} else { // battle royale
		stormUpdate();
		if (++zDmgT >= 60) {
			zDmgT = 0;
			int d = (int)(stormDps() + 0.5f);
			if (outsideStorm(px, pz)) damagePlayer(d, 0.0f, 0.0f, -3);
			for (int i = 0; i < MAX_BOTS; i++) if (bots[i].active && !bots[i].dead && outsideStorm(bots[i].x, bots[i].z)) damageBot(i, d, -3);
		}
		for (int i = 0; i < N_LOOT; i++) {
			Loot* l = &loot[i];
			if (!l->active || gState != GS_PLAY) continue;
			float dx = l->x - px, dz = l->z - pz;
			if (dx * dx + dz * dz < 1.5f * 1.5f) lootPickPlayer(l);
		}
		for (int i = 0; i < MAX_BOTS; i++) {
			Bot* b = &bots[i];
			if (!b->active) continue;
			if (b->dead) { if (++b->deadT > 300) b->active = 0; continue; }
			botBR(i);
		}
		if (gState == GS_PLAY && botsAlive() == 0) { gState = GS_OVER; gOverT = 0; gWon = 1; gPlace = 1; }
	}
}

// ============================================================================================================
// WORLD SYSTEMS (Free Roam): districts, cash, doors and interiors, shops, the L train, wildlife
// ============================================================================================================

// ---- districts -------------------------------------------------------------------------------------------
enum { Z_DOWNTOWN, Z_RES, Z_MIXED, Z_COMM, Z_IND, Z_DOCK, Z_PARK, Z_LOT, Z_RAIL, Z_FWY, Z_N };
static int blockOfV(float v) { int i = (int)floorf((v + CITY_H) / ROAD_SP); return i < 0 ? 0 : (i > GRID - 1 ? GRID - 1 : i); }
static int zoneAt(float x, float z) {
	int i = blockOfV(x), j = blockOfV(z);
	int t = blkType[i][j];
	float dt = dtFactor(BLK_C(i), BLK_C(j));
	switch (t) {
	case BT_TOWER: return Z_DOWNTOWN;
	case BT_MID: case BT_PLAZA: return dt > 0.45f ? Z_DOWNTOWN : (t == BT_PLAZA ? Z_PARK : Z_COMM);
	case BT_LOW: return dt > 0.45f ? Z_DOWNTOWN : Z_RES;
	case BT_HOUSES: return Z_RES;
	case BT_MIXED: return Z_MIXED;
	case BT_COMM: return Z_COMM;
	case BT_INDUS: return Z_IND;
	case BT_DOCK: return Z_DOCK;
	case BT_PARK: case BT_COURT: return Z_PARK;
	case BT_LOT: return Z_LOT;
	case BT_RAIL: return Z_RAIL;
	default: return Z_FWY;
	}
}
// 7 x 7 neighbourhood names, north to south and west to east (each is a 2 x 2 block district)
static const char* const kHood[CN][CN] = {
	{"Albany Park",     "Ravenswood",        "Lincoln Square",   "Andersonville", "Uptown",       "Lakeview",      "Wrigleyville"},
	{"Irving Park",     "Roscoe Village",    "North Center",     "Lincoln Park",  "Old Town",     "Gold Coast",    "Lakefront Trail"},
	{"Logan Square",    "Bucktown",          "Wicker Park",      "River West",    "River North",  "Streeterville", "Navy Pier"},
	{"Humboldt Park",   "Ukrainian Village", "West Town",        "West Loop",     "The Loop",     "Millennium Park", "Grant Park"},
	{"Garfield Park",   "Little Italy",      "Pilsen",           "South Loop",    "Chinatown",    "Museum Campus", "Lakefront Trail"},
	{"Back of the Yards", "Bridgeport",      "McKinley Park",    "Stockyards",    "Canal Basin",  "Calumet Docks", "South Harbor"},
	{"Brighton Park",   "Archer Heights",    "Pullman Yards",    "Fulton Yards",  "Rail Yards",   "Steel Docks",   "Harbor Piers"}
};
static const char* hoodAt(float x, float z) {
	int ix = (int)floorf((x + CITY_H) / CELL_W), iz = (int)floorf((z + CITY_H) / CELL_W);
	ix = ix < 0 ? 0 : (ix > CN - 1 ? CN - 1 : ix);
	iz = iz < 0 ? 0 : (iz > CN - 1 ? CN - 1 : iz);
	return kHood[iz][ix];
}
static const char* const kZoneName[Z_N] = {"DOWNTOWN", "RESIDENTIAL", "MIXED USE", "COMMERCIAL", "INDUSTRIAL", "DOCKS", "PARK", "VACANT LOTS", "RAIL LINE", "EXPRESSWAY"};

// ---- the L train: runs along the elevated line, stops at both stations -------------------------------------
#define TRAIN_CARS 6
#define TCAR_L     9.0f
static float trX = -300.0f, trV = 0.0f;
static int   trDir = 1, trDwell = 0, trAt = -1;
static void updateTrain(void) {
	if (trDwell > 0) { trDwell--; trV = 0.0f; return; }
	float want = 0.55f;
	for (int s = 0; s < 2; s++) {
		float sx = BLK_C(kStationI[s]), d = (sx - trX) * (float)trDir;
		if (trAt == s) { if (d < -30.0f) trAt = -1; continue; }
		if (d >= 0.0f && d < 130.0f) {
			float lim = sqrtf(2.0f * 0.0042f * d) + 0.05f;
			if (lim < want) want = lim;
			if (d < 0.4f) { trDwell = 420; trV = 0.0f; trAt = s; trX = sx; return; }
		}
	}
	trV += (want - trV) * 0.03f;
	trX += trV * (float)trDir;
	if (trX > 430.0f) { trDir = -1; trAt = -1; }
	if (trX < -430.0f) { trDir = 1; trAt = -1; }
}
// one car: body, window band, stripe, roof and bogies. (xc, zc) = centre, y0 = top of the rails
static void drawTrainCar(float xc, float zc, float y0, int front, int rear, int dir) {
	const float hl = TCAR_L * 0.5f - 0.2f, hw = 1.35f;
	addBoxG(xc - hl, zc - hw, xc + hl, zc + hw, y0 + 0.55f, y0 + 3.1f, 0.74f, 0.76f, 0.80f, 0.80f, 1.0f);
	addBoxY(xc - hl + 0.5f, zc - hw - 0.03f, xc + hl - 0.5f, zc + hw + 0.03f, y0 + 1.55f, y0 + 2.45f, 0.10f, 0.14f, 0.20f);
	addBoxY(xc - hl, zc - hw - 0.04f, xc + hl, zc + hw + 0.04f, y0 + 0.95f, y0 + 1.25f, 0.10f, 0.32f, 0.70f);
	addBoxY(xc - hl + 0.2f, zc - hw + 0.3f, xc + hl - 0.2f, zc + hw - 0.3f, y0 + 3.1f, y0 + 3.3f, 0.52f, 0.54f, 0.58f);
	addBoxY(xc - 3.0f, zc - 0.9f, xc - 1.8f, zc + 0.9f, y0 + 0.1f, y0 + 0.55f, 0.12f, 0.12f, 0.14f);
	addBoxY(xc + 1.8f, zc - 0.9f, xc + 3.0f, zc + 0.9f, y0 + 0.1f, y0 + 0.55f, 0.12f, 0.12f, 0.14f);
	(void)front; (void)rear; (void)dir;
}
static void drawTrain(void) {
	for (int i = 0; i < TRAIN_CARS; i++) {
		float xc = trX + ((float)i - ((float)TRAIN_CARS - 1.0f) * 0.5f) * (TCAR_L + 0.5f);
		if (!inView(xc, TRK_Z)) continue;
		drawTrainCar(xc, TRK_Z, RAIL_H + 0.09f, 0, 0, trDir);
	}
}
static void drawTrainLit(float night) {
	float xf = trX + (float)trDir * (((float)TRAIN_CARS) * 0.5f * (TCAR_L + 0.5f) - 0.2f), xr = trX - (float)trDir * (((float)TRAIN_CARS) * 0.5f * (TCAR_L + 0.5f) - 0.2f);
	float y = RAIL_H + 0.09f;
	if (inView(xf, TRK_Z)) for (int s = -1; s <= 1; s += 2) addBoxY(xf - 0.1f, TRK_Z + (float)s * 0.8f - 0.2f, xf + 0.1f, TRK_Z + (float)s * 0.8f + 0.2f, y + 0.9f, y + 1.3f, 1.0f, 0.95f, 0.75f);
	if (inView(xr, TRK_Z)) for (int s = -1; s <= 1; s += 2) addBoxY(xr - 0.1f, TRK_Z + (float)s * 0.8f - 0.2f, xr + 0.1f, TRK_Z + (float)s * 0.8f + 0.2f, y + 0.9f, y + 1.3f, 0.9f, 0.1f, 0.08f);
	if (night > 0.25f) // lit windows
		for (int i = 0; i < TRAIN_CARS; i++) {
			float xc = trX + ((float)i - ((float)TRAIN_CARS - 1.0f) * 0.5f) * (TCAR_L + 0.5f);
			if (!inView(xc, TRK_Z)) continue;
			addBoxY(xc - 3.9f, TRK_Z - 1.40f, xc + 3.9f, TRK_Z + 1.40f, y + 1.65f, y + 2.35f, 1.0f, 0.92f, 0.62f);
		}
}

// ---- the train in the station hall: arrives, waits at the platform, leaves ------------------------------------
static int stT = 0;
static float stTrainOff(void) { // x offset of the train centre relative to the hall centre
	int t = stT % 1700;
	if (t < 380) { float u = 1.0f - (float)t / 380.0f; return 80.0f * u * u; }
	if (t < 1000) return 0.0f;
	if (t < 1380) { float u = (float)(t - 1000) / 380.0f; return -80.0f * u * u; }
	return 400.0f;
}
static void drawStationTrain(void) {
	const float cx = RM_X(RM_STATION), off = stTrainOff();
	for (int i = 0; i < TRAIN_CARS; i++) {
		float xc = cx + off + ((float)i - ((float)TRAIN_CARS - 1.0f) * 0.5f) * (TCAR_L + 0.5f);
		if (xc < cx - 16.0f || xc > cx + 16.0f) continue;
		drawTrainCar(xc, -8.45f, -0.75f, 0, 0, 1);
	}
}

// ---- cash on the street ---------------------------------------------------------------------------------------
#define N_CASH 44
typedef struct { float x, z; int val, active, respawn; } CashP;
static CashP cashP[N_CASH];
static int   wareGot[6];
static float gTeleX = 0.0f, gTeleZ = 0.0f, gTeleYaw = 0.0f;
static int   gTele = 0;

static void initCash(void) {
	memset(cashP, 0, sizeof cashP);
	for (int i = 0; i < N_CASH; i++) {
		unsigned h = hash2(i, 6161);
		int road = (int)((h >> 3) % ROADS), axis = (int)((h >> 7) & 1);
		float side = ((h >> 8) & 1) ? 1.0f : -1.0f;
		float s = -CITY_H + 30.0f + (float)((h >> 12) % (unsigned)(2.0f * CITY_H - 60.0f));
		float lat = ROAD_C(road) + side * (SW_OFF + 0.2f);
		for (int tries = 0; tries < 10; tries++) {
			float x = axis ? lat : s, z = axis ? s : lat;
			if (!blockedR(x, z, 0.5f) && fabsf(x) < CITY_H - 4.0f && fabsf(z) < CITY_H - 4.0f) { cashP[i].x = x; cashP[i].z = z; cashP[i].active = 1; break; }
			s += 1.7f;
		}
		cashP[i].val = ((h >> 20) % 5 == 0) ? 20 : (((h >> 20) % 5 < 3) ? 5 : 10);
	}
}

// ---- appearance bought in shops --------------------------------------------------------------------------------

// ---- shops ---------------------------------------------------------------------------------------------------------
enum { IK_BOOST, IK_AMMO, IK_SHADES, IK_CAP, IK_JACKET, IK_REST, IK_TICKET };
typedef struct { const char* name; int price, kind, a, b; } ShopItem;
static const ShopItem shopStore[]   = {{"ENERGY DRINK", 4, IK_BOOST, 90, 0}, {"CHIPS", 2, IK_BOOST, 30, 0}, {"CHICAGO CAP", 12, IK_CAP, 0, 0}, {"SUNGLASSES", 15, IK_SHADES, 0, 0}};
static const ShopItem shopDiner[]   = {{"COFFEE", 3, IK_BOOST, 60, 0}, {"MILKSHAKE", 5, IK_BOOST, 100, 0}, {"DEEP DISH SLICE", 7, IK_BOOST, 130, 0}, {"CHEESEBURGER", 8, IK_BOOST, 160, 0}};
static const ShopItem shopLaundry[] = {{"RED JACKET", 6, IK_JACKET, 1, 0}, {"BLUE JACKET", 6, IK_JACKET, 2, 0}, {"GREEN JACKET", 6, IK_JACKET, 3, 0}, {"BLACK JACKET", 6, IK_JACKET, 0, 0}};
static const ShopItem shopApt[]     = {{"SLEEP UNTIL MORNING", 0, IK_REST, W_SUNNY, 0}, {"SLEEP UNTIL SUNSET", 0, IK_REST, W_SUNSET, 0}, {"SLEEP UNTIL NIGHT", 0, IK_REST, W_NIGHT, 0}};
static const ShopItem shopWare[]    = {{"PISTOL AMMO x24", 8, IK_AMMO, WP_PISTOL, 24}, {"SMG AMMO x60", 15, IK_AMMO, WP_SMG, 60}, {"SHOTGUN SHELLS x8", 12, IK_AMMO, WP_SHOTGUN, 8}, {"ROCKETS x2", 25, IK_AMMO, WP_ROCKET, 2}};
static const ShopItem shopStation[] = {{"TICKET TO OTHER STATION", 5, IK_TICKET, 0, 0}};
static const ShopItem* shopItems(int room, int* n) {
	switch (room) {
	case RM_STORE:   *n = 4; return shopStore;
	case RM_DINER:   *n = 4; return shopDiner;
	case RM_LAUNDRY: *n = 4; return shopLaundry;
	case RM_APT:     *n = 3; return shopApt;
	case RM_WARE:    *n = 4; return shopWare;
	default:         *n = 1; return shopStation;
	}
}
static const char* const kServiceTxt[RM_N] = {"TALK TO THE CLERK", "ORDER FOOD", "WASH & FOLD", "USE THE BED", "VISIT THE DEALER", "TICKET MACHINE"};

static int   gCash = 40;
static int   gBoost = 0;                       // frames of speed boost left
static int   gShop = -1, gShopSel = 0, gShopMsgT = 0;
static char  gShopMsg[64];
static int   gTrans = 0, gTransAct = 0, gTransArg = 0;   // screen fade: at the midpoint the queued action runs
enum { ACT_ENTER, ACT_EXIT, ACT_RIDE, ACT_REST };
static int   gRetDoor = -1;                    // the door the player went in through
static int   gPrompt = 0;                      // 0 none, 1 enter, 2 exit, 3 service
static int   gPromptDoor = -1;
static char  gPromptTxt[48];
static const char* gZoneBanner = NULL;         // neighbourhood name shown for a moment when it changes
static int   gZoneBannerT = 0;
static Ped   keeperPed[RM_N];
static const char* const kKeeperName[RM_N] = {"CLERK", "COOK", "ATTENDANT", "", "DEALER", ""};

static void shopSay(const char* s) { snprintf(gShopMsg, sizeof gShopMsg, "%s", s); gShopMsgT = 150; }

static void startTrans(int act, int arg) { if (gTrans > 0) return; gTrans = 26; gTransAct = act; gTransArg = arg; }

static void shopBuy(int room, int idx) {
	int n;
	const ShopItem* it = &shopItems(room, &n)[idx];
	char b[48];
	if (gCash < it->price) { shopSay("NOT ENOUGH CASH"); playSfx(SFX_CLICK, 0.6f); return; }
	switch (it->kind) {
	case IK_BOOST:
		gBoost += it->a * 60; if (gBoost > 400 * 60) gBoost = 400 * 60;
		snprintf(b, sizeof b, "SPEED BOOST +%ds", it->a); break;
	case IK_AMMO:
		if (ammo[it->a] >= 999) { shopSay("AMMO ALREADY FULL"); return; }
		ammo[it->a] += it->b; if (ammo[it->a] > 999) ammo[it->a] = 999;
		snprintf(b, sizeof b, "+%d %s AMMO", it->b, wdef[it->a].name); break;
	case IK_SHADES:
		if (gShades) { shopSay("YOU ALREADY OWN THESE"); return; }
		gShades = 1; snprintf(b, sizeof b, "NICE SHADES!"); break;
	case IK_CAP:
		if (gCap) { shopSay("YOU ALREADY OWN THIS"); return; }
		gCap = 1; snprintf(b, sizeof b, "CHICAGO CAP ON"); break;
	case IK_JACKET:
		if (gJacketIx == it->a) { shopSay("ALREADY WEARING THAT"); return; }
		gJacketIx = it->a; snprintf(b, sizeof b, "FRESH JACKET"); break;
	case IK_REST:
		weather = it->a; gShop = -1; startTrans(ACT_REST, it->a);
		snprintf(b, sizeof b, "%s", it->a == W_SUNNY ? "GOOD MORNING" : (it->a == W_SUNSET ? "SUNSET" : "NIGHT FALLS"));
		toast(b); return;
	default: // ticket
		gShop = -1; gCash -= it->price; startTrans(ACT_RIDE, 0);
		snprintf(b, sizeof b, "TICKET  -$%d", it->price); toast(b); playSfx(SFX_PICKUP, 0.7f); return;
	}
	gCash -= it->price;
	if (it->price > 0) { char c[64]; snprintf(c, sizeof c, "%s  -$%d", b, it->price); shopSay(c); } else shopSay(b);
	playSfx(SFX_PICKUP, 0.7f);
}

// shop input: D-pad moves, A buys / leaves, B closes, touch picks a row
static void shopListRect(int n, int i, float* y0, float* h) { (void)n; *y0 = 66.0f + 29.0f * (float)i; *h = 23.0f; }
static void updateShop(u32 down, const touchPosition* tp) {
	int n;
	shopItems(gShop, &n);
	int rows = n + 1, choose = (down & KEY_A) ? 1 : 0;
	if (down & KEY_DDOWN) { gShopSel = (gShopSel + 1) % rows; playSfx(SFX_CLICK, 0.4f); }
	if (down & KEY_DUP)   { gShopSel = (gShopSel + rows - 1) % rows; playSfx(SFX_CLICK, 0.4f); }
	if (down & KEY_TOUCH) {
		for (int i = 0; i < rows; i++) {
			float y0, h;
			shopListRect(rows, i, &y0, &h);
			if (tp->px >= 50 && tp->px <= 270 && tp->py >= y0 && tp->py < y0 + h) { gShopSel = i; choose = 1; }
		}
	}
	if (down & KEY_B) { gShop = -1; return; }
	if (choose) { if (gShopSel >= n) { gShop = -1; playSfx(SFX_CLICK, 0.4f); } else shopBuy(gShop, gShopSel); }
}

static void roomEnterInit(int room) {
	stT = 0;
	if (room == RM_WARE) memset(wareGot, 0, sizeof wareGot);
}

// a free spot just outside a door (some doors have a bench, hydrant or planter right beside the exact spot)
static void doorExitSpot(const Door* d, float* ox, float* oz) {
	static const float ts[5] = {0.6f, 0.2f, 1.0f, 1.5f, 0.0f}, ls[5] = {0.0f, 0.6f, -0.6f, 1.2f, -1.2f};
	for (int a = 0; a < 5; a++)
		for (int b = 0; b < 5; b++) {
			float x = d->x + d->fx * ts[a] - d->fz * ls[b], z = d->z + d->fz * ts[a] + d->fx * ls[b];
			if (!blockedR(x, z, PLAYER_R)) { *ox = x; *oz = z; return; }
		}
	*ox = d->x; *oz = d->z;
}

// the world action that runs at the midpoint of a fade
static void runTransAct(void) {
	switch (gTransAct) {
	case ACT_ENTER: {
		const Door* d = &doors[gTransArg];
		gRetDoor = gTransArg; gRoom = d->room; roomEnterInit(gRoom);
		gTeleX = rooms[gRoom].spX; gTeleZ = rooms[gRoom].spZ; gTeleYaw = 0.0f; gTele = 1;
		playSfx(SFX_SWITCH, 0.5f);
		break;
	}
	case ACT_EXIT: {
		const Door* d = &doors[gRetDoor >= 0 ? gRetDoor : 0];
		gRoom = -1;
		doorExitSpot(d, &gTeleX, &gTeleZ); gTeleYaw = atan2f(-d->fx, -d->fz); gTele = 1;
		playSfx(SFX_SWITCH, 0.5f);
		break;
	}
	case ACT_RIDE: { // the other station's front door
		int other = -1;
		for (int i = 0; i < nDoor; i++) if (doors[i].room == RM_STATION && i != gRetDoor) other = i;
		if (other < 0) other = gRetDoor;
		const Door* d = &doors[other >= 0 ? other : 0];
		gRoom = -1; gRetDoor = -1;
		doorExitSpot(d, &gTeleX, &gTeleZ); gTeleYaw = atan2f(-d->fx, -d->fz); gTele = 1;
		playSfx(SFX_HORN, 0.3f);
		break;
	}
	default: break; // ACT_REST: the weather was already set, the fade covers the change
	}
}

static void worldReset(void) {
	gRoom = -1; gShop = -1; gTrans = 0; gPrompt = 0; gRetDoor = -1; gTele = 0;
}

// the door / service prompt for the current position
static void updatePrompt(float px, float pz) {
	gPrompt = 0; gPromptDoor = -1;
	if (gRoom < 0) {
		float best = 2.1f * 2.1f;
		for (int i = 0; i < nDoor; i++) {
			float dx = doors[i].x - px, dz = doors[i].z - pz, d2 = dx * dx + dz * dz;
			if (d2 < best) { best = d2; gPromptDoor = i; }
		}
		if (gPromptDoor >= 0) { gPrompt = 1; snprintf(gPromptTxt, sizeof gPromptTxt, "ENTER %s", roomName[doors[gPromptDoor].room]); }
	} else {
		const Room* m = &rooms[gRoom];
		float ex = m->exX - px, ez = m->exZ - pz, ax = m->aX - px, az = m->aZ - pz;
		if (ax * ax + az * az < 2.3f * 2.3f) { gPrompt = 3; snprintf(gPromptTxt, sizeof gPromptTxt, "%s", kServiceTxt[gRoom]); }
		else if (ex * ex + ez * ez < 2.0f * 2.0f) { gPrompt = 2; snprintf(gPromptTxt, sizeof gPromptTxt, "LEAVE"); }
	}
}

static void updateWorld(float px, float pz, u32 down, int canAct, int frame) {
	(void)frame;
	if (gBoost > 0) gBoost--;
	if (gShopMsgT > 0) gShopMsgT--;
	if (gZoneBannerT > 0) gZoneBannerT--;
	// the fade between outside and inside
	if (gTrans > 0) {
		gTrans--;
		if (gTrans == 13) runTransAct();
		gPrompt = 0;
		return;
	}
	if (gMode != M_FREE) { gPrompt = 0; return; }
	if (gRoom < 0) {
		static const char* lastHood = NULL;
		const char* hd = hoodAt(px, pz);
		if (hd != lastHood) { lastHood = hd; gZoneBanner = hd; gZoneBannerT = 170; }
		for (int i = 0; i < N_CASH; i++) { // cash lying around
			CashP* c = &cashP[i];
			if (!c->active) { if (c->respawn > 0 && --c->respawn == 0) c->active = 1; continue; }
			float dx = c->x - px, dz = c->z - pz;
			if (dx * dx + dz * dz < 1.5f * 1.5f) {
				c->active = 0; c->respawn = 5400; gCash += c->val;
				char b[24]; snprintf(b, sizeof b, "+$%d", c->val); toast(b); playSfx(SFX_PICKUP, 0.6f);
			}
		}
	} else {
		const Room* m = &rooms[gRoom];
		for (int i = 0; i < m->nCrate; i++) { // warehouse job: pick up the loose crates
			if (wareGot[i]) continue;
			float dx = m->crate[i][0] - px, dz = m->crate[i][1] - pz;
			if (dx * dx + dz * dz < 1.4f * 1.4f) { wareGot[i] = 1; gCash += 15; toast("CRATE DELIVERED  +$15"); playSfx(SFX_PICKUP, 0.6f); }
		}
		stT++;
	}
	if (!canAct) { gPrompt = 0; return; }
	updatePrompt(px, pz);
	if (down & KEY_A) {
		if (gPrompt == 1) startTrans(ACT_ENTER, gPromptDoor);
		else if (gPrompt == 2) startTrans(ACT_EXIT, 0);
		else if (gPrompt == 3) { gShop = gRoom; gShopSel = 0; gShopMsgT = 0; playSfx(SFX_SWITCH, 0.5f); }
	}
}

// ---- wildlife: pigeons in plazas, parks and courts; squirrels near the park trees ---------------------------------
#define N_PIGEON 14
#define N_SQUIRREL 10
#define N_ANIM (N_PIGEON + N_SQUIRREL)
typedef struct {
	int kind, state, t;            // kind 0 pigeon, 1 squirrel
	float x, z, y, face, ph, tx, tz, hx, hz, hr, treeX, treeZ, fromX, fromZ;
	int hasTree;
	float clr[3];
} Animal;
static Animal anim[N_ANIM];
enum { AS_IDLE, AS_WALK, AS_FLY, AS_RUN, AS_CLIMB };

static void initWildlife(void) {
	memset(anim, 0, sizeof anim);
	int homesP[GRID * GRID][2], nP = 0, homesS[GRID * GRID][2], nS = 0;
	for (int i = 0; i < GRID; i++) for (int j = 0; j < GRID; j++) {
		int t = blkType[i][j];
		if (t == BT_PARK || t == BT_PLAZA || t == BT_COURT || t == BT_COMM || t == BT_LOT || t == BT_MIXED) { homesP[nP][0] = i; homesP[nP][1] = j; nP++; }
		if (t == BT_PARK || t == BT_HOUSES) { homesS[nS][0] = i; homesS[nS][1] = j; nS++; }
	}
	for (int a = 0; a < N_ANIM; a++) {
		Animal* an = &anim[a];
		unsigned h = hash2(a, 8821);
		an->kind = a >= N_PIGEON;
		int nn = an->kind ? nS : nP;
		if (nn == 0) continue;
		int pick = (int)((h >> 4) % (unsigned)nn);
		int bi = (an->kind ? homesS : homesP)[pick][0], bj = (an->kind ? homesS : homesP)[pick][1];
		float bx = BLK_C(bi), bz = BLK_C(bj);
		an->hx = bx; an->hz = bz; an->hr = an->kind ? 7.0f : 9.5f;
		if (an->kind) { // find a tree of this block
			int cnt = 0;
			for (int n = 0; n < nBld; n++) {
				const Building* b = &bldg[n];
				if (b->kind != 1) continue;
				float tx = (b->x0 + b->x1) * 0.5f, tz = (b->z0 + b->z1) * 0.5f;
				if (fabsf(tx - bx) < LOT_H && fabsf(tz - bz) < LOT_H && (hash2(n, a) % 3 == 0 || cnt == 0)) { an->treeX = tx; an->treeZ = tz; an->hasTree = 1; cnt++; }
			}
			if (an->hasTree) { an->hx = an->treeX; an->hz = an->treeZ; an->hr = 5.0f; }
		}
		for (int tries = 0; tries < 20; tries++) {
			float x = an->hx + ((float)((h >> (tries % 8)) % 19) - 9.0f) * an->hr / 9.5f * 0.7f, z = an->hz + ((float)((h >> (tries % 8 + 3)) % 19) - 9.0f) * an->hr / 9.5f * 0.7f;
			h = hash2((int)h, tries + a);
			if (!blockedR(x, z, 0.35f) && fabsf(x - bx) < LOT_H && fabsf(z - bz) < LOT_H) { an->x = x; an->z = z; break; }
			an->x = an->hx; an->z = an->hz;
		}
		an->face = (float)(h % 628) * 0.01f; an->ph = (float)(h % 100) * 0.1f; an->t = 30 + (int)(h % 120);
		unsigned c = hash2(a, 313);
		if (an->kind == 0) { // pigeon colours: mostly blue-grey, a few white / brown
			int v = (int)(c % 10);
			if (v < 7) { an->clr[0] = 0.50f; an->clr[1] = 0.52f; an->clr[2] = 0.58f; }
			else if (v < 9) { an->clr[0] = 0.40f; an->clr[1] = 0.34f; an->clr[2] = 0.30f; }
			else { an->clr[0] = 0.90f; an->clr[1] = 0.90f; an->clr[2] = 0.88f; }
		} else {
			if (c % 3) { an->clr[0] = 0.52f; an->clr[1] = 0.50f; an->clr[2] = 0.48f; } else { an->clr[0] = 0.60f; an->clr[1] = 0.34f; an->clr[2] = 0.16f; }
		}
	}
}

static int animMove(Animal* a, float dx, float dz) { // slides along obstacles; returns 1 if it moved
	int moved = 0;
	if (!blockedR(a->x + dx, a->z, 0.3f)) { a->x += dx; moved = 1; }
	if (!blockedR(a->x, a->z + dz, 0.3f)) { a->z += dz; moved = 1; }
	return moved;
}

static void updateWildlife(float px, float pz) {
	for (int i = 0; i < N_ANIM; i++) {
		Animal* a = &anim[i];
		float dx = a->x - px, dz = a->z - pz, d2 = dx * dx + dz * dz;
		if (d2 > 110.0f * 110.0f) continue; // far away: frozen
		float d = sqrtf(d2) + 0.001f;
		a->ph += 0.2f;
		if (a->kind == 0) { // pigeon
			if (a->state != AS_FLY && d < 4.8f) { // scared: fly off to another spot of its zone
				a->state = AS_FLY; a->t = 0; a->fromX = a->x; a->fromZ = a->z;
				float ang = atan2f(dz, dx), best = -1.0f;
				for (int k = 0; k < 5; k++) {
					float aa = ang + ((float)k - 2.0f) * 0.5f, r = 8.0f + (float)((i * 7 + k * 3) % 6);
					float tx = a->x + cosf(aa) * r, tz = a->z + sinf(aa) * r;
					float hx = tx - a->hx, hz = tz - a->hz;
					if (hx * hx + hz * hz > (a->hr + 4.0f) * (a->hr + 4.0f) || blockedR(tx, tz, 0.35f)) continue;
					float sc = hypotf(tx - px, tz - pz);
					if (sc > best) { best = sc; a->tx = tx; a->tz = tz; }
				}
				if (best < 0.0f) { a->tx = a->hx; a->tz = a->hz; if (blockedR(a->tx, a->tz, 0.35f)) { a->tx = a->x; a->tz = a->z; } }
				a->face = atan2f(-(a->tx - a->x), -(a->tz - a->z));
			}
			if (a->state == AS_FLY) {
				a->t++;
				float u = (float)a->t / 100.0f;
				if (u >= 1.0f) { a->state = AS_IDLE; a->y = 0.0f; a->x = a->tx; a->z = a->tz; a->t = 60 + (i * 13) % 120; }
				else { a->x = a->fromX + (a->tx - a->fromX) * u; a->z = a->fromZ + (a->tz - a->fromZ) * u; a->y = 3.2f * sinf(3.14159f * u); }
			} else if (a->state == AS_WALK) {
				float wx = a->tx - a->x, wz = a->tz - a->z, wd = sqrtf(wx * wx + wz * wz);
				if (wd < 0.2f || !animMove(a, wx / wd * 0.03f, wz / wd * 0.03f)) { a->state = AS_IDLE; a->t = 40 + (i * 17) % 160; }
				else a->face = atan2f(-wx, -wz);
			} else if (--a->t <= 0) {
				unsigned h = hash2(i + (int)a->ph, a->t + 91);
				if (h % 3 == 0) { // wander a little inside the zone
					a->tx = a->hx + ((float)(h % 19) - 9.0f) * a->hr / 12.0f; a->tz = a->hz + ((float)((h >> 8) % 19) - 9.0f) * a->hr / 12.0f;
					a->state = AS_WALK;
				} else a->t = 40 + (int)(h % 120);
			}
		} else { // squirrel
			if (a->state == AS_IDLE && d < 6.0f) { a->state = AS_RUN; a->t = 0; }
			if (a->state == AS_RUN) {
				float gx, gz;
				if (a->hasTree) { gx = a->treeX - a->x; gz = a->treeZ - a->z; }
				else { gx = dx; gz = dz; }
				float gd = sqrtf(gx * gx + gz * gz) + 0.001f;
				if (a->hasTree && gd < 0.9f) { a->state = AS_CLIMB; }
				else { animMove(a, gx / gd * 0.15f, gz / gd * 0.15f); a->face = atan2f(-gx, -gz); }
				if (!a->hasTree && d > 14.0f) a->state = AS_IDLE;
			}
			if (a->state == AS_CLIMB) {
				if (d > 11.0f) { a->y -= 0.04f; if (a->y <= 0.0f) { a->y = 0.0f; a->state = AS_IDLE; a->t = 90; } }
				else if (a->y < 2.4f) a->y += 0.05f;
				a->face = atan2f(-(a->treeX - a->x), -(a->treeZ - a->z));
			} else if (a->state == AS_IDLE && --a->t <= 0) { // a few hops near the home tree
				unsigned h = hash2(i * 3 + (int)a->ph, 1717);
				float ang = (float)(h % 628) * 0.01f, r = 0.7f + (float)((h >> 8) % 10) * 0.2f;
				float tx = a->x + cosf(ang) * r, tz = a->z + sinf(ang) * r, hx = tx - a->hx, hz = tz - a->hz;
				if (hx * hx + hz * hz < a->hr * a->hr && animMove(a, cosf(ang) * r, sinf(ang) * r)) a->face = atan2f(-cosf(ang), -sinf(ang));
				a->t = 50 + (int)(h % 140);
			}
		}
	}
}

static void drawWildlife(void) {
	for (int i = 0; i < N_ANIM; i++) {
		const Animal* a = &anim[i];
		if (!inView(a->x, a->z)) continue;
		float dx = a->x - gCamX, dz = a->z - gCamZ;
		if (dx * dx + dz * dz > 70.0f * 70.0f) continue; // tiny: not worth drawing from afar
		gPx = a->x; gPz = a->z; gFace = a->face; gLean = 0.0f; gBob = a->y; gSx = 1.0f; gSy = 1.0f;
		const float* c = a->clr;
		if (a->kind == 0) {
			int fly = a->state == AS_FLY, peck = a->state == AS_IDLE && ((int)(a->ph * 0.5f) % 7 == 0);
			gLean = peck ? 0.5f : (fly ? -0.25f : 0.0f);
			addPart(0, 0.17f, 0, 0, 0, 0, 0.065f, 0.065f, 0.13f, 0, c[0], c[1], c[2]);               // body
			addPart(0, 0.17f, 0, 0, 0.07f, -0.14f, 0.045f, 0.05f, 0.045f, 0, c[0] * 0.65f, c[1] * 0.7f, c[2] * 0.75f); // head
			addPart(0, 0.17f, 0, 0, 0.065f, -0.20f, 0.012f, 0.012f, 0.03f, 0, 0.95f, 0.65f, 0.20f);  // beak
			float w = fly ? sinf(a->ph * 2.6f) * 0.13f : 0.0f;
			addPart(0.09f, 0.17f, 0, 0, 0.02f + w, 0.0f, 0.07f, 0.01f, 0.10f, 0, c[0] * 0.8f, c[1] * 0.8f, c[2] * 0.8f);   // wings
			addPart(-0.09f, 0.17f, 0, 0, 0.02f + w, 0.0f, 0.07f, 0.01f, 0.10f, 0, c[0] * 0.8f, c[1] * 0.8f, c[2] * 0.8f);
			if (!fly) { addPart(0.03f, 0.10f, 0, 0, -0.05f, 0, 0.01f, 0.05f, 0.01f, 0, 0.8f, 0.4f, 0.4f); addPart(-0.03f, 0.10f, 0, 0, -0.05f, 0, 0.01f, 0.05f, 0.01f, 0, 0.8f, 0.4f, 0.4f); }
			if (!fly) addShadow(a->x, a->z, 0.14f, 0.18f, 0.30f);
		} else {
			float climb = a->state == AS_CLIMB ? -1.1f : 0.0f;
			gLean = climb;
			addPart(0, 0.14f, 0, 0, 0, 0, 0.06f, 0.06f, 0.14f, 0, c[0], c[1], c[2]);                    // body
			addPart(0, 0.14f, 0, 0, 0.05f, -0.17f, 0.05f, 0.05f, 0.05f, 0, c[0] * 1.05f, c[1] * 1.05f, c[2] * 1.05f); // head
			addPart(0, 0.14f, 0, 0, 0.19f, 0.20f, 0.04f, 0.13f, 0.05f, 0.0f, c[0] * 0.85f, c[1] * 0.85f, c[2] * 0.85f); // tail
			addPart(0, 0.14f, 0, 0.0f, 0.12f, -0.16f, 0.07f, 0.02f, 0.03f, 0, 0.85f, 0.85f, 0.82f);        // ears / face patch
			if (a->state != AS_CLIMB) addShadow(a->x, a->z, 0.13f, 0.2f, 0.30f);
		}
		gLean = 0.0f; gBob = 0.0f;
	}
}

// ---- the dynamic geometry of the world systems -----------------------------------------------------------------
static void drawWorldDyn(int frame) {
	if (gRoom < 0) {
		drawTrain();
		if (gTraffic) drawWildlife();
	} else {
		const Room* m = &rooms[gRoom];
		if (m->kHas) {
			const Ped* p = &keeperPed[gRoom];
			gBaseY = 0.0f;
			drawBody(p, m->kX, m->kZ, m->kFace, (float)frame * 0.04f, 0, 0, 0, 0, 0, 0.0f);
		}
		for (int i = 0; i < m->nCrate; i++) {
			if (wareGot[i]) continue;
			float x = m->crate[i][0], z = m->crate[i][1];
			addBoxG(x - 0.45f, z - 0.45f, x + 0.45f, z + 0.45f, 0.0f, 0.8f, 0.78f, 0.60f, 0.38f, 0.8f, 1.0f);
			addBoxY(x - 0.47f, z - 0.08f, x + 0.47f, z + 0.08f, 0.55f, 0.6f, 0.95f, 0.80f, 0.15f);
		}
		if (gRoom == RM_STATION) drawStationTrain();
	}
}
// emissive extras: street lights, cash bills, the train's lights, crates' glow
static void drawWorldLit(int frame, float night) {
	if (gRoom >= 0) return;
	float k = 0.50f + 0.50f * night;
	for (int i = 0; i < nLamp; i++) {
		const Lamp* l = &lamps[i];
		if (!inView(l->x, l->z)) continue;
		addBoxY(l->x - l->w * 0.5f, l->z - l->w * 0.3f, l->x + l->w * 0.5f, l->z + l->w * 0.3f, l->y - 0.1f, l->y + 0.08f, 1.0f * k, 0.92f * k, 0.62f * k);
	}
	for (int i = 0; i < N_CASH; i++) {
		const CashP* c = &cashP[i];
		if (!c->active || !inView(c->x, c->z)) continue;
		float t = (float)frame * 0.07f + (float)i, w = 0.12f + 0.28f * fabsf(cosf(t)), y = 0.95f + 0.12f * sinf(t * 1.3f);
		float g = c->val >= 20 ? 0.95f : 0.30f;
		(void)g;
		if (c->val >= 20) addBoxY(c->x - w, c->z - 0.17f, c->x + w, c->z + 0.17f, y, y + 0.05f, 0.95f, 0.80f, 0.20f);
		else              addBoxY(c->x - w, c->z - 0.17f, c->x + w, c->z + 0.17f, y, y + 0.05f, 0.25f, 0.85f, 0.40f);
	}
	drawTrainLit(night);
}

static void initWorld(void) {
	for (int k = 0; k < RM_N; k++) {
		styleNpc(&keeperPed[k], hash2(k, 4711));
		keeperPed[k].walk = 0; keeperPed[k].hp = 4; keeperPed[k].dead = 0; keeperPed[k].panic = 0; keeperPed[k].scale = 1.0f; keeperPed[k].width = 1.0f;
	}
	initCash();
	initWildlife();
}

// ---- ambient sound by district -----------------------------------------------------------------------------------
enum { AMB_TRAFFIC, AMB_RAIN, AMB_CRICKET, AMB_BIRD, AMB_IND, AMB_TUNNEL, AMB_FWY, AMB_TRAIN, AMB_ROOM, AMB_N };
static void ambTargets(float px, float pz, float out[AMB_N]) {
	static const float tab[Z_N][5] = { // traffic, birds, crickets, industrial hum, expressway roar
		{0.34f, 0.04f, 0.05f, 0.00f, 0.08f}, {0.14f, 0.30f, 0.32f, 0.00f, 0.00f}, {0.26f, 0.12f, 0.15f, 0.00f, 0.00f},
		{0.28f, 0.08f, 0.10f, 0.00f, 0.00f}, {0.14f, 0.03f, 0.05f, 0.32f, 0.00f}, {0.08f, 0.20f, 0.10f, 0.24f, 0.00f},
		{0.08f, 0.38f, 0.38f, 0.00f, 0.00f}, {0.14f, 0.10f, 0.18f, 0.06f, 0.00f}, {0.18f, 0.04f, 0.08f, 0.08f, 0.00f},
		{0.26f, 0.04f, 0.06f, 0.00f, 0.32f}
	};
	memset(out, 0, sizeof(float) * AMB_N);
	if (gRoom >= 0) { out[AMB_ROOM] = 0.30f; out[AMB_TRAFFIC] = 0.03f; if (gRoom == RM_STATION) out[AMB_TRAIN] = stTrainOff() == 0.0f ? 0.0f : 0.35f; return; }
	int z = zoneAt(px, pz);
	out[AMB_TRAFFIC] = tab[z][0]; out[AMB_BIRD] = tab[z][1]; out[AMB_CRICKET] = tab[z][2]; out[AMB_IND] = tab[z][3]; out[AMB_FWY] = tab[z][4];
	if (ceilingAt(px, pz) < 50.0f) { out[AMB_TUNNEL] = 0.40f; out[AMB_FWY] += 0.20f; out[AMB_BIRD] = 0.0f; }
	float dx = px - trX, dz = pz - TRK_Z, d = sqrtf(dx * dx + dz * dz);
	if (d < 150.0f && trV > 0.02f) out[AMB_TRAIN] = (1.0f - d / 150.0f) * 0.55f;
}

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
static void updateAudio(int inGame, int paused, float px, float pz) {
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
	{ // district ambience: each zone mixes the loops differently; indoors everything is muffled
		static float cur[AMB_N];
		float tgt[AMB_N];
		ambTargets(px, pz, tgt);
		float day = (1.0f - wNight) * (1.0f - wRain) * (1.0f - 0.5f * wDusk);
		tgt[AMB_BIRD] *= day * 1.2f;
		tgt[AMB_CRICKET] *= wNight * 1.2f;
		if (!inGame) { for (int i = 0; i < AMB_N; i++) tgt[i] = 0.0f; tgt[AMB_TRAFFIC] = 0.30f; tgt[AMB_BIRD] = 0.20f * day; }
		for (int i = 0; i < AMB_N; i++) cur[i] += (tgt[i] - cur[i]) * 0.04f;
		ambSetVol(AMB_TRAFFIC, cur[AMB_TRAFFIC] * g);
		ambSetVol(AMB_RAIN, (gRoom >= 0 ? 0.12f : 1.0f) * wRain * 0.50f * g);
		ambSetVol(AMB_CRICKET, cur[AMB_CRICKET] * g);
		ambSetVol(AMB_BIRD, cur[AMB_BIRD] * g);
		ambSetVol(AMB_IND, cur[AMB_IND] * g);
		ambSetVol(AMB_TUNNEL, cur[AMB_TUNNEL] * g);
		ambSetVol(AMB_FWY, cur[AMB_FWY] * g);
		ambSetVol(AMB_TRAIN, cur[AMB_TRAIN] * g);
		ambSetVol(AMB_ROOM, cur[AMB_ROOM] * g);
	}
	if (inGame && !paused && gRoom < 0) {
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
	"Cicero Ave", "Pulaski Rd", "Kedzie Ave", "California Ave", "Western Ave", "Ashland Ave", "Halsted St",
	"Clark St", "LaSalle St", "State St", "Wabash Ave", "Michigan Ave", "Columbus Dr"
};
static const char* hNames[ROADS] = {
	"Devon Ave", "Montrose Ave", "Irving Park Rd", "Belmont Ave", "Fullerton Ave", "North Ave", "Chicago Ave",
	"Ohio St", "Grand Ave", "Lake St", "Madison St", "Roosevelt Rd", "Cermak Rd"
};

#define N_MENU_TEXT 19
static C2D_TextBuf gStatic, gDyn;
static C2D_Text vLabel[ROADS], hLabel[ROADS], btnHack, btnBlackout;
static C2D_Text txtMenu[N_MENU_TEXT], txtActive, txtPaused, txtWeatherT, txtGfxT, txtCamT, txtSub, txtFooter, txtHint;
static C2D_Text txtWp[WP_COUNT], txtWheelT, txtRelease;

static void initMapText(void) {
	gStatic = C2D_TextBufNew(1024);
	gDyn    = C2D_TextBufNew(4096);
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
		"RESUME", "WEATHER", "GRAPHICS", "CAMERA", "MAIN MENU",
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

// colour of a city block on the map, by district type
static void mapBlockRGB(int t, int* r, int* g, int* b) {
	static const unsigned char c[14][3] = {
		{0x22, 0x2c, 0x4a}, {0x24, 0x2a, 0x3c}, {0x25, 0x2e, 0x44}, {0x1d, 0x4a, 0x2c}, {0x3a, 0x3f, 0x4c}, {0x2c, 0x29, 0x25}, {0x2d, 0x25, 0x3c},
		{0x3a, 0x2c, 0x20}, {0x30, 0x2e, 0x22}, {0x1c, 0x34, 0x48}, {0x20, 0x22, 0x28}, {0x1f, 0x48, 0x3c}, {0x1c, 0x21, 0x2e}, {0x1c, 0x21, 0x2e}
	};
	if (t < 0 || t > 13) t = 10;
	*r = c[t][0]; *g = c[t][1]; *b = c[t][2];
}
static u32 mapBlockClr(int t, int lift) {
	int r, g, b;
	mapBlockRGB(t, &r, &g, &b);
	r += lift; g += lift; b += lift + (lift > 0 ? 6 : 0);
	return C2D_Color32((u8)(r > 255 ? 255 : r), (u8)(g > 255 ? 255 : g), (u8)(b > 255 ? 255 : b), 0xFF);
}
static u32 doorClr(int room) {
	switch (room) {
	case RM_STORE:   return C2D_Color32(0x40, 0xE0, 0x70, 0xFF);
	case RM_DINER:   return C2D_Color32(0xFF, 0x98, 0x30, 0xFF);
	case RM_LAUNDRY: return C2D_Color32(0x30, 0xD8, 0xFF, 0xFF);
	case RM_APT:     return C2D_Color32(0xFF, 0x58, 0xD8, 0xFF);
	case RM_WARE:    return C2D_Color32(0xFF, 0xDC, 0x40, 0xFF);
	default:         return C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
	}
}

static void drawMap(float px, float pz, int frame, int phoneTimer, float face) {
	const u32 cBg    = C2D_Color32(0x12, 0x15, 0x1d, 0xFF);
	const u32 cRoad  = C2D_Color32(0x4a, 0x51, 0x66, 0xFF);
	const u32 cMid   = C2D_Color32(0x6b, 0x73, 0x8c, 0xFF);
	const u32 cPill  = C2D_Color32(0x00, 0x00, 0x00, 0xB4);
	const u32 cText  = C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF);
	const u32 cPink  = C2D_Color32(0xE0, 0x30, 0xE0, 0xFF);
	const u32 cWhite = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, MAP_H, cBg);

	// roads (the whole city ground), then the city blocks on top
	float ext = CITY_H + 12.0f;
	const u32 cPark  = C2D_Color32(0x1d, 0x4a, 0x2c, 0xFF);
	const u32 cTree  = C2D_Color32(0x2a, 0x7a, 0x3c, 0xFF);
	fillClipped(MAP_CX + (-ext - px) * MAP_S, MAP_CY + (-ext - pz) * MAP_S,
	            MAP_CX + ( ext - px) * MAP_S, MAP_CY + ( ext - pz) * MAP_S, cRoad);
	for (int i = 0; i < GRID; i++) {
		for (int j = 0; j < GRID; j++) {
			float bx = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)i, bz = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * (float)j;
			int t = blkType[i][j];
			fillClipped(MAP_CX + (bx - BLK_H - px) * MAP_S, MAP_CY + (bz - BLK_H - pz) * MAP_S,
			            MAP_CX + (bx + BLK_H - px) * MAP_S, MAP_CY + (bz + BLK_H - pz) * MAP_S,
			            mapBlockClr(t, 0));
		}
	}
	{ // the lake east of the city and the elevated line: gravel band, track, and the road tunnels underneath
		const u32 cLake = C2D_Color32(0x14, 0x2c, 0x4a, 0xFF);
		fillClipped(MAP_CX + (CITY_H + 12.0f - px) * MAP_S, 0.0f, 400.0f, MAP_H, cLake);
		fillClipped(MAP_CX + (-ext - px) * MAP_S, MAP_CY + (RAIL_Z - RAIL_HW - pz) * MAP_S,
		            MAP_CX + ( ext - px) * MAP_S, MAP_CY + (RAIL_Z + RAIL_HW - pz) * MAP_S, C2D_Color32(0x4e, 0x46, 0x3e, 0xFF));
		for (int k = 0; k < ROADS; k++) {
			float rx = ROAD_C(k);
			fillClipped(MAP_CX + (rx - ROAD_HW - px) * MAP_S, MAP_CY + (RAIL_Z - RAIL_HW - pz) * MAP_S,
			            MAP_CX + (rx + ROAD_HW - px) * MAP_S, MAP_CY + (RAIL_Z + RAIL_HW - pz) * MAP_S, cRoad);
		}
		for (int s = -1; s <= 1; s += 2)
			fillClipped(MAP_CX + (-ext - px) * MAP_S, MAP_CY + (TRK_Z + 0.7f * (float)s - pz) * MAP_S - 0.5f,
			            MAP_CX + ( ext - px) * MAP_S, MAP_CY + (TRK_Z + 0.7f * (float)s - pz) * MAP_S + 0.5f, C2D_Color32(0xB4, 0xB8, 0xC8, 0xFF));
	}
	for (int i = 0; i < nBld; i++) {
		const Building* b = &bldg[i];
		if (b->kind == 3) continue; // collision only
		if (b->kind == 1) {
			float cx = MAP_CX + ((b->x0 + b->x1) * 0.5f - px) * MAP_S, cy = MAP_CY + ((b->z0 + b->z1) * 0.5f - pz) * MAP_S;
			if (cx > 3.0f && cx < 317.0f && cy > 3.0f && cy < MAP_H - 3.0f)
				C2D_DrawRectSolid(cx - 2.0f, cy - 2.0f, 0.5f, 4.0f, 4.0f, cTree);
			continue;
		}
		fillClipped(MAP_CX + (b->x0 - px) * MAP_S, MAP_CY + (b->z0 - pz) * MAP_S,
		            MAP_CX + (b->x1 - px) * MAP_S, MAP_CY + (b->z1 - pz) * MAP_S,
		            b->kind == 2 ? cPark : mapBlockClr(blkType[blockOfV((b->x0 + b->x1) * 0.5f)][blockOfV((b->z0 + b->z1) * 0.5f)], 22));
	}
	// road centre lines
	for (int k = 0; k < ROADS; k++) {
		float sx = MAP_CX + (ROAD_C(k) - px) * MAP_S, sy = MAP_CY + (ROAD_C(k) - pz) * MAP_S;
		fillClipped(sx - 0.5f, MAP_CY + (-ext - pz) * MAP_S, sx + 0.5f, MAP_CY + (ext - pz) * MAP_S, cMid);
		fillClipped(MAP_CX + (-ext - px) * MAP_S, sy - 0.5f, MAP_CX + (ext - px) * MAP_S, sy + 0.5f, cMid);
	}

	{ // the expressway (translucent: the streets run under it), its centre line, and the L train
		fillClipped(MAP_CX + (FWY_X0 - px) * MAP_S, MAP_CY + (FWY_Z - FWY_HW - pz) * MAP_S,
		            MAP_CX + (FWY_X1 - px) * MAP_S, MAP_CY + (FWY_Z + FWY_HW - pz) * MAP_S, C2D_Color32(0x2a, 0x2e, 0x3a, 0xC8));
		for (float dx = FWY_X0; dx < FWY_X1; dx += 10.0f)
			fillClipped(MAP_CX + (dx - px) * MAP_S, MAP_CY + (FWY_Z - pz) * MAP_S - 0.5f, MAP_CX + (dx + 5.0f - px) * MAP_S, MAP_CY + (FWY_Z - pz) * MAP_S + 0.5f, C2D_Color32(0xE0, 0xB8, 0x30, 0xFF));
		for (int s = -1; s <= 1; s += 2)
			fillClipped(MAP_CX + (FWY_X0 - px) * MAP_S, MAP_CY + (FWY_Z + (float)s * (FWY_HW - 0.6f) - pz) * MAP_S - 0.5f,
			            MAP_CX + (FWY_X1 - px) * MAP_S, MAP_CY + (FWY_Z + (float)s * (FWY_HW - 0.6f) - pz) * MAP_S + 0.5f, C2D_Color32(0xC8, 0xCC, 0xD8, 0xFF));
		float hl = (float)TRAIN_CARS * (TCAR_L + 0.5f) * 0.5f;
		fillClipped(MAP_CX + (trX - hl - px) * MAP_S, MAP_CY + (TRK_Z - 1.4f - pz) * MAP_S, MAP_CX + (trX + hl - px) * MAP_S, MAP_CY + (TRK_Z + 1.4f - pz) * MAP_S, C2D_Color32(0xFF, 0xD8, 0x50, 0xFF));
	}
	if (gMode == M_FREE) { // cash lying around and the doors you can use
		for (int i = 0; i < N_CASH; i++) {
			if (!cashP[i].active) continue;
			float sx = MAP_CX + (cashP[i].x - px) * MAP_S, sy = MAP_CY + (cashP[i].z - pz) * MAP_S;
			if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f) C2D_DrawRectSolid(sx - 1.0f, sy - 1.0f, 0.5f, 2.0f, 2.0f, C2D_Color32(0x70, 0xFF, 0x90, 0xFF));
		}
		for (int i = 0; i < nDoor; i++) {
			float sx = MAP_CX + (doors[i].x - px) * MAP_S, sy = MAP_CY + (doors[i].z - pz) * MAP_S;
			if (sx > 5.0f && sx < 315.0f && sy > 22.0f && sy < MAP_H - 5.0f) {
				C2D_DrawRectSolid(sx - 3.5f, sy - 3.5f, 0.5f, 7.0f, 7.0f, cWhite);
				C2D_DrawRectSolid(sx - 2.5f, sy - 2.5f, 0.5f, 5.0f, 5.0f, doorClr(doors[i].room));
			}
		}
	}

	// traffic
	for (int i = 0; i < NCARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * LANE;
		float wx = (c->axis == 0) ? c->s : lane, wz = (c->axis == 0) ? lane : c->s;
		float sx = MAP_CX + (wx - px) * MAP_S, sy = MAP_CY + (wz - pz) * MAP_S;
		if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f)
			C2D_DrawRectSolid(sx - 1.5f, sy - 1.5f, 0.5f, 3.0f, 3.0f, C2D_Color32f(c->r, c->g, c->b, 1.0f));
	}
	// pedestrians
	for (int i = 0; i < NPEDS; i++) {
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
		if (gVtxOver) snprintf(fb, sizeof fb, "%d FPS FULL", gFpsValue); // the vertex buffer overflowed: some geometry was not drawn
		else          snprintf(fb, sizeof fb, "%d FPS", gFpsValue);
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
	int a  = roadIdx(px);
	int b  = roadIdx(pz);
	char line1[64], line2[96];
	snprintf(line1, sizeof line1, "%s, Chicago, IL", hoodAt(px, pz));
	snprintf(line2, sizeof line2, "%s  -  near %s & %s", kZoneName[zoneAt(px, pz)], vNames[a], hNames[b]);
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
static void txt(const char* s, float x, float y, float sc, u32 clr, int align);
static int wheelN(void) { return gMode == M_BR ? 6 : 5; } // the 6th tile is the medkit (Battle Royale)
static void wheelTile(int i, float* x, float* y) {
	float a = (-90.0f + 360.0f / (float)wheelN() * (float)i) * 0.0174533f;
	*x = 160.0f + 78.0f * cosf(a);
	*y = 112.0f + 78.0f * sinf(a);
}
static int wheelUsable(int i) { return i < WP_COUNT ? ownedW[i] : plMedkits > 0; }

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
	for (int i = 0; i < wheelN(); i++) {
		float x, y;
		wheelTile(i, &x, &y);
		float x0 = x - WH_W * 0.5f, y0 = y - WH_H * 0.5f;
		int on = (i == sel), use = wheelUsable(i);
		float dm = use ? 1.0f : 0.45f;
		C2D_DrawRectSolid(x0, y0, 0.5f, WH_W, WH_H, on ? neon(0, 229, 255, (0.22f + 0.08f * pulse) * amt) : neon(8, 24, 40, 0.88f * amt));
		frameRect(x0, y0, WH_W, WH_H, 1.0f, on ? neon(0, 229, 255, amt) : neon(0, 150, 180, 0.6f * amt * dm));
		if (i == cur) C2D_DrawRectSolid(x0, y0, 0.5f, 4.0f, WH_H, neon(255, 46, 209, amt));
		if (i < WP_COUNT) C2D_DrawText(&txtWp[i], C2D_WithColor, x0 + 8.0f, y0 + 3.0f, 0.5f, 0.38f, 0.38f,
		             on ? neon(255, 255, 255, amt) : neon(120, 220, 235, 0.9f * amt * dm));
		else txt("MEDKIT", x0 + 8.0f, y0 + 3.0f, 0.38f, on ? neon(255, 255, 255, amt) : neon(110, 255, 150, 0.9f * amt * dm), 0);
		char ab[16];
		if (i == WP_FISTS) snprintf(ab, sizeof ab, "-");
		else if (i == WP_COUNT) snprintf(ab, sizeof ab, "x%d", plMedkits);
		else if (!ownedW[i]) snprintf(ab, sizeof ab, "NONE");
		else snprintf(ab, sizeof ab, "%d", ammo[i]);
		txt(ab, x0 + 8.0f, y0 + 15.0f, 0.52f, (i != WP_FISTS && i < WP_COUNT && ownedW[i] && ammo[i] == 0) || !use ? neon(255, 80, 80, amt * dm + 0.0f) : neon(255, 255, 255, 0.95f * amt), 0);
	}
	drawCentered(&txtRelease, 160.0f, 104.0f, 0.30f, neon(255, 46, 209, 0.9f * amt));
}

// ---- menus, loading screens and HUD for the game modes ------------------------------------------------
static void txt(const char* s, float x, float y, float sc, u32 clr, int align) { // 0 left, 1 centre, 2 right
	C2D_Text t;
	C2D_TextParse(&t, gDyn, s);
	C2D_TextOptimize(&t);
	float w, h;
	C2D_TextGetDimensions(&t, sc, sc, &w, &h);
	if (align == 1) x -= w * 0.5f; else if (align == 2) x -= w;
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, sc, sc, clr);
}

static const char* const kModeName[4] = {"FREE ROAM", "DEATHMATCH", "BATTLE ROYALE", "TARGET PRACTICE"};
static const char* const kModeTag[4] = {"// OPEN CITY SANDBOX", "// ENDLESS SURVIVAL", "// LAST ONE STANDING", "// DARTS WITH GUNS"};
static const char* const kModeDesc[4][5] = {
	{"Explore Chicago at your own pace.", "Traffic, pedestrians and weather are live.", "Hack the lights, cause a blackout,", "grab a weapon from the wheel and roam.", ""},
	{"Hostile NPCs hunt you across a random", "part of the city. Ten hits will kill you.", "Hide from them to heal back to half health.", "You can never win. Survive, and every", "10/25/30/45/50 kills they get deadlier."},
	{"Drop in at a random spot with no weapons.", "Find guns, ammo and medkits around the map.", "NPCs loot, heal and fight each other too.", "The storm closes in. Be the last one alive", "to win."},
	{"Darts, but with guns. Stand still, look", "around and shoot at your own target board.", "5 rounds of 3 shots against 3 NPC rivals.", "First person only. The closer to the", "bullseye the higher the score. Top score wins."}
};

// generic neon list panel in the pause-menu style (dim: 0..1)
static void listRect(int n, int i, float y0first, float sp, float* y0, float* h) { (void)n; *y0 = y0first + (float)i * sp; *h = sp - 6.0f; }

static void menuPanel(float dim, const char* title, const char* const* items, int n, int sel, int frame,
                      const char* sub, const char* footer, float y0first, float sp, int bg) {
	if (dim < 0.01f) return;
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, bg ? neon(2, 6, 14, 0.96f * dim) : neon(2, 6, 14, 0.80f * dim));
	for (float y = 2.0f; y < 240.0f; y += 6.0f) C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.05f * dim));
	float by = fmodf((float)frame * 1.6f, 270.0f) - 15.0f;
	C2D_DrawRectSolid(0.0f, by, 0.5f, 320.0f, 3.0f, neon(0, 229, 255, 0.09f * dim));
	brackets(8.0f, 8.0f, 304.0f, 224.0f, 20.0f, 2.0f, neon(0, 229, 255, 0.95f * dim));
	brackets(14.0f, 14.0f, 292.0f, 212.0f, 8.0f, 1.0f, neon(255, 46, 209, 0.7f * dim));
	txt(title, 160.0f, 20.0f, 0.7f, neon(0, 229, 255, dim), 1);
	C2D_DrawRectSolid(50.0f, 52.0f, 0.5f, 220.0f, 1.0f, neon(0, 229, 255, 0.6f * dim));
	C2D_DrawRectSolid(50.0f, 51.0f, 0.5f, 44.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));
	if (sub) txt(sub, 160.0f, 55.0f, 0.30f, neon(255, 46, 209, 0.9f * dim), 1);
	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.12f);
	for (int i = 0; i < n; i++) {
		float y0, h, x0 = 50.0f, w = 220.0f;
		listRect(n, i, y0first, sp, &y0, &h);
		int on = (i == sel);
		C2D_DrawRectSolid(x0, y0, 0.5f, w, h, on ? neon(0, 229, 255, (0.18f + 0.07f * pulse) * dim) : neon(8, 24, 40, 0.80f * dim));
		frameRect(x0, y0, w, h, 1.0f, on ? neon(0, 229, 255, dim) : neon(0, 150, 180, 0.6f * dim));
		C2D_DrawRectSolid(x0, y0, 0.5f, on ? 6.0f : 2.0f, h, on ? neon(255, 46, 209, dim) : neon(0, 229, 255, 0.7f * dim));
		if (on) C2D_DrawRectSolid(x0 + w - 18.0f, y0 + h - 3.0f, 0.5f, 18.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));
		C2D_Text t;
		C2D_TextParse(&t, gDyn, items[i]);
		C2D_TextOptimize(&t);
		float tw, th;
		C2D_TextGetDimensions(&t, 0.55f, 0.55f, &tw, &th);
		C2D_DrawText(&t, C2D_WithColor, x0 + 20.0f, y0 + (h - th) * 0.5f, 0.5f, 0.55f, 0.55f, on ? neon(255, 255, 255, dim) : neon(120, 220, 235, 0.9f * dim));
	}
	if (footer) txt(footer, 160.0f, 213.0f, 0.34f, neon(120, 220, 235, 0.85f * dim), 1);
}

static const char* const feMain[3]   = {"SINGLE PLAYER", "MULTIPLAYER", "BACK"};
static const char* const feSingle[2] = {"FREE ROAM", "BACK"};
static const char* const feMulti[4]  = {"DEATHMATCH", "BATTLE ROYALE", "TARGET PRACTICE", "BACK"};
static int feCount(int st) { return st == 1 ? 3 : (st == 2 ? 2 : 4); }
static void feRect(int st, int i, float* y0, float* h) { listRect(feCount(st), i, st == 2 ? 96.0f : 60.0f, 36.0f, y0, h); if (st == 2) *y0 = 90.0f + (float)i * 40.0f; }

static void drawFrontEnd(int st, int sel, int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0x0b, 0x0d, 0x14, 0xFF));
	const char* sub = NULL;
	if (st == 3 && sel < 3) sub = kModeTag[1 + sel];
	if (st == 2 && sel == 0) sub = kModeTag[0];
	menuPanel(1.0f, st == 1 ? "M A I N   M E N U" : (st == 2 ? "S I N G L E   P L A Y E R" : "M U L T I P L A Y E R"),
	          st == 1 ? feMain : (st == 2 ? feSingle : feMulti), feCount(st), sel, frame, sub, "A: SELECT     B: BACK     D-PAD / TOUCH", st == 2 ? 90.0f : 60.0f, st == 2 ? 40.0f : 36.0f, 0);
}

static const char* const overItems[2] = {"RETRY", "MAIN MENU"};
static void drawOverPanel(int frame, int sel) {
	char l1[48], l2[48], l3[48];
	const char* title = "G A M E   O V E R";
	l1[0] = l2[0] = l3[0] = 0;
	if (gMode == M_DM) { snprintf(l1, sizeof l1, "KILLS  %d", gKills); snprintf(l2, sizeof l2, "LEVEL  %d / 5", gLevel); snprintf(l3, sizeof l3, "SURVIVED  %d:%02d", gTime / 3600, (gTime / 60) % 60); }
	else if (gMode == M_BR) {
		title = gWon ? "V I C T O R Y" : "E L I M I N A T E D";
		snprintf(l1, sizeof l1, gWon ? "LAST ONE ALIVE" : "PLACE  #%d / 12", gPlace); snprintf(l2, sizeof l2, "KILLS  %d", gKills); snprintf(l3, sizeof l3, "TIME  %d:%02d", gTime / 3600, (gTime / 60) % 60);
	} else if (gMode == M_TP) {
		title = gWon ? "Y O U   W I N" : (gDraw ? "D R A W" : "Y O U   L O S E");
		snprintf(l1, sizeof l1, "YOUR SCORE  %d", tpScore[TP_PL]); snprintf(l2, sizeof l2, "PLACE  #%d / 4", gPlace);
	}
	menuPanel(1.0f, title, overItems, 2, sel, frame, NULL, "A: SELECT     D-PAD / TOUCH", 140.0f, 42.0f, 1);
	txt(l1, 160.0f, 62.0f, 0.5f, neon(255, 255, 255, 1.0f), 1);
	txt(l2, 160.0f, 84.0f, 0.45f, neon(120, 220, 235, 1.0f), 1);
	txt(l3, 160.0f, 104.0f, 0.45f, neon(120, 220, 235, 1.0f), 1);
}
static void overRect(int i, float* y0, float* h) { listRect(2, i, 140.0f, 42.0f, y0, h); }

// black, futuristic loading screen with a description of the chosen mode. Returns 0 if the player backed out.
static int runLoading(C3D_RenderTarget* top, C3D_RenderTarget* bottom, int mode, int* quit) {
	static const char* const sysMsg[6] = {"BUILDING NAV GRID", "SPAWNING ENTITIES", "COMPILING LOOT TABLES", "CALIBRATING AI", "SYNCING WORLD STATE", "READY"};
	int f = 0, ready = 0, done = 0, result = 1;
	float prog = 0.0f;
	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown();
		updateAudio(0, 0, 0.0f, 0.0f);
		if (f == 40) { setupMode(mode); ready = 1; }
		if (!ready) prog += (0.45f - prog) * 0.05f; else prog += (1.0f - prog) * 0.04f;
		if (prog > 0.995f && ready) prog = 1.0f;
		done = (prog >= 1.0f && f > 140);
		if (done && (down & (KEY_A | KEY_TOUCH))) break;
		if (down & KEY_B) { if (ready) exitMode(); result = 0; break; }
		int pct = (int)(prog * 100.0f + 0.5f);
		C2D_TextBufClear(gDyn);
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_Prepare();
		C2D_TargetClear(top, C2D_Color32(0, 0, 0, 0xFF));
		C2D_SceneBegin(top);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		// drifting grid
		float gy = fmodf((float)f * 0.6f, 20.0f);
		for (float y = gy; y < 240.0f; y += 20.0f) C2D_DrawRectSolid(0.0f, y, 0.5f, 400.0f, 1.0f, neon(0, 229, 255, 0.07f));
		for (float x = 0.0f; x < 400.0f; x += 40.0f) C2D_DrawRectSolid(x, 0.0f, 0.5f, 1.0f, 240.0f, neon(0, 229, 255, 0.05f));
		float sw = fmodf((float)f * 3.0f, 300.0f) - 30.0f;
		C2D_DrawRectSolid(0.0f, sw, 0.5f, 400.0f, 2.0f, neon(0, 229, 255, 0.10f));
		brackets(8.0f, 8.0f, 384.0f, 224.0f, 22.0f, 2.0f, neon(0, 229, 255, 0.95f));
		brackets(14.0f, 14.0f, 372.0f, 212.0f, 8.0f, 1.0f, neon(255, 46, 209, 0.7f));
		txt("// MISSION BRIEFING", 24.0f, 20.0f, 0.38f, neon(255, 46, 209, 0.9f), 0);
		txt(kModeName[mode], 200.0f, 34.0f, 1.0f, neon(0, 229, 255, 1.0f), 1);
		C2D_DrawRectSolid(60.0f, 76.0f, 0.5f, 280.0f, 1.0f, neon(0, 229, 255, 0.6f));
		C2D_DrawRectSolid(60.0f, 75.0f, 0.5f, 60.0f, 3.0f, neon(255, 46, 209, 0.9f));
		txt(kModeTag[mode], 200.0f, 80.0f, 0.4f, neon(255, 46, 209, 0.9f), 1);
		for (int i = 0; i < 5; i++) if (kModeDesc[mode][i][0]) txt(kModeDesc[mode][i], 200.0f, 102.0f + 17.0f * (float)i, 0.42f, neon(215, 240, 250, 0.95f), 1);
		// progress: segmented bar
		float bx = 50.0f, bw = 300.0f;
		for (int i = 0; i < 30; i++) {
			float on = (float)(i + 1) / 30.0f <= prog ? 1.0f : 0.0f;
			C2D_DrawRectSolid(bx + (float)i * (bw / 30.0f), 200.0f, 0.5f, bw / 30.0f - 2.0f, 8.0f, on > 0.5f ? neon(0, 229, 255, 0.95f) : neon(0, 120, 150, 0.30f));
		}
		char buf[64];
		int mi = ready ? (prog > 0.99f ? 5 : 3 + (int)(prog * 2.0f) % 2) : (f / 14) % 3;
		snprintf(buf, sizeof buf, "SYS> %s ...", sysMsg[mi]);
		txt(buf, 50.0f, 214.0f, 0.34f, neon(120, 220, 235, 0.9f), 0);
		snprintf(buf, sizeof buf, "%d%%", pct);
		txt(buf, 350.0f, 212.0f, 0.45f, neon(255, 255, 255, 1.0f), 2);

		C2D_TargetClear(bottom, C2D_Color32(0, 0, 0, 0xFF));
		C2D_SceneBegin(bottom);
		brackets(8.0f, 8.0f, 304.0f, 224.0f, 20.0f, 2.0f, neon(0, 229, 255, 0.8f));
		for (int i = 0; i < 6; i++) { // spinner
			float a = (float)f * 0.1f + (float)i * 1.0472f, k = 0.25f + 0.75f * (float)((i + f / 4) % 6) / 5.0f;
			C2D_DrawRectSolid(160.0f + 26.0f * cosf(a) - 4.0f, 100.0f + 26.0f * sinf(a) - 4.0f, 0.5f, 8.0f, 8.0f, neon(0, 229, 255, k));
		}
		txt(done ? "PRESS  A  TO  DEPLOY" : "L O A D I N G", 160.0f, 150.0f, done ? 0.55f : 0.5f, (done && (f / 20) % 2) ? neon(255, 46, 209, 1.0f) : neon(220, 240, 250, 1.0f), 1);
		txt("B: BACK", 160.0f, 210.0f, 0.34f, neon(90, 110, 130, 1.0f), 1);
		C2D_Flush();
		C3D_FrameEnd(0);
		f++;
	}
	if (!aptMainLoop()) { *quit = 1; result = 0; }
	return result;
}

// ---- bottom screen for Target Practice ----
static void drawTpBottom(int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0x05, 0x08, 0x10, 0xFF));
	for (float y = 2.0f; y < 240.0f; y += 6.0f) C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.04f));
	brackets(6.0f, 6.0f, 308.0f, 228.0f, 16.0f, 2.0f, neon(0, 229, 255, 0.9f));
	txt("T A R G E T   P R A C T I C E", 160.0f, 10.0f, 0.42f, neon(0, 229, 255, 1.0f), 1);
	// your board with this turn's hits
	const float bx = 82.0f, by = 112.0f, R = 66.0f;
	static const u32 band[6] = {0, 0, 0, 0, 0, 0};
	(void)band;
	C2D_DrawCircleSolid(bx, by, 0.5f, R + 3.0f, neon(0, 80, 100, 1.0f));
	static const float rr[5] = {1.8f, 1.62f, 1.26f, 0.9f, 0.54f};
	for (int q = 0; q < 5; q++) C2D_DrawCircleSolid(bx, by, 0.5f, R * rr[q] / TP_BR, (q & 1) ? neon(235, 235, 235, 1.0f) : neon(215, 40, 40, 1.0f));
	C2D_DrawCircleSolid(bx, by, 0.5f, R * TP_RING / TP_BR, neon(255, 210, 40, 1.0f));
	for (int m = 0; m < tpMarkN[TP_PL]; m++) {
		const Mark* mk = &tpMarks[TP_PL][m];
		C2D_DrawCircleSolid(bx + mk->x * R / TP_BR, by - mk->y * R / TP_BR, 0.5f, 3.2f, neon(0, 0, 0, 1.0f));
		C2D_DrawCircleSolid(bx + mk->x * R / TP_BR, by - mk->y * R / TP_BR, 0.5f, 2.2f, mk->s ? neon(0, 229, 255, 1.0f) : neon(120, 120, 120, 1.0f));
	}
	txt("YOUR BOARD", bx, 190.0f, 0.34f, neon(120, 220, 235, 1.0f), 1);
	// scoreboard
	float x0 = 162.0f, y0 = 30.0f;
	txt("SCOREBOARD", x0 + 2.0f, y0 - 2.0f, 0.34f, neon(255, 46, 209, 1.0f), 0);
	int order[4] = {0, 1, 2, 3};
	for (int a = 0; a < 4; a++) for (int b2 = a + 1; b2 < 4; b2++) if (tpScore[order[b2]] > tpScore[order[a]]) { int t = order[a]; order[a] = order[b2]; order[b2] = t; }
	for (int i = 0; i < 4; i++) {
		int id = order[i];
		float yy = y0 + 14.0f + (float)i * 28.0f;
		int act = (gState == GS_PLAY && tpTurn == id);
		C2D_DrawRectSolid(x0, yy, 0.5f, 146.0f, 24.0f, id == TP_PL ? neon(0, 229, 255, 0.20f) : neon(8, 24, 40, 0.85f));
		frameRect(x0, yy, 146.0f, 24.0f, 1.0f, act ? neon(255, 46, 209, 1.0f) : neon(0, 150, 180, 0.6f));
		char b[32];
		snprintf(b, sizeof b, "%d. %s", i + 1, tpName[id]);
		txt(b, x0 + 8.0f, yy + 5.0f, 0.45f, id == TP_PL ? neon(255, 255, 255, 1.0f) : neon(120, 220, 235, 1.0f), 0);
		snprintf(b, sizeof b, "%d", tpScore[id]);
		txt(b, x0 + 138.0f, yy + 3.0f, 0.6f, neon(255, 255, 255, 1.0f), 2);
	}
	char b[64];
	snprintf(b, sizeof b, "ROUND %d / %d", tpRound + 1, TP_ROUNDS);
	txt(b, 235.0f, 150.0f, 0.5f, neon(0, 229, 255, 1.0f), 1);
	int yourRound = tpRoundScore[TP_PL][tpRound];
	snprintf(b, sizeof b, "THIS ROUND: %d", yourRound);
	txt(b, 235.0f, 170.0f, 0.4f, neon(220, 240, 250, 1.0f), 1);
	if (gState == GS_PLAY && tpTurn == TP_PL && tpPhase == 1) {
		snprintf(b, sizeof b, "SHOTS LEFT: %d", tpShotsLeft);
		txt(b, 235.0f, 190.0f, 0.45f, neon(255, 46, 209, 1.0f), 1);
	}
	txt("R: FIRE   L: STEADY AIM   PAD: LOOK", 160.0f, 214.0f, 0.3f, neon(90, 130, 150, 1.0f), 1);
}

// bottom-screen overlays for the modes: enemy radar, loot, storm
static void drawMapMode(float px, float pz) {
	if (gMode != M_DM && gMode != M_BR) return;
	if (gMode == M_BR) {
		for (int i = 0; i < N_LOOT; i++) {
			const Loot* l = &loot[i];
			if (!l->active) continue;
			float sx = MAP_CX + (l->x - px) * MAP_S, sy = MAP_CY + (l->z - pz) * MAP_S;
			if (sx < 4.0f || sx > 316.0f || sy < 24.0f || sy > MAP_H - 4.0f) continue;
			u32 c = l->kind == LK_WEAPON ? C2D_Color32(80, 160, 255, 255) : (l->kind == LK_AMMO ? C2D_Color32(255, 220, 60, 255) : C2D_Color32(70, 255, 120, 255));
			C2D_DrawRectSolid(sx - 2.0f, sy - 2.0f, 0.5f, 4.0f, 4.0f, c);
		}
		for (int pass = 0; pass < 2; pass++) { // storm edge (purple) and next safe circle (white)
			if (pass == 1 && zPhase >= 5) break;
			float cx = pass ? zTx : zCx, cz = pass ? zTz : zCz, R = pass ? zTr : zR;
			u32 c = pass ? C2D_Color32(255, 255, 255, 220) : C2D_Color32(190, 70, 255, 255);
			float ox = 0, oy = 0;
			for (int k = 0; k <= 72; k++) {
				float a = (float)k * 6.2832f / 72.0f;
				float sx = MAP_CX + (cx + R * sinf(a) - px) * MAP_S, sy = MAP_CY + (cz + R * cosf(a) - pz) * MAP_S;
				if (k > 0 && sx > -50.0f && sx < 370.0f && sy > -50.0f && sy < 230.0f && ox > -50.0f && ox < 370.0f && oy > -50.0f && oy < 230.0f) {
					float y0 = oy, y1 = sy;
					if ((y0 > 22.0f && y0 < MAP_H - 2.0f) && (y1 > 22.0f && y1 < MAP_H - 2.0f)) C2D_DrawLine(ox, oy, c, sx, sy, c, 2.0f, 0.5f);
				}
				ox = sx; oy = sy;
			}
		}
	}
	for (int i = 0; i < MAX_BOTS; i++) {
		const Bot* b = &bots[i];
		if (!b->active || b->dead) continue;
		float dx = b->x - px, dz = b->z - pz;
		if (gMode == M_BR && dx * dx + dz * dz > 55.0f * 55.0f) continue;
		float sx = MAP_CX + dx * MAP_S, sy = MAP_CY + dz * MAP_S;
		if (sx < 4.0f || sx > 316.0f || sy < 24.0f || sy > MAP_H - 4.0f) continue;
		C2D_DrawCircleSolid(sx, sy, 0.5f, 4.5f, C2D_Color32(255, 255, 255, 255));
		C2D_DrawCircleSolid(sx, sy, 0.5f, 3.3f, C2D_Color32(235, 40, 50, 255));
	}
	if (gMode == M_BR) {
		char b[32];
		snprintf(b, sizeof b, "MEDKITS x%d", plMedkits);
		C2D_DrawRectSolid(206.0f, 54.0f, 0.5f, 110.0f, 14.0f, C2D_Color32(0, 0, 0, 180));
		txt(b, 212.0f, 55.0f, 0.34f, plMedkits ? C2D_Color32(110, 255, 150, 255) : C2D_Color32(160, 160, 170, 255), 0);
	}
}

// ---- Free Roam HUD: cash, boost, neighbourhood banner, the A prompt, toasts --------------------------------------
static int gHudQuiet = 0;       // 1 while the pause menu is up: no prompt, no banner
static void drawFreeHud(int frame) {
	char b[64];
	// cash
	snprintf(b, sizeof b, "$ %d", gCash);
	C2D_DrawRectSolid(300.0f, 8.0f, 0.5f, 92.0f, 22.0f, neon(0, 0, 0, 0.55f));
	frameRect(300.0f, 8.0f, 92.0f, 22.0f, 1.0f, neon(110, 255, 150, 0.8f));
	txt(b, 386.0f, 10.0f, 0.55f, neon(110, 255, 150, 1.0f), 2);
	if (gBoost > 0) {
		int s = (gBoost + 59) / 60;
		float k = (float)gBoost / (float)(120 * 60); if (k > 1.0f) k = 1.0f;
		snprintf(b, sizeof b, "SPEED BOOST %d:%02d", s / 60, s % 60);
		txt(b, 386.0f, 33.0f, 0.32f, neon(0, 229, 255, 1.0f), 2);
		C2D_DrawRectSolid(300.0f, 45.0f, 0.5f, 92.0f, 3.0f, neon(0, 0, 0, 0.55f));
		C2D_DrawRectSolid(300.0f, 45.0f, 0.5f, 92.0f * k, 3.0f, neon(0, 229, 255, 0.9f));
	}
	if (!gHudQuiet) {
		// where you are
		if (gRoom >= 0) {
			txt(roomName[gRoom], 200.0f, 10.0f, 0.6f, neon(255, 255, 255, 0.95f), 1);
		} else if (gZoneBannerT > 0 && gZoneBanner) {
			float a = (float)gZoneBannerT / 30.0f; if (a > 1.0f) a = 1.0f;
			float in = (float)(170 - gZoneBannerT) / 14.0f; if (in < a) a = in < 0.0f ? 0.0f : in;
			C2D_DrawRectSolid(110.0f, 8.0f, 0.5f, 180.0f, 30.0f, neon(0, 0, 0, 0.5f * a));
			C2D_DrawRectSolid(110.0f, 8.0f, 0.5f, 3.0f, 30.0f, neon(255, 46, 209, a));
			txt(gZoneBanner, 200.0f, 9.0f, 0.5f, neon(255, 255, 255, a), 1);
			txt(kZoneName[zoneAt(gPlX, gPlZ)], 200.0f, 26.0f, 0.3f, neon(0, 229, 255, a), 1);
		}
		// the interaction prompt: always the A button
		if (gPrompt && gTrans == 0 && gShop < 0 && gState == GS_PLAY) {
			float pw = 60.0f + 7.0f * (float)strlen(gPromptTxt);
			if (pw > 300.0f) pw = 300.0f;
			float x0 = 200.0f - pw * 0.5f, pulse = 0.5f + 0.5f * sinf((float)frame * 0.14f);
			C2D_DrawRectSolid(x0, 196.0f, 0.5f, pw, 26.0f, neon(0, 0, 0, 0.62f));
			frameRect(x0, 196.0f, pw, 26.0f, 1.0f, neon(0, 229, 255, 0.7f + 0.3f * pulse));
			C2D_DrawRectSolid(x0 + 4.0f, 200.0f, 0.5f, 18.0f, 18.0f, neon(255, 46, 209, 1.0f));
			txt("A", x0 + 13.0f, 201.0f, 0.5f, neon(255, 255, 255, 1.0f), 1);
			txt(gPromptTxt, x0 + 26.0f + (pw - 30.0f) * 0.5f, 203.0f, 0.42f, neon(255, 255, 255, 1.0f), 1);
		}
	}
	if (gToast > 0 && gState == GS_PLAY) txt(gToastTxt, 200.0f, 170.0f, 0.5f, neon(255, 255, 255, gToast > 30 ? 1.0f : (float)gToast / 30.0f), 1);
}

// bottom screen while a shop menu is open
static void drawShopBottom(int frame) {
	static char lbl[8][44];
	const char* items[8];
	int n;
	const ShopItem* it = shopItems(gShop, &n);
	for (int i = 0; i < n; i++) {
		const char* own = NULL;
		if (it[i].kind == IK_CAP && gCap) own = "OWNED";
		if (it[i].kind == IK_SHADES && gShades) own = "OWNED";
		if (it[i].kind == IK_JACKET && gJacketIx == it[i].a) own = "WEARING";
		if (own) snprintf(lbl[i], sizeof lbl[i], "%s   %s", it[i].name, own);
		else if (it[i].price <= 0) snprintf(lbl[i], sizeof lbl[i], "%s   FREE", it[i].name);
		else snprintf(lbl[i], sizeof lbl[i], "%s   $%d", it[i].name, it[i].price);
		items[i] = lbl[i];
	}
	items[n] = "LEAVE";
	char sub[48];
	if (kKeeperName[gShop][0]) snprintf(sub, sizeof sub, "%s  -  CASH $%d", kKeeperName[gShop], gCash);
	else snprintf(sub, sizeof sub, "CASH $%d", gCash);
	menuPanel(1.0f, roomName[gShop], items, n + 1, gShopSel, frame, sub, gShopMsgT > 0 ? gShopMsg : "A: BUY     B: LEAVE     D-PAD / TOUCH", 66.0f, 29.0f, 1);
}

// bottom screen inside a building: a floor plan with you, the service spot and the door
static void drawRoomBottom(float px, float pz, float face, int frame) {
	const Room* m = &rooms[gRoom];
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(4, 8, 16, 1.0f));
	for (float y = 2.0f; y < 240.0f; y += 6.0f) C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.05f));
	brackets(8.0f, 8.0f, 304.0f, 224.0f, 20.0f, 2.0f, neon(0, 229, 255, 0.95f));
	txt(roomName[gRoom], 160.0f, 14.0f, 0.6f, neon(0, 229, 255, 1.0f), 1);
	char b[64];
	snprintf(b, sizeof b, "CASH $%d", gCash);
	txt(b, 160.0f, 34.0f, 0.4f, neon(110, 255, 150, 1.0f), 1);
	float rw = m->x1 - m->x0, rh = m->z1 - m->z0;
	float sc = 276.0f / rw; if (128.0f / rh < sc) sc = 128.0f / rh;
	float ox = 160.0f - (m->x0 + rw * 0.5f) * sc, oz = 114.0f - (m->z0 + rh * 0.5f) * sc;
	C2D_DrawRectSolid(ox + m->x0 * sc, oz + m->z0 * sc, 0.5f, rw * sc, rh * sc, neon(18, 26, 40, 1.0f));
	frameRect(ox + m->x0 * sc, oz + m->z0 * sc, rw * sc, rh * sc, 1.0f, neon(0, 150, 180, 0.9f));
	for (int i = 0; i < m->nSol; i++) {
		const float* q = m->sol[i];
		C2D_DrawRectSolid(ox + q[0] * sc, oz + q[1] * sc, 0.5f, (q[2] - q[0]) * sc, (q[3] - q[1]) * sc, neon(70, 90, 120, 1.0f));
	}
	for (int i = 0; i < m->nCrate; i++) if (!wareGot[i]) C2D_DrawRectSolid(ox + m->crate[i][0] * sc - 2.0f, oz + m->crate[i][1] * sc - 2.0f, 0.5f, 4.0f, 4.0f, neon(255, 220, 60, 1.0f));
	if (m->kHas) C2D_DrawCircleSolid(ox + m->kX * sc, oz + m->kZ * sc, 0.5f, 3.0f, neon(255, 150, 60, 1.0f));
	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.14f);
	C2D_DrawCircleSolid(ox + m->aX * sc, oz + m->aZ * sc, 0.5f, 4.0f + 3.0f * pulse, neon(255, 46, 209, 0.45f));
	C2D_DrawCircleSolid(ox + m->aX * sc, oz + m->aZ * sc, 0.5f, 2.5f, neon(255, 46, 209, 1.0f));
	C2D_DrawRectSolid(ox + m->exX * sc - 4.0f, oz + m->exZ * sc - 2.0f, 0.5f, 8.0f, 4.0f, neon(110, 255, 150, 1.0f));
	{
		float dx = -sinf(face), dy = -cosf(face), nx = -dy, ny = dx, X = ox + px * sc, Y = oz + pz * sc;
		C2D_DrawTriangle(X + dx * 9.0f, Y + dy * 9.0f, neon(255, 255, 255, 1.0f), X + dx * 2.0f + nx * 3.5f, Y + dy * 2.0f + ny * 3.5f, neon(255, 255, 255, 1.0f),
		                 X + dx * 2.0f - nx * 3.5f, Y + dy * 2.0f - ny * 3.5f, neon(255, 255, 255, 1.0f), 0.5f);
		C2D_DrawCircleSolid(X, Y, 0.5f, 3.5f, neon(30, 123, 255, 1.0f));
	}
	if (gPrompt) { snprintf(b, sizeof b, "A:  %s", gPromptTxt); txt(b, 160.0f, 184.0f, 0.5f, neon(255, 255, 255, 1.0f), 1); }
	else txt("PINK = SERVICE SPOT     GREEN = DOOR", 160.0f, 186.0f, 0.34f, neon(255, 46, 209, 0.9f), 1);
	txt("B: RUN     Y: CAMERA     SELECT: PAUSE", 160.0f, 208.0f, 0.34f, neon(120, 220, 235, 0.85f), 1);
}

// ---- top-screen HUD (drawn with citro2d on top of the 3D view) ----
static void drawTopHud(int frame, float camYaw) {
	if (gMode == M_FREE && gState != GS_COUNTDOWN && gGo <= 0) { drawFreeHud(frame); return; }
	const float CX = 200.0f, CY = 120.0f;
	if (gMode == M_DM || gMode == M_BR) {
		if (plFlash > 0) { // damage flash around the edges
			float a = (float)plFlash / 14.0f;
			for (int k = 0; k < 4; k++) {
				float th = 10.0f + 8.0f * (float)k, al = 0.28f * a * (1.0f - 0.2f * (float)k);
				C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, th, neon(255, 0, 20, al));
				C2D_DrawRectSolid(0.0f, 240.0f - th, 0.5f, 400.0f, th, neon(255, 0, 20, al));
				C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, th, 240.0f, neon(255, 0, 20, al));
				C2D_DrawRectSolid(400.0f - th, 0.0f, 0.5f, th, 240.0f, neon(255, 0, 20, al));
			}
		}
		if (plHp > 0.0f && plHp < 30.0f && gState == GS_PLAY) { // low health pulse
			float a = 0.10f + 0.10f * sinf((float)frame * 0.2f);
			C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 14.0f, neon(255, 0, 20, a)); C2D_DrawRectSolid(0.0f, 226.0f, 0.5f, 400.0f, 14.0f, neon(255, 0, 20, a));
			C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 14.0f, 240.0f, neon(255, 0, 20, a));  C2D_DrawRectSolid(386.0f, 0.0f, 0.5f, 14.0f, 240.0f, neon(255, 0, 20, a));
		}
		if (gMode == M_BR && gState == GS_PLAY && outsideStorm(gPlX, gPlZ)) {
			float a = 0.16f + 0.08f * sinf((float)frame * 0.15f);
			C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 20.0f, neon(170, 40, 255, a)); C2D_DrawRectSolid(0.0f, 220.0f, 0.5f, 400.0f, 20.0f, neon(170, 40, 255, a));
			C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 20.0f, 240.0f, neon(170, 40, 255, a)); C2D_DrawRectSolid(380.0f, 0.0f, 0.5f, 20.0f, 240.0f, neon(170, 40, 255, a));
		}
		float fx = -sinf(camYaw), fz = -cosf(camYaw), rx = cosf(camYaw), rz = -sinf(camYaw);
		for (int i = 0; i < 4; i++) { // red wedges pointing at whoever shot you
			if (plHitLife[i] <= 0) continue;
			float dx = plHitAng[i] - gPlX, dz = plHitZ[i] - gPlZ;
			float ang = atan2f(dx * rx + dz * rz, dx * fx + dz * fz), a = (float)plHitLife[i] / 70.0f;
			float sx = sinf(ang), sy = -cosf(ang), nx = -sy, ny = sx;
			u32 c = neon(255, 30, 30, a);
			C2D_DrawTriangle(CX + sx * 96.0f, CY + sy * 96.0f, c, CX + sx * 78.0f + nx * 11.0f, CY + sy * 78.0f + ny * 11.0f, c, CX + sx * 78.0f - nx * 11.0f, CY + sy * 78.0f - ny * 11.0f, c, 0.5f);
		}
		// health bar
		float hx = 14.0f, hy = 212.0f, hw = 150.0f, hh = 10.0f, k = plHp / 100.0f;
		if (k < 0.0f) k = 0.0f;
		C2D_DrawRectSolid(hx - 2.0f, hy - 2.0f, 0.5f, hw + 4.0f, hh + 4.0f, neon(0, 0, 0, 0.6f));
		u32 hc = k > 0.5f ? neon(80, 230, 110, 1.0f) : (k > 0.25f ? neon(255, 200, 40, 1.0f) : neon(255, 50, 50, 1.0f));
		C2D_DrawRectSolid(hx, hy, 0.5f, hw * k, hh, hc);
		if (plHeal > 0) C2D_DrawRectSolid(hx, hy, 0.5f, hw * k, 3.0f, neon(255, 255, 255, 0.7f));
		frameRect(hx - 2.0f, hy - 2.0f, hw + 4.0f, hh + 4.0f, 1.0f, neon(0, 229, 255, 0.8f));
		if (gMode == M_DM) C2D_DrawRectSolid(hx + hw * 0.5f - 1.0f, hy - 5.0f, 0.5f, 2.0f, hh + 10.0f, neon(255, 255, 255, 0.85f)); // the level you can heal back to
		char b[64];
		snprintf(b, sizeof b, "HP %d", (int)(plHp + 0.5f));
		txt(b, hx, hy - 15.0f, 0.4f, neon(255, 255, 255, 0.95f), 0);
		if (gMode == M_DM && plHp < 50.0f && gState == GS_PLAY) txt(plUnseen > 120 && plSince > 150 ? "RECOVERING..." : "BREAK LINE OF SIGHT TO RECOVER", hx + 60.0f, hy - 14.0f, 0.3f, neon(255, 210, 90, 0.9f), 0);
	}
	if (gMode == M_DM) {
		char b[64];
		snprintf(b, sizeof b, "KILLS %d", gKills);
		C2D_DrawRectSolid(10.0f, 10.0f, 0.5f, 118.0f, 40.0f, neon(0, 0, 0, 0.5f));
		txt(b, 16.0f, 12.0f, 0.6f, neon(255, 255, 255, 1.0f), 0);
		snprintf(b, sizeof b, "LEVEL %d / 5", gLevel);
		txt(b, 16.0f, 32.0f, 0.42f, neon(255, 46, 209, 1.0f), 0);
		if (gLevel < 5) { snprintf(b, sizeof b, "NEXT AT %d", dmLevelAt[gLevel + 1]); txt(b, 386.0f, 12.0f, 0.34f, neon(160, 210, 225, 0.9f), 2); }
		else txt("MAX LEVEL", 386.0f, 12.0f, 0.34f, neon(255, 90, 90, 1.0f), 2);
		if (gBanner > 0) {
			float a = gBanner > 30 ? 1.0f : (float)gBanner / 30.0f;
			snprintf(b, sizeof b, "LEVEL %d", gLevel);
			C2D_DrawRectSolid(0.0f, 52.0f, 0.5f, 400.0f, 46.0f, neon(0, 0, 0, 0.55f * a));
			C2D_DrawRectSolid(0.0f, 52.0f, 0.5f, 400.0f, 2.0f, neon(255, 46, 209, a));
			C2D_DrawRectSolid(0.0f, 96.0f, 0.5f, 400.0f, 2.0f, neon(0, 229, 255, a));
			txt(b, 200.0f, 54.0f, 0.9f, neon(255, 255, 255, a), 1);
			txt("ENEMIES ARE GETTING DEADLIER", 200.0f, 82.0f, 0.34f, neon(255, 120, 120, a), 1);
		}
	}
	if (gMode == M_BR) {
		char b[64];
		C2D_DrawRectSolid(286.0f, 8.0f, 0.5f, 106.0f, 44.0f, neon(0, 0, 0, 0.5f));
		snprintf(b, sizeof b, "ALIVE %d", botsAlive() + (plHp > 0.0f ? 1 : 0));
		txt(b, 386.0f, 10.0f, 0.55f, neon(255, 255, 255, 1.0f), 2);
		snprintf(b, sizeof b, "KILLS %d", gKills);
		txt(b, 386.0f, 29.0f, 0.4f, neon(255, 46, 209, 1.0f), 2);
		int secs = zPhase >= 5 ? 0 : ((stormShrinking() ? zShrink[zPhase] - (zT - zWait[zPhase]) : zWait[zPhase] - zT) / 60);
		if (zPhase >= 5) snprintf(b, sizeof b, "FINAL ZONE");
		else if (stormShrinking()) snprintf(b, sizeof b, "STORM CLOSING %d:%02d", secs / 60, secs % 60);
		else snprintf(b, sizeof b, "STORM IN %d:%02d", secs / 60, secs % 60);
		C2D_DrawRectSolid(10.0f, 10.0f, 0.5f, 160.0f, 16.0f, neon(0, 0, 0, 0.5f));
		txt(b, 16.0f, 11.0f, 0.36f, stormShrinking() || zPhase >= 5 ? neon(200, 110, 255, 1.0f) : neon(220, 240, 250, 1.0f), 0);
		for (int i = 0; i < FEED_N; i++) if (feedLife[i] > 0) txt(feedTxt[i], 392.0f, 58.0f + 13.0f * (float)i, 0.32f, neon(255, 255, 255, feedLife[i] > 40 ? 0.95f : feedLife[i] / 40.0f), 2);
		if (curWeapon == WP_FISTS && gState == GS_PLAY && gTime < 1500) txt("NO WEAPON - FIND ONE!", 200.0f, 190.0f, 0.4f, neon(255, 210, 90, 0.5f + 0.5f * sinf((float)frame * 0.15f)), 1);
	}
	if (gMode == M_TP) {
		char b[64];
		C2D_DrawRectSolid(120.0f, 6.0f, 0.5f, 160.0f, 44.0f, neon(0, 0, 0, 0.55f));
		snprintf(b, sizeof b, "ROUND %d / %d", tpRound + 1, TP_ROUNDS);
		txt(b, 200.0f, 8.0f, 0.5f, neon(0, 229, 255, 1.0f), 1);
		if (gState == GS_PLAY) {
			if (tpTurn == TP_PL) snprintf(b, sizeof b, tpPhase == 1 ? "YOUR TURN - %d SHOTS LEFT" : "YOUR TURN", tpShotsLeft);
			else snprintf(b, sizeof b, "%s'S TURN", tpName[tpTurn]);
			txt(b, 200.0f, 28.0f, 0.4f, neon(255, 255, 255, 1.0f), 1);
			if (tpTurn == TP_PL && tpPhase == 1) { snprintf(b, sizeof b, "%d", tpTimer / 60 + 1); txt(b, 200.0f, 40.0f, 0.3f, neon(255, 46, 209, 1.0f), 1); }
		}
		for (int i = 0; i < 4; i++) {
			snprintf(b, sizeof b, "%s  %d", tpName[i], tpScore[i]);
			txt(b, 392.0f, 10.0f + 14.0f * (float)i, 0.36f, i == TP_PL ? neon(0, 229, 255, 1.0f) : neon(255, 255, 255, 0.9f), 2);
		}
		float ch = 120.0f;
		float gap = 5.0f, a = 0.9f;
		C2D_DrawRectSolid(CX - gap - 9.0f, ch, 0.5f, 9.0f, 1.0f, neon(255, 255, 255, a)); C2D_DrawRectSolid(CX + gap, ch, 0.5f, 9.0f, 1.0f, neon(255, 255, 255, a));
		C2D_DrawRectSolid(CX, ch - gap - 9.0f, 0.5f, 1.0f, 9.0f, neon(255, 255, 255, a)); C2D_DrawRectSolid(CX, ch + gap, 0.5f, 1.0f, 9.0f, neon(255, 255, 255, a));
	}
	if (gToast > 0 && gState == GS_PLAY) txt(gToastTxt, 200.0f, 172.0f, 0.5f, neon(255, 255, 255, gToast > 30 ? 1.0f : (float)gToast / 30.0f), 1);
	if (gState == GS_COUNTDOWN) {
		int n = gCount / 60; // 3, 2, 1
		char b[16];
		snprintf(b, sizeof b, "%d", n);
		float ph = (float)(gCount % 60) / 60.0f, sc = 2.6f + 1.0f * ph;
		C2D_DrawRectSolid(0.0f, 86.0f, 0.5f, 400.0f, 70.0f, neon(0, 0, 0, 0.35f));
		txt(n >= 1 ? b : "GO", 200.0f, 98.0f - 20.0f * ph, sc, neon(0, 229, 255, 0.4f + 0.6f * (1.0f - ph)), 1);
		txt("GET READY", 200.0f, 64.0f, 0.5f, neon(255, 46, 209, 1.0f), 1);
		txt(kModeName[gMode], 200.0f, 172.0f, 0.5f, neon(255, 255, 255, 0.9f), 1);
	} else if (gGo > 0) {
		float a = (float)gGo / 60.0f;
		txt("GO!", 200.0f, 80.0f, 3.0f, neon(255, 255, 255, a), 1);
	}
	if (gState == GS_OVER) { // result banner
		float a = gOverT > 40 ? 1.0f : (float)gOverT / 40.0f;
		C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, neon(0, 0, 0, 0.55f * a));
		const char* big = "YOU DIED";
		u32 col = neon(255, 50, 50, a);
		if (gMode == M_BR) { big = gWon ? "VICTORY ROYALE" : "ELIMINATED"; col = gWon ? neon(255, 210, 40, a) : neon(255, 50, 50, a); }
		if (gMode == M_TP) { big = gWon ? "YOU WIN!" : (gDraw ? "DRAW" : "YOU LOSE"); col = gWon ? neon(255, 210, 40, a) : (gDraw ? neon(255, 255, 255, a) : neon(255, 50, 50, a)); }
		txt(big, 200.0f, 70.0f, 1.3f, col, 1);
		char b[64];
		if (gMode == M_DM) snprintf(b, sizeof b, "%d KILLS  -  LEVEL %d", gKills, gLevel);
		else if (gMode == M_BR) snprintf(b, sizeof b, gWon ? "LAST ONE STANDING" : "PLACE #%d  -  %d KILLS", gPlace, gKills);
		else snprintf(b, sizeof b, "FINAL SCORE %d", tpScore[TP_PL]);
		txt(b, 200.0f, 125.0f, 0.5f, neon(255, 255, 255, a), 1);
		if (gMode == M_TP) for (int i = 0; i < 4; i++) { snprintf(b, sizeof b, "%s  %d", tpName[i], tpScore[i]); txt(b, 200.0f, 148.0f + 14.0f * (float)i, 0.36f, i == TP_PL ? neon(0, 229, 255, a) : neon(200, 210, 220, a), 1); }
	}
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

// Which districts to draw for a camera at (cx,cz) looking along (fx,fz): everything near, plus a forward cone. A district that
// lies entirely beyond the fog is just sky colour, so it is skipped (57 = half a district's diagonal). Used by the main loop
// and by the host draw-budget test, so the test measures the real rule.
static void computeVis(float cx, float cz, float fx, float fz, int title, unsigned char* vis) {
	const float cullFar = gFog ? wFogE + 57.0f : 440.0f;
	for (int c = 0; c < CELLS; c++) {
		float dx = ((float)(c % CN) - (float)(CN - 1) * 0.5f) * CELL_W - cx, dz = ((float)(c / CN) - (float)(CN - 1) * 0.5f) * CELL_W - cz;
		float d = sqrtf(dx * dx + dz * dz), dot = dx * fx + dz * fz;
		vis[c] = ((title ? d < 150.0f : d < 100.0f) || dot + 70.0f > 0.62f * d) ? 1 : 0;
		if (!title && d > cullFar) vis[c] = 0;
	}
}

int main(int argc, char** argv) {
	gfxInitDefault();
	osSetSpeedupEnable(true); // New 3DS: 804 MHz CPU + extra cache (ignored on an Old 3DS)
	romfsInit();              // title music lives in romfs:/title.pcm
	// The city's vertex buffer is the largest single allocation (MAX_VERTS x 36 bytes = about 8.3 MB of linear memory).
	// It is taken first, and a failure stops with a readable message instead of writing through a NULL pointer later.
	vbo = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_VERTS);
	if (!vbo) {
		consoleInit(GFX_TOP, NULL);
		printf("Chicago 3DS\n\nNot enough linear memory for the city\nvertex buffer (%u KB needed).\n\nPress START to exit.\n", (unsigned)(sizeof(Vertex) * MAX_VERTS / 1024));
		while (aptMainLoop()) {
			hidScanInput();
			if (hidKeysDown() & KEY_START) break;
			gspWaitForVBlank();
		}
		romfsExit();
		gfxExit();
		return 1;
	}
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

	texOK = initWindowTex();
	if (!texOK) gWin = 0;
	initStars();
	buildCity();
	staticCount = vcount;
	GSPGPU_FlushDataCache(vbo, staticCount * sizeof(Vertex));
	initNav();
	initCars();
	initPeds();
	initCrates();
	initWorld();
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
	int   feState = 0, feSel = 0, overSel = 0, launch = -1; // front-end menus: 0 press start, 1 main, 2 single player, 3 multiplayer
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
			if (feState == 0) {
				if (down & (KEY_A | KEY_TOUCH)) { feState = 1; feSel = 0; playSfx(SFX_SWITCH, 0.6f); }
			} else {
				int n = feCount(feState), choose = (down & KEY_A) ? 1 : 0;
				if (down & KEY_DDOWN) { feSel = (feSel + 1) % n; playSfx(SFX_CLICK, 0.4f); }
				if (down & KEY_DUP)   { feSel = (feSel + n - 1) % n; playSfx(SFX_CLICK, 0.4f); }
				if (down & KEY_TOUCH) {
					for (int i = 0; i < n; i++) {
						float y0, h;
						feRect(feState, i, &y0, &h);
						if (tp.px >= 50 && tp.px <= 270 && tp.py >= y0 && tp.py < y0 + h) { feSel = i; choose = 1; }
					}
				}
				if (down & KEY_B) { feState = (feState == 1) ? 0 : 1; feSel = 0; }
				else if (choose) {
					if (feState == 1) {
						if (feSel == 0) { feState = 2; feSel = 0; } else if (feSel == 1) { feState = 3; feSel = 0; } else feState = 0;
					} else if (feState == 2) {
						if (feSel == 0) { launch = M_FREE; gLastMenu = 2; } else { feState = 1; feSel = 0; }
					} else {
						if (feSel < 3) { launch = M_DM + feSel; gLastMenu = 3; } else { feState = 1; feSel = 1; }
					}
				}
			}
			held = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; // ignore game input on the menus
		}

		// a mode was chosen: black loading screen with its description, then 3-2-1
		if (launch >= 0) {
			int m = launch, quit = 0;
			launch = -1;
			int okk = runLoading(top, bottom, m, &quit);
			if (quit) break;
			if (okk) {
				title = 0; musicWant = 0; paused = 0; menuState = 0; sel = 0; overSel = 0;
				px = gPlX; pz = gPlZ; yaw = gPlYaw; pitch = (m == M_TP) ? 0.0f : 0.3f;
				face = yaw; phoneTimer = 0; pending = 0; hackFrames = 0; frame = 0; sim = 0; aimAmt = 0.0f; wheelAmt = 0.0f;
				wheelSel = curWeapon; wheelWas = 0; lastTouchX = lastTouchY = -1; pauseDim = 0.0f;
			}
			continue;
		}

		// game over / result screen: RETRY or MAIN MENU
		if (!title && gState == GS_OVER && gOverT > 30) {
			int choose = (down & KEY_A) ? 1 : 0;
			if (down & (KEY_DDOWN | KEY_DUP)) overSel = 1 - overSel;
			if (down & KEY_TOUCH) {
				for (int i = 0; i < 2; i++) {
					float y0, h;
					overRect(i, &y0, &h);
					if (tp.px >= 50 && tp.px <= 270 && tp.py >= y0 && tp.py < y0 + h) { overSel = i; choose = 1; }
				}
			}
			if (choose) {
				if (overSel == 0) launch = gMode;
				else { exitMode(); title = 1; feState = gLastMenu; feSel = 0; musicWant = 1; titleFade = 0.0f; wheelAmt = 0.0f; }
			}
		}
		if (!title && gState == GS_OVER) { held = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; down = 0; }

		// pause menu: SELECT toggles, D-Pad / touch select, A confirm, B resume
		if (!title && gState != GS_OVER && (down & KEY_SELECT)) {
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
					else { exitMode(); title = 1; feState = gLastMenu; feSel = 0; paused = 0; menuState = 0; musicWant = 1; titleFade = 0.0f; wheelAmt = 0.0f; } // MAIN MENU
				} else if (menuState == 1) {
					if (sel < 4) weather = sel;                  // SUNNY / RAINY / SUNSET / NIGHT
					else         { menuState = 0; sel = 1; }     // BACK
				} else if (menuState == 2) {
					if (sel == 0)      gFog = !gFog;
					else if (sel == 1) { if (texOK) gWin = !gWin; }
					else if (sel == 2) gFps = !gFps;
					else               { menuState = 0; sel = 2; } // BACK
				} else {
					if (sel < 3) { if (gMode != M_TP) camStyle = sel; } // DEFAULT / WATCH DOGS / FIRST PERSON (the range is first person only)
					else         { menuState = 0; sel = 3; }     // BACK
				}
			}
			held = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; // game input is frozen while paused
		}
		if (!title && !paused && gMode == M_FREE && gShop >= 0 && gTrans == 0) { // a shop is open: the menu takes all the input
			updateShop(down, &tp);
			held = 0; down = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0;
		}
		if (!title && gTrans > 0) { held = 0; down = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; } // the screen is fading
		pauseDim += ((paused ? 1.0f : 0.0f) - pauseDim) * 0.25f;
		if (pauseDim < 0.01f) pauseDim = 0.0f;
		updateWeather();
		updateAudio(!title, paused, px, pz);
		if (title) titleFade += (1.0f - titleFade) * 0.04f;

		if (!title && !paused && gMode != M_TP && (down & KEY_Y)) camStyle = (camStyle + 1) % 3; // quick camera switch

		// weapon wheel: hold X, pick with the Circle Pad or by touch, release to equip
		int wheelOpen = (!title && !paused && gState == GS_PLAY && gMode != M_TP && (held & KEY_X)) ? 1 : 0;
		if (wheelOpen && !wheelWas) wheelSel = curWeapon;
		if (!wheelOpen && wheelWas) {
			if (wheelSel == WP_COUNT) useMedkit();
			else if (wheelSel != curWeapon && ownedW[wheelSel]) { curWeapon = wheelSel; fireCool = 12; playSfx(SFX_SWITCH, 0.6f); }
		}
		wheelWas = wheelOpen;
		wheelAmt += ((wheelOpen ? 1.0f : 0.0f) - wheelAmt) * 0.3f;
		if (wheelAmt < 0.01f) wheelAmt = 0.0f;
		if (wheelOpen) {
			float mx = (float)cp.dx, my = -(float)cp.dy; // screen coordinates, y down
			if (mx * mx + my * my > 55.0f * 55.0f) {
				float bd = -1e9f;
				for (int i = 0; i < wheelN(); i++) {
					if (!wheelUsable(i) && i != curWeapon) continue;
					float a = (-90.0f + 360.0f / (float)wheelN() * (float)i) * 0.0174533f, dd = mx * cosf(a) + my * sinf(a);
					if (dd > bd) { bd = dd; wheelSel = i; }
				}
			}
			if (down & KEY_TOUCH) {
				for (int i = 0; i < wheelN(); i++) {
					float x, y;
					wheelTile(i, &x, &y);
					if (wheelUsable(i) && fabsf((float)tp.px - x) < WH_W * 0.5f && fabsf((float)tp.py - y) < WH_H * 0.5f) wheelSel = i;
				}
			}
			cp.dx = 0; cp.dy = 0; // the stick picks a weapon instead of moving the character
		}

		// aiming: hold L / ZL, or just fire - the camera goes over the shoulder and the crosshair appears
		int armed = (curWeapon != WP_FISTS) && ownedW[curWeapon];
		int wantAim = (!title && !paused && !wheelOpen && armed && (held & (KEY_L | KEY_ZL | KEY_R | KEY_ZR))) ? 1 : 0;
		aimAmt += ((wantAim ? 1.0f : 0.0f) - aimAmt) * 0.22f;
		if (aimAmt < 0.01f) aimAmt = 0.0f;
		float lookScale = 1.0f - 0.45f * aimAmt;

		if (held & KEY_DLEFT)  yaw += 0.04f * lookScale;
		if (held & KEY_DRIGHT) yaw -= 0.04f * lookScale;
		if (held & KEY_DUP)    pitch -= 0.03f * lookScale;
		if (held & KEY_DDOWN)  pitch += 0.03f * lookScale;
		if (gMode == M_TP && !title && !wheelOpen) { // no walking at the range: the Circle Pad looks around too
			if (abs(cp.dx) > 18) yaw   -= cp.dx / 156.0f * 0.05f * lookScale;
			if (abs(cp.dy) > 18) pitch -= cp.dy / 156.0f * 0.04f * lookScale;
		}
		if (abs(cs.dx) > 15) yaw   -= cs.dx / 156.0f * 0.05f * lookScale;
		if (abs(cs.dy) > 15) pitch -= cs.dy / 156.0f * 0.04f * lookScale;

		// touch: bottom strip = HACK button, elsewhere = drag to look
		if ((held & KEY_TOUCH) && !wheelOpen) {
				if (tp.py >= BTN_Y0 && gMode != M_TP) {
					// the strip is the hack buttons (not inside a building, where the bottom screen shows the floor plan)
					if (gRoom < 0 && (down & KEY_TOUCH) && phoneTimer == 0 && tp.py < BTN_Y1) {
						pending = (tp.px < 160) ? 1 : 2; // 1 = hack lights, 2 = blackout
						phoneTimer = 120;                // character pulls out the phone
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
		if (gMode == M_TP && !title) { pMin = -0.45f; pMax = 0.45f; if (yaw > 1.0f) yaw = 1.0f; if (yaw < -1.0f) yaw = -1.0f; }
		if (pitch > pMax) pitch = pMax;
		if (pitch < pMin) pitch = pMin;

		float fx = (cp.dy / 156.0f), sx = (cp.dx / 156.0f);
		if (fabsf(fx) < 0.1f) fx = 0;
		if (fabsf(sx) < 0.1f) sx = 0;
		float len = sqrtf(fx * fx + sx * sx);
		if (len > 1.0f) { fx /= len; sx /= len; len = 1.0f; }
		if (phoneTimer > 0 || (!title && (gState != GS_PLAY || gMode == M_TP))) { fx = 0.0f; sx = 0.0f; len = 0.0f; } // standing still: on the phone, counting down, game over, or at the range
		int aimFace = (aimAmt > 0.5f) || (camStyle == 2);        // aiming / first person: body follows the camera
		int running = (held & KEY_B) && len > 0.5f && aimAmt < 0.3f;
		float speed = (running ? 0.17f : (aimAmt > 0.3f ? 0.045f : 0.07f)) * (gBoost > 0 ? 1.3f : 1.0f);
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
				if (!title) updateMode(px, pz, len > 0.0f);
			}
			updateTrain();
			if (!title) {
				int canAct = (gState == GS_PLAY && phoneTimer == 0 && !wheelOpen && gShop < 0 && aimAmt < 0.3f);
				updateWorld(px, pz, down, canAct, sim);
				if (gTraffic && gRoom < 0) updateWildlife(px, pz);
				if (gTele) { px = gTeleX; pz = gTeleZ; yaw = gTeleYaw; face = gTeleYaw; pitch = 0.3f; moveAmt = 0.0f; gTele = 0; }
			}
			sim++;
		}

		// ---- camera ----
		static const float baseDist[3] = {6.5f, 3.9f, 0.0f}, aimDist[3] = {2.8f, 2.4f, 0.0f};
		static const float baseSh[3]   = {0.0f, 0.75f, 0.0f}, aimSh[3]   = {0.8f, 0.95f, 0.0f};
		static const float baseH[3]    = {1.6f, 1.9f, 1.65f},  aimH[3]    = {1.75f, 1.85f, 1.65f};
		float cyaw = yaw, cpitch = pitch - kick;
		if (gMode == M_TP && !title) { // a slight hand tremor: steadier while holding L
			float k = 1.0f - 0.5f * aimAmt;
			cyaw   += (sinf((float)sim * 0.021f) * 0.0042f + sinf((float)sim * 0.057f) * 0.0020f) * k;
			cpitch += (sinf((float)sim * 0.027f + 1.0f) * 0.0038f + sinf((float)sim * 0.061f) * 0.0018f) * k;
		}
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
			{ // never through a ceiling: the freeway deck, a tunnel roof, the ceiling of a shop
				float ch = (gRoom >= 0) ? rooms[gRoom].h - 0.35f : ceilingAt(cx, cz) - 0.3f;
				if (cy > ch) cy = ch;
			}
		}
		if (title) { // slow cinematic orbit around the city
			orbit += 0.006f;
			cyaw = orbit; cpitch = 0.55f; cy = 190.0f;
			cx = sinf(orbit) * 330.0f; cz = cosf(orbit) * 330.0f;
		}
		if (shake > 0.01f) { cx += rnd() * shake * 0.15f; cy += rnd() * shake * 0.15f; cz += rnd() * shake * 0.15f; }

		float fov = ((camStyle == 2) ? 72.0f : 65.0f) - 16.0f * aimAmt;
		Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(fov), C3D_AspectRatioTop, 0.2f, title ? 800.0f : 400.0f, false);
		C3D_Mtx mv;
		Mtx_Identity(&mv);
		Mtx_RotateX(&mv, cpitch, true);
		Mtx_RotateY(&mv, -cyaw, true);
		Mtx_Translate(&mv, -cx, -cy, -cz, true);

		gCamX = cx; gCamZ = cz; gCamYaw = cyaw;
		gCullR = title ? 700.0f : 140.0f;
		gFwdX = -sinf(cyaw); gFwdZ = -cosf(cyaw);
		gCone = !title;
		unsigned char vis[CELLS]; // which districts the camera can see (the title orbit sees them all)
		computeVis(cx, cz, gFwdX, gFwdZ, title, vis);

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
			if (gRoom >= 0 && armed && (down & (KEY_R | KEY_ZR))) toast("NO WEAPONS INDOORS");
			if (gState == GS_PLAY && gRoom < 0 && !wheelOpen && phoneTimer == 0 && armed && (held & (KEY_R | KEY_ZR)) && fireCool == 0) {
				const WDef* w = &wdef[curWeapon];
				int fresh = (down & (KEY_R | KEY_ZR)) ? 1 : 0;
				if (gMode == M_TP) {
					if (fresh && tpTurn == TP_PL && tpPhase == 1 && tpShotsLeft > 0) { fireCool = 24; tpPlayerShot(cam, fwd, muz); }
				} else if (w->autoFire || fresh) {
					if (ammo[curWeapon] > 0) { ammo[curWeapon]--; fireCool = w->cooldown; fireWeapon(cam, fwd, muz); }
					else if (fresh) { playSfx(SFX_CLICK, 0.6f); fireCool = 10; }
				}
			}
			updateRocket();
			if (gCratesOn) updateCrates(px, pz);
			updateFx();
		}

		// ---- build this frame's dynamic geometry ----
		drawDynamic();
		{ int wst = vcount; drawWorldDyn(sim); GSPGPU_FlushDataCache(&vbo[wst], (vcount - wst) * sizeof(Vertex)); }
		if (!title && gMode == M_TP) drawTpRoom(sim);
		else if (!title && gMode != M_FREE) drawBots();
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
		drawWorldLit(sim, wNight);
		if (gCratesOn) drawCrates(sim);
		drawLoot(sim);
		drawTracers();
		GSPGPU_FlushDataCache(&vbo[litStart], (vcount - litStart) * sizeof(Vertex));
		int skyStart = vcount;            // blended: sun / moon / stars, rain, muzzle flashes, smoke, explosions
		const int inRoom = (!title && gRoom >= 0);
		if (!inRoom) { drawSkyBodies(cx, cy, cz, cyaw); drawRain(cx, cz, cyaw); }
		drawStorm();
		drawFx(cyaw);
		int dropStart = vcount;           // lens droplets + HUD (pixel coordinates, own pass)
		if (!inRoom) drawDroplets();
		float fadeA = title ? 1.0f - titleFade : 0.0f;
		if (gTrans > 0) { float u = fabsf((float)gTrans - 13.0f) / 13.0f; if (1.0f - u > fadeA) fadeA = 1.0f - u; }
		drawHud2D(aimAmt, !title && !paused && armed, fadeA);
		GSPGPU_FlushDataCache(&vbo[skyStart], (vcount - skyStart) * sizeof(Vertex));

		float t = (hackFrames > 0 && !inRoom) ? 0.25f : 1.0f;
		float fogC[3] = { wSky[0], wSky[1], wSky[2] };
		if (hackFrames > 0) { fogC[0] = 0.03f; fogC[1] = 0.03f; fogC[2] = 0.06f; }
		float fS = title ? wFogS + 260.0f : wFogS, fE = title ? wFogE + 420.0f : wFogE; // the title orbit is far above the city
		float fogK = 1.0f / (fE - fS);
		float fogA = (gFog && !inRoom) ? -fogK : 0.0f, fogB = (gFog && !inRoom) ? -fS * fogK : 0.0f;
		float glow = wNight + 0.35f * wDusk + 0.12f * wRain;
		if (glow > 1.0f) glow = 1.0f;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, inRoom ? 0x14141CFF : (hackFrames > 0 ? 0x080810FF : skyColor(1.0f)), 0);
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
		if (inRoom) C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, 1.0f, 1.0f, 1.0f, 1.0f);
		else        C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, wTint[0] * t, wTint[1] * t, wTint[2] * t, 1.0f);
		envPlain();
		if (inRoom) drawRange(rooms[gRoom].start, rooms[gRoom].count);   // the inside of a shop
		if ((title || gMode != M_TP) && !inRoom) { // the shooting range is far from the city: skip the city entirely
			drawRange(0, globalEnd);                         // ground + skyline
			drawCells(cellAStart, cellACount, vis);          // roofs, road markings, crossings, signal poles
			if (gWin) { envTextured(); C3D_TexBind(0, &texDay); }
			drawCells(cellWStart, cellWCount, vis);          // textured facades
		}
		envPlain();
		drawRange(staticCount, litStart - staticCount);  // shadows, cars, pedestrians, player

		// 2) emissive lights stay bright at night (still fogged)
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, t, t, t, 1.0f);
		drawRange(litStart, skyStart - litStart);

		// 3) lit windows: the night texture added on top of the facades
		if (gWin && glow > 0.03f && (title || gMode != M_TP) && !inRoom) {
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

		// top screen HUD (health, kills, countdown ...) drawn with citro2d over the 3D view
		C2D_Prepare();
		C2D_TextBufClear(gDyn);
		C2D_SceneBegin(top);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		gHudQuiet = paused;
		if (!title) drawTopHud(frame, cyaw);
		C2D_Flush();

		// bottom screen: street map (or the menus)
		C2D_Prepare();
		C2D_TextBufClear(gDyn);
		C2D_TargetClear(bottom, C2D_Color32(0x12, 0x15, 0x1d, 0xFF));
		C2D_SceneBegin(bottom);
		if (title) { if (feState == 0) drawTitleBottom(frame); else drawFrontEnd(feState, feSel, frame); }
		else {
			if (gMode == M_TP) drawTpBottom(frame);
			else if (gShop >= 0) drawShopBottom(frame);
			else if (gRoom >= 0) drawRoomBottom(px, pz, face, frame);
			else {
				drawMap(px, pz, frame, phoneTimer, face);
				drawMapMode(px, pz);
				drawWheel(wheelAmt, wheelSel, curWeapon, frame);
			}
			drawPause(pauseDim, menuState, sel, menuState == 3 ? camStyle : weather, frame);
			if (gState == GS_OVER) drawOverPanel(frame, overSel);
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
