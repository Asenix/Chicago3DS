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

#define GRID      8
#define PITCH     24.0f
#define BLDG_W    16.0f
#define FLOOR_H   3.5f
#define MAX_VERTS 26000
#define RAIN_N    140
#define N_DROPS   11
#define N_CARS    20
#define N_PEDS    14
#define N_STARS   70
#define ROADS     7
#define PLAYER_R  0.6f
#define TEX_N     128

typedef struct { float pos[3]; float clr[4]; float uv[2]; } Vertex;
typedef struct { float x0, z0, x1, z1, h; } Building;

static Vertex*  vbo;
static int      vcount = 0, staticCount = 0, wallStart = 0, wallEnd = 0;
static Building bldg[GRID * GRID];

// quality switches (pause menu > GRAPHICS)
static int gFog = 1, gWin = 1, gFps = 1;
static int gFpsValue = 60;

// camera position used to skip far-away dynamic geometry (keeps the frame rate at 60)
static float gCamX = 0.0f, gCamZ = 0.0f, gCullR = 140.0f;

static unsigned hash2(int a, int b) {
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static int inView(float x, float z) {
	float dx = x - gCamX, dz = z - gCamZ;
	return dx * dx + dz * dz < gCullR * gCullR;
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
// Roads run along x = ROAD_C(k) and z = ROAD_C(k), k = 0..6. Intersections are (a, b).
#define ROAD_C(k) (-72.0f + 24.0f * (float)(k))

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
	float u = (x1 - x0) / 16.0f, v = h / (4.0f * FLOOR_H);
	wallFace(p000, p100, p110, p010, r * 0.55f, g * 0.55f, b * 0.55f, uo,         uo + u,         vo, vo + v);
	wallFace(p101, p001, p011, p111, r * 0.78f, g * 0.78f, b * 0.78f, uo + 0.25f, uo + 0.25f + u, vo, vo + v);
	wallFace(p001, p000, p010, p011, r * 0.60f, g * 0.60f, b * 0.60f, uo + 0.50f, uo + 0.50f + u, vo, vo + v);
	wallFace(p100, p101, p111, p110, r * 0.88f, g * 0.88f, b * 0.88f, uo + 0.75f, uo + 0.75f + u, vo, vo + v);
}

static void buildCity(void) {
	float ext = GRID * PITCH * 0.5f, G = ext + 300.0f;
	// asphalt ground, extends far so fog can melt it into the horizon
	float g0[3] = {-G, 0, -G}, g1[3] = {G, 0, -G}, g2[3] = {G, 0, G}, g3[3] = {-G, 0, G};
	quad(g0, g3, g2, g1, 0.16f, 0.17f, 0.19f);

	// distant skyline ring (never reachable, heavily fogged)
	for (int i = 0; i < 48; i++) {
		unsigned h = hash2(i, 31337);
		float ang = (float)i * 6.2832f / 48.0f + (float)(h & 15) * 0.004f;
		float rad = 150.0f + (float)((h >> 4) % 50);
		float w   = 9.0f + (float)((h >> 10) % 9);
		float hh  = 22.0f + (float)((h >> 14) % 70);
		float cx = sinf(ang) * rad, cz = cosf(ang) * rad;
		float tone = 0.42f + (float)((h >> 20) % 20) / 100.0f;
		addBoxG(cx - w, cz - w, cx + w, cz + w, 0.0f, hh, tone * 0.95f, tone, tone * 1.08f, 0.5f, 1.0f);
	}

	// sidewalks, roofs, rooftop units
	for (int i = 0; i < GRID; i++) {
		for (int j = 0; j < GRID; j++) {
			float cx = (i - (GRID - 1) * 0.5f) * PITCH;
			float cz = (j - (GRID - 1) * 0.5f) * PITCH;
			unsigned h = hash2(i, j);
			Building* b = &bldg[i * GRID + j];
			b->x0 = cx - BLDG_W * 0.5f; b->x1 = cx + BLDG_W * 0.5f;
			b->z0 = cz - BLDG_W * 0.5f; b->z1 = cz + BLDG_W * 0.5f;
			b->h  = FLOOR_H * (float)(2 + (h % 12));

			float s0[3] = {b->x0 - 1.5f, 0.03f, b->z0 - 1.5f}, s1[3] = {b->x1 + 1.5f, 0.03f, b->z0 - 1.5f};
			float s2[3] = {b->x1 + 1.5f, 0.03f, b->z1 + 1.5f}, s3[3] = {b->x0 - 1.5f, 0.03f, b->z1 + 1.5f};
			quad(s0, s3, s2, s1, 0.45f, 0.45f, 0.47f);

			float tone = 0.45f + (float)((h >> 8) % 30) / 100.0f;
			float r0[3] = {b->x0, b->h, b->z0}, r1[3] = {b->x1, b->h, b->z0};
			float r2[3] = {b->x1, b->h, b->z1}, r3[3] = {b->x0, b->h, b->z1};
			quad(r0, r1, r2, r3, 0.30f + tone * 0.15f, 0.31f + tone * 0.15f, 0.33f + tone * 0.15f);
			float ux = cx + ((int)((h >> 3) % 9) - 4) * 1.1f, uz = cz + ((int)((h >> 7) % 9) - 4) * 1.1f;
			addBoxG(ux - 0.9f, uz - 0.9f, ux + 0.9f, uz + 0.9f, b->h, b->h + 1.1f, 0.52f, 0.54f, 0.57f, 0.8f, 1.0f);
		}
	}

	// road markings: dashed centre lines, zebra crossings, signal poles
	for (int k = 0; k < ROADS; k++) {
		float c0 = ROAD_C(k);
		for (float s = -ext; s < ext; s += 6.0f) {
			float m = s + 1.25f;
			int skip = 0;
			for (int q = 0; q < ROADS; q++) if (fabsf(m - ROAD_C(q)) < 4.8f) skip = 1;
			if (skip) continue;
			float a[3] = {c0 - 0.12f, 0.05f, s}, b[3] = {c0 + 0.12f, 0.05f, s};
			float c[3] = {c0 + 0.12f, 0.05f, s + 2.5f}, d[3] = {c0 - 0.12f, 0.05f, s + 2.5f};
			quad(a, b, c, d, 0.85f, 0.72f, 0.16f);
			float e[3] = {s, 0.05f, c0 - 0.12f}, f[3] = {s, 0.05f, c0 + 0.12f};
			float g[3] = {s + 2.5f, 0.05f, c0 + 0.12f}, hh[3] = {s + 2.5f, 0.05f, c0 - 0.12f};
			quad(e, f, g, hh, 0.85f, 0.72f, 0.16f);
		}
	}
	for (int a = 0; a < ROADS; a++) {
		for (int b = 0; b < ROADS; b++) {
			float xc = ROAD_C(a), zc = ROAD_C(b);
			for (int arm = 0; arm < 4; arm++) {
				float sg = (arm & 1) ? 1.0f : -1.0f;
				for (int st = -1; st <= 1; st++) {
					float off = st * 1.7f;
					float p0[3], p1[3], p2[3], p3[3];
					if (arm < 2) { // vertical road arm: bars run along z
						float zz = zc + sg * 3.6f, xx = xc + off;
						p0[0] = xx - 0.4f; p0[1] = 0.05f; p0[2] = zz - 0.8f;
						p1[0] = xx + 0.4f; p1[1] = 0.05f; p1[2] = zz - 0.8f;
						p2[0] = xx + 0.4f; p2[1] = 0.05f; p2[2] = zz + 0.8f;
						p3[0] = xx - 0.4f; p3[1] = 0.05f; p3[2] = zz + 0.8f;
					} else {       // horizontal road arm: bars run along x
						float xx = xc + sg * 3.6f, zz = zc + off;
						p0[0] = xx - 0.8f; p0[1] = 0.05f; p0[2] = zz - 0.4f;
						p1[0] = xx + 0.8f; p1[1] = 0.05f; p1[2] = zz - 0.4f;
						p2[0] = xx + 0.8f; p2[1] = 0.05f; p2[2] = zz + 0.4f;
						p3[0] = xx - 0.8f; p3[1] = 0.05f; p3[2] = zz + 0.4f;
					}
					quad(p0, p1, p2, p3, 0.78f, 0.78f, 0.76f);
				}
			}
			float x = xc + 3.4f, z = zc + 3.4f;
			addBoxG(x - 0.15f, z - 0.15f, x + 0.15f, z + 0.15f, 0.0f, 4.0f, 0.2f, 0.2f, 0.22f, 0.8f, 1.0f);
		}
	}

	// textured facades go last so they can be drawn (and re-drawn for night glow) as one range
	wallStart = vcount;
	for (int i = 0; i < GRID; i++) {
		for (int j = 0; j < GRID; j++) {
			unsigned h = hash2(i, j);
			const Building* b = &bldg[i * GRID + j];
			float tone = 0.45f + (float)((h >> 8) % 30) / 100.0f;
			float tint = (float)((h >> 16) % 3);
			float r  = tone * (tint == 0 ? 1.00f : 0.85f);
			float g  = tone * (tint == 1 ? 1.00f : 0.90f);
			float bl = tone * (tint == 2 ? 1.05f : 0.95f);
			addWalls(b->x0, b->z0, b->x1, b->z1, b->h, r, g, bl,
			         (float)((h >> 20) & 3) * 0.25f, (float)((h >> 24) & 3) * 0.25f);
		}
	}
	wallEnd = vcount;
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
		c->s     = -90.0f + (float)((h >> 12) % 180);
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
		float lane = ROAD_C(c->road) + c->dir * 1.3f;
		int stop = 0;

		// red/yellow light just ahead
		for (int k = 0; k < ROADS; k++) {
			float gap = (ROAD_C(k) - c->s) * c->dir;
			if (gap > 3.0f && gap < 5.0f) {
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
				if (gap > 0.0f && gap < 5.0f) stop = 1;
			}
		}
		// player standing in the way
		float ppar = (c->axis == 0) ? px : pz, plat = (c->axis == 0) ? pz : px;
		float pgap = (ppar - c->s) * c->dir;
		if (pgap > 0.0f && pgap < 4.5f && fabsf(plat - lane) < 1.8f) stop = 1;

		c->brake = stop;
		if (!stop) c->s += c->dir * c->speed;
		if (c->s >  100.0f) c->s = -100.0f;
		if (c->s < -100.0f) c->s =  100.0f;
	}
}

