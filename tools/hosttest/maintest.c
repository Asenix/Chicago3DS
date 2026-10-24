/*
 * Runs the game's real main(): splash, title, front-end menus, loading screen, then play, for a few thousand frames, with
 * scripted button / stick input. Rendering, audio and the GPU are the host stubs (hoststubs.c), so this checks that the
 * frame code runs end to end without crashing (ASAN/UBSAN), that the vertex buffer is never overrun, that nothing becomes
 * NaN, and that the camera stays inside the world. It does NOT measure speed: a PC says nothing about a 3DS frame time.
 *
 * usage: maintest <free|dm|br|tp> [scans]     env MAINTEST_FAILALLOC=<bytes>: make linearAlloc fail past that many bytes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define main game_main
#include "../../source/main.c"
#undef main
extern int g_host_scans, g_host_frames_ended, g_host_console_inits, g_host_linear_fail_over;
extern size_t g_host_linear_now, g_host_linear_peak;
extern void (*g_host_scan_cb)(int), (*g_host_frame_cb)(void);
extern u32 g_host_keys_down, g_host_keys_held;
extern s16 g_host_circle_dx, g_host_circle_dy;
extern u16 g_host_touch_x, g_host_touch_y;

static const char* kMode;
static int kTitleOnly = 0;
static int gTotalScans;
static int maxVcount = 0, maxTitleV = 0, maxPlayV = 0, nanFrames = 0, outOfWorld = 0, overrun = 0, playFrames = 0;
static float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
static int menuKeys[8], nMenuKeys = 0;

static void scanCb(int n) {
	u32 down = 0, held = 0;
	g_host_circle_dx = g_host_circle_dy = 0; g_host_touch_x = g_host_touch_y = 0;
	if (getenv("MAINTEST_FAILALLOC")) { g_host_keys_down = (n == 5) ? KEY_START : 0; g_host_keys_held = 0; return; }   // the memory screen waits for START
	if (n < nMenuKeys * 8) {                       // front-end menus: one key every 8 scans
		if (n % 8 == 0) down = (u32)menuKeys[n / 8];
	} else if (n < 420 || kTitleOnly) {            // loading screen: A when "PRESS A TO DEPLOY" shows (title-only: stay on the title)
		if (kTitleOnly) { if (n == 0) down = KEY_START; }   // START skips the splash
		else if (n % 8 == 0) down = KEY_A;
	} else {                                       // play: walk, turn, run, look, wheel, pause, camera
		int t = n - 420;
		g_host_circle_dy = (t / 300) % 4 == 3 ? 0 : 140;                 // walk forward most of the time
		g_host_circle_dx = (s16)(((t / 90) % 3 - 1) * 60);               // wander left and right
		if ((t / 200) % 2) held |= KEY_B;                                // run half the time
		if ((t / 150) % 5 == 1) { held |= KEY_DLEFT; }
		if ((t / 150) % 5 == 3) { held |= KEY_DRIGHT; }
		if (t % 500 == 250) down |= KEY_Y;                               // cycle camera
		if (t % 700 == 300) { held |= KEY_X; }                           // weapon wheel
		if (t % 900 == 600) down |= KEY_SELECT;                          // pause
		if (t % 900 == 640) down |= KEY_SELECT;                          // resume
		if (t % 400 == 100) down |= KEY_A;                               // interact (doors, shops)
		if (t % 60 < 3 && strcmp(kMode, "free")) held |= KEY_R;          // fire in the multiplayer modes
	}
	g_host_keys_down = down; g_host_keys_held = held | down;
}

static void frameCb(void) {
	if (vcount > maxVcount) maxVcount = vcount;
	if (vcount >= MAX_VERTS) overrun++;   /* vcount stops at MAX_VERTS, so "at the limit" is the overflow signal; gVtxOver counts the refused vertices */
	if (g_host_scans < 420 || kTitleOnly) { if (vcount > maxTitleV) maxTitleV = vcount; return; }
	if (vcount > maxPlayV) maxPlayV = vcount;
	playFrames++;
	if (!isfinite(gCamX) || !isfinite(gCamZ) || !isfinite(gPlX) || !isfinite(gPlZ)) nanFrames++;
	// the player cannot walk past blockedR()'s limit of CITY_H + 20; the third-person camera trails a few metres behind. The Target
	// Practice range is a separate arena far east of the city (x ~ 1000) and is exempt.
	if (gMode != M_TP && (fabsf(gPlX) > CITY_H + 20.5f || fabsf(gPlZ) > CITY_H + 20.5f || fabsf(gCamX) > CITY_H + 40.0f || fabsf(gCamZ) > CITY_H + 40.0f)) outOfWorld++;
	if (gCamX < minX) { minX = gCamX; }
	if (gCamX > maxX) { maxX = gCamX; }
	if (gCamZ < minZ) { minZ = gCamZ; }
	if (gCamZ > maxZ) { maxZ = gCamZ; }
}

