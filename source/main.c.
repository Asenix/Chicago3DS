// chicago3ds - low-poly open-city walking demo for Nintendo 3DS (original assets)
// Top screen: 3D city. Bottom screen: live street map that tracks the player.
// Controls: Circle Pad = move, hold B = run, C-Stick / L / R = turn, D-Pad Up/Down = look,
// touch the HACK LIGHTS / BLACKOUT buttons to pull out the phone.

#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "vshader_shbin.h"

#define CLEAR_COLOR 0x68B0D8FF

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define GRID      8
#define PITCH     24.0f
#define BLDG_W    16.0f
#define MAX_VERTS 12000
#define RAIN_N    140
#define N_DROPS   11
#define N_CARS    20
#define ROADS     7
#define PLAYER_R  0.6f
#define EYE_H     1.7f

typedef struct { float pos[3]; float clr[4]; } Vertex;
typedef struct { float x0, z0, x1, z1, h; } Building;

static Vertex*  vbo;
static int      vcount = 0;
static Building bldg[GRID * GRID];

static unsigned hash2(int a, int b) {
	unsigned h = (unsigned)a * 374761393u + (unsigned)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

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

static void buildCity(void) {
	float ext = GRID * PITCH * 0.5f;
	// asphalt ground
	float g0[3] = {-ext - 40, 0, -ext - 40}, g1[3] = {ext + 40, 0, -ext - 40};
	float g2[3] = {ext + 40, 0, ext + 40},   g3[3] = {-ext - 40, 0, ext + 40};
	quad(g0, g3, g2, g1, 0.16f, 0.17f, 0.19f);

	for (int i = 0; i < GRID; i++) {
		for (int j = 0; j < GRID; j++) {
			float cx = (i - (GRID - 1) * 0.5f) * PITCH;
			float cz = (j - (GRID - 1) * 0.5f) * PITCH;
			unsigned h = hash2(i, j);
			Building* b = &bldg[i * GRID + j];
			b->x0 = cx - BLDG_W * 0.5f; b->x1 = cx + BLDG_W * 0.5f;
			b->z0 = cz - BLDG_W * 0.5f; b->z1 = cz + BLDG_W * 0.5f;
			b->h  = 6.0f + (float)(h % 40);

			// sidewalk pad
			float s0[3] = {b->x0 - 1.5f, 0.03f, b->z0 - 1.5f}, s1[3] = {b->x1 + 1.5f, 0.03f, b->z0 - 1.5f};
			float s2[3] = {b->x1 + 1.5f, 0.03f, b->z1 + 1.5f}, s3[3] = {b->x0 - 1.5f, 0.03f, b->z1 + 1.5f};
			quad(s0, s3, s2, s1, 0.45f, 0.45f, 0.47f);

			float tone = 0.45f + (float)((h >> 8) % 30) / 100.0f;
			float tint = (float)((h >> 16) % 3);
			float r = tone * (tint == 0 ? 1.00f : 0.85f);
			float g = tone * (tint == 1 ? 1.00f : 0.90f);
			float bl = tone * (tint == 2 ? 1.05f : 0.95f);
			addBox(b->x0, b->z0, b->x1, b->z1, b->h, r, g, bl);
		}
	}
}

// ---- traffic -------------------------------------------------------------
// Roads run along x = ROAD_C(k) and z = ROAD_C(k), k = 0..6. Intersections are (a, b).
#define ROAD_C(k) (-72.0f + 24.0f * (float)(k))

typedef struct { int axis, road, dir; float s, speed, r, g, b; } Car;
static Car cars[N_CARS];
static int staticCount = 0;
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

		if (!stop) c->s += c->dir * c->speed;
		if (c->s >  100.0f) c->s = -100.0f;
		if (c->s < -100.0f) c->s =  100.0f;
	}
}