static int blocked(float x, float z) {
	for (int i = 0; i < GRID * GRID; i++) {
		const Building* b = &bldg[i];
		if (x > b->x0 - PLAYER_R && x < b->x1 + PLAYER_R &&
		    z > b->z0 - PLAYER_R && z < b->z1 + PLAYER_R) return 1;
	}
	float lim = GRID * PITCH * 0.5f + 20.0f;
	return (x < -lim || x > lim || z < -lim || z > lim);
}

// ---- characters ------------------------------------------------------------
// Blocky low-poly person built from rotated boxes. Body space: feet at the origin,
// facing -Z. Each part swings about its joint (X axis), then the whole body leans
// and turns to its heading.
static float gPx, gPz, gFace, gLean, gBob;

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
		x += jx; y1 += jy; z1 += jz;
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
	float p0[3] = {x - hx, 0.07f, z - hz}, p1[3] = {x + hx, 0.07f, z - hz};
	float p2[3] = {x + hx, 0.07f, z + hz}, p3[3] = {x - hx, 0.07f, z + hz};
	gAlpha = a;
	quad(p0, p1, p2, p3, 0.0f, 0.0f, 0.0f);
	gAlpha = 1.0f;
}

// ---- pedestrians -------------------------------------------------------------
// They walk the sidewalks and wait at the kerb until the cross traffic has a red light.
typedef struct { int axis, road, side, dir, walk; float s, speed, ph; float shirt[3], pants[3], skin[3]; } Ped;
static Ped peds[N_PEDS];