int main(int argc, char** argv) {
	kMode = argc > 1 ? argv[1] : "free";
	gTotalScans = argc > 2 ? atoi(argv[2]) : 3000;
	// front-end keys. The first A only skips the splash; then A = press start, and Single Player > Free Roam is A,A. Multiplayer is one
	// DOWN away, and DM / BR / TP are its first three entries. (Without this, stray A pulses would always launch Free Roam.)
	static const int kFree[] = {KEY_A, KEY_A, KEY_A, KEY_A};
	static const int kDm[]   = {KEY_A, KEY_A, KEY_DDOWN, KEY_A, KEY_A};
	static const int kBr[]   = {KEY_A, KEY_A, KEY_DDOWN, KEY_A, KEY_DDOWN, KEY_A};
	static const int kTp[]   = {KEY_A, KEY_A, KEY_DDOWN, KEY_A, KEY_DDOWN, KEY_DDOWN, KEY_A};
	const int* seq; int n;
	if (!strcmp(kMode, "free"))    { seq = kFree; n = 4; }
	else if (!strcmp(kMode, "dm")) { seq = kDm;   n = 5; }
	else if (!strcmp(kMode, "br")) { seq = kBr;   n = 6; }
	else if (!strcmp(kMode, "tp")) { seq = kTp;   n = 7; }
	else if (!strcmp(kMode, "title")) { seq = kFree; n = 0; kTitleOnly = 1; }   // skips the splash, then sits on the title orbit
	else { fprintf(stderr, "unknown mode %s\n", kMode); return 2; }
	memcpy(menuKeys, seq, (size_t)n * sizeof(int)); nMenuKeys = n;
	const char* fa = getenv("MAINTEST_FAILALLOC");
	if (fa) g_host_linear_fail_over = atoi(fa);

	extern int g_host_frames_to_run;
	g_host_frames_to_run = gTotalScans;
	g_host_scan_cb = scanCb; g_host_frame_cb = frameCb;
	int rc = game_main(0, NULL);

	printf("[%s] main() returned %d after %d scans, %d frames built, %d in play\n", kMode, rc, g_host_scans, g_host_frames_ended, playFrames);
	printf("[%s] linear memory peak %.2f MiB (vertex buffer %.2f MiB); static %d verts; largest frame %d of %d (title/menus %d, play %d); dropped by capacity limits: %d\n",
	       kMode, g_host_linear_peak / 1048576.0, sizeof(Vertex) * (double)MAX_VERTS / 1048576.0, staticCount, maxVcount, MAX_VERTS, maxTitleV, maxPlayV, gDropped);
	if (fa) {
		int ok = (rc != 0 && g_host_console_inits == 1 && g_host_frames_ended == 0);
		printf("[%s] linearAlloc failing past %s bytes: %s\n", kMode, fa, ok ? "clean exit with the memory screen" : "FAIL (expected a clean error exit before any frame)");
		return ok ? 0 : 1;
	}
	printf("[%s] final game mode %d (0 free, 1 DM, 2 BR, 3 TP), state %d, player (%.1f, %.1f)\n", kMode, gMode, gState, gPlX, gPlZ);
	printf("[%s] camera range x %.0f..%.0f z %.0f..%.0f (walkable limit +-%.0f; TP is a separate arena)\n", kMode, minX, maxX, minZ, maxZ, (double)(CITY_H + 20.0f));
	int fails = 0;
	if (rc != 0)             { printf("FAIL: main() returned %d\n", rc); fails++; }
	if (kTitleOnly) { if (g_host_frames_ended < 400) { printf("FAIL: the title only ran %d frames\n", g_host_frames_ended); fails++; } }
	else if (playFrames < 500) { printf("FAIL: only %d play frames: the scripted menu did not reach the game\n", playFrames); fails++; }
	if (g_host_linear_peak > 16u * 1048576u) { printf("FAIL: linear memory peak %.2f MiB is over the 16 MiB budget\n", g_host_linear_peak / 1048576.0); fails++; }
	if (overrun || gVtxOver) { printf("FAIL: the vertex buffer filled up: %u vertices were refused (%d frames at the limit)\n", gVtxOver, overrun); fails++; }
	if (nanFrames)           { printf("FAIL: %d frames with NaN/inf camera or player\n", nanFrames); fails++; }
	if (outOfWorld)          { printf("FAIL: player/camera left the playable area in %d frames\n", outOfWorld); fails++; }
	if (gDropped)            { printf("FAIL: %d objects dropped by capacity limits\n", gDropped); fails++; }
	printf("[%s] %s\n", kMode, fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
