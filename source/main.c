// chicago3ds - low-poly open-city sandbox + story mission for Nintendo 3DS (original assets, original code)
// Top screen: the 3D world. Bottom screen: live map / mission objective, weapon wheel, pause menu.
// TITLE MENU: STORY MODE (3 save files, type your name, mission 1 in a factory) or FREE ROAM (the open city).
//
// CONTROLS
//   Circle Pad  move              B (hold)  run             C-Stick / D-Pad Up,Down / touch-drag  look
//   L or ZL     aim (hold)        R or ZR   fire / punch    X  reload        D-Pad Left/Right  switch weapon
//   Y (hold)    weapon wheel      START     pause menu      (SETTINGS inside the pause menu: camera style, volume)
//   Touch the HACK LIGHTS / BLACKOUT buttons on the map to pull out the phone.
//
// Needs romfs assets (music + sound effects, packed into the .3dsx by the Makefile) and, for sound,
// the DSP firmware dump (sdmc:/3ds/dspfirm.cdc). Without them the game still runs, just silently,
// and the loading screen tells you which piece is missing.

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>


#include "vshader_shbin.h"

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define NRX        6        // streets running north-south (their x positions are in RX[])
#define NRZ        6        // streets running east-west (their z positions are in RZ[])
#define ROAD_HALF  8.0f     // streets are 16 wide: four lanes
#define WALK_W     4.0f     // sidewalk width beside every street
#define INSET      (ROAD_HALF + WALK_W)
#define MAX_VERTS  90000
#define MAX_SOLID  560
#define MAX_WL     64
#define RAIN_N     140
#define N_DROPS    11
#define N_CARS     32
#define N_NPC      18
#define N_FOE      24
#define PLAYER_R   0.6f
enum { LV_CITY = 0, LV_FACTORY = 1 };
static int   gLevel = LV_CITY;         // which world is built into the vertex buffer
static float gFootY = 0.0f;            // floor height under the player (the factory has many floors)
static int   nCarOn = 32, nNpcOn = 18; // cars / pedestrians that exist in the current world (none in the factory)
#define EYE_H      1.7f

typedef struct { float pos[3]; float clr[4]; } Vertex;

static Vertex*  vbo;
static int      vcount = 0;

static unsigned hash2(int a, int b) {
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

// small xorshift random generator, 0..1 (gameplay)
static unsigned rngS = 2463534242u;
static float frand(void) {
	rngS ^= rngS << 13; rngS ^= rngS >> 17; rngS ^= rngS << 5;
	return (float)(rngS & 0xFFFFFF) / 16777216.0f;
}
static float frange(float a, float b) { return a + (b - a) * frand(); }

// the city is generated from its own generator so it is always the same city
static unsigned cityS = 0x9E3779B9u;
static float crand(void) {
	cityS ^= cityS << 13; cityS ^= cityS >> 17; cityS ^= cityS << 5;
	return (float)(cityS & 0xFFFFFF) / 16777216.0f;
}
static float cr(float a, float b) { return a + (b - a) * crand(); }

// sound effects / loops / music, loaded from romfs:/<name>.raw (signed 16-bit PCM, 22050 Hz)
enum {
	S_PISTOL, S_SMG, S_SHOTGUN, S_ROCKET, S_EXPLODE, S_RELOAD, S_CLICK, S_HIT, S_THUD, S_HORN, S_SIREN,
	S_UI_MOVE, S_UI_OK, S_SWITCH, S_AMB_CITY, S_AMB_RAIN, S_AMB_NIGHT, S_MUSIC,
	// story mode: voice acting + factory ambience (made by tools/make_voices.py)
	S_V_GM1, S_V_GM2, S_V_HERO_THROW, S_V_FEM_HALL, S_V_GM_HALL, S_V_HERO_PLAYED, S_V_VINCE1, S_V_VINCE2,
	S_V_BARK1, S_V_BARK2, S_V_BARK3, S_AMB_FACTORY, S_COUNT
};
enum { WP_FISTS = 0, WP_PISTOL, WP_SMG, WP_SHOTGUN, WP_ROCKET, N_WP };
enum { M_BOOT = 0, M_TITLE = 1, M_GAME = 2 };
enum { CAM_DEFAULT = 0, CAM_SHOULDER = 1, CAM_FIRST = 2, N_CAM = 3 };

static float gAlpha = 1.0f; // alpha written into new vertices (rain, sky bodies, droplets)

static void addV(float x, float y, float z, float r, float g, float b) {
	if (vcount >= MAX_VERTS) return;
	Vertex* v = &vbo[vcount++];
	v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
	v->clr[0] = r; v->clr[1] = g; v->clr[2] = b; v->clr[3] = gAlpha;
}

static void quad(const float a[3], const float b[3], const float c[3], const float d[3],
                 float r, float g, float bl) {
	addV(a[0], a[1], a[2], r, g, bl); addV(b[0], b[1], b[2], r, g, bl); addV(c[0], c[1], c[2], r, g, bl);
	addV(a[0], a[1], a[2], r, g, bl); addV(c[0], c[1], c[2], r, g, bl); addV(d[0], d[1], d[2], r, g, bl);
}

static void addBoxY(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) {
	float p000[3] = {x0, y0, z0}, p100[3] = {x1, y0, z0}, p110[3] = {x1, y1, z0}, p010[3] = {x0, y1, z0};
	float p001[3] = {x0, y0, z1}, p101[3] = {x1, y0, z1}, p111[3] = {x1, y1, z1}, p011[3] = {x0, y1, z1};
	quad(p000, p100, p110, p010, r * 0.65f, g * 0.65f, b * 0.65f); // -Z face
	quad(p101, p001, p011, p111, r * 0.65f, g * 0.65f, b * 0.65f); // +Z face
	quad(p001, p000, p010, p011, r * 0.80f, g * 0.80f, b * 0.80f); // -X face
	quad(p100, p101, p111, p110, r * 0.80f, g * 0.80f, b * 0.80f); // +X face
	quad(p010, p110, p111, p011, r, g, b);                         // roof
}

static void addBox(float x0, float z0, float x1, float z1, float h, float r, float g, float b) {
	addBoxY(x0, z0, x1, z1, 0.0f, h, r, g, b);
}

// flat rectangle lying on the ground at height y
static void flat(float x0, float z0, float x1, float z1, float y, float r, float g, float b) {
	float a[3] = {x0, y, z0}, c[3] = {x1, y, z0}, d[3] = {x1, y, z1}, e[3] = {x0, y, z1};
	quad(a, c, d, e, r, g, b);
}

// flat rectangle whose long axis is `axis` (0 = along x, 1 = along z): a0..a1 along it, l0..l1 across it
static void flatAx(int axis, float a0, float a1, float l0, float l1, float y, float r, float g, float b) {
	if (axis == 0) flat(a0, l0, a1, l1, y, r, g, b); else flat(l0, a0, l1, a1, y, r, g, b);
}

// ---- the city -------------------------------------------------------------------
// Six streets each way, 16 wide, spaced unevenly. The blocks between them are different kinds of place
// (houses, mid-rise, towers with plazas, parks, squares, a parking lot) with lots of open space.
static const float RX[NRX] = { -165.0f, -93.0f, -25.0f, 48.0f, 118.0f, 185.0f };
static const float RZ[NRZ] = { -160.0f, -90.0f, -28.0f, 44.0f, 116.0f, 180.0f };
static float gXmin, gXmax, gZmin, gZmax;     // walkable area, inside the perimeter wall
static float gStartX, gStartZ;               // where the player starts

enum { B_LOW, B_MID, B_TOWERS, B_PARK, B_PLAZA, B_LOT };
static const unsigned char blockMap[NRZ - 1][NRX - 1] = {   // [row north->south][column west->east]
	{ B_LOW,  B_MID,  B_PARK,   B_LOW,    B_MID    },
	{ B_MID,  B_LOW,  B_MID,    B_PLAZA,  B_TOWERS },
	{ B_PARK, B_MID,  B_PLAZA,  B_TOWERS, B_MID    },
	{ B_LOW,  B_LOT,  B_TOWERS, B_TOWERS, B_PARK   },
	{ B_MID,  B_LOW,  B_MID,    B_PLAZA,  B_LOW    },
};

typedef struct { float x0, z0, x1, z1; int kind; } Blk;       // the open area of a block (inside its sidewalks)
static Blk blk[NRZ - 1][NRX - 1];

enum { SK_BLDG = 0, SK_TREE, SK_PROP, SK_WALL, SK_FLOOR };
typedef struct { float x0, z0, x1, z1, y0, y1; unsigned char kind, win; } Solid;
static Solid solids[MAX_SOLID];
static int   nSolid = 0;

// lines pedestrians stroll along: sidewalks beside every street, plus paths through parks and squares
typedef struct { int axis; float lat, lo, hi; } WLine;
static WLine wl[MAX_WL];
static int   nWl = 0, nWlSide = 0;

static int worldStaticEnd = 0, staticCount = 0;

static Solid* addSolid(float x0, float z0, float x1, float z1, float y0, float y1, int kind, int win) {
	if (nSolid >= MAX_SOLID) return NULL;
	Solid* s = &solids[nSolid++];
	s->x0 = x0; s->z0 = z0; s->x1 = x1; s->z1 = z1; s->y0 = y0; s->y1 = y1;
	s->kind = (unsigned char)kind; s->win = (unsigned char)win;
	return s;
}

static void addWLine(int axis, float lat, float lo, float hi) {
	if (nWl >= MAX_WL) return;
	wl[nWl].axis = axis; wl[nWl].lat = lat; wl[nWl].lo = lo; wl[nWl].hi = hi;
	nWl++;
}

// vertical rectangle on a wall. axis 0: the wall is at z = fix and spans x; axis 1: at x = fix, spans z
static void wallQuad(int axis, float fix, float a0, float a1, float y0, float y1, float r, float g, float b) {
	float p0[3], p1[3], p2[3], p3[3];
	if (axis == 0) {
		p0[0] = a0; p0[1] = y0; p0[2] = fix; p1[0] = a1; p1[1] = y0; p1[2] = fix;
		p2[0] = a1; p2[1] = y1; p2[2] = fix; p3[0] = a0; p3[1] = y1; p3[2] = fix;
	} else {
		p0[0] = fix; p0[1] = y0; p0[2] = a0; p1[0] = fix; p1[1] = y0; p1[2] = a1;
		p2[0] = fix; p2[1] = y1; p2[2] = a1; p3[0] = fix; p3[1] = y1; p3[2] = a0;
	}
	quad(p0, p1, p2, p3, r, g, b);
}

// Windows. Pass 0 draws the dark glass, pass 1 draws only the lit ones (they live in the emissive part
// of the buffer, so at night they glow). Which windows are lit is decided by a hash, so both passes agree.
static void facadeWindows(const Solid* s, int idx, int pass) {
	float h = s->y1;
	for (int f = 0; f < 4; f++) {
		int   axis = (f < 2) ? 0 : 1;
		float fix  = (f == 0) ? s->z0 - 0.05f : (f == 1) ? s->z1 + 0.05f : (f == 2) ? s->x0 - 0.05f : s->x1 + 0.05f;
		float a0   = (axis == 0) ? s->x0 : s->z0, a1 = (axis == 0) ? s->x1 : s->z1;
		float W    = a1 - a0;
		if (W < 4.0f) continue;
		if (s->win == 2) { // ribbon windows for tall towers: one dark band per floor, broken into segments
			int   m   = (int)(W / 7.5f + 0.5f);
			if (m < 1) m = 1;
			float seg = W / (float)m;
			int   row = 0;
			for (float y = 6.2f; y + 2.3f < h - 1.0f; y += 4.0f, row++) {
				for (int i = 0; i < m; i++) {
					int lit = (hash2(idx * 53 + f * 11 + i, row * 7 + 3) % 100) < 30;
					if (lit != pass) continue;
					wallQuad(axis, fix, a0 + seg * i + 0.7f, a0 + seg * (i + 1) - 0.7f, y, y + 2.3f,
					         pass ? 1.00f : 0.20f, pass ? 0.82f : 0.28f, pass ? 0.45f : 0.38f);
				}
			}
		} else {           // individual windows: style 1 = mid-rise, style 3 = house
			float y0 = (s->win == 3) ? 1.4f : 2.6f, pitch = (s->win == 3) ? 3.0f : 3.6f, wh = (s->win == 3) ? 1.7f : 2.1f;
			int   n  = (int)((W - 1.6f) / 3.4f);
			if (n < 1) n = 1;
			float step = W / (float)n;
			int   row = 0;
			for (float y = y0; y + wh < h - 0.8f; y += pitch, row++) {
				for (int c = 0; c < n; c++) {
					int lit = (hash2(idx * 131 + f * 17 + c, row * 13 + 5) % 100) < 30;
					if (lit != pass) continue;
					float cx = a0 + step * ((float)c + 0.5f);
					wallQuad(axis, fix, cx - 0.95f, cx + 0.95f, y, y + wh,
					         pass ? 1.00f : 0.20f, pass ? 0.82f : 0.28f, pass ? 0.45f : 0.38f);
				}
			}
		}
	}
}

static void bldgColor(int kind, float* r, float* g, float* b) {
	static const float pal[8][3] = {
		{0.66f, 0.38f, 0.30f}, {0.74f, 0.52f, 0.36f}, {0.78f, 0.74f, 0.64f}, {0.58f, 0.60f, 0.64f},
		{0.40f, 0.50f, 0.60f}, {0.30f, 0.48f, 0.52f}, {0.32f, 0.34f, 0.40f}, {0.82f, 0.80f, 0.76f}
	};
	static const int lo[3] = {0, 0, 3}, cnt[3] = {4, 5, 5}; // mid-rise brick/stone, towers glass/steel
	int i = lo[kind] + (int)(crand() * (float)cnt[kind]) % cnt[kind];
	*r = pal[i][0] * cr(0.92f, 1.06f); *g = pal[i][1] * cr(0.92f, 1.06f); *b = pal[i][2] * cr(0.92f, 1.06f);
}

static void roofClutter(float x0, float z0, float x1, float z1, float h) {
	int n = 1 + (int)(crand() * 2.0f);
	float mid = (x0 + x1) * 0.5f;
	for (int i = 0; i < n; i++) {
		float w = cr(1.8f, 3.0f);
		float lo = (i == 0) ? x0 + 1.0f : mid + 0.5f, hi = (i == 0) ? mid - 0.5f - w : x1 - 1.0f - w;
		if (hi <= lo) continue;
		float x = cr(lo, hi), z = cr(z0 + 1.0f, z1 - 1.0f - w);
		addBoxY(x, z, x + w, z + w, h, h + 1.0f + 0.4f * (float)i + cr(0.0f, 0.5f), 0.42f, 0.43f, 0.45f);
	}
}

static void addBuilding(float x0, float z0, float x1, float z1, float h, float r, float g, float b) {
	addBoxY(x0, z0, x1, z1, 0.0f, h, r, g, b);
	addSolid(x0, z0, x1, z1, 0.0f, h, SK_BLDG, 1);
	if (h > 9.0f) roofClutter(x0, z0, x1, z1, h);
	// lighter cornice
	addBoxY(x0 - 0.25f, z0 - 0.25f, x1 + 0.25f, z1 + 0.25f, h - 0.5f, h + 0.06f, r * 1.12f, g * 1.12f, b * 1.12f);
}

static void addTower(float x0, float z0, float x1, float z1, float h, float r, float g, float b) {
	const float pod = 5.5f, in = 1.6f;
	addBoxY(x0, z0, x1, z1, 0.0f, pod, r * 1.08f, g * 1.08f, b * 1.08f);
	addSolid(x0, z0, x1, z1, 0.0f, pod, SK_BLDG, 1);
	addBoxY(x0 + in, z0 + in, x1 - in, z1 - in, pod, h, r, g, b);
	addSolid(x0 + in, z0 + in, x1 - in, z1 - in, pod, h, SK_BLDG, 2);
	if (h > 55.0f) addBoxY(x0 + in + 1.8f, z0 + in + 1.8f, x1 - in - 1.8f, z1 - in - 1.8f, h, h + 3.5f, r * 0.7f, g * 0.7f, b * 0.7f);
	float cx = (x0 + x1) * 0.5f, cz = (z0 + z1) * 0.5f;
	addBoxY(cx - 0.18f, cz - 0.18f, cx + 0.18f, cz + 0.18f, h + (h > 55.0f ? 3.5f : 0.0f),
	        h + (h > 55.0f ? 3.5f : 0.0f) + 9.0f, 0.55f, 0.55f, 0.58f);
}

// trunk and canopy. solidMode: 0 = decoration only, 1 = trunk blocks you, 2 = trunk and canopy block you / bullets
static void addTree(float x, float z, float size, int solidMode) {
	float gr = cr(0.20f, 0.32f), gg = cr(0.46f, 0.60f), gb = cr(0.18f, 0.28f);
	if (crand() < 0.10f) { gr = cr(0.70f, 0.85f); gg = cr(0.45f, 0.60f); gb = 0.15f; } // the odd autumn tree
	float s2 = size * 0.5f;
	addBoxY(x - 0.28f, z - 0.28f, x + 0.28f, z + 0.28f, 0.0f, 2.5f, 0.38f, 0.26f, 0.16f);
	addBoxY(x - s2, z - s2, x + s2, z + s2, 2.3f, 2.3f + size * 0.95f, gr, gg, gb);
	addBoxY(x - s2 * 0.62f, z - s2 * 0.62f, x + s2 * 0.62f, z + s2 * 0.62f,
	        2.3f + size * 0.95f, 2.3f + size * 1.45f, gr * 1.1f, gg * 1.1f, gb * 1.1f);
	if (solidMode >= 1) addSolid(x - 0.35f, z - 0.35f, x + 0.35f, z + 0.35f, 0.0f, 2.5f, SK_TREE, 0);
	if (solidMode >= 2) addSolid(x - s2, z - s2, x + s2, z + s2, 2.3f, 2.3f + size * 0.95f, SK_TREE, 0);
}

static void addBench(float x, float z, int alongX) {
	float hx = alongX ? 1.0f : 0.3f, hz = alongX ? 0.3f : 1.0f;
	addBoxY(x - hx, z - hz, x + hx, z + hz, 0.40f, 0.55f, 0.50f, 0.34f, 0.20f);
	if (alongX) addBoxY(x - hx, z - hz, x + hx, z - hz + 0.1f, 0.55f, 1.05f, 0.45f, 0.30f, 0.18f);
	else        addBoxY(x - hx, z - hz, x - hx + 0.1f, z + hz, 0.55f, 1.05f, 0.45f, 0.30f, 0.18f);
	addBoxY(x - hx, z - hz, x - hx + 0.12f, z - hz + 0.12f, 0.0f, 0.40f, 0.2f, 0.2f, 0.22f);
	addBoxY(x + hx - 0.12f, z + hz - 0.12f, x + hx, z + hz, 0.0f, 0.40f, 0.2f, 0.2f, 0.22f);
	addSolid(x - hx, z - hz, x + hx, z + hz, 0.0f, 1.0f, SK_PROP, 0);
}

static void parkedCar(float cx, float cz, float r, float g, float b) { // static, pointing along z
	addBoxY(cx - 0.95f, cz - 2.0f, cx + 0.95f, cz + 2.0f, 0.12f, 0.55f, 0.12f, 0.12f, 0.14f);
	addBoxY(cx - 1.0f, cz - 2.1f, cx + 1.0f, cz + 2.1f, 0.55f, 1.15f, r, g, b);
	addBoxY(cx - 0.85f, cz - 1.0f, cx + 0.85f, cz + 1.1f, 1.15f, 1.7f, 0.15f, 0.20f, 0.27f);
	addBoxY(cx - 0.88f, cz - 1.0f, cx + 0.88f, cz + 1.1f, 1.7f, 1.78f, r * 0.9f, g * 0.9f, b * 0.9f);
	addSolid(cx - 1.0f, cz - 2.1f, cx + 1.0f, cz + 2.1f, 0.0f, 1.78f, SK_PROP, 0);
}

static void addHouse(float x0, float z0, float x1, float z1, float h, float r, float g, float b, int doorSouth) {
	addBoxY(x0, z0, x1, z1, 0.0f, h, r, g, b);
	addSolid(x0, z0, x1, z1, 0.0f, h, SK_BLDG, 3);
	static const float roof[4][3] = {{0.45f, 0.20f, 0.18f}, {0.30f, 0.30f, 0.34f}, {0.36f, 0.26f, 0.20f}, {0.22f, 0.30f, 0.38f}};
	const float* rf = roof[(int)(crand() * 4.0f) & 3];
	addBoxY(x0 - 0.5f, z0 - 0.5f, x1 + 0.5f, z1 + 0.5f, h, h + 0.45f, rf[0], rf[1], rf[2]);
	addBoxY(x0 + 1.6f, z0 + 0.8f, x1 - 1.6f, z1 - 0.8f, h + 0.45f, h + 1.7f, rf[0] * 0.9f, rf[1] * 0.9f, rf[2] * 0.9f);
	float cx = (x0 + x1) * 0.5f;
	wallQuad(0, doorSouth ? z1 + 0.06f : z0 - 0.06f, cx - 0.6f, cx + 0.6f, 0.08f, 2.1f, 0.40f, 0.26f, 0.16f);
}

typedef struct { float x0, z0, x1, z1; } Rect;

static void splitRect(Rect r, int alongX, float ratio, float gap, Rect* a, Rect* b) {
	if (alongX) {
		float m = r.x0 + (r.x1 - r.x0) * ratio;
		a->x0 = r.x0; a->z0 = r.z0; a->x1 = m - gap * 0.5f; a->z1 = r.z1;
		b->x0 = m + gap * 0.5f; b->z0 = r.z0; b->x1 = r.x1; b->z1 = r.z1;
	} else {
		float m = r.z0 + (r.z1 - r.z0) * ratio;
		a->x0 = r.x0; a->z0 = r.z0; a->x1 = r.x1; a->z1 = m - gap * 0.5f;
		b->x0 = r.x0; b->z0 = m + gap * 0.5f; b->x1 = r.x1; b->z1 = r.z1;
	}
}

// ---- block generators -------------------------------------------------------------
static void genLow(const Blk* B) {
	flat(B->x0, B->z0, B->x1, B->z1, 0.10f, 0.34f, 0.55f, 0.30f); // lawn
	int cols = (int)((B->x1 - B->x0) / 15.0f); if (cols < 2) cols = 2;
	int rows = (int)((B->z1 - B->z0) / 16.0f); if (rows < 2) rows = 2;
	float cw = (B->x1 - B->x0) / (float)cols, rh = (B->z1 - B->z0) / (float)rows;
	static const float sid[6][3] = {
		{0.85f, 0.80f, 0.68f}, {0.70f, 0.45f, 0.38f}, {0.62f, 0.70f, 0.74f},
		{0.80f, 0.72f, 0.50f}, {0.68f, 0.62f, 0.72f}, {0.55f, 0.66f, 0.55f}
	};
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			float cx = B->x0 + cw * ((float)c + 0.5f) + cr(-1.2f, 1.2f);
			float cz = B->z0 + rh * ((float)r + 0.5f) + cr(-1.5f, 1.5f);
			if (crand() < 0.14f) { addTree(cx, cz, cr(3.4f, 4.4f), 2); continue; }
			float w = cr(8.0f, 10.5f), d = cr(8.0f, 10.5f), h = cr(5.2f, 8.8f);
			const float* s = sid[(int)(crand() * 6.0f) % 6];
			addHouse(cx - w * 0.5f, cz - d * 0.5f, cx + w * 0.5f, cz + d * 0.5f, h, s[0], s[1], s[2], r >= rows / 2);
			if (crand() < 0.5f) addTree(cx + (crand() < 0.5f ? -1.0f : 1.0f) * (w * 0.5f + 2.6f), cz + cr(-2.0f, 2.0f), cr(2.8f, 3.6f), 1);
		}
	}
}

static void genMid(const Blk* B) {
	flat(B->x0, B->z0, B->x1, B->z1, 0.10f, 0.55f, 0.55f, 0.53f);
	Rect R = { B->x0 + 4.0f, B->z0 + 4.0f, B->x1 - 4.0f, B->z1 - 4.0f };
	int  alongX = (R.x1 - R.x0) >= (R.z1 - R.z0);
	Rect leaf[4]; int n = 0;
	Rect A, Bq;
	splitRect(R, alongX, cr(0.42f, 0.58f), cr(8.0f, 10.0f), &A, &Bq);
	if (crand() < 0.55f) { Rect a1, a2; splitRect(A, !alongX, cr(0.4f, 0.6f), cr(7.0f, 9.0f), &a1, &a2); leaf[n++] = a1; leaf[n++] = a2; }
	else leaf[n++] = A;
	if (crand() < 0.30f) { Rect b1, b2; splitRect(Bq, !alongX, cr(0.4f, 0.6f), cr(7.0f, 9.0f), &b1, &b2); leaf[n++] = b1; leaf[n++] = b2; }
	else leaf[n++] = Bq;
	for (int i = 0; i < n; i++) {
		float lw = leaf[i].x1 - leaf[i].x0, ld = leaf[i].z1 - leaf[i].z0;
		float bx0 = 0, bz0 = 0, bx1 = 0, bz1 = 0;
		int built = 0;
		if (lw >= 11.0f && ld >= 11.0f) {
			float w = fminf(lw, cr(12.0f, 20.0f)), d = fminf(ld, cr(12.0f, 20.0f));
			bx0 = leaf[i].x0 + crand() * (lw - w); bz0 = leaf[i].z0 + crand() * (ld - d);
			bx1 = bx0 + w; bz1 = bz0 + d;
			float r, g, b;
			bldgColor(1, &r, &g, &b);
			addBuilding(bx0, bz0, bx1, bz1, cr(14.0f, 30.0f), r, g, b);
			built = 1;
		}
		float lastX = -1e9f, lastZ = -1e9f;
		int planted = 0;
		for (int t = 0; t < 10 && planted < (built ? 2 : 1); t++) { // trees in the leftover space
			float x = cr(leaf[i].x0 + 1.5f, leaf[i].x1 - 1.5f), z = cr(leaf[i].z0 + 1.5f, leaf[i].z1 - 1.5f);
			if (built && x > bx0 - 2.5f && x < bx1 + 2.5f && z > bz0 - 2.5f && z < bz1 + 2.5f) continue;
			if (fabsf(x - lastX) < 5.5f && fabsf(z - lastZ) < 5.5f) continue;
			addTree(x, z, cr(3.0f, 4.0f), 2);
			lastX = x; lastZ = z; planted++;
		}
	}
}

static void genTowers(const Blk* B) {
	flat(B->x0, B->z0, B->x1, B->z1, 0.10f, 0.64f, 0.63f, 0.60f);
	flat(B->x0 + 2.0f, B->z0 + 2.0f, B->x1 - 2.0f, B->z1 - 2.0f, 0.14f, 0.70f, 0.69f, 0.66f);
	float W = B->x1 - B->x0;
	float tw1 = cr(12.0f, 14.0f), td1 = cr(12.0f, 14.0f), tw2 = cr(11.5f, 13.5f), td2 = cr(11.5f, 13.5f);
	if (W - 10.0f - tw1 - tw2 < 4.0f) { tw1 = tw2 = (W - 14.0f) * 0.5f; }
	int flip = crand() < 0.5f;
	float t1x = flip ? B->x1 - 5.0f - tw1 : B->x0 + 5.0f, t1z = B->z0 + 5.0f;
	float t2x = flip ? B->x0 + 5.0f : B->x1 - 5.0f - tw2,  t2z = B->z1 - 5.0f - td2;
	float r, g, b;
	bldgColor(2, &r, &g, &b);
	addTower(t1x, t1z, t1x + tw1, t1z + td1, cr(55.0f, 95.0f), r, g, b);
	bldgColor(2, &r, &g, &b);
	addTower(t2x, t2z, t2x + tw2, t2z + td2, cr(38.0f, 70.0f), r, g, b);
	// the two open corners get trees and a bench
	float ox = flip ? B->x0 + 8.0f : B->x1 - 8.0f, oz = B->z0 + 8.0f;
	float px = flip ? B->x1 - 8.0f : B->x0 + 8.0f, pz = B->z1 - 8.0f;
	static const float off[3][2] = {{0.0f, 0.0f}, {5.0f, 1.5f}, {1.5f, 5.0f}};
	for (int i = 0; i < 3; i++) {
		addTree(ox + (flip ? 1.0f : -1.0f) * off[i][0], oz + off[i][1], cr(3.2f, 4.2f), 2);
		addTree(px + (flip ? -1.0f : 1.0f) * off[i][0], pz - off[i][1], cr(3.2f, 4.2f), 2);
	}
	addBench((B->x0 + B->x1) * 0.5f, (B->z0 + B->z1) * 0.5f, 1);
}

static void genPark(const Blk* B) {
	float cx = (B->x0 + B->x1) * 0.5f, cz = (B->z0 + B->z1) * 0.5f, W = B->x1 - B->x0, D = B->z1 - B->z0;
	flat(B->x0, B->z0, B->x1, B->z1, 0.10f, 0.30f, 0.52f, 0.27f);
	flat(B->x0, cz - 1.8f, B->x1, cz + 1.8f, 0.14f, 0.74f, 0.68f, 0.54f);
	flat(cx - 1.8f, B->z0, cx + 1.8f, B->z1, 0.14f, 0.74f, 0.68f, 0.54f);
	flat(cx - 6.0f, cz - 6.0f, cx + 6.0f, cz + 6.0f, 0.18f, 0.70f, 0.64f, 0.52f);
	// pond in one quadrant, a small monument in the opposite one
	float sx = crand() < 0.5f ? -1.0f : 1.0f, sz = crand() < 0.5f ? -1.0f : 1.0f;
	float pxc = cx + sx * W * 0.26f, pzc = cz + sz * D * 0.26f;
	float mxc = cx - sx * W * 0.26f, mzc = cz - sz * D * 0.26f;
	flat(pxc - 7.0f, pzc - 5.5f, pxc + 7.0f, pzc + 5.5f, 0.14f, 0.56f, 0.56f, 0.56f);
	flat(pxc - 6.2f, pzc - 4.7f, pxc + 6.2f, pzc + 4.7f, 0.18f, 0.22f, 0.45f, 0.68f);
	addSolid(pxc - 6.2f, pzc - 4.7f, pxc + 6.2f, pzc + 4.7f, 0.0f, 0.3f, SK_PROP, 0);
	addBoxY(mxc - 1.0f, mzc - 1.0f, mxc + 1.0f, mzc + 1.0f, 0.0f, 0.5f, 0.6f, 0.6f, 0.62f);
	addBoxY(mxc - 0.45f, mzc - 0.45f, mxc + 0.45f, mzc + 0.45f, 0.5f, 3.2f, 0.72f, 0.72f, 0.74f);
	addSolid(mxc - 1.0f, mzc - 1.0f, mxc + 1.0f, mzc + 1.0f, 0.0f, 3.2f, SK_PROP, 0);
	// trees, kept off the paths and the pond
	float tx[16], tz[16]; int nt = 0;
	for (int t = 0; t < 60 && nt < 14; t++) {
		float x = cr(B->x0 + 2.5f, B->x1 - 2.5f), z = cr(B->z0 + 2.5f, B->z1 - 2.5f);
		if (fabsf(x - cx) < 4.8f || fabsf(z - cz) < 4.8f) continue;
		if (fabsf(x - pxc) < 9.0f && fabsf(z - pzc) < 7.5f) continue;
		if (fabsf(x - mxc) < 4.0f && fabsf(z - mzc) < 4.0f) continue;
		int ok = 1;
		for (int k = 0; k < nt; k++) if (fabsf(x - tx[k]) < 5.0f && fabsf(z - tz[k]) < 5.0f) ok = 0;
		if (!ok) continue;
		tx[nt] = x; tz[nt] = z; nt++;
		addTree(x, z, cr(3.4f, 4.8f), 2);
	}
	addBench(cx - 9.0f, cz - 3.2f, 1); addBench(cx + 9.0f, cz + 3.2f, 1);
	addBench(cx - 3.2f, cz + 9.0f, 0); addBench(cx + 3.2f, cz - 9.0f, 0);
	addWLine(0, cz, B->x0 + 2.0f, B->x1 - 2.0f);
	addWLine(1, cx, B->z0 + 2.0f, B->z1 - 2.0f);
}

static void genPlaza(const Blk* B, int big) {
	float cx = (B->x0 + B->x1) * 0.5f, cz = (B->z0 + B->z1) * 0.5f;
	flat(B->x0, B->z0, B->x1, B->z1, 0.10f, 0.72f, 0.70f, 0.66f);
	float ring = big ? 15.0f : 12.0f;
	flat(cx - ring, cz - ring, cx + ring, cz + ring, 0.14f, 0.58f, 0.56f, 0.52f);
	flat(cx - ring + 1.5f, cz - ring + 1.5f, cx + ring - 1.5f, cz + ring - 1.5f, 0.18f, 0.74f, 0.72f, 0.68f);
	float bs = big ? 7.0f : 5.0f;
	addBoxY(cx - bs, cz - bs, cx + bs, cz + bs, 0.0f, 0.7f, 0.62f, 0.62f, 0.64f);          // fountain basin
	flat(cx - bs + 0.5f, cz - bs + 0.5f, cx + bs - 0.5f, cz + bs - 0.5f, 0.74f, 0.28f, 0.55f, 0.78f);
	addSolid(cx - bs, cz - bs, cx + bs, cz + bs, 0.0f, 0.8f, SK_PROP, 0);
	if (big) { // obelisk on a stepped base
		addBoxY(cx - 2.2f, cz - 2.2f, cx + 2.2f, cz + 2.2f, 0.7f, 1.4f, 0.66f, 0.66f, 0.68f);
		addBoxY(cx - 0.9f, cz - 0.9f, cx + 0.9f, cz + 0.9f, 1.4f, 15.0f, 0.78f, 0.78f, 0.80f);
		addBoxY(cx - 0.45f, cz - 0.45f, cx + 0.45f, cz + 0.45f, 15.0f, 16.5f, 0.82f, 0.82f, 0.84f);
		addSolid(cx - 2.2f, cz - 2.2f, cx + 2.2f, cz + 2.2f, 0.0f, 16.5f, SK_PROP, 0);
	} else {
		addBoxY(cx - 0.6f, cz - 0.6f, cx + 0.6f, cz + 0.6f, 0.7f, 3.4f, 0.72f, 0.72f, 0.74f);
		addBoxY(cx - 1.4f, cz - 1.4f, cx + 1.4f, cz + 1.4f, 3.4f, 3.7f, 0.50f, 0.70f, 0.85f);
	}
	// two little cafes in opposite corners
	float c1x = B->x0 + 3.0f, c1z = B->z0 + 3.0f, c2x = B->x1 - 8.6f, c2z = B->z1 - 8.6f;
	for (int k = 0; k < 2; k++) {
		float kx = k ? c2x : c1x, kz = k ? c2z : c1z;
		addBoxY(kx, kz, kx + 5.6f, kz + 5.6f, 0.0f, 4.2f, k ? 0.80f : 0.60f, k ? 0.55f : 0.70f, k ? 0.40f : 0.62f);
		addBoxY(kx - 0.5f, kz - 0.5f, kx + 6.1f, kz + 6.1f, 4.2f, 4.6f, 0.30f, 0.30f, 0.34f);
		addSolid(kx - 0.5f, kz - 0.5f, kx + 6.1f, kz + 6.1f, 0.0f, 4.6f, SK_BLDG, 0);
	}
	// rows of trees along the north and south edges, benches round the fountain
	for (float x = B->x0 + 14.0f; x < B->x1 - 12.0f; x += 8.0f) {
		addTree(x, B->z0 + 3.0f, cr(3.0f, 3.8f), 2);
		addTree(x, B->z1 - 3.0f, cr(3.0f, 3.8f), 2);
	}
	float bo = bs + 2.2f;
	addBench(cx, cz - bo, 1); addBench(cx, cz + bo, 1); addBench(cx - bo, cz, 0); addBench(cx + bo, cz, 0);
	float pl = big ? 12.0f : 9.0f; // strolling lines either side of the fountain, clear of benches, cafes and tree rows
	addWLine(0, cz - pl, B->x0 + 2.0f, B->x1 - 2.0f); addWLine(0, cz + pl, B->x0 + 2.0f, B->x1 - 2.0f);
	addWLine(1, cx - pl, B->z0 + 6.0f, B->z1 - 6.0f); addWLine(1, cx + pl, B->z0 + 6.0f, B->z1 - 6.0f);
}

static void genLot(const Blk* B) {
	float cx = (B->x0 + B->x1) * 0.5f;
	flat(B->x0, B->z0, B->x1, B->z1, 0.10f, 0.22f, 0.22f, 0.24f);
	// a low shop along the north edge
	addBoxY(cx - 11.0f, B->z0 + 3.0f, cx + 11.0f, B->z0 + 12.0f, 0.0f, 5.5f, 0.72f, 0.68f, 0.60f);
	addBoxY(cx - 11.3f, B->z0 + 2.7f, cx + 11.3f, B->z0 + 12.3f, 5.5f, 6.0f, 0.30f, 0.30f, 0.34f);
	addSolid(cx - 11.3f, B->z0 + 2.7f, cx + 11.3f, B->z0 + 12.3f, 0.0f, 6.0f, SK_BLDG, 0);
	// two rows of parked cars with stall lines
	static const float pal[6][3] = {{0.85f,0.15f,0.15f},{0.15f,0.35f,0.85f},{0.85f,0.85f,0.88f},{0.12f,0.12f,0.14f},{0.20f,0.65f,0.30f},{0.75f,0.50f,0.15f}};
	for (int row = 0; row < 2; row++) {
		float x = cx - 8.0f + row * 16.0f;
		for (int i = 0; i < 6; i++) {
			float z = B->z0 + 19.0f + i * 5.0f;
			if (z + 2.5f > B->z1 - 3.0f) break;
			flat(x - 1.9f, z - 2.5f, x - 1.75f, z + 2.5f, 0.14f, 0.85f, 0.85f, 0.85f);
			if (crand() < 0.7f) { const float* p = pal[(int)(crand() * 6.0f) % 6]; parkedCar(x, z, p[0], p[1], p[2]); }
		}
	}
	for (float x = B->x0 + 5.0f; x < B->x1 - 4.0f; x += 9.0f) addTree(x, B->z1 - 3.0f, 3.2f, 2);
}

// ---- roads ---------------------------------------------------------------------------
static void roadMarkings(int axis) {
	const float Y = 0.10f;
	int nr = axis == 0 ? NRZ : NRX, nc = axis == 0 ? NRX : NRZ;
	const float* lat = axis == 0 ? RZ : RX;
	const float* cross = axis == 0 ? RX : RZ;
	float lo = (axis == 0 ? gXmin : gZmin) + 1.0f, hi = (axis == 0 ? gXmax : gZmax) - 1.0f;
	for (int k = 0; k < nr; k++) {
		float c = lat[k], a = lo;
		for (int m = 0; m <= nc; m++) {
			float b = (m < nc) ? cross[m] - 12.0f : hi;
			if (b - a > 6.0f) {
				flatAx(axis, a, b, c - 0.30f, c - 0.12f, Y, 0.95f, 0.78f, 0.15f);   // double yellow centre line
				flatAx(axis, a, b, c + 0.12f, c + 0.30f, Y, 0.95f, 0.78f, 0.15f);
				for (float d = a + 3.0f; d + 4.5f < b - 2.0f; d += 12.0f) {         // dashed lane lines
					flatAx(axis, d, d + 4.5f, c - 4.1f, c - 3.9f, Y, 0.90f, 0.90f, 0.90f);
					flatAx(axis, d, d + 4.5f, c + 3.9f, c + 4.1f, Y, 0.90f, 0.90f, 0.90f);
				}
			}
			if (m < nc) a = cross[m] + 12.0f;
		}
	}
}

static void intersectionMarkings(void) {
	const float Y = 0.10f;
	for (int a = 0; a < NRX; a++) {
		for (int b = 0; b < NRZ; b++) {
			for (int arm = 0; arm < 4; arm++) {
				int   axis = arm < 2 ? 0 : 1;           // the street this arm belongs to runs along `axis`
				float sg   = (arm & 1) ? 1.0f : -1.0f;
				float c0   = axis == 0 ? RX[a] : RZ[b];  // centre along the street
				float cl   = axis == 0 ? RZ[b] : RX[a];  // centre across it
				float e0 = c0 + sg * 8.5f, e1 = c0 + sg * 10.7f;
				float lo = fminf(e0, e1), hi = fmaxf(e0, e1);
				for (int i = -2; i <= 2; i++)             // zebra crossing
					flatAx(axis, lo, hi, cl + i * 2.6f - 0.65f, cl + i * 2.6f + 0.65f, Y, 0.92f, 0.92f, 0.92f);
				float s0 = c0 + sg * 11.0f, s1 = c0 + sg * 11.7f;   // stop line across the incoming lanes
				float incoming = -sg;                              // cars on this arm travel toward the centre
				float l0 = incoming < 0 ? cl - 7.6f : cl + 0.4f, l1 = incoming < 0 ? cl - 0.4f : cl + 7.6f;
				flatAx(axis, fminf(s0, s1), fmaxf(s0, s1), l0, l1, Y, 0.92f, 0.92f, 0.92f);
			}
		}
	}
}

// street lights. Pass 0: the poles (world pass). Pass 1: the lamp heads (emissive pass).
static void lamps(int pass) {
	for (int axis = 0; axis < 2; axis++) {
		int nr = axis == 0 ? NRZ : NRX, nc = axis == 0 ? NRX : NRZ;
		const float* lat = axis == 0 ? RZ : RX;
		const float* cross = axis == 0 ? RX : RZ;
		float lo = (axis == 0 ? gXmin : gZmin) + 1.0f, hi = (axis == 0 ? gXmax : gZmax) - 1.0f;
		for (int k = 0; k < nr; k++) {
			float a = lo;
			for (int m = 0; m <= nc; m++) {
				float b = (m < nc) ? cross[m] - 15.0f : hi;
				if (b - a > 20.0f) {
					for (int i = 0; i < 2; i++) {
						float s = a + (b - a) * (i == 0 ? 0.28f : 0.72f);
						float side = (((k + m + i) & 1) ? 1.0f : -1.0f);
						float l = lat[k] + side * (ROAD_HALF + WALK_W - 0.7f);
						float x = axis == 0 ? s : l, z = axis == 0 ? l : s;
						float tx = axis == 0 ? 0.0f : -side * 0.55f, tz = axis == 0 ? -side * 0.55f : 0.0f;
						if (pass == 0) addBoxY(x - 0.11f, z - 0.11f, x + 0.11f, z + 0.11f, 0.0f, 6.2f, 0.20f, 0.21f, 0.23f);
						else           addBoxY(x + tx - 0.45f, z + tz - 0.45f, x + tx + 0.45f, z + tz + 0.45f, 6.0f, 6.3f, 1.00f, 0.90f, 0.62f);
					}
				}
				if (m < nc) a = cross[m] + 15.0f;
			}
		}
	}
}

// is coordinate v (an x if axisX == 0, a z if 1) inside a street or its sidewalk, with a margin?
static int inStreet(float v, float margin, int axisZ) {
	int n = axisZ ? NRZ : NRX;
	const float* r = axisZ ? RZ : RX;
	for (int k = 0; k < n; k++) if (fabsf(v - r[k]) < INSET + margin) return 1;
	return 0;
}

static void buildCity(void) {
	vcount = 0; nSolid = 0; nWl = 0;
	cityS = 0x9E3779B9u;
	gXmin = RX[0] - 24.0f; gXmax = RX[NRX - 1] + 26.0f; gZmin = RZ[0] - 24.0f; gZmax = RZ[NRZ - 1] + 24.0f;

	// ground, lake to the east
	flat(-900.0f, -900.0f, gXmax + 1.5f, 900.0f, 0.0f, 0.38f, 0.44f, 0.37f);
	flat(gXmax + 1.5f, -900.0f, 1400.0f, 900.0f, 0.02f, 0.18f, 0.40f, 0.62f);

	// asphalt
	for (int k = 0; k < NRZ; k++) flat(gXmin - 1.0f, RZ[k] - ROAD_HALF, gXmax + 1.0f, RZ[k] + ROAD_HALF, 0.03f, 0.17f, 0.18f, 0.20f);
	for (int k = 0; k < NRX; k++) flat(RX[k] - ROAD_HALF, gZmin - 1.0f, RX[k] + ROAD_HALF, gZmax + 1.0f, 0.03f, 0.17f, 0.18f, 0.20f);

	// sidewalk cells between the streets, including the ring round the edge
	for (int j = 0; j <= NRZ; j++) {
		float z0 = (j == 0) ? gZmin : RZ[j - 1] + ROAD_HALF, z1 = (j == NRZ) ? gZmax : RZ[j] - ROAD_HALF;
		for (int i = 0; i <= NRX; i++) {
			float x0 = (i == 0) ? gXmin : RX[i - 1] + ROAD_HALF, x1 = (i == NRX) ? gXmax : RX[i] - ROAD_HALF;
			int inner = (i >= 1 && i < NRX && j >= 1 && j < NRZ);
			flat(x0, z0, x1, z1, 0.03f, 0.62f, 0.62f, 0.64f);
			// light kerb stripes along the road edges of the cell
			if (i > 0)   flat(x0, z0, x0 + 0.3f, z1, 0.07f, 0.78f, 0.78f, 0.80f);
			if (i < NRX) flat(x1 - 0.3f, z0, x1, z1, 0.07f, 0.78f, 0.78f, 0.80f);
			if (j > 0)   flat(x0, z0, x1, z0 + 0.3f, 0.07f, 0.78f, 0.78f, 0.80f);
			if (j < NRZ) flat(x0, z1 - 0.3f, x1, z1, 0.07f, 0.78f, 0.78f, 0.80f);
			if (!inner && i < NRX) {   // outer ring: a lawn behind the sidewalk (the east edge stays a paved promenade)
				float gx0 = x0 + (i > 0 ? WALK_W : 1.0f), gx1 = x1 - (i < NRX ? WALK_W : 1.0f);
				float gz0 = z0 + (j > 0 ? WALK_W : 1.0f), gz1 = z1 - (j < NRZ ? WALK_W : 1.0f);
				if (gx1 - gx0 > 3.0f && gz1 - gz0 > 3.0f) flat(gx0, gz0, gx1, gz1, 0.10f, 0.34f, 0.50f, 0.30f);
			}
			if (inner) {
				Blk* B = &blk[j - 1][i - 1];
				B->x0 = x0 + WALK_W; B->x1 = x1 - WALK_W; B->z0 = z0 + WALK_W; B->z1 = z1 - WALK_W;
				B->kind = blockMap[j - 1][i - 1];
			}
		}
	}

	// the blocks
	for (int j = 0; j < NRZ - 1; j++) {
		for (int i = 0; i < NRX - 1; i++) {
			Blk* B = &blk[j][i];
			cityS = hash2(i * 17 + 3, j * 29 + 7) | 1u;
			switch (B->kind) {
			case B_LOW:    genLow(B); break;
			case B_MID:    genMid(B); break;
			case B_TOWERS: genTowers(B); break;
			case B_PARK:   genPark(B); break;
			case B_PLAZA:  genPlaza(B, i == 2 && j == 2); break;
			default:       genLot(B); break;
			}
		}
	}
	{ // start on the south side of the central square, looking north at the fountain
		const Blk* B = &blk[2][2];
		gStartX = (B->x0 + B->x1) * 0.5f + 11.0f;
		gStartZ = (B->z0 + B->z1) * 0.5f + 20.0f;
	}

	// ring of trees round the edge (trunks only block movement), never in a street or its sidewalk
	cityS = 0xC0FFEEu;
	for (float z = gZmin + 6.0f; z < gZmax - 4.0f; z += 24.0f) { float zz = z + cr(-3.0f, 3.0f); if (!inStreet(zz, 5.0f, 1)) addTree(gXmin + 4.0f, zz, cr(3.0f, 4.0f), 1); }
	for (float x = gXmin + 20.0f; x < gXmax - 10.0f; x += 24.0f) {
		float x1 = x + cr(-3.0f, 3.0f), x2 = x + cr(-3.0f, 3.0f);
		if (!inStreet(x1, 5.0f, 0)) addTree(x1, gZmin + 4.0f, cr(3.0f, 4.0f), 1);
		if (!inStreet(x2, 5.0f, 0)) addTree(x2, gZmax - 4.0f, cr(3.0f, 4.0f), 1);
	}
	for (float z = gZmin + 10.0f; z < gZmax - 10.0f; z += 40.0f) if (!inStreet(z, 2.0f, 1)) addBench(gXmax - 3.0f, z, 0);

	// perimeter walls and the lakefront seawall
	addBoxY(gXmin - 1.5f, gZmin - 1.5f, gXmin, gZmax + 1.5f, 0.0f, 1.8f, 0.62f, 0.62f, 0.64f);
	addBoxY(gXmin, gZmin - 1.5f, gXmax, gZmin, 0.0f, 1.8f, 0.62f, 0.62f, 0.64f);
	addBoxY(gXmin, gZmax, gXmax, gZmax + 1.5f, 0.0f, 1.8f, 0.62f, 0.62f, 0.64f);
	addBoxY(gXmax, gZmin - 1.5f, gXmax + 1.5f, gZmax + 1.5f, 0.0f, 1.0f, 0.74f, 0.74f, 0.76f);
	addSolid(gXmin - 1.5f, gZmin - 1.5f, gXmin, gZmax + 1.5f, 0.0f, 1.8f, SK_WALL, 0);
	addSolid(gXmin, gZmin - 1.5f, gXmax, gZmin, 0.0f, 1.8f, SK_WALL, 0);
	addSolid(gXmin, gZmax, gXmax, gZmax + 1.5f, 0.0f, 1.8f, SK_WALL, 0);
	addSolid(gXmax, gZmin - 1.5f, gXmax + 1.5f, gZmax + 1.5f, 0.0f, 1.0f, SK_WALL, 0);

	// distant skyline beyond the walls (north, south and west; the lake is east)
	cityS = 0x5EED5u;
	for (int k = 0; k < 12; k++) {
		float x = -300.0f + (float)k * 48.0f + cr(-12.0f, 12.0f);
		float w = cr(18.0f, 30.0f), d = cr(18.0f, 30.0f), h = cr(40.0f, 115.0f), v = cr(0.92f, 1.08f);
		addBox(x, gZmin - 60.0f - cr(0.0f, 70.0f), x + w, gZmin - 60.0f - cr(0.0f, 70.0f) + d, h, 0.50f * v, 0.56f * v, 0.64f * v);
		addBox(x, gZmax + 60.0f + cr(0.0f, 70.0f), x + w, gZmax + 60.0f + cr(0.0f, 70.0f) + d, h * cr(0.7f, 1.0f), 0.52f * v, 0.57f * v, 0.65f * v);
	}
	for (int k = 0; k < 10; k++) {
		float z = -240.0f + (float)k * 52.0f + cr(-12.0f, 12.0f);
		float w = cr(18.0f, 30.0f), d = cr(18.0f, 30.0f), h = cr(40.0f, 110.0f), v = cr(0.92f, 1.08f);
		addBox(gXmin - 60.0f - cr(0.0f, 70.0f) - w, z, gXmin - 60.0f - cr(0.0f, 70.0f), z + d, h, 0.51f * v, 0.56f * v, 0.64f * v);
	}

	// road paint
	roadMarkings(0); roadMarkings(1); intersectionMarkings();
	lamps(0);
	// traffic-light poles (the light boxes themselves change colour, so they are drawn per frame)
	for (int a = 0; a < NRX; a++)
		for (int b = 0; b < NRZ; b++)
			addBox(RX[a] + ROAD_HALF + 0.75f, RZ[b] + ROAD_HALF + 0.75f, RX[a] + ROAD_HALF + 1.05f, RZ[b] + ROAD_HALF + 1.05f,
			       4.4f, 0.2f, 0.2f, 0.22f);

	// pedestrian lines along both sides of every street
	for (int k = 0; k < NRZ; k++) { addWLine(0, RZ[k] - (ROAD_HALF + 2.2f), gXmin + 6.0f, gXmax - 6.0f); addWLine(0, RZ[k] + (ROAD_HALF + 2.2f), gXmin + 6.0f, gXmax - 6.0f); }
	for (int k = 0; k < NRX; k++) { addWLine(1, RX[k] - (ROAD_HALF + 2.2f), gZmin + 6.0f, gZmax - 6.0f); addWLine(1, RX[k] + (ROAD_HALF + 2.2f), gZmin + 6.0f, gZmax - 6.0f); }
	// (the park / square lines were added first; move the sidewalk lines to the front)
	{
		WLine tmp[MAX_WL];
		int side = 0, other = 0;
		int total = nWl;
		for (int i = 0; i < total; i++) { if (i >= total - 2 * (NRZ + NRX)) tmp[side++] = wl[i]; }
		for (int i = 0; i < total - 2 * (NRZ + NRX); i++) tmp[side + other++] = wl[i];
		memcpy(wl, tmp, sizeof(WLine) * (size_t)total);
		nWlSide = side;
	}

	// dark window glass (world pass), then the lit pass: lit windows and lamp heads (emissive tint)
	for (int i = 0; i < nSolid; i++) if (solids[i].kind == SK_BLDG && solids[i].win) facadeWindows(&solids[i], i, 0);
	worldStaticEnd = vcount;
	for (int i = 0; i < nSolid; i++) if (solids[i].kind == SK_BLDG && solids[i].win) facadeWindows(&solids[i], i, 1);
	lamps(1);
	staticCount = vcount;
}

// ---- traffic -------------------------------------------------------------
// Streets run along x = RX[k] (north-south) and z = RZ[k] (east-west). Each has four lanes.
// Intersections are (a, b): a = index of the north-south street, b = index of the east-west one.
static const float LANE_OFF[4] = { -6.0f, -2.0f, 2.0f, 6.0f }; // lane offsets from the street centre; dir = sign

enum { CT_SEDAN, CT_SUV, CT_TRUCK, CT_BUS, CT_TAXI };
typedef struct { int axis, road, lane, dir, type; float s, speed, len, hw, r, g, b; } Car;
static Car cars[N_CARS];
static int hackA = -1, hackB = -1, hackTimer = 0;

#define LIGHT_CYCLE 860
// 0 = green, 1 = yellow, 2 = red. axis 0 = traffic moving along x, axis 1 = along z.
// Every phase change is followed by a long all-red gap so cars never meet in the middle.
static int lightState(int a, int b, int axis, int frame) {
	if (hackTimer > 0 && a == hackA && b == hackB) return 2;
	int t = (frame + (a * 7 + b * 13) * 97) % LIGHT_CYCLE;
	if (axis == 1) t = (t + LIGHT_CYCLE - 430) % LIGHT_CYCLE;
	return (t < 280) ? 0 : (t < 320) ? 1 : 2;
}

static float laneCoord(const Car* c) {
	return (c->axis == 0 ? RZ[c->road] : RX[c->road]) + LANE_OFF[c->lane];
}
static float roadLo(int axis) { return (axis == 0 ? gXmin : gZmin) + 8.0f; }   // cars turn around before they reach the wall
static float roadHi(int axis) { return (axis == 0 ? gXmax : gZmax) - 8.0f; }

static void initCars(void) {
	static const float pal[10][3] = {
		{0.85f, 0.15f, 0.15f}, {0.15f, 0.35f, 0.85f}, {0.85f, 0.85f, 0.88f}, {0.12f, 0.12f, 0.14f}, {0.20f, 0.65f, 0.30f},
		{0.55f, 0.57f, 0.60f}, {0.50f, 0.20f, 0.55f}, {0.80f, 0.45f, 0.12f}, {0.30f, 0.45f, 0.60f}, {0.62f, 0.12f, 0.20f}
	};
	for (int i = 0; i < N_CARS; i++) {
		Car* c = &cars[i];
		for (int tries = 0; tries < 40; tries++) {
			unsigned h = hash2(i * 31 + tries, 991);
			c->axis = i & 1;
			c->road = (int)((h >> 3) % (unsigned)(c->axis == 0 ? NRZ : NRX));
			c->lane = (int)((h >> 9) & 3);
			c->dir  = LANE_OFF[c->lane] > 0 ? 1 : -1;
			float lo = roadLo(c->axis), hi = roadHi(c->axis);
			c->s = lo + 10.0f + (float)((h >> 12) % 1000) / 1000.0f * (hi - lo - 20.0f);
			int ok = 1;
			const float* cross = c->axis == 0 ? RX : RZ;
			for (int k = 0; k < (c->axis == 0 ? NRX : NRZ); k++) if (fabsf(c->s - cross[k]) < 22.0f) ok = 0;
			for (int j = 0; j < i; j++)
				if (cars[j].axis == c->axis && cars[j].road == c->road && cars[j].lane == c->lane && fabsf(cars[j].s - c->s) < 45.0f) ok = 0;
			if (ok) break;
		}
		unsigned h = hash2(i, 77);
		float k = (float)(h % 100) / 100.0f;
		c->type = k < 0.50f ? CT_SEDAN : k < 0.68f ? CT_SUV : k < 0.80f ? CT_TRUCK : k < 0.86f ? CT_BUS : CT_TAXI;
		c->len = c->type == CT_BUS ? 10.0f : c->type == CT_TRUCK ? 6.0f : c->type == CT_SUV ? 4.8f : 4.3f;
		c->hw  = c->type == CT_BUS ? 1.25f : c->type == CT_TRUCK ? 1.1f : c->type == CT_SUV ? 1.05f : 1.0f;
		c->speed = 0.16f + (float)((h >> 8) % 11) * 0.009f;
		const float* p = pal[(h >> 16) % 10];
		if (c->type == CT_TAXI) { c->r = 0.95f; c->g = 0.78f; c->b = 0.10f; }
		else if (c->type == CT_BUS) { c->r = 0.20f; c->g = 0.45f; c->b = 0.75f; }
		else { c->r = p[0]; c->g = p[1]; c->b = p[2]; }
	}
}

static void updateCars(int frame, float px, float pz) {
	if (hackTimer > 0) hackTimer--;
	for (int i = 0; i < nCarOn; i++) {
		Car* c = &cars[i];
		float lane = laneCoord(c);
		int stop = 0;
		const float* cross = c->axis == 0 ? RX : RZ;
		int nc = c->axis == 0 ? NRX : NRZ;

		// red / yellow light ahead: stop just before the crosswalk
		float hi = 11.9f + c->len * 0.5f;
		for (int k = 0; k < nc; k++) {
			float gap = (cross[k] - c->s) * (float)c->dir;
			if (gap > hi - 1.0f && gap < hi) {
				int a = (c->axis == 0) ? k : c->road;
				int b = (c->axis == 0) ? c->road : k;
				if (lightState(a, b, c->axis, frame) != 0) stop = 1;
			}
		}
		// car ahead in the same lane: keep a comfortable gap
		for (int j = 0; j < nCarOn && !stop; j++) {
			if (j == i) continue;
			const Car* o = &cars[j];
			if (o->axis == c->axis && o->road == c->road && o->lane == c->lane) {
				float gap = (o->s - c->s) * (float)c->dir;
				if (gap > 0.0f && gap < (c->len + o->len) * 0.5f + 6.5f) stop = 1;
			}
		}
		// the player standing in the way
		float ppar = (c->axis == 0) ? px : pz, plat = (c->axis == 0) ? pz : px;
		float pgap = (ppar - c->s) * (float)c->dir;
		if (pgap > 0.0f && pgap < c->len * 0.5f + 5.0f && fabsf(plat - lane) < c->hw + 1.2f) stop = 1;

		if (!stop) c->s += (float)c->dir * c->speed;
		float lo2 = roadLo(c->axis), hi2 = roadHi(c->axis);
		if (c->s > hi2) c->s = lo2;
		if (c->s < lo2) c->s = hi2;
	}
}

static void carBox(const Car* c, float* cx, float* cz, float* hx, float* hz) {
	float lane = laneCoord(c);
	*hx = (c->axis == 0) ? c->len * 0.5f : c->hw;
	*hz = (c->axis == 0) ? c->hw : c->len * 0.5f;
	*cx = (c->axis == 0) ? c->s : lane;
	*cz = (c->axis == 0) ? lane : c->s;
}

// box in car space: `a0..a1` measured forward from the centre, `l0..l1` sideways
static void cbox(const Car* c, float a0, float a1, float l0, float l1, float y0, float y1, float r, float g, float b) {
	float cx, cz, hx, hz;
	carBox(c, &cx, &cz, &hx, &hz);
	float f0 = c->dir > 0 ? a0 : -a1, f1 = c->dir > 0 ? a1 : -a0; // forward = dir along the street axis
	if (c->axis == 0) addBoxY(cx + f0, cz + l0, cx + f1, cz + l1, y0, y1, r, g, b);
	else              addBoxY(cx + l0, cz + f0, cx + l1, cz + f1, y0, y1, r, g, b);
}

// Rebuilds the dynamic tail of the vertex buffer: the cars.
static void drawCars(void) {
	for (int i = 0; i < nCarOn; i++) {
		const Car* c = &cars[i];
		float L = c->len * 0.5f, W = c->hw;
		switch (c->type) {
		case CT_BUS:
			cbox(c, -L, L, -W + 0.05f, W - 0.05f, 0.15f, 0.65f, 0.12f, 0.12f, 0.14f);
			cbox(c, -L, L, -W, W, 0.65f, 1.55f, c->r, c->g, c->b);
			cbox(c, -L + 0.2f, L - 0.2f, -W - 0.02f, W + 0.02f, 1.55f, 2.45f, 0.15f, 0.20f, 0.27f);
			cbox(c, -L, L, -W, W, 2.45f, 2.9f, c->r * 0.9f, c->g * 0.9f, c->b * 0.9f);
			break;
		case CT_TRUCK:
			cbox(c, -L + 0.1f, L - 0.1f, -W + 0.05f, W - 0.05f, 0.15f, 0.65f, 0.12f, 0.12f, 0.14f);
			cbox(c, L - 1.8f, L, -W, W, 0.65f, 1.9f, c->r, c->g, c->b);                       // cab
			cbox(c, L - 1.3f, L - 0.3f, -W - 0.02f, W + 0.02f, 1.2f, 1.8f, 0.15f, 0.20f, 0.27f);
			cbox(c, -L, L - 2.0f, -W, W, 0.65f, 2.7f, 0.82f, 0.82f, 0.80f);                 // cargo box
			break;
		case CT_SUV:
			cbox(c, -L + 0.1f, L - 0.1f, -W + 0.05f, W - 0.05f, 0.15f, 0.65f, 0.12f, 0.12f, 0.14f);
			cbox(c, -L, L, -W, W, 0.65f, 1.35f, c->r, c->g, c->b);
			cbox(c, -L + 0.4f, L - 0.9f, -W + 0.1f, W - 0.1f, 1.35f, 2.0f, 0.15f, 0.20f, 0.27f);
			cbox(c, -L + 0.4f, L - 0.9f, -W + 0.1f, W - 0.1f, 2.0f, 2.1f, c->r * 0.9f, c->g * 0.9f, c->b * 0.9f);
			break;
		default: // sedan, taxi
			cbox(c, -L + 0.1f, L - 0.1f, -W + 0.05f, W - 0.05f, 0.15f, 0.55f, 0.12f, 0.12f, 0.14f);
			cbox(c, -L, L, -W, W, 0.55f, 1.15f, c->r, c->g, c->b);
			cbox(c, -L + 0.8f, L - 1.0f, -W + 0.12f, W - 0.12f, 1.15f, 1.7f, 0.15f, 0.20f, 0.27f);
			cbox(c, -L + 0.8f, L - 1.0f, -W + 0.12f, W - 0.12f, 1.7f, 1.78f, c->r * 0.9f, c->g * 0.9f, c->b * 0.9f);
			if (c->type == CT_TAXI) cbox(c, -0.3f, 0.3f, -0.3f, 0.3f, 1.78f, 2.1f, 1.0f, 0.95f, 0.70f);
			break;
		}
	}
}

// Emissive geometry (traffic lights, head- and tail-lights): drawn with its own tint so it stays bright at night.
static void drawLights(int frame, float night, float camx, float camz) {
	if (night > 0.3f) {
		for (int i = 0; i < nCarOn; i++) {
			const Car* c = &cars[i];
			float L = c->len * 0.5f, W = c->hw;
			cbox(c, L - 0.1f, L + 0.15f, -W + 0.15f, -W + 0.55f, 0.55f, 0.95f, 1.0f, 0.95f, 0.70f);
			cbox(c, L - 0.1f, L + 0.15f,  W - 0.55f,  W - 0.15f, 0.55f, 0.95f, 1.0f, 0.95f, 0.70f);
			cbox(c, -L - 0.1f, -L + 0.1f, -W + 0.15f, -W + 0.5f, 0.6f, 0.9f, 0.9f, 0.1f, 0.1f);
			cbox(c, -L - 0.1f, -L + 0.1f,  W - 0.5f,  W - 0.15f, 0.6f, 0.9f, 0.9f, 0.1f, 0.1f);
		}
	}
	for (int a = 0; a < NRX; a++) {
		for (int b = 0; b < NRZ; b++) {
			float x = RX[a] + ROAD_HALF + 0.9f, z = RZ[b] + ROAD_HALF + 0.9f;
			if ((x - camx) * (x - camx) + (z - camz) * (z - camz) > 140.0f * 140.0f) continue; // too far to see
			int hacked = (hackTimer > 0 && a == hackA && b == hackB);
			for (int axis = 0; axis < 2; axis++) {
				int st = lightState(a, b, axis, frame);
				float r, g, bl;
				if (hacked)       { r = 1.0f; g = 0.2f;  bl = 1.0f; }
				else if (st == 0) { r = 0.1f; g = 1.0f;  bl = 0.2f; }
				else if (st == 1) { r = 1.0f; g = 0.85f; bl = 0.1f; }
				else              { r = 1.0f; g = 0.1f;  bl = 0.1f; }
				float y0 = 4.4f + axis * 0.8f;
				addBoxY(x - 0.4f, z - 0.4f, x + 0.4f, z + 0.4f, y0, y0 + 0.7f, r, g, bl);
			}
		}
	}
}

// 1 if a person standing at (x, z) would be inside something solid (trees count only at trunk height)
static int blocked(float x, float z) {
	for (int i = 0; i < nSolid; i++) {
		const Solid* b = &solids[i];
		if (b->y0 > gFootY + 1.9f) continue;
		if (gLevel == LV_FACTORY && (b->kind == SK_FLOOR || b->y1 < gFootY + 0.4f)) continue;
		if (x > b->x0 - PLAYER_R && x < b->x1 + PLAYER_R && z > b->z0 - PLAYER_R && z < b->z1 + PLAYER_R) return 1;
	}
	return (x < gXmin + 0.8f || x > gXmax - 0.8f || z < gZmin + 0.8f || z > gZmax - 0.8f);
}

// ---- people (shared by the player and the pedestrians) ----------------------------------------
// Blocky low-poly person built from rotated boxes. Body space: feet at the origin, facing -Z.
// Each part swings about its joint (X axis), then the whole body leans and turns to its heading.
static float gPx, gPz, gFace, gLean, gBob, gPy;
static float gSx = 1.0f, gSy = 1.0f; // body width / height scale of the person being drawn

static void addPart(float jx, float jy, float jz, float ox, float oy, float oz,
                    float hx, float hy, float hz, float swing, float r, float g, float b) {
	jx *= gSx; jy *= gSy; jz *= gSx; ox *= gSx; oy *= gSy; oz *= gSx; hx *= gSx; hy *= gSy; hz *= gSx;
	float cs = cosf(swing), sn = sinf(swing);
	float cl = cosf(gLean), sl = sinf(gLean);
	float cf = cosf(gFace), sf = sinf(gFace);
	float P[8][3];
	for (int i = 0; i < 8; i++) {
		float x = ox + ((i & 1) ? hx : -hx);
		float y = oy + ((i & 2) ? hy : -hy);
		float z = oz + ((i & 4) ? hz : -hz);
		float y1 = y * cs - z * sn, z1 = y * sn + z * cs;      // joint swing
		x += jx; y1 += jy; z1 += jz;
		float y2 = y1 * cl - z1 * sl, z2 = y1 * sl + z1 * cl;  // body lean
		P[i][0] = gPx + x * cf + z2 * sf;
		P[i][1] = y2 + gBob + 0.20f + gPy;
		P[i][2] = gPz - x * sf + z2 * cf;
	}
	quad(P[0], P[1], P[3], P[2], r * 0.70f, g * 0.70f, b * 0.70f);
	quad(P[4], P[5], P[7], P[6], r * 0.70f, g * 0.70f, b * 0.70f);
	quad(P[0], P[4], P[6], P[2], r * 0.85f, g * 0.85f, b * 0.85f);
	quad(P[1], P[5], P[7], P[3], r * 0.85f, g * 0.85f, b * 0.85f);
	quad(P[2], P[3], P[7], P[6], r, g, b);
	quad(P[0], P[1], P[5], P[4], r * 0.50f, g * 0.50f, b * 0.50f);
}

// ---- weather ----------------------------------------------------------------
// Cheap by design: sky colour + world tint changes, ~140 rain streaks, ~11 lens droplets.
enum { W_SUNNY = 0, W_RAINY = 1, W_NIGHT = 2 };
static int   weather = W_SUNNY;
static float wSky[3]  = {0.41f, 0.69f, 0.85f};
static float wTint[3] = {1.0f, 1.0f, 1.0f};
static float wRain = 0.0f, wNight = 0.0f, wTime = 0.0f;

static float stTint[3] = {0.9f, 0.9f, 0.95f};   // tint of the area the hero is in (story mode)
static void updateWeather(void) { // eases toward the chosen weather (about one second)
	if (gLevel == LV_FACTORY) {   // story mode: always night outside, the building's own lights are on
		static const float skyN[3] = {0.025f, 0.03f, 0.06f};
		for (int i = 0; i < 3; i++) { wSky[i] += (skyN[i] - wSky[i]) * 0.2f; wTint[i] += (stTint[i] - wTint[i]) * 0.08f; }
		wRain *= 0.8f; wNight += (1.0f - wNight) * 0.3f; wTime += 1.0f;
		return;
	}
	static const float sky[3][3]  = {{0.41f, 0.69f, 0.85f}, {0.36f, 0.40f, 0.45f}, {0.03f, 0.04f, 0.10f}};
	static const float tint[3][3] = {{1.00f, 1.00f, 1.00f}, {0.70f, 0.76f, 0.84f}, {0.30f, 0.34f, 0.52f}};
	for (int i = 0; i < 3; i++) {
		wSky[i]  += (sky[weather][i]  - wSky[i])  * 0.04f;
		wTint[i] += (tint[weather][i] - wTint[i]) * 0.04f;
	}
	wRain  += ((weather == W_RAINY) ? 1.0f - wRain  : -wRain)  * 0.04f;
	wNight += ((weather == W_NIGHT) ? 1.0f - wNight : -wNight) * 0.04f;
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

// Sun by day, moon and stars at night. Drawn far away, blended, behind nothing.
static void drawSkyBodies(float cx, float cy, float cz, float cyaw) {
	float rx = cosf(cyaw), rz = -sinf(cyaw);
	float day = (1.0f - wNight) * (1.0f - wRain);
	if (day > 0.02f) {
		addBillboard(cx + 95.0f, cy + 112.0f, cz - 262.0f, 46.0f, 46.0f, rx, rz, 1.0f, 0.95f, 0.70f, 0.22f * day);
		addBillboard(cx + 95.0f, cy + 112.0f, cz - 262.0f, 22.0f, 22.0f, rx, rz, 1.0f, 0.97f, 0.80f, day);
	}
	if (wNight > 0.02f) {
		addBillboard(cx - 120.0f, cy + 120.0f, cz - 250.0f, 15.0f, 15.0f, rx, rz, 0.90f, 0.93f, 1.0f, wNight);
		for (int i = 0; i < 70; i++) {
			unsigned h = hash2(i, 5150);
			float az = (float)(h & 1023) / 1023.0f * 6.2832f;
			float el = 0.10f + (float)((h >> 10) & 1023) / 1023.0f * 1.1f;
			float tw = 0.55f + 0.45f * sinf(wTime * 0.05f + (float)i);
			addBillboard(cx + 300.0f * cosf(el) * sinf(az), cy + 300.0f * sinf(el), cz - 300.0f * cosf(el) * cosf(az),
			             1.8f, 1.8f, rx, rz, 1.0f, 1.0f, 0.95f, wNight * 0.9f * tw);
		}
	}
}

// Thin falling streaks in a box around the camera, tiled so they stay put in the world.
static void drawRain(float cx, float cz, float cyaw) {
	int n = (int)((float)RAIN_N * wRain);
	if (n <= 0) return;
	float rx = cosf(cyaw) * 0.03f, rz = -sinf(cyaw) * 0.03f;
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
		addV(x, y, 0.5f, cr, cg, cb);
		addV(x + r * cosf(a0), y + r * sinf(a0), 0.5f, cr, cg, cb);
		addV(x + r * cosf(a1), y + r * sinf(a1), 0.5f, cr, cg, cb);
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

// ---- audio -----------------------------------------------------------------------
// The DSP plays raw PCM straight out of memory. Everything is loaded once while the logos
// are on screen (at most 256 KB per frame, so the animation never stalls) and then triggered by index.
// Files come from romfs:/<name>.raw (packed into the .3dsx by the Makefile); if that is missing the
// game also tries sdmc:/3ds/chicago3ds/<name>.raw.
typedef struct { const char* file; int stereo; } SoundDef;
static const SoundDef sdef[S_COUNT] = {
	{"pistol", 0}, {"smg", 0}, {"shotgun", 0}, {"rocket", 0}, {"explode", 0}, {"reload", 0}, {"click", 0},
	{"hit", 0}, {"thud", 0}, {"horn", 0}, {"siren", 0}, {"ui_move", 0}, {"ui_ok", 0}, {"switch", 0},
	{"amb_city", 0}, {"amb_rain", 0}, {"amb_night", 0}, {"music", 1},
	{"v_gm1", 0}, {"v_gm2", 0}, {"v_hero_throw", 0}, {"v_fem_hall", 0}, {"v_gm_hall", 0}, {"v_hero_played", 0},
	{"v_vince1", 0}, {"v_vince2", 0}, {"v_bark1", 0}, {"v_bark2", 0}, {"v_bark3", 0}, {"amb_factory", 0}
};
typedef struct { s16* data; u32 frames; } Sound;
static Sound snd[S_COUNT];
static int audioOK = 0, romfsOK = 0, loadStep = 0, loadFail = 0, assetsFound = 0;
static Result dspRes = 0;

enum { CH_MUSIC = 0, CH_AMB_CITY, CH_AMB_RAIN, CH_AMB_NIGHT, CH_AMB_FACT, CH_SFX0, N_SFX_CH = 12, CH_VOICE = CH_SFX0 + N_SFX_CH, N_CH = CH_VOICE + 1 };
static ndspWaveBuf wbuf[N_CH];
static int sfxNext = 0;
static int musicLevel = 4, sfxLevel = 4;   // 0..5, changed in the SETTINGS menu
#define SND_RATE 22050.0f

static void audioInit(void) {
	romfsOK = R_SUCCEEDED(romfsInit()) ? 1 : 0;
	dspRes = ndspInit();
	if (R_SUCCEEDED(dspRes)) {
		ndspSetOutputMode(NDSP_OUTPUT_STEREO);
		audioOK = 1;
	}
}

static void audioExit(void) {
	if (audioOK) {
		for (int c = 0; c < N_CH; c++) ndspChnReset(c);
		ndspExit();
	}
	for (int i = 0; i < S_COUNT; i++) if (snd[i].data) linearFree(snd[i].data);
	if (romfsOK) romfsExit();
}

static FILE* ldFile = NULL;
static long  ldSize = 0, ldPos = 0;
static char* ldBuf = NULL;
static int   ldId = -1;

static int loadDone(void) { return loadStep >= S_COUNT && !ldFile; }

static void loadTick(void) {
	if (loadDone()) return;
	if (!ldFile) {
		ldId = loadStep++;
		if (!audioOK) return;
		char path[96];
		if (romfsOK) {
			snprintf(path, sizeof path, "romfs:/%s.raw", sdef[ldId].file);
			ldFile = fopen(path, "rb");
		}
		if (!ldFile) {
			snprintf(path, sizeof path, "sdmc:/3ds/chicago3ds/%s.raw", sdef[ldId].file);
			ldFile = fopen(path, "rb");
		}
		if (!ldFile) { loadFail++; return; }
		fseek(ldFile, 0, SEEK_END);
		ldSize = ftell(ldFile);
		fseek(ldFile, 0, SEEK_SET);
		ldPos = 0;
		ldBuf = (ldSize > 0) ? (char*)linearAlloc((size_t)ldSize) : NULL;
		if (!ldBuf) { fclose(ldFile); ldFile = NULL; loadFail++; return; }
	}
	long want = ldSize - ldPos;
	if (want > 262144) want = 262144;
	size_t got = fread(ldBuf + ldPos, 1, (size_t)want, ldFile);
	ldPos += (long)got;
	if ((long)got != want || ldPos >= ldSize) {
		fclose(ldFile);
		ldFile = NULL;
		if (ldPos == ldSize) {
			DSP_FlushDataCache(ldBuf, (u32)ldSize);
			snd[ldId].data = (s16*)ldBuf;
			snd[ldId].frames = (u32)ldSize / (sdef[ldId].stereo ? 4u : 2u);
			assetsFound++;
		} else {
			linearFree(ldBuf);
			loadFail++;
		}
		ldBuf = NULL;
	}
}

static void chSetVol(int ch, float l, float r) {
	float mix[12];
	memset(mix, 0, sizeof mix);
	mix[0] = l; mix[1] = r;
	ndspChnSetMix(ch, mix);
}

static void chStart(int ch, int id, float rate, float l, float r, int loop) {
	if (!audioOK || !snd[id].data) return;
	ndspChnReset(ch);
	ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
	ndspChnSetRate(ch, rate);
	ndspChnSetFormat(ch, sdef[id].stereo ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
	chSetVol(ch, l, r);
	memset(&wbuf[ch], 0, sizeof(ndspWaveBuf));
	wbuf[ch].data_vaddr = snd[id].data;
	wbuf[ch].nsamples = snd[id].frames;
	wbuf[ch].looping = loop ? true : false;
	ndspChnWaveBufAdd(ch, &wbuf[ch]);
}

// one-shot effect. pan: -1 left .. +1 right, pitch: playback speed multiplier
static void sfxPlay(int id, float vol, float pan, float pitch) {
	if (!audioOK || !snd[id].data) return;
	int ch = CH_SFX0 + (sfxNext++ % N_SFX_CH);
	float v = vol * (float)sfxLevel / 5.0f;
	float l = v * (pan > 0.0f ? 1.0f - pan : 1.0f);
	float r = v * (pan < 0.0f ? 1.0f + pan : 1.0f);
	chStart(ch, id, SND_RATE * pitch, l, r, 0);
}

static int   ambOn = 0, musicOn = 0, ambKind = 0;
static float musicVol = 0.0f, musicApplied = -1.0f, ambCityV = -1.0f, ambRainV = -1.0f, ambNightV = -1.0f, ambFactV = -1.0f;

static void applyAmb(int ch, float v, float* cache) {
	if (fabsf(v - *cache) > 0.004f) { chSetVol(ch, v, v); *cache = v; }
}

// Title music (looping) on the title screen; city / rain / night ambience in the game.
// duck < 1 lowers the ambience (used while the pause menu is open).
static void audioUpdate(int mode, float duck) {
	if (!audioOK) return;
	float target = (mode == M_TITLE) ? 1.0f : 0.0f;
	musicVol += (target - musicVol) * 0.025f;
	if (target > 0.0f && !musicOn && snd[S_MUSIC].data) {
		chStart(CH_MUSIC, S_MUSIC, SND_RATE, 0.0f, 0.0f, 1);
		musicOn = 1; musicApplied = 0.0f;
	}
	if (musicOn) {
		if (target == 0.0f && musicVol < 0.01f) {
			ndspChnReset(CH_MUSIC);
			musicOn = 0; musicVol = 0.0f;
		} else {
			applyAmb(CH_MUSIC, musicVol * (float)musicLevel / 5.0f, &musicApplied);
		}
	}

	int wantAmb = (mode == M_GAME), kind = (gLevel == LV_FACTORY) ? 1 : 0;
	if (ambOn && (!wantAmb || kind != ambKind)) {
		ndspChnReset(CH_AMB_CITY); ndspChnReset(CH_AMB_RAIN); ndspChnReset(CH_AMB_NIGHT); ndspChnReset(CH_AMB_FACT);
		ambOn = 0;
	}
	if (wantAmb && !ambOn) {
		if (kind == 0) {
			chStart(CH_AMB_CITY,  S_AMB_CITY,  SND_RATE, 0.0f, 0.0f, 1);
			chStart(CH_AMB_RAIN,  S_AMB_RAIN,  SND_RATE, 0.0f, 0.0f, 1);
			chStart(CH_AMB_NIGHT, S_AMB_NIGHT, SND_RATE, 0.0f, 0.0f, 1);
		} else {
			chStart(CH_AMB_FACT, S_AMB_FACTORY, SND_RATE, 0.0f, 0.0f, 1);
		}
		ambOn = 1; ambKind = kind; ambCityV = ambRainV = ambNightV = ambFactV = 0.0f;
	}
	if (ambOn && kind == 1) {
		applyAmb(CH_AMB_FACT, 0.85f * (float)sfxLevel / 5.0f * duck, &ambFactV);
	} else if (ambOn) {
		float sv = (float)sfxLevel / 5.0f * duck;
		applyAmb(CH_AMB_CITY,  0.55f * (1.0f - 0.45f * wNight) * (1.0f - 0.5f * wRain) * sv, &ambCityV);
		applyAmb(CH_AMB_RAIN,  0.80f * wRain * sv, &ambRainV);
		applyAmb(CH_AMB_NIGHT, 0.65f * wNight * sv, &ambNightV);
	}
}

// voices play on their own channel so a line is never cut off by gunfire (a new line replaces the old one)
static void voicePlay(int id, float vol) {
	if (!audioOK || !snd[id].data) return;
	float v = vol * (float)(sfxLevel > 0 ? sfxLevel + 1 : 0) / 6.0f;
	if (v > 1.0f) v = 1.0f;
	chStart(CH_VOICE, id, SND_RATE, v, v, 0);
}
static void voiceStop(void) { if (audioOK) ndspChnReset(CH_VOICE); }
// seconds of speech in a clip; if the clip was not loaded, estimate from the text so the timing still works
static float voiceDur(int id, const char* text) {
	if (snd[id].data) return (float)snd[id].frames / SND_RATE;
	return 0.35f + 0.068f * (float)strlen(text);
}

// far-off car horns and the occasional siren, at random places in the stereo field
static int hornT = 300, sirenT = 1500;
static void ambienceTick(void) {
	if (--hornT <= 0) {
		sfxPlay(S_HORN, frange(0.25f, 0.50f), frange(-0.8f, 0.8f), frange(0.85f, 1.20f));
		hornT = (int)frange(300.0f, 900.0f);
	}
	if (--sirenT <= 0) {
		sfxPlay(S_SIREN, 0.22f, frange(-0.9f, 0.9f), frange(0.90f, 1.10f));
		sirenT = (int)frange(1800.0f, 3600.0f);
	}
}

// ---- people ---------------------------------------------------------------------------
// Used for the player and for the pedestrians. Parts are boxes hung off joints; see addPart.
// hair: 0 buzz cut, 1 short, 2 long, 3 cap, 4 beanie, 5 afro, 6 ponytail, 7 wide hat
// top : 0 t-shirt, 1 long sleeves, 2 hoodie, 3 dress
// bottom: 0 long trousers, 1 shorts, 2 skirt / dress (bare legs or tights)
// extra bits: 1 backpack, 2 sunglasses
typedef struct {
	float jr, jg, jb;   // top
	float pr, pg, pb;   // trousers / skirt
	float sr, sg, sb;   // skin
	float hr, hg, hb;   // hair
	float xr, xg, xb;   // shoes
	float ar, ag, ab;   // accessory colour: cap / hat / backpack
	unsigned char hair, top, bottom, extra, bare;
	float w, h;         // body width / height scale
} Outfit;

static const Outfit playerOutfit = {
	0.10f, 0.10f, 0.12f,   0.13f, 0.15f, 0.28f,   0.85f, 0.66f, 0.52f,   0.08f, 0.08f, 0.09f,
	0.07f, 0.07f, 0.08f,   0.12f, 0.12f, 0.14f,   3, 1, 0, 0, 0,   1.0f, 1.0f
};

typedef struct {
	float px, pz, face, phase, moveAmt; // moveAmt: 0 idle, 1 walk, 2 run
	float phoneE;                       // 0..1 phone raised
	float fall;                         // 0..1 fallen over
	int   gun;                          // WP_*
	float aimE;                         // 0..1 weapon raised to aim
	float gunPitch;                     // aim pitch, radians (positive = looking down)
	float punch;                        // 0..1 punch extension
	int   onlyArms;                     // first person: draw just the right arm and weapon
	float py;                           // height of the floor under the feet
	int   useArm; float armOv;          // cutscenes: force the right arm angle (radians, 0 = hanging, 1.57 = forward)
	int   fallFwd;                      // fall forward instead of backward
	int   grenade;                      // holding a grenade in the right hand
	float armLUp;                       // 0..1 left arm raised (hands up / two-handed aim)
} Pose;

// weapon models, in the right arm's frame (the arm hangs along -y, so the weapon points along -y too)
static void addGun(int gun, float swing) {
	const float jx = 0.36f, jy = 1.50f;
	switch (gun) {
	case WP_PISTOL:
		addPart(jx, jy, 0.0f, 0.0f, -0.70f, 0.0f,  0.03f, 0.12f, 0.05f, swing, 0.16f, 0.16f, 0.19f);
		addPart(jx, jy, 0.0f, 0.0f, -0.63f, 0.07f, 0.03f, 0.05f, 0.03f, swing, 0.10f, 0.10f, 0.12f);
		break;
	case WP_SMG:
		addPart(jx, jy, 0.0f, 0.0f, -0.80f, 0.0f,  0.04f, 0.24f, 0.06f, swing, 0.13f, 0.13f, 0.15f);
		addPart(jx, jy, 0.0f, 0.0f, -0.74f, 0.11f, 0.03f, 0.08f, 0.04f, swing, 0.08f, 0.08f, 0.10f);
		break;
	case WP_SHOTGUN:
		addPart(jx, jy, 0.0f, 0.0f, -1.00f, 0.0f,  0.035f, 0.36f, 0.035f, swing, 0.17f, 0.17f, 0.20f);
		addPart(jx, jy, 0.0f, 0.0f, -0.72f, 0.0f,  0.05f,  0.20f, 0.06f,  swing, 0.45f, 0.28f, 0.14f);
		break;
	case WP_ROCKET:
		addPart(jx, jy, 0.0f, 0.0f, -0.88f, 0.0f,  0.11f, 0.50f, 0.11f, swing, 0.28f, 0.33f, 0.22f);
		addPart(jx, jy, 0.0f, 0.0f, -0.38f, 0.0f,  0.13f, 0.08f, 0.13f, swing, 0.12f, 0.12f, 0.14f);
		break;
	default: break;
	}
}

// one arm: long sleeves are one box; short sleeves / sleeveless show skin on the forearm
static void addArm(float jx, float swing, const Outfit* o) {
	int longSleeve = (o->top == 1 || o->top == 2);
	int sleeveless = (o->top == 3);
	if (longSleeve) {
		addPart(jx, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, swing, o->jr, o->jg, o->jb);
	} else {
		if (sleeveless) addPart(jx, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.065f, 0.30f, 0.065f, swing, o->sr, o->sg, o->sb);
		else {
			addPart(jx, 1.50f, 0.0f, 0.0f, -0.12f, 0.0f, 0.075f, 0.12f, 0.075f, swing, o->jr, o->jg, o->jb);
			addPart(jx, 1.50f, 0.0f, 0.0f, -0.42f, 0.0f, 0.065f, 0.18f, 0.065f, swing, o->sr, o->sg, o->sb);
		}
	}
	if (longSleeve) // hands
		addPart(jx, 1.50f, 0.0f, 0.0f, -0.62f, 0.0f, 0.06f, 0.05f, 0.06f, swing, o->sr, o->sg, o->sb);
}

static void addLeg(float jx, float swing, const Outfit* o) {
	float lr = o->pr, lg = o->pg, lb = o->pb;
	if (o->bottom == 2 && o->bare) { lr = o->sr; lg = o->sg; lb = o->sb; }
	if (o->bottom == 1) { // shorts: trousers colour above the knee, skin below
		addPart(jx, 0.90f, 0.0f, 0.0f, -0.20f, 0.0f, 0.09f, 0.20f, 0.10f, swing, o->pr, o->pg, o->pb);
		addPart(jx, 0.90f, 0.0f, 0.0f, -0.62f, 0.0f, 0.075f, 0.22f, 0.085f, swing, o->sr, o->sg, o->sb);
	} else {
		addPart(jx, 0.90f, 0.0f, 0.0f, -0.42f, 0.0f, 0.09f, 0.42f, 0.10f, swing, lr, lg, lb);
	}
	addPart(jx, 0.90f, 0.0f, 0.0f, -0.86f, -0.03f, 0.095f, 0.05f, 0.14f, swing, o->xr, o->xg, o->xb); // shoe
}

static void addHair(const Outfit* o) {
	const float J = 1.62f;
	switch (o->hair) {
	case 0: addPart(0, J, 0, 0.0f, 0.13f, 0.0f, 0.135f, 0.025f, 0.135f, 0.0f, o->hr, o->hg, o->hb); break;
	case 1:
		addPart(0, J, 0, 0.0f, 0.12f, 0.0f, 0.14f, 0.05f, 0.14f, 0.0f, o->hr, o->hg, o->hb);
		addPart(0, J, 0, 0.0f, 0.02f, 0.125f, 0.135f, 0.11f, 0.02f, 0.0f, o->hr, o->hg, o->hb);
		break;
	case 2:
		addPart(0, J, 0, 0.0f, 0.12f, 0.0f, 0.145f, 0.05f, 0.145f, 0.0f, o->hr, o->hg, o->hb);
		addPart(0, J, 0, 0.0f, -0.10f, 0.13f, 0.145f, 0.25f, 0.03f, 0.0f, o->hr, o->hg, o->hb);
		addPart(0, J, 0, -0.14f, -0.02f, 0.02f, 0.02f, 0.14f, 0.12f, 0.0f, o->hr, o->hg, o->hb);
		addPart(0, J, 0,  0.14f, -0.02f, 0.02f, 0.02f, 0.14f, 0.12f, 0.0f, o->hr, o->hg, o->hb);
		break;
	case 3:
		addPart(0, J, 0, 0.0f, 0.14f, 0.0f, 0.15f, 0.05f, 0.15f, 0.0f, o->ar, o->ag, o->ab);
		addPart(0, J, 0, 0.0f, 0.10f, -0.17f, 0.12f, 0.015f, 0.08f, 0.0f, o->ar * 0.8f, o->ag * 0.8f, o->ab * 0.8f);
		break;
	case 4: addPart(0, J, 0, 0.0f, 0.15f, 0.0f, 0.15f, 0.08f, 0.15f, 0.0f, o->ar, o->ag, o->ab); break;
	case 5: addPart(0, J, 0, 0.0f, 0.10f, 0.0f, 0.20f, 0.16f, 0.20f, 0.0f, o->hr, o->hg, o->hb); break;
	case 6:
		addPart(0, J, 0, 0.0f, 0.12f, 0.0f, 0.14f, 0.05f, 0.14f, 0.0f, o->hr, o->hg, o->hb);
		addPart(0, J, 0, 0.0f, 0.02f, 0.125f, 0.135f, 0.11f, 0.02f, 0.0f, o->hr, o->hg, o->hb);
		addPart(0, J, 0, 0.0f, -0.08f, 0.19f, 0.045f, 0.15f, 0.045f, 0.0f, o->hr, o->hg, o->hb);
		break;
	default:
		addPart(0, J, 0, 0.0f, 0.12f, 0.0f, 0.29f, 0.012f, 0.29f, 0.0f, o->ar, o->ag, o->ab);
		addPart(0, J, 0, 0.0f, 0.20f, 0.0f, 0.14f, 0.07f, 0.14f, 0.0f, o->ar * 0.85f, o->ag * 0.85f, o->ab * 0.85f);
		break;
	}
}

static void drawPerson(const Pose* p, const Outfit* o) {
	float walk = p->moveAmt < 1.0f ? p->moveAmt : 1.0f;
	float run  = p->moveAmt > 1.0f ? p->moveAmt - 1.0f : 0.0f;
	float fl   = p->fall;
	gPx = p->px; gPz = p->pz; gFace = p->face; gPy = p->py;
	gSx = o->w; gSy = o->h;
	gLean = -(0.04f * walk + 0.18f * run) * (1.0f - fl) + (p->fallFwd ? -1.5f : 1.5f) * fl; // lean into a run, fall over
	gBob  = fabsf(cosf(p->phase)) * 0.05f * p->moveAmt * (1.0f - fl) + 0.16f * fl;
	float s = sinf(p->phase) * (0.45f * walk + 0.45f * run) * (1.0f - fl);    // leg / arm swing

	// right arm: swings, holds a weapon low, aims it, punches, or holds up the phone
	float armR = s * 0.9f;
	if (p->gun != WP_FISTS) armR = 0.55f + (1.5708f - p->gunPitch - 0.55f) * p->aimE;
	armR += (1.5708f - armR) * p->punch;
	armR += (1.65f - armR) * p->phoneE;
	if (p->useArm) armR = p->armOv;

	if (!p->onlyArms) {
		addLeg(-0.15f,  s, o);
		addLeg( 0.15f, -s, o);
		addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.36f, 0.0f, 0.27f, 0.36f, 0.14f, 0.0f, o->jr, o->jg, o->jb);    // torso
		if (o->bottom == 2) { // skirt / dress skirt
			float kr = (o->top == 3) ? o->jr : o->pr, kg = (o->top == 3) ? o->jg : o->pg, kb = (o->top == 3) ? o->jb : o->pb;
			addPart(0.0f, 0.90f, 0.0f, 0.0f, -0.08f, 0.0f, 0.295f, 0.26f, 0.165f, 0.0f, kr, kg, kb);
		}
		if (o->top == 2) // hood
			addPart(0.0f, 1.55f, 0.0f, 0.0f, 0.04f, 0.125f, 0.17f, 0.10f, 0.06f, 0.0f, o->jr * 0.9f, o->jg * 0.9f, o->jb * 0.9f);
		addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.00f, 0.0f, 0.13f, 0.14f, 0.13f, 0.0f, o->sr, o->sg, o->sb);    // head
		addHair(o);
		if (o->extra & 2) // sunglasses
			addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.03f, -0.135f, 0.12f, 0.025f, 0.012f, 0.0f, 0.05f, 0.05f, 0.07f);
		if (o->extra & 1) // backpack
			addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.42f, 0.21f, 0.19f, 0.25f, 0.07f, 0.0f, o->ar, o->ag, o->ab);
		addArm(-0.36f, p->armLUp > 0.0f ? -s * 0.9f * (1.0f - p->armLUp) + 2.6f * p->armLUp : -s * 0.9f, o);
		if (!(o->extra & 2)) { // eyes (so close-ups have a face)
			addPart(0.0f, 1.62f, 0.0f, -0.055f, 0.03f, -0.133f, 0.022f, 0.016f, 0.010f, 0.0f, 0.95f, 0.95f, 0.95f);
			addPart(0.0f, 1.62f, 0.0f,  0.055f, 0.03f, -0.133f, 0.022f, 0.016f, 0.010f, 0.0f, 0.95f, 0.95f, 0.95f);
			addPart(0.0f, 1.62f, 0.0f, -0.055f, 0.03f, -0.1345f, 0.011f, 0.012f, 0.010f, 0.0f, 0.06f, 0.05f, 0.05f);
			addPart(0.0f, 1.62f, 0.0f,  0.055f, 0.03f, -0.1345f, 0.011f, 0.012f, 0.010f, 0.0f, 0.06f, 0.05f, 0.05f);
		}
		if (o->extra & 4) { // suit: white shirt front and a dark tie
			addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.42f, -0.141f, 0.075f, 0.26f, 0.008f, 0.0f, 0.92f, 0.92f, 0.94f);
			addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.36f, -0.147f, 0.022f, 0.22f, 0.008f, 0.0f, 0.10f, 0.08f, 0.09f);
		}
	}
	addArm(0.36f, armR, o);
	if (p->phoneE > 0.05f)       // glowing phone in the raised hand
		addPart(0.36f, 1.50f, 0.0f, 0.0f, -0.66f, 0.0f, 0.07f, 0.03f, 0.11f, armR, 0.20f, 0.90f, 1.00f);
	else if (p->grenade)
		addPart(0.36f, 1.50f, 0.0f, 0.0f, -0.70f, 0.0f, 0.06f, 0.075f, 0.06f, armR, 0.12f, 0.20f, 0.10f);
	else if (p->gun != WP_FISTS)
		addGun(p->gun, armR);
	gSx = 1.0f; gSy = 1.0f; gPy = 0.0f;
}

// ---- pedestrians ------------------------------------------------------------------------
// They stroll along the sidewalks and park paths. Four hits from anything kills one (see DMG_PER_SHOT).
enum { NPC_WALK = 0, NPC_DYING = 1, NPC_GONE = 2 };
typedef struct {
	int line, dir, state, hp, timer, hitFlash, panic;
	float s, speed, phase;
	Outfit outfit;
} Npc;
static Npc npcs[N_NPC];
static int kills = 0;

// Armed gang members (story mode). Cutscene actors use the same struct with cut = 1.
enum { FS_IDLE = 0, FS_ALERT = 1, FS_DEAD = 2, FS_OFF = 3 };
typedef struct {
	float x, z, y, yaw, homeX, homeZ, phase, moveAmt, aimE, strafeDir;
	int   stage, state, hp, gun, cool, burst, hitFlash, deadT, strafeT, delay, cut, female;
	Outfit outfit;
} Foe;
static Foe foes[N_FOE];
static int nFoe = 0;
static int damageFoe(int i, int dmg);

static void npcPos(const Npc* n, float* x, float* z) {
	const WLine* l = &wl[n->line];
	if (l->axis == 0) { *x = n->s; *z = l->lat; } else { *x = l->lat; *z = n->s; }
}

static const float* pick(const float* tab, int n, int stride) { return tab + stride * ((int)(frand() * (float)n) % n); }

// A wide spread of skin tones, hair, clothes and body shapes: no two people look much alike.
static void randOutfit(Outfit* o) {
	static const float skin[12][3] = {
		{0.98f, 0.84f, 0.73f}, {0.94f, 0.76f, 0.62f}, {0.87f, 0.68f, 0.52f}, {0.80f, 0.60f, 0.43f},
		{0.72f, 0.52f, 0.36f}, {0.62f, 0.43f, 0.29f}, {0.52f, 0.35f, 0.24f}, {0.42f, 0.28f, 0.19f},
		{0.33f, 0.22f, 0.15f}, {0.26f, 0.17f, 0.12f}, {0.90f, 0.72f, 0.58f}, {0.76f, 0.56f, 0.40f}
	};
	static const float hair[14][3] = {
		{0.05f, 0.04f, 0.04f}, {0.08f, 0.07f, 0.06f}, {0.20f, 0.12f, 0.07f}, {0.35f, 0.21f, 0.11f}, {0.50f, 0.33f, 0.18f},
		{0.55f, 0.20f, 0.10f}, {0.85f, 0.70f, 0.38f}, {0.92f, 0.85f, 0.60f}, {0.55f, 0.55f, 0.58f}, {0.88f, 0.88f, 0.90f},
		{0.80f, 0.35f, 0.12f}, {0.20f, 0.45f, 0.85f}, {0.88f, 0.35f, 0.65f}, {0.25f, 0.70f, 0.50f}
	};
	static const float tops[20][3] = {
		{0.85f, 0.20f, 0.20f}, {0.20f, 0.50f, 0.85f}, {0.90f, 0.75f, 0.20f}, {0.25f, 0.70f, 0.35f}, {0.88f, 0.88f, 0.90f},
		{0.55f, 0.25f, 0.65f}, {0.95f, 0.55f, 0.15f}, {0.30f, 0.30f, 0.35f}, {0.95f, 0.45f, 0.60f}, {0.15f, 0.62f, 0.62f},
		{0.10f, 0.12f, 0.20f}, {0.62f, 0.15f, 0.22f}, {0.75f, 0.68f, 0.52f}, {0.35f, 0.48f, 0.30f}, {0.95f, 0.92f, 0.80f},
		{0.45f, 0.62f, 0.90f}, {0.80f, 0.30f, 0.45f}, {0.60f, 0.40f, 0.25f}, {0.18f, 0.18f, 0.18f}, {0.98f, 0.80f, 0.25f}
	};
	static const float pants[10][3] = {
		{0.20f, 0.30f, 0.52f}, {0.12f, 0.17f, 0.32f}, {0.12f, 0.12f, 0.14f}, {0.30f, 0.30f, 0.33f}, {0.62f, 0.52f, 0.36f},
		{0.35f, 0.38f, 0.22f}, {0.88f, 0.88f, 0.86f}, {0.45f, 0.14f, 0.18f}, {0.55f, 0.42f, 0.28f}, {0.30f, 0.45f, 0.65f}
	};
	static const float shoes[5][3] = {{0.10f, 0.10f, 0.11f}, {0.85f, 0.85f, 0.85f}, {0.40f, 0.26f, 0.16f}, {0.75f, 0.20f, 0.18f}, {0.20f, 0.35f, 0.65f}};
	const float *s = pick(&skin[0][0], 12, 3), *t = pick(&tops[0][0], 20, 3), *p = pick(&pants[0][0], 10, 3);
	const float *x = pick(&shoes[0][0], 5, 3), *a = pick(&tops[0][0], 20, 3);
	// dyed hair is rare: pick from the natural colours most of the time
	const float* h = (frand() < 0.06f) ? pick(&hair[11][0], 3, 3) : pick(&hair[0][0], 11, 3);
	float shade = frange(0.85f, 1.10f);
	o->jr = t[0] * shade; o->jg = t[1] * shade; o->jb = t[2] * shade;
	o->pr = p[0]; o->pg = p[1]; o->pb = p[2];
	o->sr = s[0]; o->sg = s[1]; o->sb = s[2];
	o->hr = h[0]; o->hg = h[1]; o->hb = h[2];
	o->xr = x[0]; o->xg = x[1]; o->xb = x[2];
	o->ar = a[0] * 0.9f; o->ag = a[1] * 0.9f; o->ab = a[2] * 0.9f;
	float k = frand();
	o->top = k < 0.34f ? 0 : k < 0.58f ? 1 : k < 0.76f ? 2 : 3;
	k = frand();
	o->bottom = (o->top == 3) ? 2 : (k < 0.12f ? 2 : k < 0.34f ? 1 : 0);
	o->bare = (frand() < 0.55f) ? 1 : 0;
	k = frand();
	o->hair = k < 0.10f ? 0 : k < 0.38f ? 1 : k < 0.58f ? 2 : k < 0.68f ? 3 : k < 0.74f ? 4 : k < 0.82f ? 5 : k < 0.94f ? 6 : 7;
	o->extra = (frand() < 0.18f ? 1 : 0) | (frand() < 0.15f ? 2 : 0);
	o->w = frange(0.88f, 1.20f);
	o->h = frange(0.90f, 1.08f);
}

static void placeNpc(Npc* n, float px, float pz) {
	int best = 0; float bs = 0.0f;
	for (int tries = 0; tries < 14; tries++) {
		int ln = (frand() < 0.75f || nWl <= nWlSide) ? (int)(frand() * (float)nWlSide) % nWlSide
		                                             : nWlSide + (int)(frand() * (float)(nWl - nWlSide)) % (nWl - nWlSide);
		const WLine* l = &wl[ln];
		float pp = (l->axis == 0) ? px : pz, pl = (l->axis == 0) ? pz : px;
		if (fabsf(l->lat - pl) > 75.0f) continue;               // keep the crowd near the player
		float s = pp + frange(-75.0f, 75.0f);
		if (s < l->lo) s = l->lo;
		if (s > l->hi) s = l->hi;
		float x = (l->axis == 0) ? s : l->lat, z = (l->axis == 0) ? l->lat : s;
		float d2 = (x - px) * (x - px) + (z - pz) * (z - pz);
		if (d2 > 24.0f * 24.0f) { best = ln; bs = s; break; }
		best = ln; bs = s;
	}
	const WLine* l = &wl[best];
	n->line = best;
	n->s    = bs < l->lo ? l->lo : (bs > l->hi ? l->hi : bs);
	n->dir  = (frand() < 0.5f) ? 1 : -1;
	n->state = NPC_WALK; n->hp = 100; n->timer = 0; n->hitFlash = 0; n->panic = 0;
	n->speed = frange(0.035f, 0.060f);
	n->phase = frange(0.0f, 6.0f);
	randOutfit(&n->outfit);
}

static void initNpcs(float px, float pz) {
	for (int i = 0; i < nNpcOn; i++) placeNpc(&npcs[i], px, pz);
}

static void updateNpcs(float px, float pz) {
	for (int i = 0; i < nNpcOn; i++) {
		Npc* n = &npcs[i];
		if (n->hitFlash > 0) n->hitFlash--;
		if (n->state == NPC_WALK) {
			const WLine* l = &wl[n->line];
			float sp = (n->panic > 0) ? n->speed * 2.6f : n->speed;
			if (n->panic > 0) n->panic--;
			n->s += (float)n->dir * sp;
			if (n->s > l->hi) { n->s = l->hi; n->dir = -n->dir; }
			if (n->s < l->lo) { n->s = l->lo; n->dir = -n->dir; }
			n->phase += (n->panic > 0) ? 0.20f : 0.09f;
		} else if (n->state == NPC_DYING) {
			if (++n->timer > 240) { n->state = NPC_GONE; n->timer = 0; }
		} else { // gone: somebody else strolls in later, away from the player
			if (++n->timer > 420) placeNpc(n, px, pz);
		}
	}
}

static void drawNpcs(float camx, float camz) {
	for (int i = 0; i < nNpcOn; i++) {
		const Npc* n = &npcs[i];
		if (n->state == NPC_GONE) continue;
		Pose p;
		memset(&p, 0, sizeof p);
		npcPos(n, &p.px, &p.pz);
		float dx = p.px - camx, dz = p.pz - camz;
		if (dx * dx + dz * dz > 115.0f * 115.0f) continue; // too far to matter
		int axis = wl[n->line].axis;
		p.face    = (axis == 0) ? -(float)n->dir * 1.5708f : (n->dir > 0 ? 3.14159f : 0.0f);
		p.phase   = n->phase;
		p.moveAmt = (n->state == NPC_WALK) ? (n->panic > 0 ? 2.0f : 1.0f) : 0.0f;
		p.fall    = (n->state == NPC_DYING) ? fminf((float)n->timer / 16.0f, 1.0f) : 0.0f;
		Outfit o = n->outfit;
		if (n->hitFlash > 0) { o.jr = 1.0f; o.jg = 0.15f; o.jb = 0.15f; }
		drawPerson(&p, &o);
	}
}

// ---- weapons, bullets, explosions ---------------------------------------------------------------
typedef struct {
	const char* name;
	int clipMax, startReserve, interval, reloadT, autofire, pellets;
	float spread, kick, shake;
} WeaponDef;
static const WeaponDef wdef[N_WP] = {
	{"FISTS",     0,   0, 22,   0, 0, 0, 0.000f, 0.000f, 0.00f},
	{"PISTOL",   12,  96, 14,  80, 0, 1, 0.006f, 0.035f, 0.03f},
	{"SMG",      30, 180,  4, 100, 1, 1, 0.020f, 0.012f, 0.02f},
	{"SHOTGUN",   6,  36, 46, 110, 0, 6, 0.060f, 0.100f, 0.18f},
	{"ROCKET",    1,   8, 90, 140, 0, 1, 0.000f, 0.120f, 0.35f},
};
#define DMG_PER_SHOT 25  // an NPC has 100 hp, so every weapon needs 4 shots (one shotgun blast = one shot)

static int   wClip[N_WP], wRes[N_WP];
static int   curW = WP_PISTOL;
static int   fireCD = 0, reloadT = 0, fireRecent = 0, punchT = 0, hitMark = 0, hitKill = 0;
static float kick = 0.0f, shake = 0.0f;

static void dirFrom(float yaw, float pitch, float d[3]) {
	d[0] = -sinf(yaw) * cosf(pitch);
	d[1] = -sinf(pitch);
	d[2] = -cosf(yaw) * cosf(pitch);
}

static float carH(const Car* c) {
	return c->type == CT_BUS ? 2.9f : c->type == CT_TRUCK ? 2.7f : c->type == CT_SUV ? 2.1f : 1.9f;
}

static int slab(float o, float d, float lo, float hi, float* t0, float* t1) {
	if (fabsf(d) < 1e-6f) return (o >= lo && o <= hi);
	float a = (lo - o) / d, b = (hi - o) / d;
	if (a > b) { float t = a; a = b; b = t; }
	if (a > *t0) *t0 = a;
	if (b < *t1) *t1 = b;
	return *t0 <= *t1;
}

// Nearest thing the ray o + t*d hits for t in (tmin, tmax): buildings, cars, pedestrians, the ground.
// Returns t, and the pedestrian index in *npc (or -1 when it hit scenery / nothing).
static float castRay(const float o[3], const float d[3], float tmin, float tmax, int* npc) {
	float best = tmax;
	*npc = -1;
	for (int i = 0; i < nSolid; i++) {
		const Solid* b = &solids[i];
		float t0 = tmin, t1 = best;
		if (slab(o[0], d[0], b->x0, b->x1, &t0, &t1) && slab(o[1], d[1], b->y0, b->y1, &t0, &t1) &&
		    slab(o[2], d[2], b->z0, b->z1, &t0, &t1)) { best = t0; *npc = -1; }
	}
	for (int i = 0; i < nCarOn; i++) {
		float cx, cz, hx, hz;
		carBox(&cars[i], &cx, &cz, &hx, &hz);
		float t0 = tmin, t1 = best;
		if (slab(o[0], d[0], cx - hx, cx + hx, &t0, &t1) && slab(o[1], d[1], 0.0f, carH(&cars[i]), &t0, &t1) &&
		    slab(o[2], d[2], cz - hz, cz + hz, &t0, &t1)) { best = t0; *npc = -1; }
	}
	for (int i = 0; i < nNpcOn + nFoe; i++) {
		float x, z, fy0 = 0.0f;
		if (i < nNpcOn) {
			if (npcs[i].state != NPC_WALK) continue;
			npcPos(&npcs[i], &x, &z);
		} else {
			const Foe* f = &foes[i - nNpcOn];
			if (f->state == FS_DEAD || f->state == FS_OFF) continue;
			x = f->x; z = f->z; fy0 = f->y;
		}
		float rx = o[0] - x, rz = o[2] - z;
		float a = d[0] * d[0] + d[2] * d[2];
		if (a < 1e-8f) continue;
		float bq = rx * d[0] + rz * d[2];
		float c  = rx * rx + rz * rz - 0.45f * 0.45f;
		float disc = bq * bq - a * c;
		if (disc < 0.0f) continue;
		float t = (-bq - sqrtf(disc)) / a;
		if (t > tmin && t < best) {
			float y = o[1] + d[1] * t - fy0;
			if (y >= 0.0f && y <= 2.0f) { best = t; *npc = (i < nNpcOn) ? i : N_NPC + (i - nNpcOn); }
		}
	}
	if (d[1] < -1e-5f && gLevel == LV_CITY) {
		float t = -o[1] / d[1];
		if (t > tmin && t < best) { best = t; *npc = -1; }
	}
	return best;
}

static int pointSolid(const float p[3]) {
	if (p[1] <= 0.2f) return 1;
	for (int i = 0; i < nSolid; i++) {
		const Solid* b = &solids[i];
		if (p[0] > b->x0 && p[0] < b->x1 && p[2] > b->z0 && p[2] < b->z1 && p[1] > b->y0 && p[1] < b->y1) return 1;
	}
	for (int i = 0; i < nCarOn; i++) {
		float cx, cz, hx, hz;
		carBox(&cars[i], &cx, &cz, &hx, &hz);
		if (fabsf(p[0] - cx) < hx && fabsf(p[2] - cz) < hz && p[1] < carH(&cars[i])) return 1;
	}
	return 0;
}

// returns 1 if this hit killed the pedestrian
static int damageNpc(int i, int dmg) {
	if (i >= N_NPC) return damageFoe(i - N_NPC, dmg);
	Npc* n = &npcs[i];
	if (n->state != NPC_WALK) return 0;
	n->hp -= dmg;
	n->hitFlash = 8;
	n->panic = 240;
	if (n->hp <= 0) {
		n->state = NPC_DYING; n->timer = 0;
		kills++;
		if (curW != WP_FISTS) wRes[curW] += wdef[curW].clipMax / 2 + 1; // a little ammo for each kill
		sfxPlay(S_THUD, 0.9f, 0.0f, frange(0.9f, 1.1f));
		return 1;
	}
	sfxPlay(S_HIT, 0.6f, 0.0f, frange(0.95f, 1.1f));
	return 0;
}

// visual effects: tracers, sparks / smoke / debris, rockets, explosions
#define N_TR  20
#define N_SPK 28
#define N_RK  3
#define N_BM  3
typedef struct { float a[3], b[3]; int life; } Tracer;
typedef struct { float p[3], v[3], size, r, g, b; int life, maxLife; } Spark;
typedef struct { int on; float p[3], d[3]; int life; } Rocket;
typedef struct { int on; float p[3]; int life; } Boom;
static Tracer trs[N_TR];  static int trNext = 0;
static Spark  sps[N_SPK]; static int spNext = 0;
static Rocket rks[N_RK];
static Boom   bms[N_BM];

static void addTracerFx(const float a[3], const float b[3]) {
	Tracer* t = &trs[trNext];
	trNext = (trNext + 1) % N_TR;
	memcpy(t->a, a, sizeof t->a); memcpy(t->b, b, sizeof t->b);
	t->life = 6;
}

static void addSpark(const float p[3], const float v[3], float size, int life, float r, float g, float b) {
	Spark* s = &sps[spNext];
	spNext = (spNext + 1) % N_SPK;
	memcpy(s->p, p, sizeof s->p);
	if (v) memcpy(s->v, v, sizeof s->v); else { s->v[0] = s->v[1] = s->v[2] = 0.0f; }
	s->size = size; s->life = life; s->maxLife = life;
	s->r = r; s->g = g; s->b = b;
}

static void clearFx(void) {
	memset(trs, 0, sizeof trs); memset(sps, 0, sizeof sps);
	memset(rks, 0, sizeof rks); memset(bms, 0, sizeof bms);
}

static void explodeAt(const float p[3], float px, float pz) {
	for (int i = 0; i < N_BM; i++) {
		if (!bms[i].on) { bms[i].on = 1; bms[i].life = 28; memcpy(bms[i].p, p, sizeof bms[i].p); break; }
	}
	float dist = sqrtf((p[0] - px) * (p[0] - px) + (p[2] - pz) * (p[2] - pz));
	float vol = 1.1f - dist / 70.0f;
	sfxPlay(S_EXPLODE, vol < 0.25f ? 0.25f : (vol > 1.0f ? 1.0f : vol), 0.0f, frange(0.92f, 1.05f));
	float sh = 1.4f - dist / 20.0f;
	if (sh > shake) shake = sh > 1.2f ? 1.2f : sh;
	for (int k = 0; k < 8; k++) {
		float v[3] = { frange(-0.25f, 0.25f), frange(0.15f, 0.45f), frange(-0.25f, 0.25f) };
		addSpark(p, v, frange(0.10f, 0.22f), 26, 1.0f, frange(0.45f, 0.75f), 0.15f);
	}
	for (int i = 0; i < nNpcOn; i++) { // splash damage
		if (npcs[i].state != NPC_WALK) continue;
		float x, z;
		npcPos(&npcs[i], &x, &z);
		float dx = x - p[0], dy = 0.9f - p[1], dz = z - p[2];
		if (dx * dx + dy * dy + dz * dz < 7.0f * 7.0f) {
			hitMark = 10;
			if (damageNpc(i, DMG_PER_SHOT)) hitKill = 1; else if (hitKill != 1) hitKill = 0;
		}
	}
}

static void updateRockets(float px, float pz) {
	for (int i = 0; i < N_RK; i++) {
		Rocket* r = &rks[i];
		if (!r->on) continue;
		for (int k = 0; k < 3; k++) r->p[k] += r->d[k] * 0.9f;
		r->life--;
		float up[3] = { 0.0f, 0.01f, 0.0f };
		addSpark(r->p, up, 0.16f, 20, 0.75f, 0.75f, 0.78f); // smoke trail
		int boom = (r->life <= 0) || pointSolid(r->p);
		for (int j = 0; j < nNpcOn && !boom; j++) {
			if (npcs[j].state != NPC_WALK) continue;
			float x, z;
			npcPos(&npcs[j], &x, &z);
			if ((x - r->p[0]) * (x - r->p[0]) + (z - r->p[2]) * (z - r->p[2]) < 0.9f * 0.9f && r->p[1] < 2.0f) boom = 1;
		}
		if (boom) { r->on = 0; explodeAt(r->p, px, pz); }
	}
}

static void updateFx(void) {
	for (int i = 0; i < N_TR; i++) if (trs[i].life > 0) trs[i].life--;
	for (int i = 0; i < N_SPK; i++) {
		Spark* s = &sps[i];
		if (s->life <= 0) continue;
		s->life--;
		for (int k = 0; k < 3; k++) s->p[k] += s->v[k];
		s->v[1] -= 0.012f;
	}
	for (int i = 0; i < N_BM; i++) if (bms[i].on && --bms[i].life <= 0) bms[i].on = 0;
	if (hitMark > 0) hitMark--;
	if (fireCD > 0) fireCD--;
	if (fireRecent > 0) fireRecent--;
	if (punchT > 0) punchT--;
	kick *= 0.88f;
	shake *= 0.90f;
}

static void addTracerQuads(const float a[3], const float b[3], float w, float r, float g, float bl, float alpha) {
	float dx = b[0] - a[0], dz = b[2] - a[2];
	float hl = sqrtf(dx * dx + dz * dz);
	float qx = w, qz = 0.0f;
	if (hl > 0.001f) { qx = -dz / hl * w; qz = dx / hl * w; }
	float v0[3] = {a[0] - qx, a[1], a[2] - qz}, v1[3] = {a[0] + qx, a[1], a[2] + qz};
	float v2[3] = {b[0] + qx, b[1], b[2] + qz}, v3[3] = {b[0] - qx, b[1], b[2] - qz};
	float u0[3] = {a[0], a[1] - w, a[2]}, u1[3] = {a[0], a[1] + w, a[2]};
	float u2[3] = {b[0], b[1] + w, b[2]}, u3[3] = {b[0], b[1] - w, b[2]};
	gAlpha = alpha;
	quad(v0, v1, v2, v3, r, g, bl);
	quad(u0, u1, u2, u3, r, g, bl);
	gAlpha = 1.0f;
}

// all blended: drawn in the transparent pass
static void drawFx(void) {
	for (int i = 0; i < N_TR; i++) {
		if (trs[i].life <= 0) continue;
		addTracerQuads(trs[i].a, trs[i].b, 0.025f, 1.0f, 0.92f, 0.55f, 0.55f * (float)trs[i].life / 6.0f);
	}
	for (int i = 0; i < N_SPK; i++) {
		const Spark* s = &sps[i];
		if (s->life <= 0) continue;
		gAlpha = fminf(1.0f, (float)s->life / (float)s->maxLife * 1.6f);
		addBoxY(s->p[0] - s->size, s->p[2] - s->size, s->p[0] + s->size, s->p[2] + s->size,
		        s->p[1] - s->size, s->p[1] + s->size, s->r, s->g, s->b);
	}
	for (int i = 0; i < N_BM; i++) {
		const Boom* b = &bms[i];
		if (!b->on) continue;
		float u = 1.0f - (float)b->life / 28.0f;
		float r = 1.5f + u * 6.5f;
		gAlpha = 0.80f * (1.0f - u);
		addBoxY(b->p[0] - r, b->p[2] - r, b->p[0] + r, b->p[2] + r, b->p[1] - r, b->p[1] + r, 1.0f, 0.45f, 0.10f);
		gAlpha = 0.95f * (1.0f - u * u);
		addBoxY(b->p[0] - r * 0.55f, b->p[2] - r * 0.55f, b->p[0] + r * 0.55f, b->p[2] + r * 0.55f,
		        b->p[1] - r * 0.55f, b->p[1] + r * 0.55f, 1.0f, 0.90f, 0.35f);
	}
	for (int i = 0; i < N_RK; i++) {
		const Rocket* r = &rks[i];
		if (!r->on) continue;
		gAlpha = 1.0f;
		addBoxY(r->p[0] - 0.12f, r->p[2] - 0.12f, r->p[0] + 0.12f, r->p[2] + 0.12f, r->p[1] - 0.12f, r->p[1] + 0.12f, 0.35f, 0.38f, 0.32f);
		addBoxY(r->p[0] - r->d[0] * 0.35f - 0.10f, r->p[2] - r->d[2] * 0.35f - 0.10f,
		        r->p[0] - r->d[0] * 0.35f + 0.10f, r->p[2] - r->d[2] * 0.35f + 0.10f,
		        r->p[1] - r->d[1] * 0.35f - 0.10f, r->p[1] - r->d[1] * 0.35f + 0.10f, 1.0f, 0.65f, 0.15f);
	}
	gAlpha = 1.0f;
}

typedef struct {
	float cam[3];       // where the shot starts (the camera) ...
	float yaw, pitch;   // ... and where the crosshair points
	float tmin;         // ignore anything closer than this (the player's own body)
	float muzzle[3];    // where the tracer / flash is drawn
	float aimE;         // 0..1, hip fire is less accurate
} ShotCtx;

static void fireShot(const ShotCtx* c) {
	const WeaponDef* w = &wdef[curW];
	char hitN[N_NPC + N_FOE];
	memset(hitN, 0, sizeof hitN);
	int anyHit = 0, anyKill = 0;
	float spread = w->spread * (1.0f + 2.2f * (1.0f - c->aimE));

	if (curW == WP_ROCKET) {
		float d[3];
		dirFrom(c->yaw, c->pitch, d);
		int dummy;
		float t = castRay(c->cam, d, c->tmin, 90.0f, &dummy);
		float tgt[3] = { c->cam[0] + d[0] * t, c->cam[1] + d[1] * t, c->cam[2] + d[2] * t };
		float md[3] = { tgt[0] - c->muzzle[0], tgt[1] - c->muzzle[1], tgt[2] - c->muzzle[2] };
		float ml = sqrtf(md[0] * md[0] + md[1] * md[1] + md[2] * md[2]);
		if (ml < 0.01f) { md[0] = d[0]; md[1] = d[1]; md[2] = d[2]; ml = 1.0f; }
		for (int i = 0; i < N_RK; i++) {
			if (rks[i].on) continue;
			rks[i].on = 1; rks[i].life = 140;
			for (int k = 0; k < 3; k++) { rks[i].p[k] = c->muzzle[k]; rks[i].d[k] = md[k] / ml; }
			break;
		}
	} else {
		for (int k = 0; k < w->pellets; k++) {
			float d[3];
			dirFrom(c->yaw + (frand() - 0.5f) * 2.0f * spread, c->pitch + (frand() - 0.5f) * 2.0f * spread, d);
			int n;
			float t = castRay(c->cam, d, c->tmin, 120.0f, &n);
			float hp[3] = { c->cam[0] + d[0] * t, c->cam[1] + d[1] * t, c->cam[2] + d[2] * t };
			addTracerFx(c->muzzle, hp);
			if (n >= 0) {
				if (!hitN[n]) {
					hitN[n] = 1; anyHit = 1;
					if (damageNpc(n, DMG_PER_SHOT)) anyKill = 1;
				}
				float v[3] = { frange(-0.05f, 0.05f), 0.04f, frange(-0.05f, 0.05f) };
				addSpark(hp, v, 0.07f, 10, 1.0f, 0.35f, 0.25f);
			} else if (t < 119.0f) {
				float v[3] = { frange(-0.06f, 0.06f), 0.05f, frange(-0.06f, 0.06f) };
				addSpark(hp, v, 0.07f, 10, 0.85f, 0.80f, 0.65f);
			}
		}
		if (anyHit) { hitMark = 10; hitKill = anyKill; }
	}
	addSpark(c->muzzle, NULL, curW == WP_SHOTGUN || curW == WP_ROCKET ? 0.30f : 0.16f, 3, 1.0f, 0.90f, 0.45f); // muzzle flash
	sfxPlay(S_PISTOL + (curW - WP_PISTOL), 1.0f, 0.0f, frange(0.97f, 1.03f)); // S_PISTOL, S_SMG, S_SHOTGUN, S_ROCKET
	kick += w->kick;
	if (kick > 0.25f) kick = 0.25f;
	if (w->shake > shake) shake = w->shake;
}

// fists: hit the first pedestrian within reach, in front of the player
static void punch(float px, float pz, float face) {
	float fx = -sinf(face), fz = -cosf(face);
	for (int i = 0; i < nNpcOn; i++) {
		if (npcs[i].state != NPC_WALK) continue;
		float x, z;
		npcPos(&npcs[i], &x, &z);
		float dx = x - px, dz = z - pz;
		float dist = sqrtf(dx * dx + dz * dz);
		if (dist < 1.8f && (dx * fx + dz * fz) > 0.4f * dist) {
			hitMark = 10;
			hitKill = damageNpc(i, DMG_PER_SHOT);
			return;
		}
	}
}

// ---- bottom screen: street map ----------------------------------------------
// North is up (-Z). The map scrolls so the player circle always stays in the centre.
#define MAP_S   2.2f    // pixels per world unit
#define MAP_CX  160.0f
#define MAP_CY  88.0f
#define MAP_H   176.0f  // map viewport height; buttons and the city text sit below it
#define BTN_Y0  178
#define BTN_Y1  204

// Streets running north-south, west to east (x = RX[k]); streets running east-west, north to south (z = RZ[k]).
static const char* vNames[NRX] = {
	"Western Ave", "Ashland Ave", "Halsted St", "Clark St", "State St", "Michigan Ave"
};
static const char* hNames[NRZ] = {
	"Diversey Pkwy", "Fullerton Ave", "North Ave", "Division St", "Chicago Ave", "Madison St"
};
// Chicago neighborhoods, one per block (rows north to south, columns west to east).
static const char* hoods[NRZ - 1][NRX - 1] = {
	{ "Logan Square",      "Roscoe Village", "Lakeview",      "Wrigleyville",  "Uptown"        },
	{ "Bucktown",          "Lincoln Park",   "Old Town",      "Gold Coast",    "Streeterville" },
	{ "Wicker Park",       "West Town",      "River North",   "Near North",    "Navy Pier"     },
	{ "Ukrainian Village", "Greektown",      "The Loop",      "South Loop",    "Grant Park"    },
	{ "Pilsen",            "Little Italy",   "Printers Row",  "Prairie Ave",   "Museum Campus" }
};

static C2D_TextBuf gStatic, gDyn;
static C2D_Text vLabel[NRX], hLabel[NRZ], btnHack, btnBlackout, txtHint;

static void initMapText(void) {
	gStatic = C2D_TextBufNew(1024);
	gDyn    = C2D_TextBufNew(4096);
	for (int k = 0; k < NRX; k++) { C2D_TextParse(&vLabel[k], gStatic, vNames[k]); C2D_TextOptimize(&vLabel[k]); }
	for (int k = 0; k < NRZ; k++) { C2D_TextParse(&hLabel[k], gStatic, hNames[k]); C2D_TextOptimize(&hLabel[k]); }
	C2D_TextParse(&btnHack, gStatic, "HACK LIGHTS");  C2D_TextOptimize(&btnHack);
	C2D_TextParse(&btnBlackout, gStatic, "BLACKOUT"); C2D_TextOptimize(&btnBlackout);
	C2D_TextParse(&txtHint, gStatic, "START: PAUSE"); C2D_TextOptimize(&txtHint);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static u32 neon(int r, int g, int b, float a) {
	a = clampf(a, 0.0f, 1.0f);
	return C2D_Color32(r, g, b, (int)(a * 255.0f));
}

// text that changes: parsed into the per-frame buffer. align 0 = left, 1 = centre, 2 = right
static void drawStr(float x, float y, float scale, u32 clr, int align, const char* s) {
	C2D_Text t;
	C2D_TextParse(&t, gDyn, s);
	C2D_TextOptimize(&t);
	float w, h;
	C2D_TextGetDimensions(&t, scale, scale, &w, &h);
	if (align == 1) x -= w * 0.5f; else if (align == 2) x -= w;
	C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, clr);
}

static void drawCentered(const C2D_Text* t, float cx, float y, float scale, u32 clr) {
	float w, h;
	C2D_TextGetDimensions(t, scale, scale, &w, &h);
	C2D_DrawText(t, C2D_WithColor, cx - w * 0.5f, y, 0.5f, scale, scale, clr);
}

static void fillClipped(float x0, float y0, float x1, float y1, u32 clr) {
	if (x0 < 0.0f) x0 = 0.0f;
	if (y0 < 0.0f) y0 = 0.0f;
	if (x1 > 320.0f) x1 = 320.0f;
	if (y1 > MAP_H) y1 = MAP_H;
	if (x1 > x0 && y1 > y0) C2D_DrawRectSolid(x0, y0, 0.5f, x1 - x0, y1 - y0, clr);
}

static void mapRect(float x0, float z0, float x1, float z1, float px, float pz, u32 clr) {
	fillClipped(MAP_CX + (x0 - px) * MAP_S, MAP_CY + (z0 - pz) * MAP_S, MAP_CX + (x1 - px) * MAP_S, MAP_CY + (z1 - pz) * MAP_S, clr);
}

// which neighbourhood / nearest cross streets a point is in
static void whereAmI(float x, float z, const char** hood, int* va, int* hb) {
	int i = -1, j = -1;
	for (int k = 0; k < NRX; k++) if (RX[k] < x) i = k;
	for (int k = 0; k < NRZ; k++) if (RZ[k] < z) j = k;
	*hood = hoods[clampi(j, 0, NRZ - 2)][clampi(i, 0, NRX - 2)];
	float bd = 1e9f; *va = 0;
	for (int k = 0; k < NRX; k++) if (fabsf(RX[k] - x) < bd) { bd = fabsf(RX[k] - x); *va = k; }
	bd = 1e9f; *hb = 0;
	for (int k = 0; k < NRZ; k++) if (fabsf(RZ[k] - z) < bd) { bd = fabsf(RZ[k] - z); *hb = k; }
}

static void weaponLine(int w, char* out, size_t n) {
	if (w == WP_FISTS) snprintf(out, n, "FISTS");
	else snprintf(out, n, "%s  %d/%d", wdef[w].name, wClip[w], wRes[w]);
}

static void drawMap(float px, float pz, float face, int frame, int phoneTimer) {
	const u32 cBg    = C2D_Color32(0x12, 0x15, 0x1d, 0xFF);
	const u32 cRoad  = C2D_Color32(0x4a, 0x51, 0x66, 0xFF);
	const u32 cMid   = C2D_Color32(0x6b, 0x73, 0x8c, 0xFF);
	const u32 cBlock = C2D_Color32(0x23, 0x28, 0x38, 0xFF);
	const u32 cBldg  = C2D_Color32(0x39, 0x40, 0x56, 0xFF);
	const u32 cPark  = C2D_Color32(0x24, 0x4a, 0x32, 0xFF);
	const u32 cPlaza = C2D_Color32(0x3a, 0x3d, 0x4c, 0xFF);
	const u32 cWater = C2D_Color32(0x1c, 0x3c, 0x66, 0xFF);
	const u32 cPill  = C2D_Color32(0x00, 0x00, 0x00, 0xB4);
	const u32 cText  = C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF);
	const u32 cPink  = C2D_Color32(0xE0, 0x30, 0xE0, 0xFF);

	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, MAP_H, cBg);
	mapRect(gXmax, -900.0f, 900.0f, 900.0f, px, pz, cWater);
	mapRect(gXmin, gZmin, gXmax, gZmax, px, pz, cBlock);
	for (int k = 0; k < NRZ; k++) mapRect(gXmin, RZ[k] - ROAD_HALF, gXmax, RZ[k] + ROAD_HALF, px, pz, cRoad);
	for (int k = 0; k < NRX; k++) mapRect(RX[k] - ROAD_HALF, gZmin, RX[k] + ROAD_HALF, gZmax, px, pz, cRoad);
	for (int j = 0; j < NRZ - 1; j++) {
		for (int i = 0; i < NRX - 1; i++) {
			const Blk* B = &blk[j][i];
			if (B->kind == B_PARK) mapRect(B->x0, B->z0, B->x1, B->z1, px, pz, cPark);
			else if (B->kind == B_PLAZA || B->kind == B_TOWERS) mapRect(B->x0, B->z0, B->x1, B->z1, px, pz, cPlaza);
		}
	}
	for (int i = 0; i < nSolid; i++) {
		const Solid* s = &solids[i];
		if (s->kind == SK_BLDG && s->y0 < 1.0f) mapRect(s->x0, s->z0, s->x1, s->z1, px, pz, cBldg);
	}
	for (int k = 0; k < NRX; k++) fillClipped(MAP_CX + (RX[k] - px) * MAP_S - 0.5f, 0.0f, MAP_CX + (RX[k] - px) * MAP_S + 0.5f, MAP_H, cMid);
	for (int k = 0; k < NRZ; k++) fillClipped(0.0f, MAP_CY + (RZ[k] - pz) * MAP_S - 0.5f, 320.0f, MAP_CY + (RZ[k] - pz) * MAP_S + 0.5f, cMid);

	// traffic and pedestrians
	for (int i = 0; i < nCarOn; i++) {
		const Car* c = &cars[i];
		float cx, cz, hx, hz;
		carBox(c, &cx, &cz, &hx, &hz);
		float sx = MAP_CX + (cx - px) * MAP_S, sy = MAP_CY + (cz - pz) * MAP_S;
		if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f)
			C2D_DrawRectSolid(sx - 1.5f, sy - 1.5f, 0.5f, 3.0f, 3.0f, C2D_Color32f(c->r, c->g, c->b, 1.0f));
	}
	for (int i = 0; i < nNpcOn; i++) {
		if (npcs[i].state != NPC_WALK) continue;
		float x, z;
		npcPos(&npcs[i], &x, &z);
		float sx = MAP_CX + (x - px) * MAP_S, sy = MAP_CY + (z - pz) * MAP_S;
		if (sx > 2.0f && sx < 318.0f && sy > 2.0f && sy < MAP_H - 2.0f)
			C2D_DrawRectSolid(sx - 1.0f, sy - 1.0f, 0.5f, 2.0f, 2.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xB0));
	}

	// hacked traffic light marker
	if (hackTimer > 0) {
		float sx = MAP_CX + (RX[hackA] - px) * MAP_S, sy = MAP_CY + (RZ[hackB] - pz) * MAP_S;
		if (sx > 18.0f && sx < 302.0f && sy > 18.0f && sy < MAP_H - 18.0f) {
			float r = 12.0f + (float)(frame % 30) * 0.3f;
			C2D_DrawCircleSolid(sx, sy, 0.5f, r, cPink);
			C2D_DrawCircleSolid(sx, sy, 0.5f, r - 3.0f, cRoad);
			C2D_DrawCircleSolid(sx, sy, 0.5f, 3.0f, cPink);
		}
	}

	// street names: north-south streets along the top edge, east-west streets along the left edge
	for (int k = 0; k < NRX; k++) {
		float w, h;
		float sx = MAP_CX + (RX[k] - px) * MAP_S;
		C2D_TextGetDimensions(&vLabel[k], 0.38f, 0.38f, &w, &h);
		float lx = sx - w * 0.5f;
		if (lx > 2.0f && lx + w < 318.0f) {
			C2D_DrawRectSolid(lx - 3.0f, 2.0f, 0.5f, w + 6.0f, h + 3.0f, cPill);
			C2D_DrawText(&vLabel[k], C2D_WithColor, lx, 3.0f, 0.5f, 0.38f, 0.38f, cText);
		}
	}
	for (int k = 0; k < NRZ; k++) {
		float w, h;
		float sy = MAP_CY + (RZ[k] - pz) * MAP_S;
		C2D_TextGetDimensions(&hLabel[k], 0.38f, 0.38f, &w, &h);
		if (sy > 22.0f && sy < MAP_H - 10.0f) {
			C2D_DrawRectSolid(3.0f, sy - h * 0.5f - 1.0f, 0.5f, w + 6.0f, h + 2.0f, cPill);
			C2D_DrawText(&hLabel[k], C2D_WithColor, 6.0f, sy - h * 0.5f, 0.5f, 0.38f, 0.38f, cText);
		}
	}

	// weapon + pause hint
	{
		float w, h;
		C2D_TextGetDimensions(&txtHint, 0.34f, 0.34f, &w, &h);
		C2D_DrawRectSolid(317.0f - w - 8.0f, MAP_H - h - 7.0f, 0.5f, w + 8.0f, h + 4.0f, cPill);
		C2D_DrawText(&txtHint, C2D_WithColor, 317.0f - w - 4.0f, MAP_H - h - 5.0f, 0.5f, 0.34f, 0.34f, cText);
		char wl2[48];
		weaponLine(curW, wl2, sizeof wl2);
		C2D_DrawRectSolid(3.0f, MAP_H - 17.0f, 0.5f, 108.0f, 15.0f, cPill);
		drawStr(7.0f, MAP_H - 16.0f, 0.38f, cText, 0, wl2);
	}

	// the player: blue circle with a white outline and a heading marker, always at the map centre
	{
		float fx = -sinf(face), fz = -cosf(face);
		float tx = MAP_CX + fx * 15.0f, ty = MAP_CY + fz * 15.0f;
		float bx = MAP_CX + fx * 8.0f,  by = MAP_CY + fz * 8.0f;
		C2D_DrawTriangle(tx, ty, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), bx - fz * 5.0f, by + fx * 5.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF),
		                 bx + fz * 5.0f, by - fx * 5.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), 0.5f);
		C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 8.5f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
		C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 6.5f, C2D_Color32(0x1E, 0x7B, 0xFF, 0xFF));
	}

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
	const char* hood; int a, b;
	whereAmI(px, pz, &hood, &a, &b);
	char line1[64], line2[96];
	snprintf(line1, sizeof line1, "%s, Chicago, IL", hood);
	snprintf(line2, sizeof line2, "Near %s & %s", vNames[a], hNames[b]);
	drawStr(160.0f, (float)BTN_Y1 + 1.0f,  0.55f, cText, 1, line1);
	drawStr(160.0f, (float)BTN_Y1 + 19.0f, 0.40f, C2D_Color32(0x9a, 0xa3, 0xb8, 0xFF), 1, line2);
}

// ---- weapon wheel (bottom screen, hold Y) ------------------------------------------------
// Five slots round a ring, first one at the top, clockwise.
static int wheelOrder[5] = { WP_PISTOL, WP_SMG, WP_SHOTGUN, WP_ROCKET, WP_FISTS };
static int wheelN = 5;   // the story mission only allows the SMG and the shotgun
#define WHEEL_CX 160.0f
#define WHEEL_CY 120.0f
#define WHEEL_R  72.0f
#define TILE_W   82.0f
#define TILE_H   46.0f

static void wheelTile(int slot, float* cx, float* cy) {
	float a = -1.5708f + (float)slot * (6.28319f / (float)wheelN);
	*cx = WHEEL_CX + cosf(a) * WHEEL_R * 1.02f;
	*cy = WHEEL_CY + sinf(a) * WHEEL_R * 0.98f;
}

static int wheelSlotOf(int weapon) {
	for (int i = 0; i < wheelN; i++) if (wheelOrder[i] == weapon) return i;
	return 0;
}

static void drawWheel(float open, int selSlot, int frame) {
	if (open < 0.01f) return;
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(2, 6, 14, 0.88f * open));
	for (float y = 2.0f; y < 240.0f; y += 6.0f)
		C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.05f * open));
	C2D_DrawCircleSolid(WHEEL_CX, WHEEL_CY, 0.5f, 100.0f, neon(0, 229, 255, 0.10f * open));
	C2D_DrawCircleSolid(WHEEL_CX, WHEEL_CY, 0.5f, 98.0f,  neon(4, 12, 24, 0.85f * open));
	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.14f);
	for (int i = 0; i < wheelN; i++) {
		int w = wheelOrder[i];
		float cx, cy;
		wheelTile(i, &cx, &cy);
		int on = (i == selSlot);
		float x0 = cx - TILE_W * 0.5f, y0 = cy - TILE_H * 0.5f;
		C2D_DrawRectSolid(x0, y0, 0.5f, TILE_W, TILE_H, on ? neon(0, 229, 255, (0.22f + 0.08f * pulse) * open) : neon(8, 24, 40, 0.85f * open));
		C2D_DrawRectSolid(x0, y0, 0.5f, TILE_W, 1.0f, on ? neon(0, 229, 255, open) : neon(0, 150, 180, 0.6f * open));
		C2D_DrawRectSolid(x0, y0 + TILE_H - 1.0f, 0.5f, TILE_W, 1.0f, on ? neon(0, 229, 255, open) : neon(0, 150, 180, 0.6f * open));
		C2D_DrawRectSolid(x0, y0, 0.5f, 1.0f, TILE_H, on ? neon(0, 229, 255, open) : neon(0, 150, 180, 0.6f * open));
		C2D_DrawRectSolid(x0 + TILE_W - 1.0f, y0, 0.5f, 1.0f, TILE_H, on ? neon(0, 229, 255, open) : neon(0, 150, 180, 0.6f * open));
		C2D_DrawRectSolid(x0, y0, 0.5f, on ? 5.0f : 2.0f, TILE_H, on ? neon(255, 46, 209, open) : neon(0, 229, 255, 0.7f * open));
		u32 tc = on ? neon(255, 255, 255, open) : neon(120, 220, 235, 0.95f * open);
		drawStr(cx + 2.0f, y0 + 4.0f, 0.42f, tc, 1, wdef[w].name);
		char am[24];
		if (w == WP_FISTS) snprintf(am, sizeof am, "--");
		else snprintf(am, sizeof am, "%d / %d", wClip[w], wRes[w]);
		u32 ac = (w != WP_FISTS && wClip[w] + wRes[w] == 0) ? neon(255, 80, 80, open) : (on ? neon(255, 46, 209, open) : neon(160, 235, 245, 0.95f * open));
		drawStr(cx + 2.0f, y0 + 21.0f, 0.55f, ac, 1, am);
		if (w == curW) C2D_DrawRectSolid(x0 + TILE_W - 9.0f, y0 + 4.0f, 0.5f, 5.0f, 5.0f, neon(255, 46, 209, open));
	}
	drawStr(WHEEL_CX, WHEEL_CY - 14.0f, 0.42f, neon(0, 229, 255, open), 1, "WEAPONS");
	drawStr(WHEEL_CX, WHEEL_CY + 1.0f,  0.34f, neon(255, 46, 209, 0.9f * open), 1, "CIRCLE PAD / TAP");
	drawStr(WHEEL_CX, 226.0f, 0.34f, neon(120, 220, 235, 0.85f * open), 1, "RELEASE Y TO EQUIP");
}

// ---- pause / settings menus (bottom screen only) -------------------------------------------
// Pages: 0 main, 1 weather, 2 settings. B always closes the menu (or goes back to the title).
enum { MP_MAIN = 0, MP_WEATHER = 1, MP_SETTINGS = 2 };
static const char* camNames[N_CAM] = { "DEFAULT", "OVER THE SHOULDER", "FIRST PERSON" };
static int camStyle = CAM_DEFAULT;

static void menuRect(int i, float* y0, float* h) { *y0 = 64.0f + (float)i * 36.0f; *h = 30.0f; }

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

// item kinds: 0 plain, 1 value text, 2 meter 0..5, 3 "active" tag
static void drawMenu(float dim, const char* title, const char* sub, int n, const char* const* labels, const int* kind,
                     const char* const* valText, const int* valNum, int sel, int frame, const char* footer) {
	if (dim < 0.01f) return;
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(2, 6, 14, 0.80f * dim));
	for (float y = 2.0f; y < 240.0f; y += 6.0f) // scanlines
		C2D_DrawRectSolid(0.0f, y, 0.5f, 320.0f, 1.0f, neon(0, 229, 255, 0.05f * dim));
	float by = fmodf((float)frame * 1.6f, 270.0f) - 15.0f; // sweeping scan bar
	C2D_DrawRectSolid(0.0f, by, 0.5f, 320.0f, 3.0f, neon(0, 229, 255, 0.09f * dim));
	brackets(8.0f, 8.0f, 304.0f, 224.0f, 20.0f, 2.0f, neon(0, 229, 255, 0.95f * dim));
	brackets(14.0f, 14.0f, 292.0f, 212.0f, 8.0f, 1.0f, neon(255, 46, 209, 0.7f * dim));

	drawStr(160.0f, 20.0f, 0.8f, neon(0, 229, 255, dim), 1, title);
	C2D_DrawRectSolid(50.0f, 52.0f, 0.5f, 220.0f, 1.0f, neon(0, 229, 255, 0.6f * dim));
	C2D_DrawRectSolid(50.0f, 51.0f, 0.5f, 44.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));
	drawStr(160.0f, 55.0f, 0.34f, neon(255, 46, 209, 0.9f * dim), 1, sub);

	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.12f);
	for (int i = 0; i < n; i++) {
		float y0, h, x0 = 40.0f, w = 240.0f;
		menuRect(i, &y0, &h);
		int on = (i == sel);
		C2D_DrawRectSolid(x0, y0, 0.5f, w, h, on ? neon(0, 229, 255, (0.18f + 0.07f * pulse) * dim) : neon(8, 24, 40, 0.80f * dim));
		frameRect(x0, y0, w, h, 1.0f, on ? neon(0, 229, 255, dim) : neon(0, 150, 180, 0.6f * dim));
		C2D_DrawRectSolid(x0, y0, 0.5f, on ? 6.0f : 2.0f, h, on ? neon(255, 46, 209, dim) : neon(0, 229, 255, 0.7f * dim));
		if (on) C2D_DrawRectSolid(x0 + w - 18.0f, y0 + h - 3.0f, 0.5f, 18.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));
		drawStr(x0 + 16.0f, y0 + 6.0f, 0.55f, on ? neon(255, 255, 255, dim) : neon(120, 220, 235, 0.9f * dim), 0, labels[i]);
		if (kind[i] == 1) {
			drawStr(x0 + w - 10.0f, y0 + 9.0f, 0.38f, neon(255, 46, 209, dim), 2, valText[i]);
		} else if (kind[i] == 2) {
			for (int k = 0; k < 5; k++)
				C2D_DrawRectSolid(x0 + w - 92.0f + (float)k * 17.0f, y0 + 8.0f, 0.5f, 13.0f, 14.0f,
				                  k < valNum[i] ? neon(0, 229, 255, dim) : neon(0, 100, 130, 0.5f * dim));
		} else if (kind[i] == 3 && valNum[i]) {
			drawStr(x0 + w - 10.0f, y0 + 9.0f, 0.38f, neon(255, 46, 209, dim), 2, "ACTIVE");
		}
	}
	drawStr(160.0f, 213.0f, 0.34f, neon(120, 220, 235, 0.85f * dim), 1, footer);
}

// ---- boot logos, loading, title ------------------------------------------------------------------
// Original placeholder studio logos (the names are made up; change them in studioName[]).
#define LOGO_FRAMES 220            // fade in 40, hold 70, fade out slowly 90, a beat of black 20
#define N_LOGOS     3
static const char* studioName[N_LOGOS] = { "PRAIRIE LIGHT GAMES", "GRIDLINE INTERACTIVE", "A HOMEBREW PROJECT" };
static const char* studioTag[N_LOGOS]  = { "presents", "in association with", "CHICAGO 3DS" };

static float logoAlpha(int f) {
	if (f < 40)  return (float)f / 40.0f;
	if (f < 110) return 1.0f;
	if (f < 200) return 1.0f - (float)(f - 110) / 90.0f;
	return 0.0f;
}

static void drawLogoEmblem(int which, float cx, float cy, float a) {
	switch (which) {
	case 0: { // rising sun over horizon lines
		C2D_DrawCircleSolid(cx, cy - 6.0f, 0.5f, 34.0f, neon(255, 190, 70, a));
		C2D_DrawRectSolid(cx - 60.0f, cy + 4.0f, 0.5f, 120.0f, 40.0f, neon(0, 0, 0, a));
		for (int i = 0; i < 5; i++) C2D_DrawRectSolid(cx - 52.0f + (float)i * 4.0f, cy + 8.0f + (float)i * 7.0f, 0.5f, 104.0f - (float)i * 8.0f, 2.0f + (float)i * 0.6f, neon(255, 120, 60, a));
		break;
	}
	case 1: { // a 3x3 grid with one lit cell
		for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
			int lit = (r == 1 && c == 2);
			C2D_DrawRectSolid(cx - 33.0f + (float)c * 24.0f, cy - 33.0f + (float)r * 24.0f, 0.5f, 18.0f, 18.0f,
			                  lit ? neon(0, 229, 255, a) : neon(70, 90, 120, a));
		}
		break;
	}
	default: { // skyline silhouette with a blue sky glow
		C2D_DrawRectSolid(cx - 62.0f, cy - 34.0f, 0.5f, 124.0f, 70.0f, neon(20, 40, 90, a * 0.8f));
		static const int hh[9] = {26, 40, 32, 58, 44, 66, 36, 50, 28};
		for (int i = 0; i < 9; i++) C2D_DrawRectSolid(cx - 58.0f + (float)i * 13.0f, cy + 36.0f - (float)hh[i], 0.5f, 11.0f, (float)hh[i], neon(8, 10, 18, a));
		for (int i = 0; i < 9; i++) for (int k = 0; k < hh[i] / 10; k++)
			if (((i * 7 + k * 3) % 3) == 0) C2D_DrawRectSolid(cx - 55.0f + (float)i * 13.0f, cy + 30.0f - (float)hh[i] + (float)k * 10.0f + 6.0f, 0.5f, 3.0f, 3.0f, neon(255, 210, 110, a));
		break;
	}
	}
}

// top screen during the boot sequence
static void drawBootTop(int t) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, C2D_Color32(0, 0, 0, 0xFF));
	int which = t / LOGO_FRAMES;
	if (which >= N_LOGOS) return;
	float a = logoAlpha(t % LOGO_FRAMES);
	if (a <= 0.0f) return;
	drawLogoEmblem(which, 200.0f, 92.0f, a);
	drawStr(200.0f, 146.0f, 0.85f, neon(240, 242, 248, a), 1, studioName[which]);
	drawStr(200.0f, 178.0f, 0.5f,  neon(154, 163, 184, a), 1, studioTag[which]);
}

// What the loading screen reports about sound, so a silent game is never a mystery.
static void audioStatusText(const char** l1, const char** l2, int* bad) {
	*bad = 0; *l2 = "";
	if (!audioOK) {
		*bad = 1; *l1 = "SOUND OFF - DSP firmware not found";
		*l2 = "Run the DSP1 app once (makes sdmc:/3ds/dspfirm.cdc)";
	} else if (loadDone() && assetsFound == 0) {
		*bad = 1; *l1 = "SOUND OFF - audio files not packed in the game";
		*l2 = "Upload assets.zip to GitHub, then rebuild";
	} else if (loadDone() && loadFail > 0) {
		*bad = 1; *l1 = "SOME SOUND FILES MISSING"; *l2 = "Music or effects may be silent";
	} else if (loadDone()) {
		*l1 = "SOUND READY";
	} else {
		*l1 = "LOADING SOUND...";
	}
}

static void drawLoadBottom(int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0x05, 0x07, 0x0c, 0xFF));
	drawStr(160.0f, 62.0f, 0.9f, neon(0, 229, 255, 1.0f), 1, "LOADING");
	float prog = audioOK ? (float)loadStep / (float)S_COUNT : 1.0f;
	if (loadDone()) prog = 1.0f;
	C2D_DrawRectSolid(60.0f, 112.0f, 0.5f, 200.0f, 8.0f, neon(0, 100, 130, 0.5f));
	C2D_DrawRectSolid(60.0f, 112.0f, 0.5f, 200.0f * prog, 8.0f, neon(0, 229, 255, 1.0f));
	float sweep = fmodf((float)frame * 3.0f, 230.0f) - 15.0f;
	C2D_DrawRectSolid(60.0f + sweep, 112.0f, 0.5f, 14.0f, 8.0f, neon(255, 46, 209, 0.5f));
	const char *l1, *l2; int bad;
	audioStatusText(&l1, &l2, &bad);
	drawStr(160.0f, 140.0f, 0.42f, bad ? neon(255, 110, 110, 1.0f) : neon(154, 163, 184, 1.0f), 1, l1);
	if (l2[0]) drawStr(160.0f, 158.0f, 0.34f, neon(200, 200, 210, 1.0f), 1, l2);
	if (!audioOK) {
		char e[48]; snprintf(e, sizeof e, "(dsp error 0x%08lX)", (unsigned long)dspRes);
		drawStr(160.0f, 174.0f, 0.30f, neon(120, 125, 140, 1.0f), 1, e);
	}
	if (loadDone()) drawStr(160.0f, 214.0f, 0.36f, neon(120, 220, 235, 0.6f + 0.4f * sinf((float)frame * 0.1f)), 1, "A: SKIP");
}

static void drawTitleBottom(int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0x0b, 0x0d, 0x14, 0xFF));
	drawStr(160.0f, 52.0f, 1.2f, C2D_Color32(0x4a, 0x9d, 0xff, 0xFF), 1, "CHICAGO 3DS");
	drawStr(160.0f, 104.0f, 0.5f, C2D_Color32(0x9a, 0xa3, 0xb8, 0xFF), 1, "a low-poly hacking sandbox");
	if ((frame / 30) % 2 == 0) drawStr(160.0f, 150.0f, 0.6f, C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF), 1, "PRESS A or TAP TO START");
	drawStr(160.0f, 190.0f, 0.4f, C2D_Color32(0x6b, 0x73, 0x8c, 0xFF), 1, "X = settings     START = quit");
	if (!audioOK) drawStr(160.0f, 214.0f, 0.34f, C2D_Color32(0xFF, 0x6e, 0x6e, 0xFF), 1, "No sound: DSP firmware (dspfirm.cdc) not found");
	else if (assetsFound == 0) drawStr(160.0f, 214.0f, 0.34f, C2D_Color32(0xFF, 0x6e, 0x6e, 0xFF), 1, "No sound: audio files missing from the build");
}

static void drawTitleTop(int frame) {
	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.05f);
	C2D_DrawRectSolid(0.0f, 70.0f, 0.5f, 400.0f, 70.0f, neon(0, 0, 0, 0.45f));
	C2D_DrawRectSolid(0.0f, 70.0f, 0.5f, 400.0f, 2.0f, neon(0, 229, 255, 0.8f));
	C2D_DrawRectSolid(0.0f, 138.0f, 0.5f, 400.0f, 2.0f, neon(255, 46, 209, 0.8f));
	drawStr(203.0f, 80.0f, 1.35f, neon(0, 0, 0, 0.6f), 1, "CHICAGO 3DS");
	drawStr(200.0f, 77.0f, 1.35f, neon(240, 248, 255, 1.0f), 1, "CHICAGO 3DS");
	drawStr(200.0f, 118.0f, 0.42f, neon(0, 229, 255, 0.85f), 1, "// STORY MODE  +  OPEN CITY");
	(void)pulse;
}

// ---- top-screen HUD ----------------------------------------------------------------------------------------
static void drawHud(int frame, int showCross, float aimE, float reloadFrac, int sim) {
	const u32 cWhite = C2D_Color32(0xFF, 0xFF, 0xFF, 0xE8), cDark = C2D_Color32(0, 0, 0, 0x90);
	if (showCross) {
		float gap = 4.0f + (1.0f - aimE) * 8.0f + kick * 60.0f;
		float cx = 200.0f, cy = 120.0f;
		C2D_DrawRectSolid(cx - gap - 7.0f, cy - 0.5f, 0.5f, 7.0f, 1.5f, cWhite);
		C2D_DrawRectSolid(cx + gap,        cy - 0.5f, 0.5f, 7.0f, 1.5f, cWhite);
		C2D_DrawRectSolid(cx - 0.5f, cy - gap - 7.0f, 0.5f, 1.5f, 7.0f, cWhite);
		C2D_DrawRectSolid(cx - 0.5f, cy + gap,        0.5f, 1.5f, 7.0f, cWhite);
		C2D_DrawRectSolid(cx - 1.0f, cy - 1.0f, 0.5f, 2.0f, 2.0f, cWhite);
		if (reloadFrac > 0.0f) {
			C2D_DrawRectSolid(cx - 24.0f, cy + 24.0f, 0.5f, 48.0f, 4.0f, cDark);
			C2D_DrawRectSolid(cx - 24.0f, cy + 24.0f, 0.5f, 48.0f * reloadFrac, 4.0f, C2D_Color32(0xFF, 0xD0, 0x40, 0xFF));
		}
	}
	if (hitMark > 0) { // hit marker: four little diagonals, red on a kill
		u32 c = hitKill ? C2D_Color32(0xFF, 0x40, 0x40, 0xFF) : C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
		float cx = 200.0f, cy = 120.0f;
		for (int i = 0; i < 4; i++) {
			float sx = (i & 1) ? 1.0f : -1.0f, sy = (i & 2) ? 1.0f : -1.0f;
			for (int k = 0; k < 5; k++) C2D_DrawRectSolid(cx + sx * (6.0f + (float)k) - 1.0f, cy + sy * (6.0f + (float)k) - 1.0f, 0.5f, 2.0f, 2.0f, c);
		}
	}
	// ammo box
	{
		C2D_DrawRectSolid(272.0f, 196.0f, 0.5f, 122.0f, 38.0f, cDark);
		C2D_DrawRectSolid(272.0f, 196.0f, 0.5f, 3.0f, 38.0f, C2D_Color32(0, 229, 255, 0xFF));
		drawStr(282.0f, 198.0f, 0.42f, C2D_Color32(0x9a, 0xe8, 0xf5, 0xFF), 0, wdef[curW].name);
		char am[24];
		if (curW == WP_FISTS) snprintf(am, sizeof am, "--");
		else snprintf(am, sizeof am, "%d / %d", wClip[curW], wRes[curW]);
		int empty = (curW != WP_FISTS && wClip[curW] + wRes[curW] == 0);
		drawStr(282.0f, 211.0f, 0.62f, empty ? C2D_Color32(0xFF, 0x60, 0x60, 0xFF) : cWhite, 0, am);
	}
	char k[24];
	snprintf(k, sizeof k, "KILLS %d", kills);
	C2D_DrawRectSolid(322.0f, 6.0f, 0.5f, 72.0f, 18.0f, cDark);
	drawStr(358.0f, 8.0f, 0.45f, cWhite, 1, k);
	if (sim < 900) { // controls reminder for the first few seconds
		float a = sim < 700 ? 1.0f : (float)(900 - sim) / 200.0f;
		C2D_DrawRectSolid(6.0f, 214.0f, 0.5f, 252.0f, 20.0f, neon(0, 0, 0, 0.55f * a));
		drawStr(10.0f, 217.0f, 0.34f, neon(255, 255, 255, a), 0, "L: AIM   R: FIRE   X: RELOAD   Y: WEAPONS   START: MENU");
	}
}


// ================================================================================================
// STORY MODE
// Mission 1 happens inside a factory: a big ground-floor workshop, then five stairwells and five
// hallways climbing to the roof. Each area has its own spot on the plan (nothing overlaps), and the
// floor height under the player comes from the area he stands in, so stairs are just ramps.
// ================================================================================================
#define N_AREA  12
#define N_STEP  12
#define N_GATE  6
#define DOORH   3.2f
enum { AK_FACTORY, AK_STAIR, AK_HALL, AK_ROOF };
typedef struct { float x0, z0, x1, z1, yA, yB, h; int dir, kind; const char* name; float tint[3]; } Area;
static Area areas[N_AREA];
static int  nArea = 0;

// dir: 0 flat, 1 rises toward +x, 2 toward -x, 3 toward +z, 4 toward -z
static void defArea(int i, float x0, float z0, float x1, float z1, float yA, float yB, float h, int dir, int kind,
                    const char* name, float tr, float tg, float tb) {
	Area* a = &areas[i];
	a->x0 = x0; a->z0 = z0; a->x1 = x1; a->z1 = z1; a->yA = yA; a->yB = yB; a->h = h;
	a->dir = dir; a->kind = kind; a->name = name; a->tint[0] = tr; a->tint[1] = tg; a->tint[2] = tb;
	if (i + 1 > nArea) nArea = i + 1;
}

static float areaRamp(const Area* a, float x, float z, int stepped) {
	if (a->dir == 0) return a->yA;
	float t;
	switch (a->dir) {
	case 1:  t = (x - a->x0) / (a->x1 - a->x0); break;
	case 2:  t = (a->x1 - x) / (a->x1 - a->x0); break;
	case 3:  t = (z - a->z0) / (a->z1 - a->z0); break;
	default: t = (a->z1 - z) / (a->z1 - a->z0); break;
	}
	if (t < 0.0f) t = 0.0f;
	if (t > 0.9999f) t = 0.9999f;
	if (stepped) t = (float)((int)(t * (float)N_STEP) + 1) / (float)N_STEP;
	return a->yA + (a->yB - a->yA) * t;
}

static int areaAt(float x, float z) {
	for (int i = 0; i < nArea; i++) {
		const Area* a = &areas[i];
		if (x >= a->x0 && x < a->x1 && z >= a->z0 && z < a->z1) return i;
	}
	return -1;
}

// height of the floor (stairs count as steps); outside every area it keeps `fallback`
static float groundAt(float x, float z, float fallback, int* ai) {
	int i = areaAt(x, z);
	if (ai) *ai = i;
	return (i < 0) ? fallback : areaRamp(&areas[i], x, z, 1);
}

// Emissive things (lamps, windows, screens) are collected while building and written after the
// ordinary geometry, so they can glow through their own tint.
typedef struct { float p[6]; float r, g, b; int type; } LitItem;
#define MAX_LIT 1500
static LitItem litList[MAX_LIT];
static int nLit = 0;

static void litBox(float x0, float z0, float x1, float z1, float y0, float y1, float r, float g, float b) {
	if (nLit >= MAX_LIT) return;
	LitItem* l = &litList[nLit++];
	l->p[0] = x0; l->p[1] = z0; l->p[2] = x1; l->p[3] = z1; l->p[4] = y0; l->p[5] = y1;
	l->r = r; l->g = g; l->b = b; l->type = 0;
}
static void litWall(int axis, float fix, float a0, float a1, float y0, float y1, float r, float g, float b) {
	if (nLit >= MAX_LIT) return;
	LitItem* l = &litList[nLit++];
	l->p[0] = fix; l->p[1] = a0; l->p[2] = a1; l->p[3] = y0; l->p[4] = y1; l->p[5] = (float)axis;
	l->r = r; l->g = g; l->b = b; l->type = 1;
}

// doors that stay shut until the area is cleared
typedef struct { int axis; float fix, a0, a1, y, open; int solid; } Gate;
static Gate gates[N_GATE];
static int  nGate = 0;

static void addGate(int axis, float fix, float a0, float a1, float y) {
	if (nGate >= N_GATE) return;
	Gate* g = &gates[nGate++];
	g->axis = axis; g->fix = fix; g->a0 = a0; g->a1 = a1; g->y = y; g->open = 0.0f;
	Solid* s = (axis == 0) ? addSolid(a0, fix - 0.3f, a1, fix + 0.3f, y, y + DOORH, SK_WALL, 0)
	                       : addSolid(fix - 0.3f, a0, fix + 0.3f, a1, y, y + DOORH, SK_WALL, 0);
	g->solid = s ? (int)(s - solids) : -1;
}

// ---- building blocks ---------------------------------------------------------------------------
// A wall: `axis` 0 = lies along x at z = fix, 1 = lies along z at x = fix. `out` (+1/-1) is the side the
// solid thickness goes (away from the room). Two colour bands and a dark skirting board.
static void wallRun(int axis, float fix, float a0, float a1, int out, float y, float h, const float c[3], int solid) {
	if (a1 - a0 < 0.05f) return;
	wallQuad(axis, fix, a0, a1, y, y + 1.15f, c[0] * 0.78f, c[1] * 0.78f, c[2] * 0.78f);
	wallQuad(axis, fix, a0, a1, y + 1.15f, y + h, c[0], c[1], c[2]);
	wallQuad(axis, fix - (float)out * 0.03f, a0, a1, y, y + 0.22f, c[0] * 0.40f, c[1] * 0.40f, c[2] * 0.40f);
	if (solid) {
		const float t = 0.9f;
		if (axis == 0) addSolid(a0, out > 0 ? fix : fix - t, a1, out > 0 ? fix + t : fix, y - 0.3f, y + h, SK_WALL, 0);
		else           addSolid(out > 0 ? fix : fix - t, a0, out > 0 ? fix + t : fix, a1, y - 0.3f, y + h, SK_WALL, 0);
	}
}

// a wall with a doorway g0..g1 in it
static void wallGap(int axis, float fix, float a0, float a1, float g0, float g1, int out, float y, float h, const float c[3]) {
	wallRun(axis, fix, a0, g0, out, y, h, c, 1);
	wallRun(axis, fix, g1, a1, out, y, h, c, 1);
	wallQuad(axis, fix, g0, g1, y + DOORH, y + h, c[0] * 0.62f, c[1] * 0.62f, c[2] * 0.62f);
	// door frame: dark posts and a lintel strip, with a green exit light over it
	if (axis == 0) {
		addBoxY(g0 - 0.15f, fix - 0.15f, g0, fix + 0.15f, y, y + DOORH, 0.16f, 0.16f, 0.18f);
		addBoxY(g1, fix - 0.15f, g1 + 0.15f, fix + 0.15f, y, y + DOORH, 0.16f, 0.16f, 0.18f);
		litBox((g0 + g1) * 0.5f - 0.5f, fix - 0.12f, (g0 + g1) * 0.5f + 0.5f, fix + 0.12f, y + DOORH + 0.2f, y + DOORH + 0.45f, 0.2f, 1.0f, 0.4f);
	} else {
		addBoxY(fix - 0.15f, g0 - 0.15f, fix + 0.15f, g0, y, y + DOORH, 0.16f, 0.16f, 0.18f);
		addBoxY(fix - 0.15f, g1, fix + 0.15f, g1 + 0.15f, y, y + DOORH, 0.16f, 0.16f, 0.18f);
		litBox(fix - 0.12f, (g0 + g1) * 0.5f - 0.5f, fix + 0.12f, (g0 + g1) * 0.5f + 0.5f, y + DOORH + 0.2f, y + DOORH + 0.45f, 0.2f, 1.0f, 0.4f);
	}
}

static void propBox(float x0, float z0, float x1, float z1, float y, float h, float r, float g, float b) {
	addBoxY(x0, z0, x1, z1, y, y + h, r, g, b);
	addSolid(x0, z0, x1, z1, y, y + h, SK_PROP, 0);
}
static void crate(float x, float z, float s, float y, float r, float g, float b) {
	propBox(x - s * 0.5f, z - s * 0.5f, x + s * 0.5f, z + s * 0.5f, y, s, r, g, b);
	addBoxY(x - s * 0.5f - 0.02f, z - s * 0.5f - 0.02f, x + s * 0.5f + 0.02f, z - s * 0.5f + 0.12f, y + s * 0.42f, y + s * 0.58f, r * 0.6f, g * 0.6f, b * 0.6f);
	addBoxY(x - s * 0.5f - 0.02f, z + s * 0.5f - 0.12f, x + s * 0.5f + 0.02f, z + s * 0.5f + 0.02f, y + s * 0.42f, y + s * 0.58f, r * 0.6f, g * 0.6f, b * 0.6f);
}
static void barrel(float x, float z, float y, float r, float g, float b) {
	propBox(x - 0.45f, z - 0.45f, x + 0.45f, z + 0.45f, y, 1.15f, r, g, b);
	addBoxY(x - 0.47f, z - 0.47f, x + 0.47f, z + 0.47f, y + 0.30f, y + 0.40f, r * 0.7f, g * 0.7f, b * 0.7f);
	addBoxY(x - 0.47f, z - 0.47f, x + 0.47f, z + 0.47f, y + 0.78f, y + 0.88f, r * 0.7f, g * 0.7f, b * 0.7f);
}
static void conveyor(float x0, float z0, float x1, float z1, float y) {
	propBox(x0, z0, x1, z1, y, 0.95f, 0.22f, 0.24f, 0.28f);
	addBoxY(x0 + 0.12f, z0 + 0.12f, x1 - 0.12f, z1 - 0.12f, y + 0.95f, y + 1.05f, 0.07f, 0.07f, 0.08f);
	addBoxY(x0, z0, x1, z1, y + 0.40f, y + 0.48f, 0.90f, 0.72f, 0.10f);
	int alongX = (x1 - x0) > (z1 - z0);
	float len = alongX ? x1 - x0 : z1 - z0;
	for (float s = 1.0f; s < len - 0.6f; s += 1.6f)
		addBoxY(alongX ? x0 + s : x0 + 0.02f, alongX ? z0 + 0.02f : z0 + s, alongX ? x0 + s + 0.16f : x1 - 0.02f, alongX ? z1 - 0.02f : z0 + s + 0.16f, y + 1.05f, y + 1.08f, 0.16f, 0.16f, 0.18f);
}
static void press(float x0, float z0, float x1, float z1, float y, float h) {
	propBox(x0, z0, x1, z1, y, h * 0.55f, 0.30f, 0.36f, 0.46f);
	addBoxY(x0 + 0.5f, z0 + 0.5f, x1 - 0.5f, z1 - 0.5f, y + h * 0.55f, y + h, 0.22f, 0.26f, 0.34f);
	addBoxY(x0 - 0.05f, z0 - 0.05f, x1 + 0.05f, z0 + 0.35f, y + h * 0.55f - 0.5f, y + h * 0.55f - 0.1f, 0.92f, 0.74f, 0.10f);
	litBox(x0 + 0.6f, z1 - 0.05f, x0 + 1.4f, z1 + 0.04f, y + h * 0.35f, y + h * 0.35f + 0.5f, 0.2f, 0.9f, 0.4f);
	litBox(x1 - 1.4f, z1 - 0.05f, x1 - 0.6f, z1 + 0.04f, y + h * 0.35f, y + h * 0.35f + 0.5f, 1.0f, 0.3f, 0.2f);
}
static void pillar(float x, float z, float y, float h) {
	propBox(x - 0.6f, z - 0.6f, x + 0.6f, z + 0.6f, y, h, 0.40f, 0.40f, 0.42f);
	addBoxY(x - 0.7f, z - 0.7f, x + 0.7f, z + 0.7f, y, y + 0.6f, 0.80f, 0.66f, 0.10f);
}

// stairs: steps, sloped walls and ceiling, a couple of wall lamps
static void buildStair(const Area* a, const float wc[3], const float tc[3]) {
	int xdir = (a->dir <= 2);
	float lo = xdir ? a->x0 : a->z0, hi = xdir ? a->x1 : a->z1, L = hi - lo;
	float rise = a->yB - a->yA, riser = rise / (float)N_STEP;
	int fwd = (a->dir == 1 || a->dir == 3);
	for (int i = 0; i < N_STEP; i++) {
		float f0 = (float)i / (float)N_STEP, f1 = (float)(i + 1) / (float)N_STEP;
		float s0 = fwd ? lo + L * f0 : hi - L * f1, s1 = fwd ? lo + L * f1 : hi - L * f0;
		float top = a->yA + riser * (float)(i + 1), sh = (i & 1) ? 0.90f : 1.0f;
		if (xdir) addBoxY(s0, a->z0 + 0.01f, s1, a->z1 - 0.01f, top - riser, top, tc[0] * sh, tc[1] * sh, tc[2] * sh);
		else      addBoxY(a->x0 + 0.01f, s0, a->x1 - 0.01f, s1, top - riser, top, tc[0] * sh, tc[1] * sh, tc[2] * sh);
		// nosing strip
		float n0 = fwd ? s0 : s1 - 0.12f;
		if (xdir) addBoxY(n0, a->z0 + 0.01f, n0 + 0.12f, a->z1 - 0.01f, top, top + 0.02f, 0.85f, 0.70f, 0.12f);
		else      addBoxY(a->x0 + 0.01f, n0, a->x1 - 0.01f, n0 + 0.12f, top, top + 0.02f, 0.85f, 0.70f, 0.12f);
	}
	float yLo = fwd ? a->yA : a->yB, yHi = fwd ? a->yB : a->yA;   // floor height at the low / high coordinate end
	float h = a->h;
	for (int side = 0; side < 2; side++) {
		float w = xdir ? (side ? a->z1 : a->z0) : (side ? a->x1 : a->x0);
		float o = side ? 0.02f : -0.02f;
		float p0[3], p1[3], p2[3], p3[3];
		if (xdir) {
			p0[0] = lo; p0[1] = yLo; p0[2] = w + o; p1[0] = hi; p1[1] = yHi; p1[2] = w + o;
			p2[0] = hi; p2[1] = yHi + h; p2[2] = w + o; p3[0] = lo; p3[1] = yLo + h; p3[2] = w + o;
		} else {
			p0[0] = w + o; p0[1] = yLo; p0[2] = lo; p1[0] = w + o; p1[1] = yHi; p1[2] = hi;
			p2[0] = w + o; p2[1] = yHi + h; p2[2] = hi; p3[0] = w + o; p3[1] = yLo + h; p3[2] = lo;
		}
		quad(p0, p1, p2, p3, wc[0], wc[1], wc[2]);
		float ymin = (yLo < yHi ? yLo : yHi) - 0.5f, ymax = (yLo > yHi ? yLo : yHi) + h;
		if (xdir) addSolid(lo, side ? a->z1 : a->z0 - 0.9f, hi, side ? a->z1 + 0.9f : a->z0, ymin, ymax, SK_WALL, 0);
		else      addSolid(side ? a->x1 : a->x0 - 0.9f, lo, side ? a->x1 + 0.9f : a->x0, hi, ymin, ymax, SK_WALL, 0);
		for (int k = 0; k < 2; k++) { // wall lamps
			float f = k ? 0.72f : 0.28f, s = lo + L * f, yy = yLo + (yHi - yLo) * f + 2.7f;
			if (xdir) litBox(s - 0.25f, side ? w - 0.16f : w, s + 0.25f, side ? w : w + 0.16f, yy, yy + 0.5f, 1.0f, 0.92f, 0.72f);
			else      litBox(side ? w - 0.16f : w, s - 0.25f, side ? w : w + 0.16f, s + 0.25f, yy, yy + 0.5f, 1.0f, 0.92f, 0.72f);
		}
	}
	{ // sloped ceiling
		float q0[3], q1[3], q2[3], q3[3];
		if (xdir) {
			q0[0] = lo; q0[1] = yLo + h; q0[2] = a->z0; q1[0] = hi; q1[1] = yHi + h; q1[2] = a->z0;
			q2[0] = hi; q2[1] = yHi + h; q2[2] = a->z1; q3[0] = lo; q3[1] = yLo + h; q3[2] = a->z1;
		} else {
			q0[0] = a->x0; q0[1] = yLo + h; q0[2] = lo; q1[0] = a->x0; q1[1] = yHi + h; q1[2] = hi;
			q2[0] = a->x1; q2[1] = yHi + h; q2[2] = hi; q3[0] = a->x1; q3[1] = yLo + h; q3[2] = lo;
		}
		quad(q0, q1, q2, q3, wc[0] * 0.45f, wc[1] * 0.45f, wc[2] * 0.45f);
	}
}

// ---- hallways -------------------------------------------------------------------------------
typedef struct { float wall[3], floor[3], prop1[3], prop2[3], light[3]; } Theme;
static const Theme themes[5] = {
	{ {0.66f, 0.62f, 0.52f}, {0.34f, 0.32f, 0.29f}, {0.46f, 0.31f, 0.18f}, {0.45f, 0.47f, 0.50f}, {1.00f, 0.96f, 0.82f} }, // offices
	{ {0.52f, 0.52f, 0.55f}, {0.30f, 0.30f, 0.32f}, {0.58f, 0.42f, 0.24f}, {0.28f, 0.42f, 0.62f}, {1.00f, 0.98f, 0.90f} }, // warehouse
	{ {0.44f, 0.52f, 0.49f}, {0.26f, 0.30f, 0.30f}, {0.30f, 0.36f, 0.42f}, {0.60f, 0.46f, 0.26f}, {0.86f, 1.00f, 0.92f} }, // storage
	{ {0.52f, 0.40f, 0.35f}, {0.30f, 0.26f, 0.25f}, {0.58f, 0.60f, 0.64f}, {0.70f, 0.30f, 0.20f}, {1.00f, 0.78f, 0.45f} }, // boiler rooms
	{ {0.28f, 0.33f, 0.46f}, {0.34f, 0.37f, 0.44f}, {0.12f, 0.13f, 0.18f}, {0.20f, 0.24f, 0.34f}, {0.55f, 0.85f, 1.00f} }, // server floor
};

// gapsLo / gapsHi: doorway on the low / high end of the long walls is described by the caller; this draws the
// floor, ceiling, lights, door panels and the obstacles down the middle section
static void hallInterior(const Area* a, int theme, int seed) {
	const Theme* T = &themes[theme];
	float y = a->yA, h = a->h;
	float cx = (a->z0 + a->z1) * 0.5f;
	flat(a->x0, a->z0, a->x1, a->z1, y, T->floor[0], T->floor[1], T->floor[2]);
	flat(a->x0, a->z0, a->x1, a->z1, y + h, T->wall[0] * 0.30f, T->wall[1] * 0.30f, T->wall[2] * 0.30f);
	addSolid(a->x0, a->z0, a->x1, a->z1, y - 0.5f, y, SK_FLOOR, 0);
	addSolid(a->x0, a->z0, a->x1, a->z1, y + h, y + h + 0.5f, SK_FLOOR, 0);
	flat(a->x0 + 0.3f, cx - 0.9f, a->x1 - 0.3f, cx + 0.9f, y + 0.03f, T->floor[0] * 0.72f, T->floor[1] * 0.72f, T->floor[2] * 0.72f); // runner
	flat(a->x0 + 0.3f, a->z0 + 0.25f, a->x1 - 0.3f, a->z0 + 0.45f, y + 0.03f, 0.85f, 0.70f, 0.10f);   // safety lines along both walls
	flat(a->x0 + 0.3f, a->z1 - 0.45f, a->x1 - 0.3f, a->z1 - 0.25f, y + 0.03f, 0.85f, 0.70f, 0.10f);
	// ceiling lights and pools of light on the floor
	for (float x = a->x0 + 8.0f; x < a->x1 - 4.0f; x += 9.0f) {
		litBox(x - 1.5f, cx - 0.4f, x + 1.5f, cx + 0.4f, y + h - 0.14f, y + h - 0.02f, T->light[0], T->light[1], T->light[2]);
		flat(x - 3.0f, a->z0 + 0.5f, x + 3.0f, a->z1 - 0.5f, y + 0.05f, T->floor[0] * 1.22f, T->floor[1] * 1.22f, T->floor[2] * 1.22f);
	}
	// door panels on both long walls (decoration), pipes / cable trays along the ceiling
	for (float x = a->x0 + 13.0f; x < a->x1 - 11.0f; x += 11.0f) {
		float d0 = x, d1 = x + 1.5f;
		wallQuad(0, a->z0 + 0.03f, d0, d1, y, y + 2.5f, T->wall[0] * 0.38f, T->wall[1] * 0.38f, T->wall[2] * 0.38f);
		wallQuad(0, a->z1 - 0.03f, d0 + 4.0f, d1 + 4.0f, y, y + 2.5f, T->wall[0] * 0.38f, T->wall[1] * 0.38f, T->wall[2] * 0.38f);
		litBox(d0 + 0.2f, a->z0 + 0.02f, d1 - 0.2f, a->z0 + 0.06f, y + 2.7f, y + 2.85f, 0.9f, 0.9f, 1.0f);
	}
	addBoxY(a->x0, a->z0 + 0.05f, a->x1, a->z0 + 0.45f, y + h - 0.9f, y + h - 0.55f, T->wall[0] * 0.55f, T->wall[1] * 0.55f, T->wall[2] * 0.55f);
	addBoxY(a->x0, a->z1 - 0.45f, a->x1, a->z1 - 0.05f, y + h - 0.9f, y + h - 0.55f, T->wall[0] * 0.5f, T->wall[1] * 0.5f, T->wall[2] * 0.5f);
	// obstacles alternate sides, never closer than 11 units to either end (the stairs live there)
	int side = (seed & 1);
	for (float x = a->x0 + 12.0f; x < a->x1 - 12.0f; x += 7.5f + (float)((seed + (int)x) % 3)) {
		float depth = 1.5f + 0.1f * (float)((int)(x * 3.0f + (float)seed) % 5);
		float w = 1.6f + 0.3f * (float)((int)(x + (float)seed * 7.0f) % 4);
		float oz0 = side ? a->z1 - 0.5f - depth : a->z0 + 0.5f;
		float oz1 = side ? a->z1 - 0.5f : a->z0 + 0.5f + depth;
		float oh = 1.0f + 0.25f * (float)((int)(x * 5.0f + (float)seed) % 4);
		switch (theme) {
		case 0:  if (((int)x + seed) % 2) { propBox(x, oz0, x + w + 0.8f, oz1, y, 0.9f, T->prop1[0], T->prop1[1], T->prop1[2]); }   // desk
		         else { propBox(x, oz0, x + 1.2f, oz1, y, 1.9f, T->prop2[0], T->prop2[1], T->prop2[2]); }                          // filing cabinet
		         break;
		case 1:  crate(x + 0.9f, (oz0 + oz1) * 0.5f, depth > 1.8f ? 1.8f : 1.5f, y, T->prop1[0], T->prop1[1], T->prop1[2]);
		         if (((int)x + seed) % 3 == 0) { barrel(x + 2.3f, (oz0 + oz1) * 0.5f, y, T->prop2[0], T->prop2[1], T->prop2[2]); }
		         break;
		case 2:  propBox(x, oz0, x + w + 1.0f, oz1, y, 1.9f, T->prop1[0], T->prop1[1], T->prop1[2]);                               // lockers / shelving
		         addBoxY(x, oz0, x + w + 1.0f, oz0 + 0.1f, y + 1.0f, y + 1.1f, 0.12f, 0.14f, 0.16f); break;
		case 3:  propBox(x, oz0, x + 1.7f, oz1, y, oh + 0.3f, T->prop1[0], T->prop1[1], T->prop1[2]);                               // tanks / boilers
		         addBoxY(x + 0.2f, oz0 + 0.2f, x + 1.5f, oz1 - 0.2f, y + oh + 0.3f, y + oh + 0.8f, T->prop2[0], T->prop2[1], T->prop2[2]); break;
		default: propBox(x, oz0, x + 1.3f, oz1, y, 1.9f, T->prop1[0], T->prop1[1], T->prop1[2]);                                    // server racks
		         for (int k = 0; k < 5; k++) litBox(x + 0.15f, side ? oz0 - 0.03f : oz1 - 0.01f, x + 0.55f, side ? oz0 + 0.01f : oz1 + 0.03f, y + 0.4f + (float)k * 0.3f, y + 0.46f + (float)k * 0.3f,
		                                            (k + seed) % 3 == 0 ? 1.0f : 0.2f, (k + seed) % 3 == 1 ? 1.0f : 0.5f, 1.0f);
		         break;
		}
		side ^= 1;
	}
}

// ---- the whole factory -------------------------------------------------------------------------

static void buildFactory(void) {
	vcount = 0; nSolid = 0; nWl = 0; nLit = 0; nGate = 0; nArea = 0;
	cityS = 0xFAC70u;
	gXmin = -400.0f; gXmax = 600.0f; gZmin = -400.0f; gZmax = 600.0f;

	defArea(0,  -30.0f, -24.0f,  30.0f,  24.0f,  0.0f,  0.0f, 12.0f, 0, AK_FACTORY, "FACTORY - GROUND FLOOR", 0.90f, 0.90f, 0.95f);
	defArea(1,   30.0f,  -2.5f,  44.0f,   2.5f,  0.0f,  6.0f,  4.4f, 1, AK_STAIR,   "STAIRWELL 1", 0.95f, 0.92f, 0.88f);
	defArea(2,   44.0f,  -3.5f, 110.0f,   3.5f,  6.0f,  6.0f,  5.6f, 0, AK_HALL,    "HALLWAY 1",   0.95f, 0.93f, 0.88f);
	defArea(3,  105.0f,   3.5f, 110.0f,  17.5f,  6.0f, 12.0f,  4.4f, 3, AK_STAIR,   "STAIRWELL 2", 0.95f, 0.92f, 0.88f);
	defArea(4,   44.0f,  17.5f, 110.0f,  24.5f, 12.0f, 12.0f,  5.6f, 0, AK_HALL,    "HALLWAY 2",   0.92f, 0.94f, 0.98f);
	defArea(5,   44.0f,  24.5f,  49.0f,  38.5f, 12.0f, 18.0f,  4.4f, 3, AK_STAIR,   "STAIRWELL 3", 0.95f, 0.92f, 0.88f);
	defArea(6,   44.0f,  38.5f, 110.0f,  45.5f, 18.0f, 18.0f,  5.6f, 0, AK_HALL,    "HALLWAY 3",   0.90f, 0.96f, 0.94f);
	defArea(7,  105.0f,  45.5f, 110.0f,  59.5f, 18.0f, 24.0f,  4.4f, 3, AK_STAIR,   "STAIRWELL 4", 0.95f, 0.92f, 0.88f);
	defArea(8,   44.0f,  59.5f, 110.0f,  66.5f, 24.0f, 24.0f,  5.6f, 0, AK_HALL,    "HALLWAY 4",   1.00f, 0.92f, 0.86f);
	defArea(9,   44.0f,  66.5f,  49.0f,  80.5f, 24.0f, 30.0f,  4.4f, 3, AK_STAIR,   "STAIRWELL 5", 0.95f, 0.92f, 0.88f);
	defArea(10,  44.0f,  80.5f, 110.0f,  87.5f, 30.0f, 30.0f,  5.6f, 0, AK_HALL,    "HALLWAY 5",   0.88f, 0.94f, 1.00f);
	defArea(11, 110.0f,  74.0f, 165.0f,  94.0f, 30.0f, 30.0f, 40.0f, 0, AK_ROOF,    "ROOFTOP",     0.50f, 0.56f, 0.82f);

	// void below everything (the roof looks out over a dark city)
	flat(-500.0f, -500.0f, 800.0f, 800.0f, -0.6f, 0.05f, 0.055f, 0.07f);

	// ---------------- ground floor: the workshop ----------------
	{
		const float wall[3] = {0.40f, 0.36f, 0.33f};
		const Area* a = &areas[0];
		flat(a->x0, a->z0, a->x1, a->z1, 0.0f, 0.30f, 0.31f, 0.33f);
		addSolid(a->x0, a->z0, a->x1, a->z1, -0.5f, 0.0f, SK_FLOOR, 0);
		flat(a->x0, a->z0, a->x1, a->z1, a->h, 0.10f, 0.10f, 0.12f);
		addSolid(a->x0, a->z0, a->x1, a->z1, a->h, a->h + 0.5f, SK_FLOOR, 0);
		// worn patches and safety lines on the floor
		for (int i = 0; i < 9; i++) {
			float px = -26.0f + (float)(i % 3) * 20.0f, pz = -18.0f + (float)(i / 3) * 18.0f;
			float k = (i & 1) ? 0.27f : 0.34f;
			flat(px, pz, px + 16.0f, pz + 13.0f, 0.03f, k, k + 0.01f, k + 0.03f);
		}
		flat(-29.5f, -0.6f, 29.5f, 0.6f, 0.05f, 0.85f, 0.70f, 0.10f);   // yellow walkway line down the middle
		flat(-29.5f, -23.5f, 29.5f, -23.0f, 0.05f, 0.85f, 0.70f, 0.10f);
		flat(-29.5f, 23.0f, 29.5f, 23.5f, 0.05f, 0.85f, 0.70f, 0.10f);
		// walls
		wallRun(0, a->z0, a->x0, a->x1, -1, 0.0f, a->h, wall, 1);
		wallRun(0, a->z1, a->x0, a->x1, +1, 0.0f, a->h, wall, 1);
		wallRun(1, a->x0, a->z0, a->z1, -1, 0.0f, a->h, wall, 1);
		wallGap(1, a->x1, a->z0, a->z1, -2.5f, 2.5f, +1, 0.0f, a->h, wall);
		addGate(1, 30.0f, -2.5f, 2.5f, 0.0f);
		// high windows (moonlight) and roof trusses with hanging lamps
		for (float x = -24.0f; x <= 24.0f; x += 8.0f) {
			litWall(0, a->z0 + 0.04f, x - 2.4f, x + 2.4f, 7.4f, 10.2f, 0.34f, 0.46f, 0.80f);
			litWall(0, a->z1 - 0.04f, x - 2.4f, x + 2.4f, 7.4f, 10.2f, 0.34f, 0.46f, 0.80f);
		}
		for (float x = -26.0f; x <= 26.0f; x += 13.0f) {
			addBoxY(x - 0.35f, a->z0 + 0.05f, x + 0.35f, a->z1 - 0.05f, 11.1f, 11.6f, 0.14f, 0.14f, 0.16f);
			for (float z = -14.0f; z <= 14.0f; z += 14.0f) {
				addBoxY(x - 0.04f, z - 0.04f, x + 0.04f, z + 0.04f, 9.9f, 11.1f, 0.10f, 0.10f, 0.11f);
				litBox(x - 0.9f, z - 0.9f, x + 0.9f, z + 0.9f, 9.45f, 9.9f, 1.0f, 0.90f, 0.68f);
				flat(x - 4.5f, z - 4.5f, x + 4.5f, z + 4.5f, 0.06f, 0.38f, 0.37f, 0.36f);   // pool of light
			}
		}
		addBoxY(a->x0 + 0.05f, a->z0 + 0.05f, a->x1 - 0.05f, a->z0 + 0.45f, 11.0f, 11.5f, 0.12f, 0.12f, 0.14f);
		addBoxY(a->x0 + 0.05f, a->z1 - 0.45f, a->x1 - 0.05f, a->z1 - 0.05f, 11.0f, 11.5f, 0.12f, 0.12f, 0.14f);
		// pipes along the north and south walls
		addBoxY(a->x0, a->z0 + 0.05f, a->x1, a->z0 + 0.45f, 5.0f, 5.45f, 0.45f, 0.40f, 0.30f);
		addBoxY(a->x0, a->z1 - 0.45f, a->x1, a->z1 - 0.05f, 5.0f, 5.45f, 0.30f, 0.38f, 0.45f);
		addBoxY(a->x0 + 0.05f, a->z0, a->x0 + 0.45f, a->z1, 5.6f, 6.0f, 0.45f, 0.40f, 0.30f);

		// cover for the opening shootout
		propBox(-19.2f, -3.0f, -17.6f, 7.0f, 0.0f, 1.2f, 0.60f, 0.60f, 0.62f);                  // the hero's concrete barrier
		addBoxY(-19.3f, -3.1f, -17.5f, 7.1f, 1.2f, 1.28f, 0.80f, 0.66f, 0.10f);
		conveyor(1.0f, -7.0f, 2.6f, 9.0f, 0.0f);                                                // the gang's cover
		// north row: presses
		press(-26.0f, -22.5f, -19.0f, -17.5f, 0.0f, 4.0f);
		press(-12.0f, -22.5f,  -5.0f, -17.5f, 0.0f, 4.6f);
		press(  6.0f, -22.5f,  13.0f, -17.5f, 0.0f, 4.0f);
		press( 18.0f, -22.5f,  26.0f, -17.5f, 0.0f, 4.3f);
		// south row: crates and shelving
		crate(-24.0f, 21.0f, 2.2f, 0.0f, 0.55f, 0.40f, 0.22f); crate(-24.0f, 21.0f, 1.9f, 2.2f, 0.50f, 0.36f, 0.20f);
		crate(-16.0f, 21.5f, 2.2f, 0.0f, 0.50f, 0.38f, 0.22f); crate(-13.4f, 21.8f, 1.8f, 0.0f, 0.58f, 0.42f, 0.24f);
		crate( 10.0f, 21.0f, 2.2f, 0.0f, 0.55f, 0.40f, 0.22f); crate( 13.0f, 21.5f, 1.8f, 0.0f, 0.50f, 0.38f, 0.22f);
		propBox(20.0f, 18.5f, 26.0f, 22.5f, 0.0f, 3.0f, 0.28f, 0.34f, 0.40f);
		for (float yy = 0.8f; yy < 3.0f; yy += 0.9f) addBoxY(20.0f, 18.4f, 26.0f, 18.5f, yy, yy + 0.1f, 0.70f, 0.40f, 0.12f);
		// pillars
		pillar(-15.0f, -12.0f, 0.0f, 12.0f); pillar(-15.0f, 12.0f, 0.0f, 12.0f);
		pillar( 15.0f, -12.0f, 0.0f, 12.0f); pillar( 15.0f, 12.0f, 0.0f, 12.0f);
		// the enemy side: conveyors, crates and barrels with a clear way to the east door
		conveyor(10.0f, -10.0f, 22.0f, -8.4f, 0.0f);
		conveyor(10.0f,   8.4f, 22.0f, 10.0f, 0.0f);
		crate(25.0f, -6.5f, 2.2f, 0.0f, 0.55f, 0.40f, 0.22f);
		crate(25.0f,  6.5f, 1.8f, 0.0f, 0.50f, 0.38f, 0.22f);
		barrel(14.0f, -0.6f, 0.0f, 0.18f, 0.32f, 0.62f); barrel(15.2f, 0.8f, 0.0f, 0.62f, 0.20f, 0.18f);
		barrel(19.0f, -4.0f, 0.0f, 0.18f, 0.32f, 0.62f);
		propBox(17.5f, 3.0f, 18.7f, 6.0f, 0.0f, 1.2f, 0.60f, 0.60f, 0.62f);
		// west end decor
		barrel(-27.5f, 10.0f, 0.0f, 0.62f, 0.20f, 0.18f); barrel(-26.4f, 11.2f, 0.0f, 0.62f, 0.20f, 0.18f);
		crate(-27.0f, -8.0f, 1.8f, 0.0f, 0.50f, 0.38f, 0.22f);
		conveyor(-28.0f, 13.5f, -22.0f, 15.0f, 0.0f);
	}

	// ---------------- stairwells and hallways ----------------
	{
		const float stairWall[3] = {0.40f, 0.40f, 0.42f}, stairTread[3] = {0.45f, 0.45f, 0.47f};
		for (int i = 1; i < 10; i += 2) buildStair(&areas[i], stairWall, stairTread);
		for (int s = 0; s < 5; s++) {
			const Area* a = &areas[2 + s * 2];
			const Theme* T = &themes[s];
			hallInterior(a, s, 3 + s * 5);
			float y = a->yA, h = a->h;
			int lastLoop = (s == 4);
			// end walls and the long walls with their doorways
			if (s == 0) wallGap(1, a->x0, a->z0, a->z1, -2.5f, 2.5f, -1, y, h, T->wall);        // arrival from stairwell 1
			else        wallRun(1, a->x0, a->z0, a->z1, -1, y, h, T->wall, 1);
			if (lastLoop) wallGap(1, a->x1, a->z0, a->z1, 82.5f, 85.5f, +1, y, h, T->wall);     // door to the roof
			else          wallRun(1, a->x1, a->z0, a->z1, +1, y, h, T->wall, 1);
			int exitEast = (s % 2 == 0);   // halls 1, 3, 5 head east and leave at the east end; 2 and 4 head west
			if (s == 0) { wallRun(0, a->z0, a->x0, a->x1, -1, y, h, T->wall, 1); wallGap(0, a->z1, a->x0, a->x1, 105.0f, 110.0f, +1, y, h, T->wall); }
			else if (s == 1) { wallGap(0, a->z0, a->x0, a->x1, 105.0f, 110.0f, -1, y, h, T->wall); wallGap(0, a->z1, a->x0, a->x1, 44.0f, 49.0f, +1, y, h, T->wall); }
			else if (s == 2) { wallGap(0, a->z0, a->x0, a->x1, 44.0f, 49.0f, -1, y, h, T->wall); wallGap(0, a->z1, a->x0, a->x1, 105.0f, 110.0f, +1, y, h, T->wall); }
			else if (s == 3) { wallGap(0, a->z0, a->x0, a->x1, 105.0f, 110.0f, -1, y, h, T->wall); wallGap(0, a->z1, a->x0, a->x1, 44.0f, 49.0f, +1, y, h, T->wall); }
			else { wallGap(0, a->z0, a->x0, a->x1, 44.0f, 49.0f, -1, y, h, T->wall); wallRun(0, a->z1, a->x0, a->x1, +1, y, h, T->wall, 1); }
			(void)exitEast;
		}
		// gates: the exit of each hall is shut until its gang is dead
		addGate(0,  3.5f, 105.0f, 110.0f,  6.0f);
		addGate(0, 24.5f,  44.0f,  49.0f, 12.0f);
		addGate(0, 45.5f, 105.0f, 110.0f, 18.0f);
		addGate(0, 66.5f,  44.0f,  49.0f, 24.0f);
		addGate(1, 110.0f, 82.5f,  85.5f, 30.0f);
	}

	// ---------------- the roof ----------------
	{
		const Area* a = &areas[11];
		const float par[3] = {0.34f, 0.34f, 0.37f};
		flat(a->x0, a->z0, a->x1, a->z1, 30.0f, 0.24f, 0.25f, 0.28f);
		addSolid(a->x0, a->z0, a->x1, a->z1, 29.5f, 30.0f, SK_FLOOR, 0);
		for (int i = 0; i < 5; i++) flat(a->x0 + 4.0f + (float)i * 11.0f, a->z0 + 2.0f, a->x0 + 13.0f + (float)i * 11.0f, a->z1 - 2.0f, 30.03f, 0.21f + 0.02f * (float)(i & 1), 0.22f, 0.25f);
		flat(a->x0, a->z0, a->x1, a->z0 + 0.5f, 30.05f, 0.80f, 0.66f, 0.10f);
		// parapets: low walls round the edge (the building's own wall is the hallway's east wall)
		wallRun(0, a->z0, a->x0, a->x1, -1, 30.0f, 1.2f, par, 1);
		wallRun(0, a->z1, a->x0, a->x1, +1, 30.0f, 1.2f, par, 1);
		wallRun(1, a->x1, a->z0, a->z1, +1, 30.0f, 1.2f, par, 1);
		wallRun(1, a->x0, a->z0, 80.5f, -1, 30.0f, 1.2f, par, 1);
		wallRun(1, a->x0, 87.5f, a->z1, -1, 30.0f, 1.2f, par, 1);
		for (float x = a->x0 + 2.0f; x < a->x1; x += 4.0f) { // parapet caps
			addBoxY(x, a->z0 - 0.05f, x + 2.0f, a->z0 + 0.05f, 31.2f, 31.3f, 0.40f, 0.40f, 0.43f);
			addBoxY(x, a->z1 - 0.05f, x + 2.0f, a->z1 + 0.05f, 31.2f, 31.3f, 0.40f, 0.40f, 0.43f);
		}
		// rooftop clutter: air conditioners, a water tank on stilts, vents, an antenna with a red light
		propBox(118.0f, 76.0f, 123.5f, 80.0f, 30.0f, 2.0f, 0.55f, 0.56f, 0.60f);
		addBoxY(118.5f, 76.5f, 123.0f, 79.5f, 32.0f, 32.3f, 0.40f, 0.41f, 0.44f);
		propBox(140.0f, 88.5f, 146.0f, 92.0f, 30.0f, 2.2f, 0.55f, 0.56f, 0.60f);
		propBox(150.0f, 76.0f, 155.0f, 79.5f, 30.0f, 1.8f, 0.55f, 0.56f, 0.60f);
		for (int i = 0; i < 4; i++) {
			float sx = (i & 1) ? 134.0f : 130.0f, sz = (i & 2) ? 80.0f : 76.5f;
			propBox(sx - 0.25f, sz - 0.25f, sx + 0.25f, sz + 0.25f, 30.0f, 3.2f, 0.30f, 0.30f, 0.33f);
		}
		addBoxY(129.4f, 75.9f, 134.6f, 80.6f, 33.2f, 36.4f, 0.42f, 0.30f, 0.20f);
		propBox(158.0f, 89.0f, 158.8f, 89.8f, 30.0f, 10.0f, 0.30f, 0.30f, 0.33f);
		litBox(157.9f, 88.9f, 158.9f, 89.9f, 40.0f, 40.7f, 1.0f, 0.10f, 0.10f);
		propBox(112.0f, 90.0f, 114.0f, 92.0f, 30.0f, 1.4f, 0.50f, 0.50f, 0.54f);
		propBox(160.0f, 78.0f, 162.0f, 80.0f, 30.0f, 1.4f, 0.50f, 0.50f, 0.54f);
		litBox(137.0f, 93.2f, 143.0f, 93.45f, 30.2f, 30.5f, 0.9f, 0.9f, 0.5f);
		// distant city: dark towers with a few lit windows, round the roof; streets as dim light lines far below
		cityS = 0x5CA1Eu;
		for (int k = 0; k < 30; k++) {
			float ang = 6.28319f * ((float)k + crand() * 0.7f) / 30.0f;
			float rad = 75.0f + crand() * 110.0f, ccx = 138.0f + cosf(ang) * rad, ccz = 84.0f + sinf(ang) * rad;
			if (ccx < 135.0f && ccz > -50.0f && ccz < 130.0f) continue;      // keep clear of the factory itself
			float w = cr(14.0f, 28.0f), d = cr(14.0f, 28.0f), hh = cr(24.0f, 100.0f), v = cr(0.85f, 1.1f);
			float x0 = ccx - w * 0.5f, x1 = ccx + w * 0.5f, z0 = ccz - d * 0.5f, z1 = ccz + d * 0.5f;
			addBoxY(x0, z0, x1, z1, -0.5f, hh, 0.13f * v, 0.15f * v, 0.20f * v);
			// facing the roof: pick the sides toward (138, 84)
			int fx = (138.0f < ccx), fz = (84.0f < ccz);
			for (int side = 0; side < 2; side++) {
				int axis = side;                                   // 0: face on a z plane, 1: on an x plane
				float fix = (axis == 0) ? (fz ? z0 - 0.05f : z1 + 0.05f) : (fx ? x0 - 0.05f : x1 + 0.05f);
				float a0 = (axis == 0) ? x0 : z0, a1 = (axis == 0) ? x1 : z1;
				for (float yy = 3.0f; yy < hh - 2.0f; yy += 4.0f)
					for (float cc = a0 + 2.0f; cc < a1 - 2.0f; cc += 4.0f)
						if (crand() < 0.30f) litWall(axis, fix, cc, cc + 1.6f, yy, yy + 1.8f, 1.0f, 0.82f + crand() * 0.1f, 0.45f);
			}
		}
		for (int k = -8; k <= 8; k++) { // a street grid of dim orange lines far below
			litBox(-500.0f, (float)k * 60.0f + 20.0f, 800.0f, (float)k * 60.0f + 21.5f, -0.5f, -0.45f, 0.50f, 0.34f, 0.14f);
			litBox((float)k * 60.0f + 20.0f, -500.0f, (float)k * 60.0f + 21.5f, 800.0f, -0.5f, -0.45f, 0.50f, 0.34f, 0.14f);
		}
	}

	// ---------------- lit pass ----------------
	worldStaticEnd = vcount;
	for (int i = 0; i < nLit; i++) {
		const LitItem* l = &litList[i];
		if (l->type == 0) addBoxY(l->p[0], l->p[1], l->p[2], l->p[3], l->p[4], l->p[5], l->r, l->g, l->b);
		else wallQuad((int)l->p[5], l->p[0], l->p[1], l->p[2], l->p[3], l->p[4], l->r, l->g, l->b);
	}
	staticCount = vcount;
}


// ---- the player's health, gang members, doors ------------------------------------------------------
static float pHP = 100.0f;
static int   pHurtT = 0, pRegenT = 0;
static float gCamX = 0, gCamY = 0, gCamZ = 0, gCamYaw = 0;   // where the camera is (for sound panning)

static void hurtPlayer(float dmg) {
	if (pHP <= 0.0f) return;
	pHP -= dmg;
	if (pHP < 0.0f) pHP = 0.0f;
	pHurtT = 24; pRegenT = 260;
	sfxPlay(S_HIT, 0.9f, 0.0f, 0.65f);
	if (shake < 0.18f) shake = 0.18f;
}

// outfits for the gang: everything red, but different faces, builds, hair and cut of clothes
static void redOutfit(Outfit* o, int female) {
	randOutfit(o);
	float k = frange(0.80f, 1.0f);
	o->jr = 0.82f * k; o->jg = 0.06f * k; o->jb = 0.07f * k;       // red top
	o->pr = 0.46f * k; o->pg = 0.05f * k; o->pb = 0.06f * k;       // dark red trousers / skirt
	o->xr = 0.18f; o->xg = 0.05f; o->xb = 0.05f;                    // dark red shoes
	o->ar = 0.90f * k; o->ag = 0.08f * k; o->ab = 0.08f * k;       // red cap / bandana
	o->extra &= 2;                                                  // sunglasses only
	if (female) {
		o->hair = (frand() < 0.5f) ? 2 : 6;
		o->top = (frand() < 0.5f) ? 3 : 0;
		o->bottom = (o->top == 3) ? 2 : ((frand() < 0.4f) ? 2 : 0);
		o->w = frange(0.86f, 0.98f); o->h = frange(0.92f, 1.02f);
	} else {
		if (o->hair == 7 || o->hair == 6 || o->hair == 2) o->hair = 3;   // red cap instead of long hair / wide hat
		o->top = (frand() < 0.45f) ? 0 : (frand() < 0.6f ? 1 : 2);
		o->bottom = (frand() < 0.15f) ? 1 : 0;
		o->w = frange(0.98f, 1.20f); o->h = frange(0.96f, 1.08f);
	}
}

typedef struct { int stage; float x, z; int gun; int female; int cut; } FoeSpawn;
static const FoeSpawn foeSpawns[] = {
	// stage 0: two talkers for the opening cutscene (cut = 1), then the five guarding the factory floor
	{0,  4.4f, -1.6f, WP_PISTOL,  0, 1}, {0,  4.8f,  4.0f, WP_SMG,     0, 1},
	{0, 12.0f, -5.5f, WP_SMG,     0, 0}, {0, 14.0f,  5.6f, WP_PISTOL,  0, 0}, {0, 20.0f, -6.6f, WP_SHOTGUN, 0, 0},
	{0, 21.0f,  6.4f, WP_PISTOL,  1, 0}, {0, 26.0f,  0.2f, WP_SMG,     0, 0},
	// hallways (cut = 2: the two who speak in the third-hallway cutscene)
	{1, 80.0f, -0.5f, WP_PISTOL,  0, 0}, {1, 96.0f,  0.8f, WP_SMG,     0, 0},
	{2, 78.0f, 21.2f, WP_SMG,     0, 0}, {2, 62.0f, 20.8f, WP_PISTOL,  1, 0},
	{3, 91.5f, 41.2f, WP_SMG,     1, 2}, {3, 95.0f, 43.0f, WP_PISTOL,  0, 2}, {3, 72.0f, 42.3f, WP_SHOTGUN, 0, 0},
	{4, 85.0f, 63.2f, WP_SMG,     0, 0}, {4, 70.0f, 62.7f, WP_SHOTGUN, 0, 0}, {4, 56.0f, 63.1f, WP_PISTOL,  1, 0},
	{5, 70.0f, 84.3f, WP_SMG,     0, 0}, {5, 84.0f, 83.7f, WP_SHOTGUN, 0, 0}, {5, 98.0f, 84.3f, WP_PISTOL,  1, 0},
};
#define N_SPAWN ((int)(sizeof foeSpawns / sizeof foeSpawns[0]))

static float stageFloorY(int stage) { return areas[stage * 2].yA; }

static void reviveFoe(Foe* f) {
	f->x = f->homeX; f->z = f->homeZ;
	f->hp = 75; f->state = FS_IDLE; f->cool = 60; f->burst = 0; f->hitFlash = 0; f->deadT = 0;
	f->strafeT = 0; f->strafeDir = 0.0f; f->delay = 0; f->aimE = 0.0f; f->moveAmt = 0.0f;
}

static void initFoes(void) {
	nFoe = 0;
	static const float faceTo[6] = { 1.5708f, 1.5708f, -1.5708f, 1.5708f, -1.5708f, 1.5708f };
	for (int i = 0; i < N_SPAWN && nFoe < N_FOE; i++) {
		const FoeSpawn* sp = &foeSpawns[i];
		Foe* f = &foes[nFoe++];
		memset(f, 0, sizeof *f);
		f->stage = sp->stage; f->homeX = sp->x; f->homeZ = sp->z; f->y = stageFloorY(sp->stage);
		f->gun = sp->gun; f->female = sp->female; f->cut = sp->cut;
		f->yaw = faceTo[sp->stage]; f->phase = frange(0.0f, 6.0f);
		if (sp->cut == 1) f->yaw = 1.5708f;
		redOutfit(&f->outfit, sp->female);
		reviveFoe(f);
	}
}

static int stageAlive(int stage) {
	int n = 0;
	for (int i = 0; i < nFoe; i++) if (foes[i].stage == stage && foes[i].cut != 1 && foes[i].state != FS_DEAD && foes[i].state != FS_OFF) n++;
	return n;
}

static int stCleared[6];       // gate opened for this stage
static int stNewClear = -1;    // set when a stage is cleared this frame (the story code reacts)
static int barkCD = 0;

static void alertStage(int stage, int delay) {
	int any = 0;
	for (int i = 0; i < nFoe; i++) {
		Foe* f = &foes[i];
		if (f->stage != stage || f->cut == 1 || f->state != FS_IDLE) continue;
		f->state = FS_ALERT; f->delay = delay + (int)frange(0.0f, 30.0f); f->cool = 30 + (int)frange(0.0f, 40.0f);
		any = 1;
	}
	if (any && barkCD <= 0) {
		voicePlay(S_V_BARK1 + (int)(frand() * 3.0f) % 3, 0.9f);
		barkCD = 600;
	}
}

static int damageFoe(int i, int dmg) {
	Foe* f = &foes[i];
	if (f->state == FS_DEAD || f->state == FS_OFF) return 0;
	f->hp -= dmg;
	f->hitFlash = 7;
	if (f->state == FS_IDLE) alertStage(f->stage, 10);
	if (f->hp <= 0) {
		f->state = FS_DEAD; f->deadT = 0; f->aimE = 0.0f;
		kills++;
		if (curW != WP_FISTS) wRes[curW] += wdef[curW].clipMax / 2 + 1;
		sfxPlay(S_THUD, 0.9f, 0.0f, frange(0.85f, 1.0f));
		if (stageAlive(f->stage) == 0 && !stCleared[f->stage]) stNewClear = f->stage;
		return 1;
	}
	sfxPlay(S_HIT, 0.6f, 0.0f, frange(0.95f, 1.1f));
	return 0;
}

// clear line between two points? (walls, floors and other people block it)
static int losClear(const float a[3], const float b[3]) {
	float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (L < 0.2f) return 1;
	d[0] /= L; d[1] /= L; d[2] /= L;
	int n;
	float t = castRay(a, d, 0.05f, L, &n);
	return t >= L - 0.05f;
}

static float panTo(float x, float z) {
	float dx = x - gCamX, dz = z - gCamZ, L = sqrtf(dx * dx + dz * dz);
	if (L < 0.5f) return 0.0f;
	float p = (dx * cosf(gCamYaw) + dz * (-sinf(gCamYaw))) / L;
	return p < -1.0f ? -1.0f : (p > 1.0f ? 1.0f : p);
}

static void foeMuzzle(const Foe* f, float m[3]) {
	float fx = -sinf(f->yaw), fz = -cosf(f->yaw);
	m[0] = f->x + fx * 0.95f + (-fz) * 0.36f * -1.0f; m[1] = f->y + 1.42f; m[2] = f->z + fz * 0.95f + fx * 0.36f * -1.0f;
	// right-hand side of the facing direction is (cos, -sin)
	m[0] = f->x + fx * 0.95f + cosf(f->yaw) * 0.36f;
	m[2] = f->z + fz * 0.95f - sinf(f->yaw) * 0.36f;
}

static void foeShoot(Foe* f, float px, float pz, float py, int moving) {
	float dist = sqrtf((px - f->x) * (px - f->x) + (pz - f->z) * (pz - f->z));
	float m[3], tgt[3] = { px, py + 1.15f, pz };
	foeMuzzle(f, m);
	float chance = 0.58f - dist * 0.026f;
	if (chance < 0.10f) chance = 0.10f;
	if (chance > 0.45f) chance = 0.45f;
	if (moving) chance *= 0.65f;
	float dmg = (f->gun == WP_PISTOL) ? 6.0f : (f->gun == WP_SMG) ? 3.5f : (dist < 6.0f ? 20.0f : dist < 11.0f ? 12.0f : 6.0f);
	int hit = frand() < chance;
	float aim[3] = { tgt[0], tgt[1], tgt[2] };
	if (!hit) { aim[0] += frange(-1.6f, 1.6f); aim[1] += frange(-0.9f, 0.7f); aim[2] += frange(-1.6f, 1.6f); }
	float d[3] = { aim[0] - m[0], aim[1] - m[1], aim[2] - m[2] };
	float L = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (L < 0.1f) return;
	d[0] /= L; d[1] /= L; d[2] /= L;
	float end[3];
	if (hit) { memcpy(end, aim, sizeof end); }
	else {
		int n;
		float t = castRay(m, d, 0.05f, 60.0f, &n);
		for (int k = 0; k < 3; k++) end[k] = m[k] + d[k] * t;
		if (t < 59.0f) { float v[3] = { frange(-0.05f, 0.05f), 0.04f, frange(-0.05f, 0.05f) }; addSpark(end, v, 0.07f, 10, 0.85f, 0.80f, 0.65f); }
	}
	addTracerFx(m, end);
	addSpark(m, NULL, f->gun == WP_SHOTGUN ? 0.28f : 0.15f, 3, 1.0f, 0.75f, 0.35f);
	float vol = 1.0f - dist / 48.0f;
	if (vol < 0.22f) vol = 0.22f;
	sfxPlay(S_PISTOL + (f->gun - WP_PISTOL), vol * 0.85f, panTo(f->x, f->z), frange(0.93f, 1.03f));
	if (hit) hurtPlayer(dmg);
}

static int foeMove(Foe* f, float dx, float dz, const Area* a) {
	float nx = f->x + dx, nz = f->z + dz;
	if (nx < a->x0 + 1.2f || nx > a->x1 - 1.2f) dx = 0.0f;
	if (nz < a->z0 + 1.2f || nz > a->z1 - 1.2f) dz = 0.0f;
	int moved = 0;
	float keepY = gFootY;
	gFootY = f->y;                       // solids are tested at the floor the gang member stands on
	if (dx != 0.0f && !blocked(f->x + dx, f->z)) { f->x += dx; moved = 1; }
	if (dz != 0.0f && !blocked(f->x, f->z + dz)) { f->z += dz; moved = 1; }
	gFootY = keepY;
	return moved;
}

static void updateFoes(float px, float pz, float py, int stage, int moving) {
	if (barkCD > 0) barkCD--;
	for (int i = 0; i < nFoe; i++) {
		Foe* f = &foes[i];
		if (f->hitFlash > 0) f->hitFlash--;
		if (f->state == FS_DEAD) { if (f->deadT < 1000) f->deadT++; continue; }
		if (f->state == FS_OFF || f->cut == 1 || f->stage != stage) continue;
		float dx = px - f->x, dz = pz - f->z, dist = sqrtf(dx * dx + dz * dz);
		float want = atan2f(-dx, -dz), dl = want - f->yaw;
		while (dl >  3.14159f) dl -= 6.28318f;
		while (dl < -3.14159f) dl += 6.28318f;
		float eye[3] = { f->x, f->y + 1.45f, f->z }, tg[3] = { px, py + 1.2f, pz };
		if (f->state == FS_IDLE) {
			f->aimE += (0.0f - f->aimE) * 0.1f;
			f->moveAmt += (0.0f - f->moveAmt) * 0.15f;
			if (dist < 34.0f && fabsf(dl) < 1.9f && losClear(eye, tg)) alertStage(f->stage, 25);
			continue;
		}
		// alert: turn to the player, keep a distance, strafe, shoot
		f->yaw += dl * 0.12f;
		int los = losClear(eye, tg);
		float lo = (f->gun == WP_SHOTGUN) ? 6.0f : 9.0f, hi = (f->gun == WP_SHOTGUN) ? 11.0f : 17.0f;
		float fx = -sinf(f->yaw), fz = -cosf(f->yaw), rx = cosf(f->yaw), rz = -sinf(f->yaw);
		float mvx = 0.0f, mvz = 0.0f;
		if (dist > hi || !los) { mvx += fx * 0.05f; mvz += fz * 0.05f; }
		else if (dist < lo)     { mvx -= fx * 0.04f; mvz -= fz * 0.04f; }
		if (--f->strafeT <= 0) { f->strafeT = 40 + (int)frange(0.0f, 70.0f); f->strafeDir = (float)((int)(frand() * 3.0f) - 1); }
		if (los) { mvx += rx * 0.028f * f->strafeDir; mvz += rz * 0.028f * f->strafeDir; }
		for (int j = 0; j < nFoe; j++) { // do not stack on each other
			if (j == i || foes[j].state == FS_DEAD || foes[j].stage != f->stage) continue;
			float ox = f->x - foes[j].x, oz = f->z - foes[j].z, od = sqrtf(ox * ox + oz * oz);
			if (od < 1.1f && od > 0.01f) { mvx += ox / od * 0.04f; mvz += oz / od * 0.04f; }
		}
		int moved = (mvx != 0.0f || mvz != 0.0f) ? foeMove(f, mvx, mvz, &areas[f->stage * 2]) : 0;
		f->moveAmt += ((moved ? 1.0f : 0.0f) - f->moveAmt) * 0.15f;
		if (moved) f->phase += 0.15f;
		if (f->delay > 0) f->delay--;
		if (f->cool > 0) f->cool--;
		float aimT = (los && f->cool < 22) ? 1.0f : 0.45f;
		if (f->burst > 0) aimT = 1.0f;
		f->aimE += (aimT - f->aimE) * 0.2f;
		if (los && f->delay <= 0 && fabsf(dl) < 0.5f) {
			if (f->burst > 0) {
				if (f->cool <= 0) { foeShoot(f, px, pz, py, moving); f->burst--; f->cool = 6; if (f->burst == 0) f->cool = 80 + (int)frange(0.0f, 60.0f); }
			} else if (f->cool <= 0 && f->aimE > 0.85f) {
				if (f->gun == WP_PISTOL)  { foeShoot(f, px, pz, py, moving); f->cool = 55 + (int)frange(0.0f, 55.0f); }
				else if (f->gun == WP_SMG) { f->burst = 3 + (int)frange(0.0f, 2.0f); }
				else if (dist < 16.0f)    { foeShoot(f, px, pz, py, moving); f->cool = 120 + (int)frange(0.0f, 50.0f); }
			}
		}
	}
}

static void drawFoes(float camx, float camz) {
	for (int i = 0; i < nFoe; i++) {
		const Foe* f = &foes[i];
		if (f->state == FS_OFF) continue;
		float dx = f->x - camx, dz = f->z - camz;
		if (dx * dx + dz * dz > 90.0f * 90.0f) continue;
		Pose p;
		memset(&p, 0, sizeof p);
		p.px = f->x; p.pz = f->z; p.py = f->y; p.face = f->yaw; p.phase = f->phase; p.moveAmt = f->moveAmt;
		p.gun = f->gun; p.aimE = f->aimE; p.gunPitch = 0.0f;
		if (f->state == FS_DEAD) { p.fall = fminf((float)f->deadT / 16.0f, 1.0f); p.aimE = 0.0f; p.moveAmt = 0.0f; }
		Outfit o = f->outfit;
		if (f->hitFlash > 0) { o.jr = 1.0f; o.jg = 0.9f; o.jb = 0.9f; }
		if (f->state == FS_DEAD) { o.jr *= 0.8f; o.jg *= 0.8f; o.jb *= 0.8f; }
		drawPerson(&p, &o);
	}
}

// ---- the doors ---------------------------------------------------------------------------------------
static void updateGates(void) {
	for (int g = 0; g < nGate; g++) {
		Gate* d = &gates[g];
		float target = stCleared[g] ? 1.0f : 0.0f;
		d->open += (target - d->open) * 0.05f;
		if (d->open > 0.985f) d->open = 1.0f;
		if (d->solid >= 0) {
			if (d->open > 0.55f) { solids[d->solid].y0 = -9999.0f; solids[d->solid].y1 = -9998.0f; }
			else { solids[d->solid].y0 = d->y; solids[d->solid].y1 = d->y + DOORH; }
		}
	}
}

// door leaves (steel panels that slide up) - drawn with the lit world
static void drawGates(void) {
	for (int g = 0; g < nGate; g++) {
		const Gate* d = &gates[g];
		if (d->open >= 0.995f) continue;
		float y0 = d->y + d->open * DOORH, y1 = d->y + DOORH;
		if (d->axis == 0) {
			addBoxY(d->a0, d->fix - 0.12f, d->a1, d->fix + 0.12f, y0, y1, 0.34f, 0.36f, 0.40f);
			addBoxY(d->a0 + 0.1f, d->fix - 0.14f, d->a1 - 0.1f, d->fix + 0.14f, y0 + (y1 - y0) * 0.42f, y0 + (y1 - y0) * 0.58f, 0.85f, 0.68f, 0.10f);
		} else {
			addBoxY(d->fix - 0.12f, d->a0, d->fix + 0.12f, d->a1, y0, y1, 0.34f, 0.36f, 0.40f);
			addBoxY(d->fix - 0.14f, d->a0 + 0.1f, d->fix + 0.14f, d->a1 - 0.1f, y0 + (y1 - y0) * 0.42f, y0 + (y1 - y0) * 0.58f, 0.85f, 0.68f, 0.10f);
		}
	}
}
// the red / green status lamps above them: not darkened by the tint
static void drawGateLamps(int frame) {
	for (int g = 0; g < nGate; g++) {
		const Gate* d = &gates[g];
		float pulse = 0.6f + 0.4f * sinf((float)frame * 0.15f);
		float r = stCleared[g] ? 0.15f : 1.0f * pulse, gg = stCleared[g] ? 1.0f : 0.10f, b = stCleared[g] ? 0.3f : 0.10f;
		float yy = d->y + DOORH + 0.62f;
		if (d->axis == 0) { addBoxY((d->a0 + d->a1) * 0.5f - 0.3f, d->fix - 0.2f, (d->a0 + d->a1) * 0.5f + 0.3f, d->fix + 0.2f, yy, yy + 0.3f, r, gg, b); }
		else              { addBoxY(d->fix - 0.2f, (d->a0 + d->a1) * 0.5f - 0.3f, d->fix + 0.2f, (d->a0 + d->a1) * 0.5f + 0.3f, yy, yy + 0.3f, r, gg, b); }
	}
}


// ================================================================================================
// STORY STATE SHARED WITH THE MAIN LOOP
// (these used to be locals of main(); the cutscenes need to move the hero and the camera too)
// ================================================================================================
static float px, pz, yaw, pitch, face, phase, moveAmt, aimE, camDist, camSide, camFov;
static float gVisY = 0.0f;       // smoothed floor height under the hero (stairs bob less this way)
static int   gAi = -1;           // index of the area the hero stands in
enum { SM_OFF = 0, SM_PLAY = 1, SM_FAIL = 2, SM_END = 3 };
static int   stMode = SM_OFF;    // not in story mode / playing / failed / mission over
static char  stName[16] = "Alex";
static int   stSlot = 0;
static int   stCP = 0;           // checkpoint = number of the stage we restart from
static int   stHall3Seen = 0, stRoofSeen = 0;
static int   stHint = 0;         // frames left showing the "area clear" line
static int   failT = 0, endT = 0;
static int   stSavePending = 0;  // mission finished: write the save, then go back to the title

static float camCeil(float x, float z) {
	int ai = areaAt(x, z);
	if (gLevel != LV_FACTORY || ai < 0 || areas[ai].kind == AK_ROOF) return 1e9f;
	return areaRamp(&areas[ai], x, z, 0) + areas[ai].h - 0.3f;
}

static void camTargets(int style, int aiming, float* dT, float* sT, float* fT) {
	if (style == CAM_FIRST)         { *dT = 0.0f; *sT = 0.0f;  *fT = aiming ? 48.0f : 68.0f; }
	else if (style == CAM_SHOULDER) { *dT = aiming ? 2.3f : 3.7f; *sT = aiming ? 0.85f : 0.75f; *fT = aiming ? 52.0f : 62.0f; }
	else                            { *dT = aiming ? 3.3f : 6.5f; *sT = aiming ? 0.95f : 0.0f;  *fT = aiming ? 52.0f : 65.0f; }
}

// where the gameplay camera sits for a hero at (x, z) looking (yw, pt): pulled in when something solid is in the way
static void chaseCamera(float x, float z, float yw, float pt, float dist, float side, int fpv, float out[3]) {
	float rgx = cosf(yw), rgz = -sinf(yw);
	if (fpv) {
		out[0] = x + sinf(yw) * 0.14f; out[2] = z + cosf(yw) * 0.14f; out[1] = gVisY + EYE_H + 0.02f;
		return;
	}
	float flat = cosf(pt), d = dist, cx, cz;
	while (d > 1.2f) {
		cx = x + flat * sinf(yw) * d + rgx * side;
		cz = z + flat * cosf(yw) * d + rgz * side;
		if (!blocked(cx, cz)) break;
		d -= 0.4f;
	}
	cx = x + flat * sinf(yw) * d + rgx * side;
	cz = z + flat * cosf(yw) * d + rgz * side;
	float cy = gVisY + 1.6f + sinf(pt) * d;
	if (gLevel == LV_FACTORY) {
		float cl = camCeil(cx, cz);
		if (cy > cl) cy = cl;
		if (cy < gVisY + 0.4f) cy = gVisY + 0.4f;
	} else if (cy < 0.4f) cy = 0.4f;
	out[0] = cx; out[1] = cy; out[2] = cz;
}

// ================================================================================================
// CUTSCENES
// A cutscene is a camera track (a few keyframes, Catmull-Rom between them, hard cuts where marked), a list of
// spoken lines with subtitles, and a few scripted events. Everything on screen is worked out from the clock
// (csT), so skipping with START just jumps the clock to the end.
// ================================================================================================
#define CS_BAR_TOP 28.0f        // cinematic black bars: top and bottom height in pixels (set both to 0 for none)
#define CS_BAR_BOT 52.0f
enum { CS_NONE = 0, CS_INTRO = 1, CS_HALL3 = 2, CS_ROOF = 3 };
typedef struct { float t, p[3], l[3], fov; int cut; } CamKey;
#define CS_MAXK 28
static CamKey csKeys[CS_MAXK];
static int    csNKeys = 0;
static int    csKind = CS_NONE, csDone = CS_NONE, csSkipped = 0;
static float  csT = 0.0f, csLen = 1.0f, csDt = 0.0f;
static u64    csLastMs = 0;
static float  csFlash = 0.0f, csBlack = 0.0f;

typedef struct { float t, dur; int snd, kind; const char* who; const char* text; } CsLine;
static CsLine csLines[6];
static int    csNLines = 0;

static void csAddLine(float t, int snd, int kind, const char* who, const char* text, float dur) {
	if (csNLines >= 6) return;
	CsLine* l = &csLines[csNLines++];
	l->t = t; l->snd = snd; l->kind = kind; l->who = who; l->text = text; l->dur = dur;
}

static void csKey(float t, float x, float y, float z, float lx, float ly, float lz, float fov, int cut) {
	if (csNKeys >= CS_MAXK) return;
	CamKey* k = &csKeys[csNKeys++];
	k->t = t; k->p[0] = x; k->p[1] = y; k->p[2] = z; k->l[0] = lx; k->l[1] = ly; k->l[2] = lz; k->fov = fov; k->cut = cut;
}

static float crom(float a, float b, float c, float d, float u) {
	return 0.5f * ((2.0f * b) + (-a + c) * u + (2.0f * a - 5.0f * b + 4.0f * c - d) * u * u + (-a + 3.0f * b - 3.0f * c + d) * u * u * u);
}

static void csTrack(float t, float pos[3], float look[3], float* fov) {
	int i = 0;
	while (i + 1 < csNKeys && csKeys[i + 1].t <= t) i++;
	const CamKey* a = &csKeys[i];
	if (i + 1 >= csNKeys || csKeys[i + 1].cut) {
		memcpy(pos, a->p, sizeof a->p); memcpy(look, a->l, sizeof a->l); *fov = a->fov;
		return;
	}
	const CamKey* b = &csKeys[i + 1];
	const CamKey* p0 = (i > 0 && !a->cut) ? &csKeys[i - 1] : a;
	const CamKey* p3 = (i + 2 < csNKeys && !csKeys[i + 2].cut) ? &csKeys[i + 2] : b;
	float u = (t - a->t) / (b->t - a->t);
	if (u < 0.0f) u = 0.0f;
	if (u > 1.0f) u = 1.0f;
	for (int k = 0; k < 3; k++) {
		pos[k]  = crom(p0->p[k], a->p[k], b->p[k], p3->p[k], u);
		look[k] = crom(p0->l[k], a->l[k], b->l[k], p3->l[k], u);
	}
	*fov = crom(p0->fov, a->fov, b->fov, p3->fov, u);
}

static void csAim(const float pos[3], const float look[3], float* yw, float* pt) {
	float dx = look[0] - pos[0], dy = look[1] - pos[1], dz = look[2] - pos[2];
	*yw = atan2f(-dx, -dz);
	*pt = atan2f(-dy, sqrtf(dx * dx + dz * dz));
}

static int csCross(float t0, float t1, float at) { return t0 < at && t1 >= at; }
static float sstep(float u) { u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u); return u * u * (3.0f - 2.0f * u); }
static float lerpf(float a, float b, float u) { return a + (b - a) * u; }

// ---- the actors that are not gang members from the foe list --------------------------------------------
static Outfit vinceOutfit;
static float  vX = 111.0f, vZ = 84.3f, vFace = -1.57f, vAim = 0.0f, vMove = 0.0f;
static int    vOn = 0;
static float  gunDropOn = 0.0f, gunDropX, gunDropZ, gunDropY;  // the dropped weapon (roof scene)
static int    gunDropKind = WP_SMG;
static float  grenOn = 0.0f, grenP[3];

static void initVince(void) {
	vinceOutfit = playerOutfit;
	vinceOutfit.jr = 0.80f; vinceOutfit.jg = 0.04f; vinceOutfit.jb = 0.06f;    // the red suit
	vinceOutfit.pr = 0.62f; vinceOutfit.pg = 0.03f; vinceOutfit.pb = 0.05f;
	vinceOutfit.sr = 0.90f; vinceOutfit.sg = 0.72f; vinceOutfit.sb = 0.62f;
	vinceOutfit.hr = 0.10f; vinceOutfit.hg = 0.09f; vinceOutfit.hb = 0.09f;
	vinceOutfit.xr = 0.05f; vinceOutfit.xg = 0.04f; vinceOutfit.xb = 0.04f;
	vinceOutfit.hair = 1; vinceOutfit.top = 1; vinceOutfit.bottom = 0; vinceOutfit.extra = 4; vinceOutfit.bare = 0;
	vinceOutfit.w = 1.10f; vinceOutfit.h = 1.04f;
}

// ---- gunfire in the background of the intro --------------------------------------------------------------
static void csShot(const float m[3], const float tgt[3], int gun, float vol, float pan) {
	addTracerFx(m, tgt);
	addSpark(m, NULL, gun == WP_SHOTGUN ? 0.26f : 0.15f, 3, 1.0f, 0.78f, 0.38f);
	float v[3] = { frange(-0.05f, 0.05f), 0.04f, frange(-0.05f, 0.05f) };
	addSpark(tgt, v, 0.08f, 10, 0.90f, 0.82f, 0.62f);
	sfxPlay(S_PISTOL + (gun - WP_PISTOL), vol, pan, frange(0.94f, 1.04f));
}

static void heroMuzzle(float m[3]) {
	float ffx = -sinf(face), ffz = -cosf(face), rfx = cosf(face), rfz = -sinf(face);
	m[0] = px + ffx * 0.95f + rfx * 0.36f; m[1] = gVisY + 1.45f; m[2] = pz + ffz * 0.95f + rfz * 0.36f;
}

// ---- intro cutscene ------------------------------------------------------------------------------------------
static float iA, iB, iC, iRel, iBoom, iPush, iOrb0, iEnd, iD1, iD2, iDH;
static const float GREN_FROM[3] = { -20.2f, 1.45f, 2.9f }, GREN_TO[3] = { 3.9f, 0.25f, 1.4f };

static const char* TXT_GM1 = "You messed with the wrong lady today.";
static const char* TXT_GM2 = "Would you shut the hell up and shoot him already.|He's gonna mess up our work, and I'm not kissing the boss's ass to make up for it.";
static const char* TXT_HERO1 = "Catch.";
static const char* TXT_FEM = "How the fuck did he get up here?";
static const char* TXT_GMH = "We'll take his ass out before he can get to us.";
static const char* TXT_HERO2 = "It was a setup. I've been played.";
static const char* TXT_V1 = "Stand down, you dog.";
static const char* TXT_V2 = "You never make my job easy.|But I'll make your death easy for you, okay?";

static void csBuildIntro(void) {
	iD1 = voiceDur(S_V_GM1, TXT_GM1);
	iD2 = voiceDur(S_V_GM2, "Would you shut the hell up and shoot him already. He's gonna mess up our work, and I'm not kissing the boss's ass to make up for it.");
	iDH = voiceDur(S_V_HERO_THROW, TXT_HERO1);
	iA = 1.0f; iB = iA + iD1 + 0.55f; iC = iB + iD2 + 0.50f;
	iRel = iC + 0.60f; iBoom = iRel + 1.25f;
	iPush = iBoom + 1.55f; iOrb0 = iPush + 4.2f; iEnd = iOrb0 + 4.6f;
	csLen = iEnd;
	csAddLine(iA, S_V_GM1,        1, "GANG MEMBER", TXT_GM1, iD1);
	csAddLine(iB, S_V_GM2,        1, "GANG MEMBER", TXT_GM2, iD2);
	csAddLine(iC, S_V_HERO_THROW, 0, stName,        TXT_HERO1, iDH);

	const float HX = -21.0f, HZ = 2.0f;
	// 1: far, high wide shot of the whole factory floor, gunfire flying
	csKey(0.0f,        -12.0f, 10.2f, -21.0f,   3.0f, 1.2f, 2.0f,  54.0f, 0);
	csKey(iB - 0.40f,   -3.0f,  6.4f, -12.5f,   4.5f, 1.3f, 1.2f,  46.0f, 0);
	// 2: the two gang members, from the hero's side, over the conveyor
	csKey(iB - 0.35f,   -4.6f,  1.95f,  6.4f,   4.6f, 1.55f, 1.8f, 42.0f, 1);
	csKey(iB + iD2 * 0.55f, -1.6f, 1.80f, 5.2f, 4.7f, 1.60f, 2.0f, 36.0f, 0);
	// 3: the hero's face, grim
	csKey(iB + iD2 * 0.55f + 0.01f, -17.0f, 1.85f, 0.7f, HX, 1.70f, HZ, 34.0f, 1);
	csKey(iC - 0.28f,   -17.4f, 1.82f,  1.0f,  HX, 1.70f, HZ, 29.0f, 0);
	// 4: over his right shoulder as the arm swings
	csKey(iC - 0.27f,   -23.4f, 1.95f,  5.2f,  -18.0f, 1.55f, 1.8f, 46.0f, 1);
	csKey(iRel + 0.30f, -22.8f, 1.85f,  4.6f,  -13.0f, 1.80f, 2.0f, 46.0f, 0);
	// 5: from the gang's side, the grenade drops in and everything goes up
	csKey(iRel + 0.31f,  -9.0f, 1.75f, -6.5f,   3.0f, 1.40f, 1.5f, 44.0f, 1);
	csKey(iBoom + 0.9f,  -8.2f, 2.05f, -5.6f,   4.5f, 1.80f, 1.4f, 52.0f, 0);
	// 6: far to close: sweeps across the whole floor (between the pillars) to the hero's face
	csKey(iBoom + 0.91f, 23.0f, 6.4f, -3.0f,     0.0f, 1.5f, 1.5f, 56.0f, 1);
	csKey(iPush,         23.0f, 6.4f, -3.0f,     0.0f, 1.5f, 1.5f, 56.0f, 0);
	csKey(iPush + 1.4f,   4.0f, 3.8f, -4.0f,   -14.0f, 1.7f, 2.0f, 50.0f, 0);
	csKey(iPush + 2.8f,  -9.0f, 2.5f, -1.0f,   -19.0f, 1.7f, 2.0f, 44.0f, 0);

	// 7: the orbit: front (east) -> south -> behind, ending exactly on the gameplay chase camera
	float dT, sT, fT, endCam[3];
	camTargets(camStyle, 0, &dT, &sT, &fT);
	chaseCamera(HX, HZ, -1.5708f, 0.25f, dT, sT, camStyle == CAM_FIRST, endCam);
	float ed[3];
	dirFrom(-1.5708f, 0.25f, ed);
	float endLook[3] = { endCam[0] + ed[0] * 8.0f, endCam[1] + ed[1] * 8.0f, endCam[2] + ed[2] * 8.0f };
	float aEnd = atan2f(endCam[2] - HZ, endCam[0] - HX);
	if (aEnd < 0.0f) aEnd += 6.28319f;
	float rEnd = sqrtf((endCam[0] - HX) * (endCam[0] - HX) + (endCam[2] - HZ) * (endCam[2] - HZ));
	const int N = 7;
	for (int j = 0; j <= N; j++) {
		float u = (float)j / (float)N, s = sstep(u);
		float t = iOrb0 + (iEnd - iOrb0) * u;
		if (j == N) { csKey(t, endCam[0], endCam[1], endCam[2], endLook[0], endLook[1], endLook[2], fT, 0); break; }
		float r = lerpf(2.5f, rEnd, s), a = aEnd * s, h = lerpf(1.88f, endCam[1], s);
		float w = s * s;
		csKey(t, HX + r * cosf(a), h, HZ + r * sinf(a),
		      lerpf(HX, endLook[0], w), lerpf(1.70f, endLook[1], w), lerpf(HZ, endLook[2], w), lerpf(34.0f, fT, s), 0);
	}
}

static void csIntroPose(Pose* p, float t) {
	p->gun = WP_SMG; p->aimE = 0.0f; p->gunPitch = 0.0f; p->moveAmt = 0.0f; p->armLUp = 0.0f;
	float w0 = iC - 0.28f, w1 = iC + 0.38f;
	if (t < w0) {
		p->aimE = 0.95f;
	} else if (t < iRel) {
		p->grenade = 1; p->useArm = 1;
		if (t < w1) p->armOv = lerpf(0.55f, -1.15f, sstep((t - w0) / (w1 - w0)));          // wind back
		else        p->armOv = lerpf(-1.15f, 1.95f, sstep((t - w1) / (iRel - w1)));        // swing through
	} else if (t < iRel + 0.9f) {
		float u = sstep((t - iRel) / 0.9f);
		p->useArm = 1; p->armOv = lerpf(1.95f, 0.55f, u);
	}
}

static void csIntroApply(float t) {
	// the two gang members who talk: alive and aiming until the grenade, then thrown back
	int n = 0;
	for (int i = 0; i < nFoe; i++) {
		Foe* f = &foes[i];
		if (f->cut != 1) { if (f->stage == 0 && f->state != FS_DEAD) { f->aimE = 1.0f; f->yaw = 1.5708f; } continue; }
		float dirx = f->homeX - GREN_TO[0], dirz = f->homeZ - GREN_TO[2], dl = sqrtf(dirx * dirx + dirz * dirz);
		if (dl < 0.01f) dl = 1.0f;
		dirx /= dl; dirz /= dl;
		(void)n; n++;
		if (t < iBoom) { f->state = FS_IDLE; f->aimE = 1.0f; f->x = f->homeX; f->z = f->homeZ; f->deadT = 0; }
		else {
			float u = (t - iBoom) / 0.55f;
			if (u > 1.0f) u = 1.0f;
			float k = 1.0f - (1.0f - u) * (1.0f - u);
			f->x = f->homeX + dirx * 2.6f * k; f->z = f->homeZ + dirz * 2.6f * k;
			f->state = FS_DEAD; f->aimE = 0.0f; f->deadT = (int)((t - iBoom) * 60.0f);
		}
	}
	// the grenade in flight
	if (t >= iRel && t < iBoom) {
		float u = (t - iRel) / (iBoom - iRel);
		grenOn = 1.0f;
		for (int k = 0; k < 3; k++) grenP[k] = GREN_FROM[k] + (GREN_TO[k] - GREN_FROM[k]) * u;
		grenP[1] += 4.0f * u * (1.0f - u) * 3.2f;
	} else grenOn = 0.0f;
	face = -1.5708f;
}

static void csIntroEvents(float t0, float t1, int silent) {
	// the shootout going on in the background, until the hero reaches for the grenade
	float tf = iC - 0.40f;
	int i0 = (int)(t0 / 0.30f), i1 = (int)(fminf(t1, tf) / 0.30f);
	if (!silent && t1 > 0.25f) {
		for (int idx = i0 + 1; idx <= i1; idx++) {
			unsigned h = hash2(idx, 77);
			float vol = 0.30f;
			if (h % 3 == 0) {            // the hero fires at the gang
				float m[3], tg[3] = { frange(2.0f, 6.0f), frange(0.4f, 1.3f), frange(-3.0f, 6.0f) };
				heroMuzzle(m);
				csShot(m, tg, WP_SMG, vol, panTo(px, pz));
			} else {                     // the gang fires at the barrier
				int pick = -1, seen = 0, want = (int)((h >> 4) % 5);
				for (int k = 0; k < nFoe; k++) {
					if (foes[k].stage != 0 || foes[k].state == FS_DEAD) continue;
					if (seen++ == want || pick < 0) pick = k;
				}
				if (pick >= 0) {
					float m[3], tg[3] = { -18.4f, frange(0.3f, 1.1f), frange(-2.5f, 6.5f) };
					foeMuzzle(&foes[pick], m);
					csShot(m, tg, foes[pick].gun, vol, panTo(foes[pick].x, foes[pick].z));
				}
			}
		}
	}
	if (csCross(t0, t1, iBoom)) {
		if (!silent) {
			explodeAt(GREN_TO, px, pz);
			csFlash = 1.0f;
			for (int k = 0; k < 10; k++) {
				float v[3] = { frange(-0.3f, 0.3f), frange(0.1f, 0.5f), frange(-0.3f, 0.3f) };
				addSpark(GREN_TO, v, frange(0.12f, 0.26f), 30, 1.0f, frange(0.5f, 0.8f), 0.2f);
			}
			shake = 1.2f;
		}
	}
}

// ---- hallway three cutscene ----------------------------------------------------------------------------------
static float hCut, hF, hM, hBack, hEnd, hDF, hDM;
static float hCam[3], hLook[3], hFov;

static void csBuildHall3(void) {
	hDF = voiceDur(S_V_FEM_HALL, TXT_FEM);
	hDM = voiceDur(S_V_GM_HALL, TXT_GMH);
	hCut = 1.0f; hF = hCut + 0.75f; hM = hF + hDF + 0.45f; hBack = hM + hDM + 0.80f; hEnd = hBack + 1.0f;
	csLen = hEnd;
	csAddLine(hF, S_V_FEM_HALL, 1, "GANG MEMBER", TXT_FEM, hDF);
	csAddLine(hM, S_V_GM_HALL,  1, "GANG MEMBER", TXT_GMH, hDM);
}

// ---- roof cutscene ------------------------------------------------------------------------------------------
static float rX0, rZ0, rWalk, rL1, rV, rV1, rD, rP, rV2, rF, rEnd, rDH, rD1, rD2;
static const float ROOF_MARK[2] = { 128.0f, 84.0f };
static const float VINCE_FROM[2] = { 111.0f, 84.4f }, VINCE_TO[2] = { 121.5f, 81.8f };

static void roofHero(float t, float* x, float* z) {
	float u = sstep(t / rWalk);
	*x = lerpf(rX0, ROOF_MARK[0], u); *z = lerpf(rZ0, ROOF_MARK[1], u);
}

static void csBuildRoof(const float cam0[3], const float look0[3], float fov0) {
	rX0 = px; rZ0 = pz;
	float dist = sqrtf((ROOF_MARK[0] - rX0) * (ROOF_MARK[0] - rX0) + (ROOF_MARK[1] - rZ0) * (ROOF_MARK[1] - rZ0));
	rWalk = dist / 2.9f;
	if (rWalk < 3.2f) rWalk = 3.2f;
	rDH = voiceDur(S_V_HERO_PLAYED, TXT_HERO2);
	rD1 = voiceDur(S_V_VINCE1, TXT_V1);
	rD2 = voiceDur(S_V_VINCE2, "You never make my job easy. But I'll make your death easy for you, okay?");
	rL1 = rWalk + 0.60f;
	rV  = rL1 + rDH + 0.75f;
	rV1 = rV + 0.70f;
	rD  = rV1 + rD1 + 0.40f;
	rP  = rD + 2.10f;
	rV2 = rP + 0.70f;
	rF  = rV2 + rD2 + 0.60f;
	rEnd = rF + 2.7f;
	csLen = rEnd;
	csAddLine(rL1, S_V_HERO_PLAYED, 0, stName,        TXT_HERO2, rDH);
	csAddLine(rV1, S_V_VINCE1,      2, "VINCE TAYLOR", TXT_V1,    rD1);
	csAddLine(rV2, S_V_VINCE2,      2, "VINCE TAYLOR", TXT_V2,    rD2);

	float hx, hz;
	csKey(0.0f, cam0[0], cam0[1], cam0[2], look0[0], look0[1], look0[2], fov0, 0);
	roofHero(rWalk * 0.35f, &hx, &hz);
	csKey(rWalk * 0.35f, hx - 5.5f, 32.9f, hz + 0.4f, hx + 3.5f, 31.3f, hz, 60.0f, 0);
	roofHero(rWalk * 0.72f, &hx, &hz);
	csKey(rWalk * 0.72f, hx + 0.4f, 32.3f, hz + 4.6f, hx + 0.6f, 31.5f, hz, 50.0f, 0);
	csKey(rWalk, ROOF_MARK[0] + 3.1f, 31.75f, ROOF_MARK[1] + 1.1f, ROOF_MARK[0], 31.68f, ROOF_MARK[1], 36.0f, 0);
	csKey(rV - 0.01f, ROOF_MARK[0] + 2.6f, 31.72f, ROOF_MARK[1] + 0.8f, ROOF_MARK[0], 31.68f, ROOF_MARK[1], 32.0f, 0);
	// Vince steps out of the door behind him: wide, from in front of the hero
	csKey(rV,  136.4f, 32.3f, 86.2f, 121.0f, 31.4f, 83.2f, 46.0f, 1);
	csKey(rD - 0.01f, 135.0f, 32.0f, 86.0f, 121.0f, 31.4f, 83.0f, 44.0f, 0);
	// he drops his gun: close on the hero
	csKey(rD,  ROOF_MARK[0] + 2.9f, 31.85f, ROOF_MARK[1] - 1.5f, ROOF_MARK[0] + 0.1f, 31.4f, ROOF_MARK[1], 38.0f, 1);
	csKey(rP,  ROOF_MARK[0] + 2.5f, 31.80f, ROOF_MARK[1] - 1.2f, ROOF_MARK[0] + 0.1f, 31.4f, ROOF_MARK[1], 36.0f, 0);
	// pan across to Vince, behind him
	csKey(rP + 1.9f, 130.8f, 31.75f, 86.8f, VINCE_TO[0], 31.45f, VINCE_TO[1], 40.0f, 0);
	csKey(rF + 0.30f, 131.8f, 32.4f, 87.4f, 125.95f, 30.55f, 81.95f, 46.0f, 0);   // rises a little so the fallen hero stays in frame
}

static void csRoofPose(Pose* p, float t) {
	p->gun = curW; p->aimE = 0.0f; p->gunPitch = 0.0f;
	if (t < rWalk) { p->moveAmt = 1.0f; }
	if (t >= rD + 0.10f) {                         // drops the weapon, hands up
		float u = sstep((t - rD - 0.10f) / 0.55f);
		p->gun = WP_FISTS; p->useArm = 1; p->armOv = lerpf(0.5f, 2.9f, u); p->armLUp = u;
	}
	if (t >= rF) {
		float u = (t - rF) / 0.9f;
		p->fall = u > 1.0f ? 1.0f : (u < 0.0f ? 0.0f : u);
		p->fallFwd = 1; p->useArm = 0; p->armLUp = 0.0f;
	}
}

static void csRoofApply(float t) {
	float hx, hz;
	roofHero(t, &hx, &hz);
	px = hx; pz = hz; face = -1.5708f;
	moveAmt = (t < rWalk) ? 1.0f : 0.0f;
	// Vince walks in through the door, pointing his gun
	if (t >= rV) {
		vOn = 1;
		float u = sstep((t - rV) / 4.3f);
		vX = lerpf(VINCE_FROM[0], VINCE_TO[0], u); vZ = lerpf(VINCE_FROM[1], VINCE_TO[1], u);
		vFace = atan2f(-(px - vX), -(pz - vZ));
		vAim = 1.0f; vMove = (t < rV + 4.3f) ? 0.9f : 0.0f;
	} else vOn = 0;
	// the dropped weapon
	if (t >= rD + 0.15f) {
		float u = (t - rD - 0.15f) / 0.45f;
		gunDropOn = 1.0f; gunDropKind = curW;
		gunDropX = ROOF_MARK[0] + 0.6f; gunDropZ = ROOF_MARK[1] + 0.35f;
		if (u < 1.0f) gunDropY = 31.0f - u * u * 0.95f;
		else gunDropY = 30.05f;
	} else gunDropOn = 0.0f;
}

static void csRoofEvents(float t0, float t1, int silent) {
	if (silent) return;
	if (csCross(t0, t1, rV)) sfxPlay(S_RELOAD, 0.8f, panTo(VINCE_FROM[0], VINCE_FROM[1]), 0.8f);
	if (csCross(t0, t1, rD + 0.62f)) sfxPlay(S_THUD, 0.9f, panTo(gunDropX, gunDropZ), 1.3f);
	if (csCross(t0, t1, rF)) {
		float m[3] = { vX + (-sinf(vFace)) * 0.95f + cosf(vFace) * 0.36f, 31.42f, vZ + (-cosf(vFace)) * 0.95f - sinf(vFace) * 0.36f };
		float tg[3] = { px, 31.3f, pz };
		addTracerFx(m, tg);
		addSpark(m, NULL, 0.30f, 4, 1.0f, 0.85f, 0.45f);
		sfxPlay(S_PISTOL, 1.0f, 0.0f, 0.82f);
		csFlash = 0.55f; shake = 0.5f;
	}
	if (csCross(t0, t1, rF + 0.35f)) sfxPlay(S_THUD, 0.9f, 0.0f, 0.8f);
}

// ---- running a cutscene ------------------------------------------------------------------------------------------
static void csStart(int kind, const float cam[3], float cyaw, float cpitch, float fov) {
	csKind = kind; csT = 0.0f; csDone = CS_NONE; csSkipped = 0;
	csNKeys = 0; csNLines = 0; csFlash = 0.0f; csBlack = 0.0f;
	vOn = 0; gunDropOn = 0.0f; grenOn = 0.0f;
	csLastMs = osGetTime();
	float d[3], look[3];
	dirFrom(cyaw, cpitch, d);
	for (int k = 0; k < 3; k++) look[k] = cam[k] + d[k] * 10.0f;
	initVince();
	if (kind == CS_INTRO) csBuildIntro();
	else if (kind == CS_HALL3) {
		hCam[0] = cam[0]; hCam[1] = cam[1]; hCam[2] = cam[2]; hFov = fov;
		memcpy(hLook, look, sizeof look);
		csBuildHall3();
		// the track: gameplay view, a short push in, hard cut to the far end of the hallway, hard cut back
		float fwdx = d[0], fwdz = d[2];
		csKey(0.0f, cam[0], cam[1], cam[2], look[0], look[1], look[2], fov, 0);
		csKey(hCut - 0.02f, cam[0] + fwdx * 0.7f, cam[1], cam[2] + fwdz * 0.7f, look[0], look[1], look[2], fov - 4.0f, 0);
		float gx = 93.2f, gz = 42.1f, gy = areas[6].yA;
		csKey(hCut, gx - 12.0f, gy + 1.85f, gz - 0.5f, gx, gy + 1.45f, gz, 42.0f, 1);
		csKey(hBack - 0.02f, gx - 8.5f, gy + 1.80f, gz - 0.3f, gx, gy + 1.45f, gz, 36.0f, 0);
		csKey(hBack, cam[0], cam[1], cam[2], look[0], look[1], look[2], fov, 1);
		csKey(hEnd, cam[0], cam[1], cam[2], look[0], look[1], look[2], fov, 0);
	} else if (kind == CS_ROOF) {
		csBuildRoof(cam, look, fov);
	}
	// make the gang members face west and be aiming in the intro / standing in the hallway scene
	if (kind == CS_INTRO) csIntroApply(0.0f);
	voiceStop();
}

static void csFinish(void) {
	int k = csKind;
	voiceStop();
	if (csSkipped) clearFx();
	csDone = k;
	csKind = CS_NONE;
	csFlash = 0.0f;
	vOn = 0; gunDropOn = 0.0f; grenOn = 0.0f;
}

static void csAdvance(float dt, int silent) {
	float t0 = csT, t1 = csT + dt;
	if (t1 > csLen) t1 = csLen;
	for (int i = 0; i < csNLines; i++)
		if (!silent && csCross(t0, t1, csLines[i].t)) voicePlay(csLines[i].snd, 1.0f);
	if (csKind == CS_INTRO) { csIntroEvents(t0, t1, silent); csIntroApply(t1); }
	else if (csKind == CS_ROOF) { csRoofEvents(t0, t1, silent); csRoofApply(t1); }
	csT = t1;
	if (csFlash > 0.0f) { csFlash -= dt * 1.6f; if (csFlash < 0.0f) csFlash = 0.0f; }
	if (csKind == CS_ROOF) { float b = (csT - (rF + 1.3f)) / 0.9f; csBlack = b < 0.0f ? 0.0f : (b > 1.0f ? 1.0f : b); }
	if (csKind == CS_ROOF && csT < rWalk) phase += dt * 6.5f;
	if (csT >= csLen) csFinish();
}

static void csUpdate(int skip) {
	if (!csKind) return;
	u64 now = osGetTime();
	float dt = (float)(now - csLastMs) * 0.001f;
	csLastMs = now;
	if (dt > 0.05f) dt = 0.05f;
	if (dt < 0.0f) dt = 0.0f;
	csDt = dt;
	if (skip) {
		csSkipped = 1;
		if (csKind == CS_INTRO) csAdvance(csLen - csT, 1);
		else if (csKind == CS_ROOF) { csT = csLen; csRoofApply(csLen); csFinish(); }
		else csAdvance(csLen - csT, 1);
		return;
	}
	csAdvance(dt, 0);
}

// camera for this frame
static void csCamera(float pos[3], float* cyw, float* cpt, float* fov) {
	float look[3];
	csTrack(csT, pos, look, fov);
	csAim(pos, look, cyw, cpt);
}

// the hero in a cutscene: overrides the pose the main loop built
static void csHeroPose(Pose* p) {
	if (csKind == CS_INTRO) csIntroPose(p, csT);
	else if (csKind == CS_ROOF) csRoofPose(p, csT);
	else { p->aimE = 0.0f; p->moveAmt = 0.0f; }
}

// scripted props and the extra actor, drawn with the lit world
static void csDrawProps(void) {
	if (grenOn > 0.0f) {
		float t = csT * 12.0f;
		addBoxY(grenP[0] - 0.07f, grenP[2] - 0.07f, grenP[0] + 0.07f, grenP[2] + 0.07f, grenP[1] - 0.09f, grenP[1] + 0.09f, 0.14f, 0.24f, 0.12f);
		addBoxY(grenP[0] - 0.02f + 0.06f * cosf(t), grenP[2] - 0.02f + 0.06f * sinf(t), grenP[0] + 0.02f + 0.06f * cosf(t), grenP[2] + 0.02f + 0.06f * sinf(t),
		        grenP[1] + 0.09f, grenP[1] + 0.14f, 0.8f, 0.8f, 0.2f);
	}
	if (gunDropOn > 0.0f) {
		float L = (gunDropKind == WP_SHOTGUN) ? 0.95f : 0.70f;
		addBoxY(gunDropX - L * 0.5f, gunDropZ - 0.05f, gunDropX + L * 0.5f, gunDropZ + 0.05f, gunDropY, gunDropY + 0.10f, 0.12f, 0.12f, 0.14f);
		if (gunDropKind == WP_SHOTGUN) addBoxY(gunDropX - L * 0.1f, gunDropZ - 0.06f, gunDropX + L * 0.35f, gunDropZ + 0.06f, gunDropY, gunDropY + 0.07f, 0.45f, 0.28f, 0.14f);
	}
	if (vOn) {
		Pose p;
		memset(&p, 0, sizeof p);
		p.px = vX; p.pz = vZ; p.py = 30.0f; p.face = vFace; p.phase = csT * 5.5f; p.moveAmt = vMove;
		p.gun = WP_PISTOL; p.aimE = vAim; p.gunPitch = 0.0f;
		drawPerson(&p, &vinceOutfit);
	}
}

// ---- screen overlay: bars, subtitles, flash, fade -------------------------------------------------------------
static void csWrap(const char* s, char out[3][64]) {
	out[0][0] = out[1][0] = out[2][0] = 0;
	int n = (int)strlen(s), line = 0, pos = 0;
	while (pos < n && line < 3) {
		int len = n - pos;
		if (len > 52) {
			int cut = 52;
			while (cut > 20 && s[pos + cut] != ' ') cut--;
			len = cut;
		}
		memcpy(out[line], s + pos, (size_t)len);
		out[line][len] = 0;
		pos += len;
		while (s[pos] == ' ') pos++;
		line++;
	}
}

// the part of a line (text between '|' marks) that is being spoken at time t
static void csChunk(const CsLine* L, float t, char chunk[200]) {
	int total = 0;
	for (const char* c = L->text; *c; c++) if (*c != '|') total++;
	float u = (t - L->t) / L->dur;
	if (u > 0.999f) u = 0.999f;
	if (u < 0.0f) u = 0.0f;
	int want = (int)(u * (float)total), acc = 0;
	const char* c = L->text;
	chunk[0] = 0;
	while (*c) {
		const char* e = c;
		int len = 0;
		while (*e && *e != '|') { e++; len++; }
		if (want < acc + len || !*e) { int cn = len < 198 ? len : 198; memcpy(chunk, c, (size_t)cn); chunk[cn] = 0; break; }
		acc += len;
		c = e + 1;
	}
}

static void csOverlay(int frame) {
	float k = 1.0f;
	if (csT < 0.6f) k = sstep(csT / 0.6f);
	if (csLen - csT < 0.7f) k = fminf(k, sstep((csLen - csT) / 0.7f));
	float top = CS_BAR_TOP * k, bot = CS_BAR_BOT * k;
	const u32 black = C2D_Color32(0, 0, 0, 0xFF);
	if (top > 0.5f) C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, top, black);
	if (bot > 0.5f) C2D_DrawRectSolid(0.0f, 240.0f - bot, 0.5f, 400.0f, bot, black);
	if (k > 0.99f) {
		drawStr(394.0f, 8.0f, 0.34f, neon(150, 156, 170, 0.8f), 2, "START: SKIP");
		for (int i = 0; i < csNLines; i++) {
			const CsLine* L = &csLines[i];
			if (csT < L->t || csT >= L->t + L->dur + 0.35f) continue;
			char chunk[200];
			csChunk(L, csT, chunk);
			char ln[3][64];
			csWrap(chunk, ln);
			u32 nc = L->kind == 0 ? neon(0, 229, 255, 1.0f) : (L->kind == 1 ? neon(255, 96, 96, 1.0f) : neon(255, 205, 90, 1.0f));
			float y0 = 240.0f - CS_BAR_BOT + 4.0f;
			drawStr(200.0f, y0, 0.38f, nc, 1, L->who);
			for (int j = 0; j < 3 && ln[j][0]; j++) drawStr(200.0f, y0 + 12.0f + (float)j * 12.0f, 0.40f, neon(245, 245, 250, 1.0f), 1, ln[j]);
		}
	}
	if (csFlash > 0.01f) C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, neon(255, 250, 235, csFlash > 1.0f ? 1.0f : csFlash));
	if (csBlack > 0.01f) C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, neon(0, 0, 0, csBlack));
}


// ================================================================================================
// STORY MODE: saves, level switching, the mission itself, its screens
// ================================================================================================
typedef struct { u32 magic; u32 used; u32 done; char name[16]; } SaveRec;
#define SAVE_MAGIC 0x43335344u
static SaveRec saves[3];

static void savePath(int i, char* out, size_t n) { snprintf(out, n, "sdmc:/3ds/chicago3ds/save%d.dat", i + 1); }

static void loadSaves(void) {
	for (int i = 0; i < 3; i++) {
		char path[80];
		SaveRec r;
		memset(&saves[i], 0, sizeof saves[i]);
		savePath(i, path, sizeof path);
		FILE* f = fopen(path, "rb");
		if (!f) continue;
		if (fread(&r, sizeof r, 1, f) == 1 && r.magic == SAVE_MAGIC && r.used) { saves[i] = r; saves[i].name[15] = 0; }
		fclose(f);
	}
}

static void writeSave(int i) {
	char path[80];
	mkdir("sdmc:/3ds", 0777);
	mkdir("sdmc:/3ds/chicago3ds", 0777);
	savePath(i, path, sizeof path);
	saves[i].magic = SAVE_MAGIC;
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fwrite(&saves[i], sizeof saves[i], 1, f);
	fclose(f);
}

static void deleteSave(int i) {
	char path[80];
	savePath(i, path, sizeof path);
	remove(path);
	memset(&saves[i], 0, sizeof saves[i]);
}

// ---- switching between the city and the factory (the vertex buffer holds one world at a time) ---------
static void loadLevel(int lv) {
	gLevel = lv;
	if (lv == LV_FACTORY) { nCarOn = 0; nNpcOn = 0; buildFactory(); }
	else { nCarOn = N_CARS; nNpcOn = N_NPC; nFoe = 0; buildCity(); initCars(); }
	GSPGPU_FlushDataCache(vbo, staticCount * sizeof(Vertex));
	clearFx();
}

// fade to black, swap the world while nothing is drawn, fade back in
enum { TR_START_STORY = 1, TR_RESTART = 2, TR_TO_TITLE = 3 };
static float trA = 0.0f;
static int   trState = 0, trHold = 0, trAct = 0;
static void trStart(int act) { if (trState) return; trAct = act; trState = 1; }
static int trUpdate(void) {          // returns the action to perform on the frame the screen is fully black
	if (trState == 1) { trA += 0.07f; if (trA >= 1.0f) { trA = 1.0f; trState = 2; trHold = 4; } }
	else if (trState == 2) { if (--trHold <= 0) { trState = 3; return trAct; } }
	else if (trState == 3) { trA -= 0.055f; if (trA <= 0.0f) { trA = 0.0f; trState = 0; } }
	return 0;
}

// ---- the mission ---------------------------------------------------------------------------------------------
static int stRestartReq = 0;
static const float CP_X[6] = { -21.0f, 46.5f, 107.0f, 46.5f, 107.0f, 46.5f };
static const float CP_Z[6] = {   2.0f,  0.0f,  21.0f, 42.0f,  63.0f, 84.0f };
static const float CP_YAW[6] = { -1.5708f, -1.5708f, 1.5708f, -1.5708f, 1.5708f, -1.5708f };

static void storyWeapons(int refill) {
	if (!refill) { for (int w = 0; w < N_WP; w++) { wClip[w] = 0; wRes[w] = 0; } }
	if (!refill || wClip[WP_SMG] + wRes[WP_SMG] < 180)       { wClip[WP_SMG] = 30; wRes[WP_SMG] = 150; }
	if (!refill || wClip[WP_SHOTGUN] + wRes[WP_SHOTGUN] < 36) { wClip[WP_SHOTGUN] = 6; wRes[WP_SHOTGUN] = 30; }
	wheelOrder[0] = WP_SMG; wheelOrder[1] = WP_SHOTGUN; wheelN = 2;
	if (curW != WP_SMG && curW != WP_SHOTGUN) curW = WP_SMG;
	reloadT = 0; fireCD = 20; fireRecent = 0; punchT = 0; hitMark = 0; kick = 0.0f; shake = 0.0f;
}

static void storySetCamera(void) {
	float d, s, f;
	camTargets(camStyle, 0, &d, &s, &f);
	camDist = d; camSide = s; camFov = f;
}

static void storyHeroAt(float x, float z, float yw) {
	px = x; pz = z; yaw = yw; face = yw; pitch = 0.25f; phase = 0.0f; moveAmt = 0.0f; aimE = 0.0f;
	gAi = areaAt(x, z);
	gFootY = groundAt(x, z, 0.0f, NULL); gVisY = gFootY;
}

static void storyBegin(void) {
	memset(stCleared, 0, sizeof stCleared);
	stNewClear = -1; barkCD = 0; stCP = 0; stHall3Seen = 0; stRoofSeen = 0; stHint = 0;
	stMode = SM_PLAY; failT = 0; endT = 0; stSavePending = 0;
	pHP = 100.0f; pHurtT = 0; pRegenT = 0;
	curW = WP_SMG;
	storyWeapons(0);
	kills = 0; hackTimer = 0;
	storyHeroAt(CP_X[0], CP_Z[0], CP_YAW[0]);
	storySetCamera();
	for (int g = 0; g < nGate; g++) gates[g].open = 0.0f;
	initFoes();
	float cam[3] = { 0, 0, 0 };
	csStart(CS_INTRO, cam, 0.0f, 0.0f, 65.0f);
}

// back to the start of the stage we reached (cleared stages stay cleared)
static void storyRestart(void) {
	stMode = SM_PLAY; failT = 0;
	pHP = 100.0f; pHurtT = 0; pRegenT = 0;
	storyWeapons(1);
	for (int i = 0; i < nFoe; i++) {
		Foe* f = &foes[i];
		if (f->cut == 1) continue;
		if (f->stage >= stCP) reviveFoe(f);
	}
	for (int s = stCP; s < 6; s++) { stCleared[s] = 0; if (s < nGate) { gates[s].open = 0.0f; } }
	stNewClear = -1; stHint = 0;
	storyHeroAt(CP_X[stCP], CP_Z[stCP], CP_YAW[stCP]);
	storySetCamera();
	clearFx();
}

static int storySwap(int act) {      // returns the main loop's new `mode`
	if (act == TR_START_STORY) { loadLevel(LV_FACTORY); storyBegin(); return M_GAME; }
	if (act == TR_RESTART) { storyRestart(); return M_GAME; }
	if (gLevel != LV_CITY) loadLevel(LV_CITY);
	stMode = SM_OFF; csKind = CS_NONE; voiceStop();
	wheelOrder[0] = WP_PISTOL; wheelOrder[1] = WP_SMG; wheelOrder[2] = WP_SHOTGUN; wheelOrder[3] = WP_ROCKET; wheelOrder[4] = WP_FISTS;
	wheelN = 5;
	gFootY = 0.0f; gVisY = 0.0f; gAi = -1;
	return M_TITLE;
}

static int storyStage(void) { return gAi < 0 ? stCP : (gAi + 1) / 2; }

// one simulation step of the mission (not while a cutscene runs or the menu is open)
static void storySim(int moving) {
	updateGates();
	int stg = storyStage();
	if (stMode == SM_PLAY) updateFoes(px, pz, gFootY, (stg >= 0 && stg <= 5) ? stg : -1, moving);
	else updateFoes(px, pz, gFootY, -1, 0);
	if (pHurtT > 0) pHurtT--;
	if (stMode == SM_PLAY) {
		if (pRegenT > 0) pRegenT--;
		else if (pHP > 0.0f && pHP < 100.0f) { pHP += 0.12f; if (pHP > 100.0f) pHP = 100.0f; }
		if (gAi >= 2 && (gAi & 1) == 0 && gAi / 2 > stCP) stCP = gAi / 2;       // reached a new hallway: checkpoint
		if (stNewClear >= 0) {
			stCleared[stNewClear] = 1; stHint = 420; stNewClear = -1;
			sfxPlay(S_UI_OK, 0.9f, 0.0f, 1.3f);
		}
		if (pHP <= 0.0f) { stMode = SM_FAIL; failT = 0; sfxPlay(S_THUD, 1.0f, 0.0f, 0.7f); }
	} else if (stMode == SM_FAIL) failT++;
	if (stHint > 0) stHint--;
}

// story events that start cutscenes: called with this frame's gameplay camera
static void storyTriggers(const float cam[3], float cyw, float cpt, float fov) {
	if (stMode != SM_PLAY || csKind != CS_NONE) return;
	if (!stHall3Seen && gAi == 6 && px >= 50.0f) { stHall3Seen = 1; csStart(CS_HALL3, cam, cyw, cpt, fov); }
	else if (!stRoofSeen && gAi == 11 && px >= 118.0f) { stRoofSeen = 1; csStart(CS_ROOF, cam, cyw, cpt, fov); }
}

// a cutscene just finished
static void storyAfterCut(void) {
	int k = csDone;
	csDone = CS_NONE;
	if (k == CS_INTRO) { storySetCamera(); alertStage(0, 20); fireCD = 20; }
	else if (k == CS_HALL3) { alertStage(3, 15); fireCD = 10; }
	else if (k == CS_ROOF) {
		stMode = SM_END; endT = 0; csBlack = 1.0f;
		saves[stSlot].done = 1;
		writeSave(stSlot);
	}
}

// ---- screens ------------------------------------------------------------------------------------------------------
static void drawStoryHud(int frame) {
	// hurt vignette
	if (pHurtT > 0) {
		float a = (float)pHurtT / 24.0f * 0.55f;
		C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 22.0f, neon(200, 0, 0, a));
		C2D_DrawRectSolid(0.0f, 218.0f, 0.5f, 400.0f, 22.0f, neon(200, 0, 0, a));
		C2D_DrawRectSolid(0.0f, 22.0f, 0.5f, 22.0f, 196.0f, neon(200, 0, 0, a));
		C2D_DrawRectSolid(378.0f, 22.0f, 0.5f, 22.0f, 196.0f, neon(200, 0, 0, a));
	}
	// health bar
	float hp = pHP / 100.0f;
	C2D_DrawRectSolid(6.0f, 6.0f, 0.5f, 118.0f, 20.0f, neon(0, 0, 0, 0.55f));
	C2D_DrawRectSolid(6.0f, 6.0f, 0.5f, 3.0f, 20.0f, neon(0, 229, 255, 1.0f));
	drawStr(14.0f, 7.0f, 0.34f, neon(154, 232, 245, 1.0f), 0, "HEALTH");
	C2D_DrawRectSolid(14.0f, 18.0f, 0.5f, 104.0f, 5.0f, neon(60, 20, 20, 0.9f));
	u32 hc = hp > 0.5f ? neon(70, 235, 120, 1.0f) : (hp > 0.25f ? neon(255, 200, 60, 1.0f) : neon(255, 70, 70, 1.0f));
	C2D_DrawRectSolid(14.0f, 18.0f, 0.5f, 104.0f * hp, 5.0f, hc);
	if (stHint > 0 && (stHint > 60 || (frame / 6) % 2 == 0)) {
		int stg = storyStage();
		const char* m = stg >= 5 ? "AREA CLEAR - THE ROOF DOOR IS OPEN" : "AREA CLEAR - THE DOOR IS UNLOCKED";
		C2D_DrawRectSolid(70.0f, 36.0f, 0.5f, 260.0f, 18.0f, neon(0, 0, 0, 0.6f));
		drawStr(200.0f, 39.0f, 0.42f, neon(120, 255, 160, 1.0f), 1, m);
	}
}

static void drawFailTop(int frame) {
	float a = failT < 40 ? (float)failT / 40.0f : 1.0f;
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, neon(30, 0, 0, 0.72f * a));
	drawStr(200.0f, 84.0f, 1.1f, neon(255, 70, 70, a), 1, "TAKEN DOWN");
	if (failT > 40 && (frame / 30) % 2 == 0) drawStr(200.0f, 140.0f, 0.5f, neon(240, 240, 245, 1.0f), 1, "A: TRY AGAIN       B: QUIT TO TITLE");
}

static void drawEndTop(int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, neon(0, 0, 0, 1.0f));
	float a = endT < 40 ? (float)endT / 40.0f : 1.0f;
	C2D_DrawRectSolid(120.0f, 108.0f, 0.5f, 160.0f, 1.0f, neon(255, 46, 209, a));
	drawStr(200.0f, 70.0f, 1.0f, neon(240, 248, 255, a), 1, "END OF MISSION 1");
	drawStr(200.0f, 118.0f, 0.42f, neon(0, 229, 255, a * 0.9f), 1, "// TO BE CONTINUED");
	char who[40];
	snprintf(who, sizeof who, "%s", stName);
	drawStr(200.0f, 150.0f, 0.5f, neon(154, 163, 184, a), 1, who);
	(void)frame;
}

// bottom screen: only the factory interior on the map, objective underneath
static void drawStoryBottom(int frame) {
	const u32 cBg = C2D_Color32(0x0a, 0x0c, 0x12, 0xFF), cText = C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF);
	const u32 cPill = C2D_Color32(0, 0, 0, 0xB4);
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, cBg);
	if (csKind) {
		drawStr(160.0f, 96.0f, 0.55f, neon(0, 229, 255, 1.0f), 1, "// CUTSCENE");
		drawStr(160.0f, 124.0f, 0.40f, neon(154, 163, 184, 1.0f), 1, "START: SKIP");
		return;
	}
	int cur = gAi;
	for (int i = 0; i < nArea; i++) {
		const Area* a = &areas[i];
		u32 c = (a->kind == AK_FACTORY) ? C2D_Color32(0x24, 0x2c, 0x3e, 0xFF)
		      : (a->kind == AK_STAIR) ? C2D_Color32(0x2c, 0x34, 0x4a, 0xFF)
		      : (a->kind == AK_ROOF) ? C2D_Color32(0x1c, 0x24, 0x38, 0xFF) : C2D_Color32(0x28, 0x32, 0x48, 0xFF);
		if (i == cur) c = C2D_Color32(0x3a, 0x48, 0x68, 0xFF);
		mapRect(a->x0, a->z0, a->x1, a->z1, px, pz, c);
		if (a->kind == AK_STAIR) {          // treads
			int xdir = (a->dir <= 2);
			for (int k = 1; k < 12; k++) {
				float f = (float)k / 12.0f;
				if (xdir) mapRect(a->x0 + (a->x1 - a->x0) * f, a->z0, a->x0 + (a->x1 - a->x0) * f + 0.3f, a->z1, px, pz, C2D_Color32(0x4c, 0x5a, 0x7e, 0xFF));
				else      mapRect(a->x0, a->z0 + (a->z1 - a->z0) * f, a->x1, a->z0 + (a->z1 - a->z0) * f + 0.3f, px, pz, C2D_Color32(0x4c, 0x5a, 0x7e, 0xFF));
			}
		}
		u32 w = (i == cur) ? C2D_Color32(0x00, 0xE5, 0xFF, 0xFF) : C2D_Color32(0x00, 0x8A, 0xA0, 0xFF);   // walls
		mapRect(a->x0, a->z0, a->x1, a->z0 + 0.55f, px, pz, w);
		mapRect(a->x0, a->z1 - 0.55f, a->x1, a->z1, px, pz, w);
		mapRect(a->x0, a->z0, a->x0 + 0.55f, a->z1, px, pz, w);
		mapRect(a->x1 - 0.55f, a->z0, a->x1, a->z1, px, pz, w);
	}
	for (int g = 0; g < nGate; g++) {        // doors: red while locked, green once open
		const Gate* d = &gates[g];
		u32 c = stCleared[g] ? C2D_Color32(0x30, 0xFF, 0x70, 0xFF) : C2D_Color32(0xFF, 0x30, 0x30, 0xFF);
		if (d->axis == 0) mapRect(d->a0, d->fix - 0.7f, d->a1, d->fix + 0.7f, px, pz, c);
		else              mapRect(d->fix - 0.7f, d->a0, d->fix + 0.7f, d->a1, px, pz, c);
	}
	int stg = storyStage();
	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.2f);
	for (int i = 0; i < nFoe; i++) {          // the gang members still standing in this part of the building
		const Foe* f = &foes[i];
		if (f->state == FS_DEAD || f->state == FS_OFF || f->cut == 1 || f->stage != stg) continue;
		float sx = MAP_CX + (f->x - px) * MAP_S, sy = MAP_CY + (f->z - pz) * MAP_S;
		if (sx > 3.0f && sx < 317.0f && sy > 3.0f && sy < MAP_H - 3.0f)
			C2D_DrawCircleSolid(sx, sy, 0.5f, 3.0f + pulse, C2D_Color32(0xFF, 0x2A, 0x2A, 0xFF));
	}
	{ // the hero: blue circle with a heading marker
		float fx = -sinf(face), fz = -cosf(face);
		float tx = MAP_CX + fx * 15.0f, ty = MAP_CY + fz * 15.0f, bx = MAP_CX + fx * 8.0f, by = MAP_CY + fz * 8.0f;
		C2D_DrawTriangle(tx, ty, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), bx - fz * 5.0f, by + fx * 5.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF),
		                 bx + fz * 5.0f, by - fx * 5.0f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF), 0.5f);
		C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 8.5f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
		C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 6.5f, C2D_Color32(0x1E, 0x7B, 0xFF, 0xFF));
	}
	// area name and ammo over the map
	{
		const char* nm = (cur >= 0) ? areas[cur].name : "FACTORY";
		float w, h;
		C2D_Text t;
		C2D_TextParse(&t, gDyn, nm);
		C2D_TextOptimize(&t);
		C2D_TextGetDimensions(&t, 0.40f, 0.40f, &w, &h);
		C2D_DrawRectSolid(3.0f, 3.0f, 0.5f, w + 10.0f, h + 4.0f, cPill);
		C2D_DrawText(&t, C2D_WithColor, 8.0f, 5.0f, 0.5f, 0.40f, 0.40f, cText);
		char wl2[48];
		weaponLine(curW, wl2, sizeof wl2);
		C2D_DrawRectSolid(3.0f, MAP_H - 17.0f, 0.5f, 108.0f, 15.0f, cPill);
		drawStr(7.0f, MAP_H - 16.0f, 0.38f, cText, 0, wl2);
		C2D_TextGetDimensions(&txtHint, 0.34f, 0.34f, &w, &h);
		C2D_DrawRectSolid(317.0f - w - 8.0f, MAP_H - h - 7.0f, 0.5f, w + 8.0f, h + 4.0f, cPill);
		C2D_DrawText(&txtHint, C2D_WithColor, 317.0f - w - 4.0f, MAP_H - h - 5.0f, 0.5f, 0.34f, 0.34f, cText);
	}
	// objective panel
	C2D_DrawRectSolid(0.0f, MAP_H, 0.5f, 320.0f, 240.0f - MAP_H, C2D_Color32(0x07, 0x09, 0x0e, 0xFF));
	C2D_DrawRectSolid(0.0f, MAP_H, 0.5f, 320.0f, 2.0f, neon(0, 229, 255, 0.9f));
	C2D_DrawRectSolid(0.0f, MAP_H, 0.5f, 56.0f, 2.0f, neon(255, 46, 209, 1.0f));
	drawStr(10.0f, MAP_H + 6.0f, 0.36f, neon(255, 46, 209, 1.0f), 0, "MISSION OBJECTIVE");
	drawStr(10.0f, MAP_H + 21.0f, 0.55f, neon(255, 255, 255, 1.0f), 0, "Kill the Factory Gang Members.");
	char sub[80];
	int alive = (stg >= 0 && stg <= 5) ? stageAlive(stg) : 0;
	u32 sc = neon(154, 163, 184, 1.0f);
	if (stg > 5) snprintf(sub, sizeof sub, "Reach the top.");
	else if (alive > 0) snprintf(sub, sizeof sub, "Gang members left in this area: %d", alive);
	else { snprintf(sub, sizeof sub, stg >= 5 ? "Area clear. Go through the door to the roof." : "Area clear. Take the stairs up."); sc = neon(120, 255, 160, 1.0f); }
	drawStr(10.0f, MAP_H + 44.0f, 0.40f, sc, 0, sub);
}

// ---- title menu, save files, name entry ---------------------------------------------------------------------------
enum { TS_MAIN = 0, TS_SAVES = 1, TS_NAME = 2, TS_DELETE = 3 };
enum { TACT_NONE = 0, TACT_STORY, TACT_FREE, TACT_SETTINGS, TACT_QUIT };
static int  tState = TS_MAIN, tSel = 0, tSlot = 0, tNameLen = 0, kbR = 1, kbC = 0;
static char tName[16];

static const char* kbRows[4] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-._" };
#define KB_X0 10.0f
#define KB_Y0 70.0f
#define KB_PX 30.0f
#define KB_PY 31.0f

static void kbPress(int r, int c) {
	if (r < 4) {
		if (tNameLen >= 12) { sfxPlay(S_CLICK, 0.5f, 0.0f, 1.0f); return; }
		char ch = kbRows[r][c];
		if (ch >= 'A' && ch <= 'Z') {
			int cap = (tNameLen == 0) || (tName[tNameLen - 1] == ' ');
			if (!cap) ch = (char)(ch + 32);
		}
		tName[tNameLen++] = ch; tName[tNameLen] = 0;
		sfxPlay(S_UI_MOVE, 0.7f, 0.0f, 1.2f);
	} else if (c <= 3) {                     // space
		if (tNameLen > 0 && tNameLen < 12 && tName[tNameLen - 1] != ' ') { tName[tNameLen++] = ' '; tName[tNameLen] = 0; sfxPlay(S_UI_MOVE, 0.7f, 0.0f, 1.0f); }
	} else if (c <= 6) {                     // delete
		if (tNameLen > 0) { tName[--tNameLen] = 0; sfxPlay(S_UI_MOVE, 0.7f, 0.0f, 0.8f); }
	}
}

static void kbRect(int r, int c, float* x, float* y, float* w, float* h) {
	*y = KB_Y0 + (float)r * KB_PY; *h = 29.0f;
	if (r < 4) { *x = KB_X0 + (float)c * KB_PX; *w = 28.0f; }
	else if (c <= 3) { *x = KB_X0; *w = 4.0f * KB_PX - 2.0f; }
	else if (c <= 6) { *x = KB_X0 + 4.0f * KB_PX; *w = 3.0f * KB_PX - 2.0f; }
	else { *x = KB_X0 + 7.0f * KB_PX; *w = 3.0f * KB_PX - 2.0f; }
}

static int nameOK(void) {
	for (int i = 0; i < tNameLen; i++) if (tName[i] != ' ') return 1;
	return 0;
}

static int nameConfirm(void) {         // returns TACT_STORY when the save was made
	if (!nameOK()) { sfxPlay(S_CLICK, 0.6f, 0.0f, 1.0f); return TACT_NONE; }
	while (tNameLen > 0 && tName[tNameLen - 1] == ' ') tName[--tNameLen] = 0;
	memset(&saves[tSlot], 0, sizeof saves[tSlot]);
	saves[tSlot].used = 1; saves[tSlot].done = 0;
	memcpy(saves[tSlot].name, tName, sizeof saves[tSlot].name);
	saves[tSlot].name[15] = 0;
	writeSave(tSlot);
	stSlot = tSlot;
	memcpy(stName, tName, sizeof stName);
	stName[15] = 0;
	sfxPlay(S_UI_OK, 0.9f, 0.0f, 1.0f);
	return TACT_STORY;
}

// all title input; returns what the main loop should do
static int titleInput(u32 down, const touchPosition* tp) {
	if (tState == TS_MAIN) {
		if (down & KEY_DDOWN) { tSel = (tSel + 1) % 4; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 1.0f); }
		if (down & KEY_DUP)   { tSel = (tSel + 3) % 4; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 1.0f); }
		int pick = -1;
		if (down & KEY_A) pick = tSel;
		if (down & KEY_TOUCH) for (int i = 0; i < 4; i++) {
			float y0, h; menuRect(i, &y0, &h);
			if (tp->px >= 40 && tp->px <= 280 && tp->py >= y0 && tp->py <= y0 + h) { tSel = i; pick = i; }
		}
		if (down & KEY_X) pick = 2;
		if (down & KEY_START) pick = 3;
		if (pick < 0) return TACT_NONE;
		sfxPlay(S_UI_OK, 0.8f, 0.0f, 0.9f);
		if (pick == 0) { tState = TS_SAVES; tSel = 0; loadSaves(); return TACT_NONE; }
		if (pick == 1) return TACT_FREE;
		if (pick == 2) return TACT_SETTINGS;
		return TACT_QUIT;
	}
	if (tState == TS_SAVES) {
		if (down & KEY_DDOWN) { tSel = (tSel + 1) % 4; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 1.0f); }
		if (down & KEY_DUP)   { tSel = (tSel + 3) % 4; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 1.0f); }
		if (down & KEY_B) { tState = TS_MAIN; tSel = 0; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 0.8f); return TACT_NONE; }
		if ((down & KEY_X) && tSel < 3 && saves[tSel].used) { tSlot = tSel; tState = TS_DELETE; sfxPlay(S_UI_MOVE, 0.7f, 0.0f, 0.7f); return TACT_NONE; }
		int pick = -1;
		if (down & KEY_A) pick = tSel;
		if (down & KEY_TOUCH) for (int i = 0; i < 4; i++) {
			float y0, h; menuRect(i, &y0, &h);
			if (tp->px >= 40 && tp->px <= 280 && tp->py >= y0 && tp->py <= y0 + h) { tSel = i; pick = i; }
		}
		if (pick < 0) return TACT_NONE;
		if (pick == 3) { tState = TS_MAIN; tSel = 0; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 0.8f); return TACT_NONE; }
		sfxPlay(S_UI_OK, 0.8f, 0.0f, 0.9f);
		if (saves[pick].used) {
			stSlot = pick; memcpy(stName, saves[pick].name, sizeof stName); stName[15] = 0;
			return TACT_STORY;
		}
		tSlot = pick; tState = TS_NAME; tNameLen = 0; tName[0] = 0; kbR = 1; kbC = 0;
		return TACT_NONE;
	}
	if (tState == TS_DELETE) {
		if (down & KEY_A) { deleteSave(tSlot); tState = TS_SAVES; sfxPlay(S_THUD, 0.7f, 0.0f, 1.0f); }
		else if (down & KEY_B) { tState = TS_SAVES; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 0.8f); }
		return TACT_NONE;
	}
	// name entry
	if (down & KEY_DRIGHT) { kbC = (kbC + 1) % 10; sfxPlay(S_UI_MOVE, 0.4f, 0.0f, 1.4f); }
	if (down & KEY_DLEFT)  { kbC = (kbC + 9) % 10; sfxPlay(S_UI_MOVE, 0.4f, 0.0f, 1.4f); }
	if (down & KEY_DDOWN)  { kbR = (kbR + 1) % 5; sfxPlay(S_UI_MOVE, 0.4f, 0.0f, 1.4f); }
	if (down & KEY_DUP)    { kbR = (kbR + 4) % 5; sfxPlay(S_UI_MOVE, 0.4f, 0.0f, 1.4f); }
	if (down & KEY_A) {
		if (kbR == 4 && kbC >= 7) return nameConfirm() == TACT_STORY ? TACT_STORY : TACT_NONE;
		kbPress(kbR, kbC);
	}
	if (down & KEY_B) {
		if (tNameLen > 0) { tName[--tNameLen] = 0; sfxPlay(S_UI_MOVE, 0.7f, 0.0f, 0.8f); }
		else { tState = TS_SAVES; tSel = tSlot; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 0.8f); }
	}
	if (down & KEY_START) return nameConfirm() == TACT_STORY ? TACT_STORY : TACT_NONE;
	if (down & KEY_TOUCH) {
		for (int r = 0; r < 5; r++) for (int c = 0; c < 10; c++) {
			float x, y, w, h;
			kbRect(r, c, &x, &y, &w, &h);
			if (tp->px >= x && tp->px <= x + w && tp->py >= y && tp->py <= y + h) {
				kbR = r; kbC = c;
				if (r == 4 && c >= 7) return nameConfirm() == TACT_STORY ? TACT_STORY : TACT_NONE;
				kbPress(r, c);
				return TACT_NONE;
			}
		}
	}
	return TACT_NONE;
}

static void drawTitleMenuBottom(int frame) {
	C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0x0b, 0x0d, 0x14, 0xFF));
	const char* labels[4]; const char* vt[4] = { "", "", "", "" };
	int kind[4] = { 0, 0, 0, 0 }, vn[4] = { 0, 0, 0, 0 };
	if (tState == TS_MAIN) {
		labels[0] = "STORY MODE"; labels[1] = "FREE ROAM"; labels[2] = "SETTINGS"; labels[3] = "QUIT";
		drawMenu(1.0f, "C H I C A G O   3 D S", "// MAIN MENU", 4, labels, kind, vt, vn, tSel, frame, "D-PAD + A  /  TAP");
		if (!audioOK) drawStr(160.0f, 196.0f, 0.30f, C2D_Color32(0xFF, 0x6e, 0x6e, 0xFF), 1, "No sound: DSP firmware (dspfirm.cdc) not found");
		else if (assetsFound == 0) drawStr(160.0f, 196.0f, 0.30f, C2D_Color32(0xFF, 0x6e, 0x6e, 0xFF), 1, "No sound: audio files missing from the build");
	} else if (tState == TS_SAVES || tState == TS_DELETE) {
		static char vbuf[3][32], lbuf[3][12];
		for (int i = 0; i < 3; i++) {
			snprintf(lbuf[i], sizeof lbuf[i], "SAVE %d", i + 1);
			labels[i] = lbuf[i];
			if (saves[i].used) { snprintf(vbuf[i], sizeof vbuf[i], "%s%s", saves[i].name, saves[i].done ? "  (M1 DONE)" : ""); }
			else snprintf(vbuf[i], sizeof vbuf[i], "- NEW GAME -");
			vt[i] = vbuf[i]; kind[i] = 1;
		}
		labels[3] = "BACK";
		drawMenu(1.0f, "S T O R Y   M O D E", "// CHOOSE A SAVE FILE", 4, labels, kind, vt, vn, tSel, frame, "A: PLAY    X: DELETE    B: BACK");
		if (tState == TS_DELETE) {
			C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(0, 0, 0, 0.8f));
			frameRect(40.0f, 70.0f, 240.0f, 100.0f, 2.0f, neon(255, 70, 70, 1.0f));
			drawStr(160.0f, 84.0f, 0.55f, neon(255, 90, 90, 1.0f), 1, "DELETE THIS SAVE?");
			drawStr(160.0f, 112.0f, 0.5f, neon(240, 240, 245, 1.0f), 1, saves[tSlot].name);
			drawStr(160.0f, 142.0f, 0.40f, neon(154, 163, 184, 1.0f), 1, "A: DELETE      B: CANCEL");
		}
	} else {
		// name entry
		frameRect(10.0f, 20.0f, 300.0f, 30.0f, 1.0f, neon(0, 229, 255, 1.0f));
		C2D_DrawRectSolid(10.0f, 20.0f, 0.5f, 300.0f, 30.0f, neon(8, 24, 40, 0.9f));
		C2D_DrawRectSolid(10.0f, 20.0f, 0.5f, 5.0f, 30.0f, neon(255, 46, 209, 1.0f));
		char shown[24];
		snprintf(shown, sizeof shown, "%s%s", tName, (frame / 25) % 2 == 0 ? "_" : " ");
		drawStr(24.0f, 23.0f, 0.65f, neon(255, 255, 255, 1.0f), 0, shown);
		drawStr(300.0f, 8.0f, 0.30f, neon(154, 163, 184, 1.0f), 2, "ENTER YOUR NAME  (A/TAP: TYPE  B: DELETE  START: OK)");
		char cnt[8];
		snprintf(cnt, sizeof cnt, "%d/12", tNameLen);
		drawStr(304.0f, 30.0f, 0.34f, neon(0, 229, 255, 0.8f), 2, cnt);
		for (int r = 0; r < 5; r++) {
			int cmax = (r < 4) ? 10 : 3;
			for (int ci = 0; ci < cmax; ci++) {
				int c = (r < 4) ? ci : (ci == 0 ? 0 : (ci == 1 ? 4 : 7));
				float x, y, w, h;
				kbRect(r, c, &x, &y, &w, &h);
				int on = (kbR == r) && ((r < 4 && kbC == c) || (r == 4 && ((kbC <= 3 && c == 0) || (kbC >= 4 && kbC <= 6 && c == 4) || (kbC >= 7 && c == 7))));
				u32 fillc = on ? neon(0, 229, 255, 0.35f) : neon(8, 24, 40, 0.9f);
				if (r == 4 && c == 7) fillc = on ? neon(255, 46, 209, 0.55f) : neon(120, 20, 100, 0.8f);
				C2D_DrawRectSolid(x, y, 0.5f, w, h, fillc);
				frameRect(x, y, w, h, 1.0f, on ? neon(255, 255, 255, 1.0f) : neon(0, 150, 180, 0.8f));
				char lab[6];
				if (r < 4) { lab[0] = kbRows[r][c]; lab[1] = 0; }
				else snprintf(lab, sizeof lab, "%s", c == 0 ? "SPACE" : (c == 4 ? "DEL" : "OK"));
				drawStr(x + w * 0.5f, y + 5.0f, r < 4 ? 0.55f : 0.45f, on ? neon(255, 255, 255, 1.0f) : neon(120, 220, 235, 1.0f), 1, lab);
			}
		}
	}
}

// ---- menu state ------------------------------------------------------------------------------------
static int mPage = MP_MAIN, mSel = 0, mOpen = 0, mFromTitle = 0, mQuit = 0;

static void menuActivate(int i, int dir) { // dir: 0 = A / tap, -1 / +1 = D-pad left / right
	if (mPage == MP_MAIN) {
		if (dir) return;
		if (i == 0) mOpen = 0;
		else if (i == 1) { if (stMode) { stRestartReq = 1; mOpen = 0; } else { mPage = MP_WEATHER; mSel = weather; } }
		else if (i == 2) { mPage = MP_SETTINGS; mSel = 0; }
		else { mQuit = 1; mOpen = 0; }
	} else if (mPage == MP_WEATHER) {
		if (dir) return;
		if (i < 3) weather = i; else { mPage = MP_MAIN; mSel = 1; }
	} else {
		if (i == 0) camStyle = (camStyle + (dir ? dir : 1) + N_CAM) % N_CAM;
		else if (i == 1) musicLevel = dir ? clampi(musicLevel + dir, 0, 5) : (musicLevel + 1) % 6;
		else if (i == 2) sfxLevel   = dir ? clampi(sfxLevel + dir, 0, 5)   : (sfxLevel + 1) % 6;
		else if (!dir) { if (mFromTitle) mOpen = 0; else { mPage = MP_MAIN; mSel = 2; } }
	}
	sfxPlay(S_UI_OK, 0.7f, 0.0f, 1.0f);
}

static void drawMenuPage(float dim, int frame) {
	const char* labels[4]; const char* vt[4] = {"", "", "", ""};
	int kind[4] = {0, 0, 0, 0}, vn[4] = {0, 0, 0, 0};
	const char* title = "P A U S E D";
	const char* footer = "A: SELECT     B: RESUME     D-PAD: MOVE";
	if (mPage == MP_MAIN) {
		labels[0] = "RESUME"; labels[1] = stMode ? "RESTART CHECKPOINT" : "WEATHER"; labels[2] = "SETTINGS"; labels[3] = "QUIT TO TITLE";
	} else if (mPage == MP_WEATHER) {
		title = "W E A T H E R";
		labels[0] = "SUNNY"; labels[1] = "RAINY"; labels[2] = "NIGHT"; labels[3] = "BACK";
		for (int i = 0; i < 3; i++) { kind[i] = 3; vn[i] = (weather == i); }
	} else {
		title = "S E T T I N G S";
		footer = "LEFT / RIGHT: CHANGE     A: SELECT     B: CLOSE";
		labels[0] = "CAMERA"; labels[1] = "MUSIC"; labels[2] = "SFX VOLUME"; labels[3] = "BACK";
		kind[0] = 1; vt[0] = camNames[camStyle];
		kind[1] = 2; vn[1] = musicLevel;
		kind[2] = 2; vn[2] = sfxLevel;
	}
	drawMenu(dim, title, "// CHICAGO GRID NET", 4, labels, kind, vt, vn, mSel, frame, footer);
}

static void startReload(void) {
	const WeaponDef* w = &wdef[curW];
	if (curW == WP_FISTS || reloadT > 0 || wClip[curW] >= w->clipMax || wRes[curW] <= 0) return;
	reloadT = w->reloadT;
	sfxPlay(S_RELOAD, 0.8f, 0.0f, 1.0f);
}

static void newGame(float* px, float* pz, float* yaw, float* pitch, float* face) {
	*px = gStartX; *pz = gStartZ; *yaw = 0.0f; *pitch = 0.25f; *face = 0.0f;
	for (int w = 0; w < N_WP; w++) { wClip[w] = wdef[w].clipMax; wRes[w] = wdef[w].startReserve; }
	curW = WP_PISTOL; reloadT = 0; fireCD = 0; fireRecent = 0; punchT = 0; hitMark = 0; kick = 0.0f; shake = 0.0f;
	kills = 0; hackTimer = 0; clearFx();
	wheelOrder[0] = WP_PISTOL; wheelOrder[1] = WP_SMG; wheelOrder[2] = WP_SHOTGUN; wheelOrder[3] = WP_ROCKET; wheelOrder[4] = WP_FISTS;
	wheelN = 5; stMode = SM_OFF; gFootY = 0.0f; gVisY = 0.0f; gAi = -1; pHP = 100.0f; pHurtT = 0;
	initNpcs(gStartX, gStartZ);
}

int main(int argc, char** argv) {
	gfxInitDefault();
	osSetSpeedupEnable(true); // New 3DS: full speed (harmless on an Old 3DS)
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	initMapText();
	audioInit();

	C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	DVLB_s* dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
	shaderProgram_s program;
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	int uProj = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	int uMV   = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
	int uTint = shaderInstanceGetUniformLocation(program.vertexShader, "tint");

	// kept in our own structs: citro2d replaces the GPU's current attribute/buffer state
	C3D_AttrInfo attr;
	AttrInfo_Init(&attr);
	AttrInfo_AddLoader(&attr, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(&attr, 1, GPU_FLOAT, 4);

	vbo = (Vertex*)linearAlloc(sizeof(Vertex) * MAX_VERTS);
	buildCity();
	GSPGPU_FlushDataCache(vbo, staticCount * sizeof(Vertex));
	initCars();

	C3D_BufInfo buf;
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, vbo, sizeof(Vertex), 2, 0x10);

	C3D_Mtx proj, ortho, ident;
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 240.0f, 0.0f, 0.0f, 1.0f, true); // 2D overlay for the rain droplets
	Mtx_Identity(&ident);

	px = gStartX; pz = gStartZ; yaw = 0.0f; pitch = 0.25f;
	face = 0.0f; phase = 0.0f; moveAmt = 0.0f; // character heading, step cycle, 0 idle / 1 walk / 2 run
	int   hackFrames = 0, frame = 0, sim = 0;
	float simAcc = 0.0f;
	int   mode = M_BOOT, bootT = 0, titleT = 0;
	float orbit = 0.0f;
	int   phoneTimer = 0, pending = 0;               // phone animation frames left, queued hack (1 lights, 2 blackout)
	int   lastTouchX = -1, lastTouchY = -1;
	float menuDim = 0.0f;
	int   wheelOpen = 0, wheelSel = 0;
	float wheelE = 0.0f; aimE = 0.0f;
	camDist = 6.5f; camSide = 0.0f; camFov = 65.0f;
	newGame(&px, &pz, &yaw, &pitch, &face);
	loadSaves();
	initVince();

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		circlePosition cp;
		hidCircleRead(&cp);
		circlePosition cs; // C-stick (New 3DS); reads zero on an Old 3DS
		hidCstickRead(&cs);
		touchPosition tp;
		hidTouchRead(&tp);

		// ---------------- boot logos + asset loading ----------------
		if (mode == M_BOOT) {
			loadTick();
			if (audioOK) loadTick(); // a little faster
			bootT++;
			int logosDone = bootT >= N_LOGOS * LOGO_FRAMES + 10;
			if (loadDone() && (logosDone || (down & (KEY_A | KEY_TOUCH)))) { mode = M_TITLE; titleT = 0; }
			if (down & KEY_START && loadDone()) mode = M_TITLE;
			down = 0; held = 0; cp.dx = cp.dy = cs.dx = cs.dy = 0;
		}

		// ---------------- title screen ----------------
		if (mode == M_TITLE) {
			titleT++;
			if (!mOpen && trState == 0) {
				int ta = titleInput(down, &tp);
				if (ta == TACT_QUIT) break;
				if (ta == TACT_SETTINGS) { mOpen = 1; mFromTitle = 1; mPage = MP_SETTINGS; mSel = 0; down = 0; }
				else if (ta == TACT_FREE) {
					newGame(&px, &pz, &yaw, &pitch, &face);
					mode = M_GAME; sim = 0; simAcc = 0.0f; phoneTimer = 0; pending = 0; hackFrames = 0; frame = 0;
					wheelOpen = 0; wheelE = 0.0f; aimE = 0.0f; moveAmt = 0.0f;
					down = 0;
				}
				else if (ta == TACT_STORY) { trStart(TR_START_STORY); down = 0; }
			}
			held = 0; cp.dx = cp.dy = cs.dx = cs.dy = 0; // ignore game input on the title screen
		}

		// ---------------- story: fades between worlds and restarts ----------------
			{
				int act = trUpdate();
				if (act) {
					mode = storySwap(act);
					if (mode == M_TITLE) { titleT = 0; tState = TS_MAIN; tSel = 0; mOpen = 0; }
					else if (act == TR_START_STORY) { sim = 0; simAcc = 0.0f; phoneTimer = 0; pending = 0; hackFrames = 0; wheelOpen = 0; wheelE = 0.0f; frame = 0; mOpen = 0; }
				}
				if (stRestartReq) { stRestartReq = 0; trStart(TR_RESTART); }
				if (trState) { down = 0; held = 0; cp.dx = cp.dy = cs.dx = cs.dy = 0; }
			}

			// ---------------- pause / settings menu ----------------
		if (mode != M_BOOT) {
			if (mode == M_GAME && !mOpen && !wheelOpen && csKind == CS_NONE && (stMode == SM_OFF || stMode == SM_PLAY) && (down & KEY_START)) {
				mOpen = 1; mFromTitle = 0; mPage = MP_MAIN; mSel = 0;
				sfxPlay(S_UI_OK, 0.6f, 0.0f, 1.2f);
				down &= ~KEY_START;
			} else if (mOpen) {
				if (down & KEY_B) {
					mOpen = 0;
				} else {
					if (down & KEY_DDOWN) { mSel = (mSel + 1) % 4; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 1.0f); }
					if (down & KEY_DUP)   { mSel = (mSel + 3) % 4; sfxPlay(S_UI_MOVE, 0.6f, 0.0f, 1.0f); }
					if (mPage == MP_SETTINGS && (down & KEY_DLEFT))  menuActivate(mSel, -1);
					if (mPage == MP_SETTINGS && (down & KEY_DRIGHT)) menuActivate(mSel, +1);
					if (down & KEY_A) menuActivate(mSel, 0);
					if (down & KEY_TOUCH) {
						for (int i = 0; i < 4; i++) {
							float my0, mh;
							menuRect(i, &my0, &mh);
							if (tp.px >= 40 && tp.px <= 280 && tp.py >= my0 && tp.py <= my0 + mh) {
								mSel = i;
								if (mPage == MP_SETTINGS && (i == 1 || i == 2)) menuActivate(i, tp.px > 170 ? +1 : -1);
								else menuActivate(i, 0);
							}
						}
					}
				}
				if (!mOpen && mQuit) { mQuit = 0; wheelOpen = 0; if (stMode) trStart(TR_TO_TITLE); else { mode = M_TITLE; titleT = 0; tState = TS_MAIN; tSel = 0; } }
				down = 0; held = 0; cp.dx = cp.dy = cs.dx = cs.dy = 0; // the game ignores input while the menu is open
			}
		}
		int csSkipReq = 0;
			if (mode == M_GAME && stMode != SM_OFF && !mOpen) {
				int cutNow = (csKind != CS_NONE);
				if (cutNow) csSkipReq = (down & KEY_START) != 0;
				if (stMode == SM_FAIL && trState == 0) { if (down & KEY_A) trStart(TR_RESTART); else if (down & KEY_B) trStart(TR_TO_TITLE); }
				if (stMode == SM_END) { endT++; if (endT > 300 && trState == 0) trStart(TR_TO_TITLE); }
				if (cutNow || stMode == SM_FAIL || stMode == SM_END) { down = 0; held = 0; cp.dx = cp.dy = cs.dx = cs.dy = 0; }
				if (cutNow) { csUpdate(csSkipReq); if (csDone) storyAfterCut(); }
			}
			menuDim += ((mOpen ? 1.0f : 0.0f) - menuDim) * 0.22f;
		if (!mOpen && menuDim < 0.01f) menuDim = 0.0f;
		updateWeather();

		// ---------------- weapon wheel ----------------
		int canPlay = (mode == M_GAME) && !mOpen && csKind == CS_NONE && (stMode == SM_OFF || stMode == SM_PLAY) && trState == 0;
		if (canPlay) {
			int y = (held & KEY_Y) != 0;
			if (y && !wheelOpen) { wheelOpen = 1; wheelSel = wheelSlotOf(curW); sfxPlay(S_SWITCH, 0.6f, 0.0f, 0.8f); }
			int apply = 0;
			if (wheelOpen) {
				float sx = (float)cp.dx, sy = -(float)cp.dy;
				if (sx * sx + sy * sy > 40.0f * 40.0f) {
					float a = atan2f(sy, sx) + 1.5708f; // 0 at the top, clockwise
					int s = (int)floorf(a / (6.28319f / (float)wheelN) + 0.5f);
					wheelSel = ((s % wheelN) + wheelN) % wheelN;
				}
				if (down & KEY_TOUCH) {
					for (int i = 0; i < wheelN; i++) {
						float tx, ty;
						wheelTile(i, &tx, &ty);
						if (fabsf((float)tp.px - tx) < TILE_W * 0.5f && fabsf((float)tp.py - ty) < TILE_H * 0.5f) { wheelSel = i; apply = 1; }
					}
				}
				if (!y) apply = 1;
				if (apply) {
					int nw = wheelOrder[wheelSel];
					if (nw != curW) { curW = nw; reloadT = 0; fireCD = 12; sfxPlay(S_SWITCH, 0.8f, 0.0f, 1.0f); }
					wheelOpen = 0;
				}
				down &= ~(KEY_TOUCH | KEY_A | KEY_B | KEY_X); // the wheel owns the pad while open
				cp.dx = cp.dy = 0;
				held &= ~(KEY_TOUCH | KEY_R | KEY_ZR | KEY_B);
			}
		} else {
			wheelOpen = 0;
		}
		wheelE += ((wheelOpen ? 1.0f : 0.0f) - wheelE) * 0.3f;
		if (!wheelOpen && wheelE < 0.01f) wheelE = 0.0f;

		// ---------------- looking ----------------
		int aimHeld  = (held & (KEY_L | KEY_ZL)) != 0 && curW != WP_FISTS;
		int fireHeld = (held & (KEY_R | KEY_ZR)) != 0;
		int fireDown = (down & (KEY_R | KEY_ZR)) != 0;
		int fp       = (camStyle == CAM_FIRST);
		float lookK  = aimE > 0.5f ? 0.6f : 1.0f; // finer aiming while zoomed
		if (held & KEY_DUP)   pitch -= 0.03f * lookK;
		if (held & KEY_DDOWN) pitch += 0.03f * lookK;
		if (abs(cs.dx) > 15) yaw   -= cs.dx / 156.0f * 0.05f * lookK;
		if (abs(cs.dy) > 15) pitch -= cs.dy / 156.0f * 0.04f * lookK;
		if (down & KEY_DLEFT && canPlay && !wheelOpen) { // quick weapon switch
			curW = stMode ? wheelOrder[(wheelSlotOf(curW) + wheelN - 1) % wheelN] : (curW + N_WP - 1) % N_WP; reloadT = 0; fireCD = 12; sfxPlay(S_SWITCH, 0.8f, 0.0f, 1.0f);
		}
		if (down & KEY_DRIGHT && canPlay && !wheelOpen) {
			curW = stMode ? wheelOrder[(wheelSlotOf(curW) + 1) % wheelN] : (curW + 1) % N_WP; reloadT = 0; fireCD = 12; sfxPlay(S_SWITCH, 0.8f, 0.0f, 1.0f);
		}

		// touch: bottom strip = HACK button, elsewhere = drag to look
		if (canPlay && !wheelOpen && (held & KEY_TOUCH)) {
			if (tp.py >= BTN_Y0) {
				if (down & KEY_TOUCH) {
					if (phoneTimer == 0 && tp.py < BTN_Y1 && !stMode) {
						pending = (tp.px < 160) ? 1 : 2; // 1 = hack lights, 2 = blackout
						phoneTimer = 120;                // character pulls out the phone
					}
				}
			} else if (lastTouchX >= 0) {
				yaw   -= (tp.px - lastTouchX) * 0.01f;
				pitch += (tp.py - lastTouchY) * 0.01f;
			}
			lastTouchX = tp.px; lastTouchY = tp.py;
		} else {
			lastTouchX = lastTouchY = -1;
		}
		float pMin = fp ? -1.35f : -0.25f, pMax = fp ? 1.35f : 1.2f;
		if (pitch > pMax) pitch = pMax;
		if (pitch < pMin) pitch = pMin;

		// ---------------- aim state and character ----------------
		int armed = (curW != WP_FISTS);
		float aimTarget = (canPlay && !wheelOpen && aimHeld) ? 1.0f : 0.0f;
		if (canPlay && !wheelOpen && armed && fireRecent > 0) aimTarget = fmaxf(aimTarget, 0.85f);
		aimE += (aimTarget - aimE) * 0.22f;
		int aiming = aimE > 0.5f;

		float fx = (cp.dy / 156.0f), sx = (cp.dx / 156.0f);
		if (fabsf(fx) < 0.1f) fx = 0;
		if (fabsf(sx) < 0.1f) sx = 0;
		float len = sqrtf(fx * fx + sx * sx);
		if (len > 1.0f) { fx /= len; sx /= len; len = 1.0f; }
		if (phoneTimer > 0 || mode != M_GAME) { fx = 0.0f; sx = 0.0f; len = 0.0f; } // standing still while on the phone
		int running = (held & KEY_B) && len > 0.5f && !aiming;
		float speed = running ? 0.17f : (aiming ? 0.045f : 0.07f);
		float dirx = -sinf(yaw), dirz = -cosf(yaw);
		float rgtx =  cosf(yaw), rgtz = -sinf(yaw);
		float nx = px + (dirx * fx + rgtx * sx) * speed;
		float nz = pz + (dirz * fx + rgtz * sx) * speed;
		if (mode == M_GAME) {
			if (!blocked(nx, pz)) px = nx;
			if (!blocked(px, nz)) pz = nz;
		}

		if (gLevel == LV_FACTORY) {
				gFootY = groundAt(px, pz, gFootY, &gAi);
				gVisY += (gFootY - gVisY) * 0.35f;
				if (fabsf(gFootY - gVisY) < 0.01f) gVisY = gFootY;
				if (gAi >= 0) memcpy(stTint, areas[gAi].tint, sizeof stTint);
			} else { gFootY = 0.0f; gVisY = 0.0f; gAi = -1; }

		// walking / running animation state; aiming and first person always face where you look
		int faceLook = aiming || fp || (armed && fireRecent > 0);
		if (faceLook) {
			float dlt = yaw - face;
			while (dlt >  3.14159f) dlt -= 6.28318f;
			while (dlt < -3.14159f) dlt += 6.28318f;
			face += dlt * 0.35f;
			if (len > 0.0f) phase += speed * 0.6f + 0.06f;
		} else if (len > 0.0f) {
			float mx = dirx * fx + rgtx * sx, mz = dirz * fx + rgtz * sx;
			float dlt = atan2f(-mx, -mz) - face;
			while (dlt >  3.14159f) dlt -= 6.28318f;
			while (dlt < -3.14159f) dlt += 6.28318f;
			face  += dlt * 0.25f;
			phase += speed * 0.6f + 0.06f;
		}
		moveAmt += ((len > 0.0f ? (running ? 2.0f : 1.0f) : 0.0f) - moveAmt) * 0.15f;

		// phone animation: the hack fires once the phone is raised
		if (phoneTimer > 0 && canPlay) {
			phoneTimer--;
			if (phoneTimer == 80) {
				if (pending == 1) {
					float bd = 1e9f;
					for (int k = 0; k < NRX; k++) if (fabsf(RX[k] - px) < bd) { bd = fabsf(RX[k] - px); hackA = k; }
					bd = 1e9f;
					for (int k = 0; k < NRZ; k++) if (fabsf(RZ[k] - pz) < bd) { bd = fabsf(RZ[k] - pz); hackB = k; }
					hackTimer = 600;
				} else if (pending == 2) {
					hackFrames = 180;
				}
				pending = 0;
			}
		}

		// ---------------- camera ----------------
		{
			float dT, sT, fT;
			if (fp)                       { dT = 0.0f; sT = 0.0f;  fT = aiming ? 48.0f : 68.0f; }
			else if (camStyle == CAM_SHOULDER) { dT = aiming ? 2.3f : 3.7f; sT = aiming ? 0.85f : 0.75f; fT = aiming ? 52.0f : 62.0f; }
			else                          { dT = aiming ? 3.3f : 6.5f; sT = aiming ? 0.95f : 0.0f;  fT = aiming ? 52.0f : 65.0f; }
			camDist += (dT - camDist) * 0.18f;
			camSide += (sT - camSide) * 0.18f;
			camFov  += (fT - camFov)  * 0.18f;
		}
		float cyaw = yaw, cpitch = pitch - kick;
		float cx, cy, cz;
		{
			float c3[3];
			chaseCamera(px, pz, yaw, pitch, camDist, camSide, fp, c3);
			cx = c3[0]; cy = c3[1]; cz = c3[2];
			if (stMode != SM_OFF && mode == M_GAME) {
				if (csKind == CS_NONE) storyTriggers(c3, cyaw, cpitch, camFov);
				if (csKind != CS_NONE) { csCamera(c3, &cyaw, &cpitch, &camFov); cx = c3[0]; cy = c3[1]; cz = c3[2]; }
			}
		}
		if (shake > 0.01f) { cx += frange(-1.0f, 1.0f) * shake * 0.12f; cy += frange(-1.0f, 1.0f) * shake * 0.12f; cz += frange(-1.0f, 1.0f) * shake * 0.12f; }
		gCamX = cx; gCamY = cy; gCamZ = cz; gCamYaw = cyaw;
		if (mode != M_GAME) { // slow cinematic orbit around the whole city (title screen and logos)
			orbit += 0.004f;
			float mx = (gXmin + gXmax) * 0.5f, mz = (gZmin + gZmax) * 0.5f;
			cyaw = orbit; cpitch = 0.52f; cy = 80.0f;
			cx = mx + sinf(orbit) * 175.0f; cz = mz + cosf(orbit) * 175.0f;
		}

		// ---------------- shooting, reloading, punching ----------------
		if (canPlay && !wheelOpen && phoneTimer == 0) {
			const WeaponDef* w = &wdef[curW];
			if (curW == WP_FISTS) {
				if (fireDown && punchT == 0) { punchT = 16; fireCD = 0; punch(px, pz, face); sfxPlay(S_THUD, 0.35f, 0.0f, 1.6f); }
			} else {
				if (reloadT > 0) {
					if (--reloadT == 0) {
						int need = w->clipMax - wClip[curW], take = need < wRes[curW] ? need : wRes[curW];
						wClip[curW] += take; wRes[curW] -= take;
					}
				}
				if ((down & KEY_X)) startReload();
				int want = w->autofire ? fireHeld : fireDown;
				if (want && fireCD == 0 && reloadT == 0) {
					if (wClip[curW] > 0) {
						ShotCtx sc;
						sc.cam[0] = cx; sc.cam[1] = cy; sc.cam[2] = cz;
						sc.yaw = cyaw; sc.pitch = cpitch;
						float dxp = px - cx, dzp = pz - cz;
						sc.tmin = fp ? 0.3f : sqrtf(dxp * dxp + dzp * dzp) + 0.7f;
						float ffx = -sinf(face), ffz = -cosf(face), rfx = cosf(face), rfz = -sinf(face);
						if (fp) {
							sc.muzzle[0] = cx + (-sinf(yaw)) * 1.0f + cosf(yaw) * 0.3f;
							sc.muzzle[1] = cy - 0.3f - sinf(pitch) * 0.9f;
							sc.muzzle[2] = cz + (-cosf(yaw)) * 1.0f - sinf(yaw) * 0.3f;
						} else {
							sc.muzzle[0] = px + ffx * 0.95f + rfx * 0.36f;
							sc.muzzle[1] = gVisY + 1.45f - sinf(pitch) * 0.9f;
							sc.muzzle[2] = pz + ffz * 0.95f + rfz * 0.36f;
						}
						sc.aimE = aimE;
						fireShot(&sc);
						wClip[curW]--;
						fireCD = w->interval;
						fireRecent = 40;
						if (wClip[curW] == 0) startReload();
					} else if (wRes[curW] > 0) {
						startReload();
					} else {
						sfxPlay(S_CLICK, 0.7f, 0.0f, 1.0f);
						fireCD = 20;
					}
				}
			}
		}

		// ---------------- simulation (slows down while the wheel is open) ----------------
		simAcc += wheelOpen ? 0.25f : 1.0f;
		while (simAcc >= 1.0f) {
			simAcc -= 1.0f;
			if (!mOpen) {
				if (hackFrames > 0) hackFrames--;
				sim++;
				if (stMode != SM_OFF && mode == M_GAME) {
					if (csKind == CS_NONE && trState != 2) storySim(len > 0.0f);
					updateFx();
				} else {
					updateCars(sim, mode == M_GAME ? px : 1e6f, mode == M_GAME ? pz : 1e6f);
					updateNpcs(px, pz);
					updateRockets(px, pz);
					updateFx();
					if (mode == M_GAME) ambienceTick();
				}
			}
		}
		audioUpdate(mode == M_BOOT ? M_BOOT : mode, mOpen ? 0.35f : 1.0f);

		// ---------------- build this frame's vertices ----------------
		C3D_Mtx mv;
		Mtx_Identity(&mv);
		Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(camFov), C3D_AspectRatioTop, 0.35f, 450.0f, false);
		Mtx_RotateX(&mv, cpitch, true);
		Mtx_RotateY(&mv, -cyaw, true);
		Mtx_Translate(&mv, -cx, -cy, -cz, true);

		int worldEnd = staticCount, lightsEnd = staticCount, skyEnd = staticCount, dropEnd = staticCount;
		if (mode != M_BOOT) {
			vcount = staticCount;
			if (gLevel == LV_CITY) { drawCars(); drawNpcs(cx, cz); }
			else { drawFoes(cx, cz); drawGates(); csDrawProps(); }
			if (mode == M_GAME) {
				Pose pp;
				memset(&pp, 0, sizeof pp);
				pp.px = px; pp.pz = pz; pp.py = gVisY; pp.face = face; pp.phase = phase; pp.moveAmt = moveAmt;
				if (phoneTimer > 0) {
					float up = (120 - phoneTimer) / 25.0f, dn = phoneTimer / 25.0f;
					float e = up < dn ? up : dn;
					if (e > 1.0f) e = 1.0f;
					pp.phoneE = e * e * (3.0f - 2.0f * e);
				}
				pp.gun = curW; pp.aimE = aimE; pp.gunPitch = cpitch;
				pp.punch = (punchT > 0) ? sinf((float)punchT / 16.0f * 3.14159f) : 0.0f;
				pp.onlyArms = fp && csKind == CS_NONE;
				if (csKind != CS_NONE) csHeroPose(&pp);
				if (stMode == SM_FAIL) { pp.fall = fminf((float)failT / 16.0f, 1.0f); pp.aimE = 0.0f; pp.moveAmt = 0.0f; pp.onlyArms = 0; }
				if (stMode == SM_END) { pp.fall = 1.0f; pp.fallFwd = 1; pp.gun = WP_FISTS; pp.moveAmt = 0.0f; pp.onlyArms = 0; }
				drawPerson(&pp, &playerOutfit);
			}
			GSPGPU_FlushDataCache(&vbo[staticCount], (vcount - staticCount) * sizeof(Vertex));
			worldEnd = vcount;
			if (gLevel == LV_CITY) drawLights(sim, wNight, cx, cz); else drawGateLamps(frame);
			lightsEnd = vcount;
			if (gLevel == LV_CITY || gAi == 11) drawSkyBodies(cx, cy, cz, cyaw);
			if (gLevel == LV_CITY) drawRain(cx, cz, cyaw);
			drawFx();
			skyEnd = vcount;
			GSPGPU_FlushDataCache(&vbo[worldEnd], (skyEnd - worldEnd) * sizeof(Vertex));
			drawDroplets();
			dropEnd = vcount;
			GSPGPU_FlushDataCache(&vbo[skyEnd], (dropEnd - skyEnd) * sizeof(Vertex));
		}

		float t = (hackFrames > 0) ? 0.25f : 1.0f;
		float litT[3];
		{
			static const float dayWin[3] = {0.20f, 0.33f, 0.80f};
			for (int i = 0; i < 3; i++) litT[i] = (dayWin[i] * wTint[i] * (1.0f - wNight) + 0.95f * wNight) * t;
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TextBufClear(gDyn);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, (mode == M_BOOT) ? 0x000000FF : (hackFrames > 0 ? 0x080810FF : skyColor(1.0f)), 0);
		C3D_FrameDrawOn(top);
		if (mode != M_BOOT && trState != 2) {
			// citro2d changes the GPU state while drawing the bottom screen, so set ours up every frame
			C3D_BindProgram(&program);
			C3D_SetAttrInfo(&attr);
			C3D_SetBufInfo(&buf);
			for (int i = 0; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
			C3D_TexEnv* env = C3D_GetTexEnv(0);
			C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
			C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
			C3D_CullFace(GPU_CULL_NONE);
			C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_ALL);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, &proj);
			C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMV, &mv);
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
			               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
			// 1) the lit world (city, cars, people): tinted by the weather
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, wTint[0] * t, wTint[1] * t, wTint[2] * t, 1.0f);
			C3D_DrawArrays(GPU_TRIANGLES, 0, worldStaticEnd);
			C3D_DrawArrays(GPU_TRIANGLES, staticCount, worldEnd - staticCount);
			// 2) lit windows and lamp heads: glow at night
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, litT[0], litT[1], litT[2], 1.0f);
			C3D_DrawArrays(GPU_TRIANGLES, worldStaticEnd, staticCount - worldStaticEnd);
			// 3) traffic lights and car lights: not darkened by weather
			C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, t, t, t, 1.0f);
			if (lightsEnd > worldEnd) C3D_DrawArrays(GPU_TRIANGLES, worldEnd, lightsEnd - worldEnd);
			// 4) sun / moon / stars / rain / bullet effects: blended, no depth writes
			C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
			if (skyEnd > lightsEnd) C3D_DrawArrays(GPU_TRIANGLES, lightsEnd, skyEnd - lightsEnd);
			// 5) raindrops on the "lens": flat 2D overlay, no depth
			if (dropEnd > skyEnd) {
				C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
				C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uProj, &ortho);
				C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uMV, &ident);
				C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, 1.0f, 1.0f, 1.0f, 1.0f);
				C3D_DrawArrays(GPU_TRIANGLES, skyEnd, dropEnd - skyEnd);
			}
		}

		// top screen 2D: logos, title text or the HUD
		C2D_Prepare();
		C2D_SceneBegin(top);
		C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
		if (mode == M_BOOT) {
			drawBootTop(bootT);
		} else if (mode == M_TITLE) {
			drawTitleTop(titleT);
		} else if (!mOpen) {
			if (csKind != CS_NONE) {
				csOverlay(frame);
			} else if (stMode == SM_END) {
				drawEndTop(frame);
			} else {
				float rf = (reloadT > 0 && curW != WP_FISTS) ? 1.0f - (float)reloadT / (float)wdef[curW].reloadT : 0.0f;
				int showCross = armed && (aimE > 0.3f || fireRecent > 0 || fp);
				drawHud(frame, showCross, aimE, rf, sim);
				if (stMode != SM_OFF) { drawStoryHud(frame); if (stMode == SM_FAIL) drawFailTop(frame); }
			}
		}
		if (trA > 0.003f) C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 400.0f, 240.0f, neon(0, 0, 0, trA));

		// bottom screen: loading, title card, street map, wheel and menus
		C2D_TargetClear(bottom, C2D_Color32(0x12, 0x15, 0x1d, 0xFF));
		C2D_SceneBegin(bottom);
		if (mode == M_BOOT) {
			drawLoadBottom(frame);
		} else if (mode == M_TITLE) {
			drawTitleMenuBottom(frame);
		} else {
			if (stMode == SM_END) C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, C2D_Color32(0, 0, 0, 0xFF));
			else if (stMode != SM_OFF) drawStoryBottom(frame);
			else drawMap(px, pz, face, frame, phoneTimer);
			drawWheel(wheelE, wheelSel, frame);
		}
		if (mode != M_BOOT) drawMenuPage(menuDim, frame);
		if (trA > 0.003f) C2D_DrawRectSolid(0.0f, 0.0f, 0.5f, 320.0f, 240.0f, neon(0, 0, 0, trA));
		C2D_Flush();
		C3D_FrameEnd(0);

		frame++;
	}

	audioExit();
	linearFree(vbo);
	shaderProgramFree(&program);
	DVLB_Free(dvlb);
	C2D_TextBufDelete(gStatic);
	C2D_TextBufDelete(gDyn);
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