static void initPeds(void) {
	static const float shirt[6][3] = {
		{0.80f, 0.20f, 0.20f}, {0.20f, 0.45f, 0.80f}, {0.90f, 0.75f, 0.20f},
		{0.85f, 0.85f, 0.85f}, {0.25f, 0.65f, 0.40f}, {0.65f, 0.30f, 0.70f}
	};
	static const float skin[4][3] = {
		{0.85f, 0.66f, 0.52f}, {0.62f, 0.45f, 0.34f}, {0.95f, 0.80f, 0.68f}, {0.45f, 0.32f, 0.24f}
	};
	for (int i = 0; i < N_PEDS; i++) {
		unsigned h = hash2(i, 777);
		Ped* p = &peds[i];
		p->axis  = i & 1;
		p->road  = (h >> 3) % ROADS;
		p->side  = ((h >> 8) & 1) ? 1 : -1;
		p->dir   = ((h >> 9) & 1) ? 1 : -1;
		p->s     = -90.0f + (float)((h >> 12) % 180);
		p->speed = 0.035f + (float)((h >> 20) % 5) * 0.006f;
		p->ph    = (float)(h % 100) * 0.1f;
		p->walk  = 1;
		const float* c = shirt[(h >> 5) % 6];
		const float* k = skin[(h >> 14) % 4];
		for (int q = 0; q < 3; q++) { p->shirt[q] = c[q]; p->skin[q] = k[q]; p->pants[q] = 0.10f + 0.06f * (float)((h >> (16 + q)) & 3); }
	}
}