// Rebuilds the dynamic tail of the vertex buffer: cars and traffic lights.
static void drawDynamic(int frame) {
	vcount = staticCount;
	for (int i = 0; i < N_CARS; i++) {
		const Car* c = &cars[i];
		float lane = ROAD_C(c->road) + c->dir * 1.3f;
		float hx = (c->axis == 0) ? 2.1f : 1.0f, hz = (c->axis == 0) ? 1.0f : 2.1f;
		float cx = (c->axis == 0) ? c->s : lane, cz = (c->axis == 0) ? lane : c->s;
		addBox(cx - hx, cz - hz, cx + hx, cz + hz, 1.1f, c->r, c->g, c->b);
		addBox(cx - hx * 0.55f, cz - hz * 0.55f, cx + hx * 0.55f, cz + hz * 0.55f, 1.7f,
		       c->r * 0.8f, c->g * 0.8f, c->b * 0.8f);
	}
	GSPGPU_FlushDataCache(&vbo[staticCount], (vcount - staticCount) * sizeof(Vertex));
}

// Emissive geometry (traffic lights, headlights): drawn with its own tint so it stays bright at night.
static void drawLights(int frame, float night) {
	int start = vcount;
	if (night > 0.3f) { // headlights
		for (int i = 0; i < N_CARS; i++) {
			const Car* c = &cars[i];
			float lane = ROAD_C(c->road) + c->dir * 1.3f;
			float wx = (c->axis == 0) ? c->s : lane, wz = (c->axis == 0) ? lane : c->s;
			float fx = (c->axis == 0) ? c->dir * 2.2f : 0.0f, fz = (c->axis == 0) ? 0.0f : c->dir * 2.2f;
			float ox = (c->axis == 0) ? 0.0f : 0.6f, oz = (c->axis == 0) ? 0.6f : 0.0f;
			for (int s = -1; s <= 1; s += 2) {
				float hx = wx + fx + s * ox, hz = wz + fz + s * oz;
				addBoxY(hx - 0.18f, hz - 0.18f, hx + 0.18f, hz + 0.18f, 0.4f, 0.75f, 1.0f, 0.95f, 0.7f);
			}
		}
	}
	for (int a = 0; a < ROADS; a++) {
		for (int b = 0; b < ROADS; b++) {
			float x = ROAD_C(a) + 3.4f, z = ROAD_C(b) + 3.4f;
			int hacked = (hackTimer > 0 && a == hackA && b == hackB);
			addBox(x - 0.15f, z - 0.15f, x + 0.15f, z + 0.15f, 4.0f, 0.2f, 0.2f, 0.22f);
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

static int blocked(float x, float z) {
	for (int i = 0; i < GRID * GRID; i++) {
		const Building* b = &bldg[i];
		if (x > b->x0 - PLAYER_R && x < b->x1 + PLAYER_R &&
		    z > b->z0 - PLAYER_R && z < b->z1 + PLAYER_R) return 1;
	}
	float lim = GRID * PITCH * 0.5f + 20.0f;
	return (x < -lim || x > lim || z < -lim || z > lim);
}

// ---- player character ------------------------------------------------------
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

// ---- weather ----------------------------------------------------------------
// Cheap by design: sky colour + world tint changes, ~140 rain streaks, ~11 lens droplets.
enum { W_SUNNY = 0, W_RAINY = 1, W_NIGHT = 2 };
static int   weather = W_SUNNY;
static float wSky[3]  = {0.41f, 0.69f, 0.85f};
static float wTint[3] = {1.0f, 1.0f, 1.0f};
static float wRain = 0.0f, wNight = 0.0f, wTime = 0.0f;

static void updateWeather(void) { // eases toward the chosen weather (about one second)
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

static void drawPlayer(float px, float pz, float face, float phase, float moveAmt, float phoneE) {
	int start = vcount;
	float walk = moveAmt < 1.0f ? moveAmt : 1.0f;
	float run  = moveAmt > 1.0f ? moveAmt - 1.0f : 0.0f;
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

static C2D_TextBuf gStatic, gDyn;
static C2D_Text vLabel[ROADS], hLabel[ROADS], btnHack, btnBlackout;
static C2D_Text txtMenu[6], txtActive, txtPaused, txtWeatherT, txtSub, txtFooter, txtHint;

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

	// pause menu
	static const char* menuNames[6] = { "RESUME", "WEATHER", "SUNNY", "RAINY", "NIGHT", "BACK" };
	for (int i = 0; i < 6; i++) {
		C2D_TextParse(&txtMenu[i], gStatic, menuNames[i]);
		C2D_TextOptimize(&txtMenu[i]);
	}
	C2D_TextParse(&txtActive,   gStatic, "ACTIVE");
	C2D_TextParse(&txtPaused,   gStatic, "P A U S E D");
	C2D_TextParse(&txtWeatherT, gStatic, "W E A T H E R");
	C2D_TextParse(&txtSub,      gStatic, "// CHICAGO GRID NET");
	C2D_TextParse(&txtFooter,   gStatic, "B: RESUME     A: SELECT     D-PAD: MOVE");
	C2D_TextParse(&txtHint,     gStatic, "START: PAUSE");
	C2D_TextOptimize(&txtActive);
	C2D_TextOptimize(&txtPaused);
	C2D_TextOptimize(&txtWeatherT);
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

static void drawMap(float px, float pz, int frame, int phoneTimer) {
	const u32 cBg    = C2D_Color32(0x12, 0x15, 0x1d, 0xFF);
	const u32 cRoad  = C2D_Color32(0x4a, 0x51, 0x66, 0xFF);
	const u32 cMid   = C2D_Color32(0x6b, 0x73, 0x8c, 0xFF);
	const u32 cBlock = C2D_Color32(0x1c, 0x21, 0x2e, 0xFF);
	const u32 cPill  = C2D_Color32(0x00, 0x00, 0x00, 0xB4);
	const u32 cText  = C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF);
	const u32 cPink  = C2D_Color32(0xE0, 0x30, 0xE0, 0xFF);

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

	// the player: blue circle with a white outline, always at the map centre
	C2D_DrawCircleSolid(MAP_CX, MAP_CY, 0.5f, 8.5f, C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF));
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
	C2D_Text t1, t2, t3, t4;
	C2D_TextParse(&t1, gDyn, "CHICAGO 3DS");
	C2D_TextParse(&t2, gDyn, "a low-poly hacking sandbox");
	C2D_TextParse(&t3, gDyn, "PRESS A or TAP TO START");
	C2D_TextParse(&t4, gDyn, "START = quit");
	C2D_TextOptimize(&t1); C2D_TextOptimize(&t2); C2D_TextOptimize(&t3); C2D_TextOptimize(&t4);
	drawCentered(&t1, 160.0f, 66.0f,  1.2f, C2D_Color32(0x4a, 0x9d, 0xff, 0xFF));
	drawCentered(&t2, 160.0f, 118.0f, 0.5f, C2D_Color32(0x9a, 0xa3, 0xb8, 0xFF));
	if ((frame / 30) % 2 == 0)
		drawCentered(&t3, 160.0f, 168.0f, 0.6f, C2D_Color32(0xF0, 0xF2, 0xF8, 0xFF));
	drawCentered(&t4, 160.0f, 214.0f, 0.4f, C2D_Color32(0x6b, 0x73, 0x8c, 0xFF));
}

// ---- pause menu (bottom screen only) ------------------------------------------
// Main menu: RESUME, WEATHER. Weather menu: SUNNY, RAINY, NIGHT, BACK. B always resumes.
static void menuRect(int state, int i, float* y0, float* h) {
	if (state == 0) { *y0 = 88.0f + (float)i * 46.0f; *h = 38.0f; }
	else            { *y0 = 62.0f + (float)i * 36.0f; *h = 30.0f; }
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

	drawCentered(state == 0 ? &txtPaused : &txtWeatherT, 160.0f, 20.0f, 0.8f, neon(0, 229, 255, dim));
	C2D_DrawRectSolid(50.0f, 52.0f, 0.5f, 220.0f, 1.0f, neon(0, 229, 255, 0.6f * dim));
	C2D_DrawRectSolid(50.0f, 51.0f, 0.5f, 44.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));
	if (state == 0) drawCentered(&txtSub, 160.0f, 60.0f, 0.36f, neon(255, 46, 209, 0.9f * dim));

	float pulse = 0.5f + 0.5f * sinf((float)frame * 0.12f);
	int n = (state == 0) ? 2 : 4;
	for (int i = 0; i < n; i++) {
		float y0, h, x0 = 50.0f, w = 220.0f;
		menuRect(state, i, &y0, &h);
		int on = (i == sel);
		C2D_DrawRectSolid(x0, y0, 0.5f, w, h, on ? neon(0, 229, 255, (0.18f + 0.07f * pulse) * dim)
		                                         : neon(8, 24, 40, 0.80f * dim));
		frameRect(x0, y0, w, h, 1.0f, on ? neon(0, 229, 255, dim) : neon(0, 150, 180, 0.6f * dim));
		C2D_DrawRectSolid(x0, y0, 0.5f, on ? 6.0f : 2.0f, h, on ? neon(255, 46, 209, dim) : neon(0, 229, 255, 0.7f * dim));
		if (on) C2D_DrawRectSolid(x0 + w - 18.0f, y0 + h - 3.0f, 0.5f, 18.0f, 3.0f, neon(255, 46, 209, 0.9f * dim));

		const C2D_Text* t = &txtMenu[(state == 0 ? 0 : 2) + i];
		float tw, th;
		C2D_TextGetDimensions(t, 0.6f, 0.6f, &tw, &th);
		C2D_DrawText(t, C2D_WithColor, x0 + 20.0f, y0 + (h - th) * 0.5f, 0.5f, 0.6f, 0.6f,
		             on ? neon(255, 255, 255, dim) : neon(120, 220, 235, 0.9f * dim));

		if (state == 1 && i < 3 && i == active) { // current weather tag
			float aw, ah;
			C2D_TextGetDimensions(&txtActive, 0.38f, 0.38f, &aw, &ah);
			C2D_DrawText(&txtActive, C2D_WithColor, x0 + w - aw - 12.0f, y0 + (h - ah) * 0.5f, 0.5f, 0.38f, 0.38f,
			             neon(255, 46, 209, dim));
		}
	}
	drawCentered(&txtFooter, 160.0f, 213.0f, 0.34f, neon(120, 220, 235, 0.85f * dim));
}

int main(int argc, char** argv) {
	gfxInitDefault();
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
	staticCount = vcount;
	GSPGPU_FlushDataCache(vbo, staticCount * sizeof(Vertex));
	initCars();

	C3D_BufInfo buf;
	BufInfo_Init(&buf);
	BufInfo_Add(&buf, vbo, sizeof(Vertex), 2, 0x10);

	C3D_Mtx proj;
	Mtx_PerspTilt(&proj, C3D_AngleFromDegrees(65.0f), C3D_AspectRatioTop, 0.2f, 400.0f, false);

	// start on a road intersection
	float px = 0.0f, pz = 0.0f, yaw = 0.0f, pitch = 0.3f;
	int   hackFrames = 0, frame = 0;
	int   title = 1, titleDraw = 1;
	float orbit = 0.0f;
	float face = 0.0f, phase = 0.0f, moveAmt = 0.0f; // character heading, step cycle, 0 idle / 1 walk / 2 run
	int   phoneTimer = 0, pending = 0;               // phone animation frames left, queued hack (1 lights, 2 blackout)
	int   lastTouchX = -1, lastTouchY = -1;

	while (aptMainLoop()) {
		hidScanInput();
		u32 down = hidKeysDown(), held = hidKeysHeld();
		if (down & KEY_START) break;

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
		} else if (down & KEY_SELECT) {
			title = 1; titleDraw = 1;
		}

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
		if (fabsf(fx) < 0.1f) fx = 0; if (fabsf(sx) < 0.1f) sx = 0;
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
		if (phoneTimer > 0) {
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

		if (hackFrames > 0) hackFrames--;
		updateCars(frame, title ? 1e6f : px, title ? 1e6f : pz);

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

		drawDynamic(frame);
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

		float t = (hackFrames > 0) ? 0.25f : 1.0f;

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C3D_RenderTargetClear(top, C3D_CLEAR_ALL, hackFrames > 0 ? 0x080810FF : CLEAR_COLOR, 0);
		C3D_FrameDrawOn(top);
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
		C3D_FVUnifSet(GPU_VERTEX_SHADER, uTint, t, t, t, 1.0f);
		C3D_DrawArrays(GPU_TRIANGLES, 0, vcount);

		// bottom screen: street map (or the title card)
		C2D_Prepare();
		C2D_TextBufClear(gDyn);
		C2D_TargetClear(bottom, C2D_Color32(0x12, 0x15, 0x1d, 0xFF));
		C2D_SceneBegin(bottom);
		if (title) drawTitleBottom(frame);
		else       drawMap(px, pz, frame, phoneTimer);
		C2D_Flush();
		C3D_FrameEnd(0);

		frame++;
	}

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