static void updatePeds(int frame) {
	for (int i = 0; i < N_PEDS; i++) {
		Ped* p = &peds[i];
		int wait = 0;
		for (int m = 0; m < ROADS; m++) {
			float gap = (ROAD_C(m) - p->s) * p->dir;
			if (gap > 2.9f && gap < 3.5f) {
				int st = (p->axis == 0) ? lightState(m, p->road, 1, frame) : lightState(p->road, m, 0, frame);
				if (st != 2) wait = 1;
			}
		}
		p->walk = !wait;
		if (!wait) { p->s += p->dir * p->speed; p->ph += p->speed * 1.3f + 0.02f; }
		if (p->s >  100.0f) p->s = -100.0f;
		if (p->s < -100.0f) p->s =  100.0f;
	}
}

static void pedPos(const Ped* p, float* x, float* z) {
	float lat = ROAD_C(p->road) + p->side * 3.3f;
	*x = (p->axis == 0) ? p->s : lat;
	*z = (p->axis == 0) ? lat : p->s;
}

static void drawPed(const Ped* p) {
	float x, z;
	pedPos(p, &x, &z);
	float mx = (p->axis == 0) ? (float)p->dir : 0.0f, mz = (p->axis == 0) ? 0.0f : (float)p->dir;
	gPx = x; gPz = z; gFace = atan2f(-mx, -mz); gLean = 0.0f;
	gBob = p->walk ? fabsf(cosf(p->ph)) * 0.03f : 0.0f;
	float s = p->walk ? sinf(p->ph) * 0.5f : 0.0f;
	addPart(-0.15f, 0.90f, 0.0f, 0.0f, -0.45f, 0.0f, 0.09f, 0.45f, 0.10f,  s, p->pants[0], p->pants[1], p->pants[2]);
	addPart( 0.15f, 0.90f, 0.0f, 0.0f, -0.45f, 0.0f, 0.09f, 0.45f, 0.10f, -s, p->pants[0], p->pants[1], p->pants[2]);
	addPart(0.0f, 0.90f, 0.0f, 0.0f, 0.36f, 0.0f, 0.27f, 0.36f, 0.14f, 0.0f, p->shirt[0], p->shirt[1], p->shirt[2]);
	addPart(0.0f, 1.62f, 0.0f, 0.0f, 0.00f, 0.0f, 0.13f, 0.14f, 0.13f, 0.0f, p->skin[0], p->skin[1], p->skin[2]);
}

// Rebuilds the dynamic tail of the vertex buffer: shadows, cars, pedestrians (only those near the camera).
static void drawDynamic(void) {
	vcount = staticCount;
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * 1.3f;
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
		float lane = ROAD_C(c->road) + c->dir * 1.3f;
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
		float lane = ROAD_C(c->road) + c->dir * 1.3f;
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
			float x = ROAD_C(a) + 3.4f, z = ROAD_C(b) + 3.4f;
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

static void drawPlayer(float px, float pz, float face, float phase, float moveAmt, float phoneE) {
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
	// arms: left swings opposite the left leg; right raises the phone when hacking
	addPart(-0.36f, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, -s * 0.9f, 0.10f, 0.10f, 0.12f);
	float armSwing = s * 0.9f * (1.0f - phoneE) + 1.65f * phoneE;
	addPart( 0.36f, 1.50f, 0.0f, 0.0f, -0.30f, 0.0f, 0.07f, 0.30f, 0.07f, armSwing, 0.10f, 0.10f, 0.12f);
	if (phoneE > 0.05f) // glowing phone in the raised hand
		addPart(0.36f, 1.50f, 0.0f, 0.0f, -0.66f, 0.0f, 0.07f, 0.03f, 0.11f, armSwing, 0.20f, 0.90f, 1.00f);

	GSPGPU_FlushDataCache(&vbo[start], (vcount - start) * sizeof(Vertex));
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

// ---- bottom screen: street map ----------------------------------------------
// North is up (-Z). The map scrolls so the player circle always stays in the centre.
#define MAP_S   3.0f    // pixels per world unit
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

#define N_MENU_TEXT 15
static C2D_TextBuf gStatic, gDyn;
static C2D_Text vLabel[ROADS], hLabel[ROADS], btnHack, btnBlackout;
static C2D_Text txtMenu[N_MENU_TEXT], txtActive, txtPaused, txtWeatherT, txtGfxT, txtSub, txtFooter, txtHint;

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

	// pause menu labels: main 0-3, weather 4-8, graphics toggles 9-14
	static const char* menuNames[N_MENU_TEXT] = {
		"RESUME", "WEATHER", "GRAPHICS", "TITLE",
		"SUNNY", "RAINY", "SUNSET", "NIGHT", "BACK",
		"FOG: ON", "FOG: OFF", "WINDOWS: ON", "WINDOWS: OFF", "FPS: ON", "FPS: OFF"
	};
	for (int i = 0; i < N_MENU_TEXT; i++) {
		C2D_TextParse(&txtMenu[i], gStatic, menuNames[i]);
		C2D_TextOptimize(&txtMenu[i]);
	}
	C2D_TextParse(&txtActive,   gStatic, "ACTIVE");
	C2D_TextParse(&txtPaused,   gStatic, "P A U S E D");
	C2D_TextParse(&txtWeatherT, gStatic, "W E A T H E R");
	C2D_TextParse(&txtGfxT,     gStatic, "G R A P H I C S");
	C2D_TextParse(&txtSub,      gStatic, "// CHICAGO GRID NET");
	C2D_TextParse(&txtFooter,   gStatic, "B: RESUME     A: SELECT     D-PAD: MOVE");
	C2D_TextParse(&txtHint,     gStatic, "SELECT: PAUSE");
	C2D_TextOptimize(&txtActive);
	C2D_TextOptimize(&txtPaused);
	C2D_TextOptimize(&txtWeatherT);
	C2D_TextOptimize(&txtGfxT);
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
	float ext = GRID * PITCH * 0.5f + 12.0f;
	fillClipped(MAP_CX + (-ext - px) * MAP_S, MAP_CY + (-ext - pz) * MAP_S,
	            MAP_CX + ( ext - px) * MAP_S, MAP_CY + ( ext - pz) * MAP_S, cRoad);
	for (int i = 0; i < GRID * GRID; i++) {
		const Building* b = &bldg[i];
		fillClipped(MAP_CX + (b->x0 - 1.5f - px) * MAP_S, MAP_CY + (b->z0 - 1.5f - pz) * MAP_S,
		            MAP_CX + (b->x1 + 1.5f - px) * MAP_S, MAP_CY + (b->z1 + 1.5f - pz) * MAP_S, cBlock);
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
		float lane = ROAD_C(c->road) + c->dir * 1.3f;
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
	int ni = clampi((int)floorf((px + 96.0f) / 48.0f), 0, 3);
	int nj = clampi((int)floorf((pz + 96.0f) / 48.0f), 0, 3);
	int a  = clampi((int)floorf((px + 72.0f) / 24.0f + 0.5f), 0, ROADS - 1);
	int b  = clampi((int)floorf((pz + 72.0f) / 24.0f + 0.5f), 0, ROADS - 1);
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
	C2D_Text t1, t2, t3, t4, t5;
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

// ---- pause menu (bottom screen only) ------------------------------------------
// Menus: 0 main (RESUME, WEATHER, GRAPHICS, TITLE), 1 weather (SUNNY, RAINY, SUNSET, NIGHT, BACK),
// 2 graphics (FOG, WINDOWS, FPS, BACK). B always resumes.
static int menuCount(int state) { return state == 1 ? 5 : 4; }

static void menuRect(int state, int i, float* y0, float* h) {
	float sp = (menuCount(state) <= 4) ? 36.0f : 30.0f;
	*y0 = 60.0f + (float)i * sp;
	*h  = sp - 6.0f;
}

static const C2D_Text* menuText(int state, int i) {
	if (state == 0) return &txtMenu[i];
	if (state == 1) return &txtMenu[4 + i];
	if (i == 0) return gFog ? &txtMenu[9]  : &txtMenu[10];
	if (i == 1) return gWin ? &txtMenu[11] : &txtMenu[12];
	if (i == 2) return gFps ? &txtMenu[13] : &txtMenu[14];
	return &txtMenu[8];
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

	drawCentered(state == 0 ? &txtPaused : (state == 1 ? &txtWeatherT : &txtGfxT), 160.0f, 20.0f, 0.8f, neon(0, 229, 255, dim));
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

		if (state == 1 && i < 4 && i == active) { // current weather tag
			float aw, ah;
			C2D_TextGetDimensions(&txtActive, 0.38f, 0.38f, &aw, &ah);
			C2D_DrawText(&txtActive, C2D_WithColor, x0 + w - aw - 12.0f, y0 + (h - ah) * 0.5f, 0.5f, 0.38f, 0.38f,
			             neon(255, 46, 209, dim));
		}
	}
	drawCentered(&txtFooter, 160.0f, 213.0f, 0.34f, neon(120, 220, 235, 0.85f * dim));
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

int main(int argc, char** argv) {
	gfxInitDefault();
	osSetSpeedupEnable(true); // New 3DS: 804 MHz CPU + extra cache (ignored on an Old 3DS)
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

	C3D_BufInfo buf;
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, vbo, sizeof(Vertex), 3, 0x210);

	C3D_Mtx proj, ortho, ident;
	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(65.0f), C3D_AspectRatioTop, 0.2f, 400.0f, false);
	Mtx_OrthoTilt(&ortho, 0.0f, 400.0f, 240.0f, 0.0f, 1.0f, -1.0f, true); // lens droplets, pixel coordinates
	Mtx_Identity(&ident);

	// start on a road intersection
	float px = 0.0f, pz = 0.0f, yaw = 0.0f, pitch = 0.3f;
	int   hackFrames = 0, frame = 0;
	int   title = 1;
	float orbit = 0.0f;
	float face = 0.0f, phase = 0.0f, moveAmt = 0.0f; // character heading, step cycle, 0 idle / 1 walk / 2 run
	int   phoneTimer = 0, pending = 0;               // phone animation frames left, queued hack (1 lights, 2 blackout)
	int   lastTouchX = -1, lastTouchY = -1;
	int   paused = 0, menuState = 0, sel = 0, sim = 0; // pause menu; sim = simulation clock (stops while paused)
	float pauseDim = 0.0f;
	int   fpsFrames = 0;
	u64   fpsT0 = osGetTime();

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
				title = 0;
				px = 0.0f; pz = 0.0f; yaw = 0.0f; pitch = 0.3f;
				face = 0.0f; phoneTimer = 0; pending = 0;
				hackFrames = 0; frame = 0;
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
					else               { title = 1; paused = 0; menuState = 0; } // TITLE
				} else if (menuState == 1) {
					if (sel < 4) weather = sel;                  // SUNNY / RAINY / SUNSET / NIGHT
					else         { menuState = 0; sel = 1; }     // BACK
				} else {
					if (sel == 0)      gFog = !gFog;
					else if (sel == 1) { if (texOK) gWin = !gWin; }
					else if (sel == 2) gFps = !gFps;
					else               { menuState = 0; sel = 2; } // BACK
				}
			}
			held = 0; cp.dx = 0; cp.dy = 0; cs.dx = 0; cs.dy = 0; // game input is frozen while paused
		}
		pauseDim += ((paused ? 1.0f : 0.0f) - pauseDim) * 0.25f;
		if (pauseDim < 0.01f) pauseDim = 0.0f;
		updateWeather();

		if (held & KEY_L) yaw += 0.04f;
		if (held & KEY_R) yaw -= 0.04f;
		if (held & KEY_DUP)   pitch -= 0.03f;
		if (held & KEY_DDOWN) pitch += 0.03f;
		if (abs(cs.dx) > 15) yaw   -= cs.dx / 156.0f * 0.05f;
		if (abs(cs.dy) > 15) pitch -= cs.dy / 156.0f * 0.04f;

		// touch: bottom strip = HACK button, elsewhere = drag to look
		if (held & KEY_TOUCH) {
			if (tp.py >= BTN_Y0) {
				if (down & KEY_TOUCH) {
					if (phoneTimer == 0 && tp.py < BTN_Y1) {
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
		if (pitch >  1.2f) pitch =  1.2f;
		if (pitch < -0.25f) pitch = -0.25f;

		float fx = (cp.dy / 156.0f), sx = (cp.dx / 156.0f);
		if (fabsf(fx) < 0.1f) fx = 0;
		if (fabsf(sx) < 0.1f) sx = 0;
		float len = sqrtf(fx * fx + sx * sx);
		if (len > 1.0f) { fx /= len; sx /= len; len = 1.0f; }
		if (phoneTimer > 0) { fx = 0.0f; sx = 0.0f; len = 0.0f; } // standing still while on the phone
		int running = (held & KEY_B) && len > 0.5f;
		float speed = running ? 0.17f : 0.07f;
		float dirx = -sinf(yaw), dirz = -cosf(yaw);
		float rgtx =  cosf(yaw), rgtz = -sinf(yaw);
		float nx = px + (dirx * fx + rgtx * sx) * speed;
		float nz = pz + (dirz * fx + rgtz * sx) * speed;
		if (!blocked(nx, pz)) px = nx;
		if (!blocked(px, nz)) pz = nz;

		// walking / running animation state
		if (len > 0.0f) {
			float mx = dirx * fx + rgtx * sx, mz = dirz * fx + rgtz * sx;
			float dlt = atan2f(-mx, -mz) - face;
			while (dlt >  3.14159f) dlt -= 6.28318f;
			while (dlt < -3.14159f) dlt += 6.28318f;
			face  += dlt * 0.25f;
			phase += speed * 0.6f + 0.06f;
		}
		moveAmt += ((len > 0.0f ? (running ? 2.0f : 1.0f) : 0.0f) - moveAmt) * 0.15f;

		// phone animation: the hack fires once the phone is raised
		if (phoneTimer > 0 && !paused) {
			phoneTimer--;
			if (phoneTimer == 80) {
				if (pending == 1) {
					hackA = (int)floorf((px + 72.0f) / 24.0f + 0.5f);
					hackB = (int)floorf((pz + 72.0f) / 24.0f + 0.5f);
					if (hackA < 0) hackA = 0;
					if (hackA > ROADS - 1) hackA = ROADS - 1;
					if (hackB < 0) hackB = 0;
					if (hackB > ROADS - 1) hackB = ROADS - 1;
					hackTimer = 600;
				} else if (pending == 2) {
					hackFrames = 180;
				}
				pending = 0;
			}
		}

		if (!paused) {
			if (hackFrames > 0) hackFrames--;
			updateCars(sim, title ? 1e6f : px, title ? 1e6f : pz);
			updatePeds(sim);
			sim++;
		}

		C3D_Mtx mv;
		Mtx_Identity(&mv);
		float cyaw = yaw, cpitch = pitch;
		float cx, cy, cz;
		{ // third-person camera orbiting the character's head
			float dist = 6.5f, flat = cosf(pitch);
			while (dist > 1.5f) { // pull in if the camera would end up inside a building
				cx = px + flat * sinf(yaw) * dist;
				cz = pz + flat * cosf(yaw) * dist;
				if (!blocked(cx, cz)) break;
				dist -= 0.5f;
			}
			cx = px + flat * sinf(yaw) * dist;
			cz = pz + flat * cosf(yaw) * dist;
			cy = 1.6f + sinf(pitch) * dist;
			if (cy < 0.4f) cy = 0.4f;
		}
		if (title) { // slow cinematic orbit around the city
			orbit += 0.006f;
			cyaw = orbit; cpitch = 0.55f; cy = 55.0f;
			cx = sinf(orbit) * 95.0f; cz = cosf(orbit) * 95.0f;
		}
		Mtx_RotateX(&mv, cpitch, true);
		Mtx_RotateY(&mv, -cyaw, true);
		Mtx_Translate(&mv, -cx, -cy, -cz, true);

		gCamX = cx; gCamZ = cz;
		gCullR = title ? 400.0f : 140.0f;

		// ---- build this frame's dynamic geometry ----
		drawDynamic();
		if (!title) {
			float phoneE = 0.0f; // 0 = phone away, 1 = phone raised (eased)
			if (phoneTimer > 0) {
				float up = (120 - phoneTimer) / 25.0f, dn = phoneTimer / 25.0f;
				phoneE = up < dn ? up : dn;
				if (phoneE > 1.0f) phoneE = 1.0f;
				phoneE = phoneE * phoneE * (3.0f - 2.0f * phoneE);
			}
			drawPlayer(px, pz, face, phase, moveAmt, phoneE);
		}
		int litStart = vcount;            // emissive: signal heads, head/tail lights
		drawLights(sim, wNight);
		int skyStart = vcount;            // blended: sun / moon / stars, rain
		drawSkyBodies(cx, cy, cz, cyaw);
		drawRain(cx, cz, cyaw);
		int dropStart = vcount;           // lens droplets (pixel coordinates, own pass)
		drawDroplets();
		GSPGPU_FlushDataCache(&vbo[skyStart], (vcount - skyStart) * sizeof(Vertex));

		float t = (hackFrames > 0) ? 0.25f : 1.0f;
		float fogC[3] = { wSky[0], wSky[1], wSky[2] };
		if (hackFrames > 0) { fogC[0] = 0.03f; fogC[1] = 0.03f; fogC[2] = 0.06f; }
		float fogK = 1.0f / (wFogE - wFogS);
		float fogA = gFog ? -fogK : 0.0f, fogB = gFog ? -wFogS * fogK : 0.0f;
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
		drawRange(0, wallStart);                         // ground, roofs, road markings, skyline
		if (gWin) { envTextured(); C3D_TexBind(0, &texDay); }
		drawRange(wallStart, wallEnd - wallStart);       // textured facades
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
			drawRange(wallStart, wallEnd - wallStart);
			envPlain();
			C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
			               GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
		}

		// 4) blended sky and rain: depth-tested but not depth-written, no fog
		C3D_DepthTest(true, GPU_GREATER, GPU_WRITE_COLOR);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uFog, 0.0f, 0.0f, 0.0f, 0.0f);
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, t, t, t, 1.0f);
		drawRange(skyStart, dropStart - skyStart);

		// 5) lens droplets, 2D overlay
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
			drawPause(pauseDim, menuState, sel, weather, frame);
		}
		C2D_Flush();
		C3D_FrameEnd(0);

		frame++;
		if (++fpsFrames >= 30) { // frame-rate readout, refreshed twice a second
			u64 now = osGetTime();
			if (now > fpsT0) gFpsValue = (int)(30000ULL / (now - fpsT0) + (u64)0);
			if (gFpsValue > 60) gFpsValue = 60;
			fpsT0 = now; fpsFrames = 0;
		}
	}

	if (texOK) { C3D_TexDelete(&texDay); C3D_TexDelete(&texNight); }
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
