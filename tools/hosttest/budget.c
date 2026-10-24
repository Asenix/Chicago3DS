/*
 * Draw-budget scan: for a grid of camera positions and headings across the whole walkable area, applies the game's REAL district
 * visibility rule (computeVis, shared with main()) to the REAL built city and counts what a frame would submit to the GPU:
 * vertices and draw calls for the static city. It says how much geometry is drawn, NOT how fast a 3DS draws it: a PC cannot
 * measure that. Use the in-game FPS readout (pause menu > settings > FPS) on a real console for frame rate.
 *
 * usage: budget [fog 0|1]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define main game_main
#include "../../source/main.c"
#undef main

static int rangesFor(const int* st, const int* cnt, const unsigned char* vis, long* verts) {
	int calls = 0, rs = 0, rc = 0;   // same merging rule as drawCells()
	for (int c = 0; c < CELLS; c++) {
		if (!vis[c] || cnt[c] == 0) continue;
		*verts += cnt[c];
		if (rc > 0 && rs + rc == st[c]) rc += cnt[c];
		else { if (rc > 0) calls++; rs = st[c]; rc = cnt[c]; }
	}
	if (rc > 0) calls++;
	return calls;
}

int main(int argc, char** argv) {
	gFog = argc > 1 ? atoi(argv[1]) : 1;
	vbo = (Vertex*)malloc(sizeof(Vertex) * MAX_VERTS);
	buildCity();
	staticCount = vcount;
	printf("static city: %d vertices (%.1f%% of the %d-vertex buffer), %d districts, ground+skyline %d always drawn\n", staticCount, 100.0 * staticCount / MAX_VERTS, MAX_VERTS, CELLS, globalEnd);
	printf("fog %s: districts beyond %.0f m (fog end %.0f m) are skipped\n", gFog ? "on" : "off", gFog ? wFogE + 57.0f : 440.0f, (double)wFogE);
	long worst = 0, sum = 0, nWorstCalls = 0; int n = 0; float wx = 0, wz = 0, wyaw = 0; int hist[10] = {0}; long over = 0;
	long worstNight = 0;
	unsigned char vis[CELLS];
	for (float cx = -300; cx <= 300; cx += 10) for (float cz = -300; cz <= 300; cz += 10) for (int k = 0; k < 8; k++) {
		float yaw = (float)k * 0.785398f, fx = -sinf(yaw), fz = -cosf(yaw);
		computeVis(cx, cz, fx, fz, 0, vis);
		long a = 0, w = 0;
		int calls = 1 + rangesFor(cellAStart, cellACount, vis, &a) + rangesFor(cellWStart, cellWCount, vis, &w);
		long v = globalEnd + a + w;
		sum += v; n++;
		if (v > worst) { worst = v; wx = cx; wz = cz; wyaw = yaw; nWorstCalls = calls; }
		if (v + w > worstNight) worstNight = v + w;   // at dusk and night the facades are drawn a second time with the lit-window texture
		int b = (int)(v / 10000); hist[b > 9 ? 9 : b]++;
		if (v > staticCount) over++;
	}
	printf("static vertices submitted per frame over %d views: average %ld, worst %ld (at %.0f,%.0f heading %.2f rad, %ld draw calls); night worst %ld\n",
	       n, sum / n, worst, wx, wz, wyaw, nWorstCalls, worstNight);
	for (int i = 0; i < 10; i++) if (hist[i]) printf("  %2d0k-%2d0k: %5d views (%.1f%%)\n", i, i + 1, hist[i], 100.0 * hist[i] / n);
	// the title orbit sees every district from 330 m out and 190 m up
	long tw = 0;
	for (int k = 0; k < 64; k++) {
		float o = (float)k * 6.2831853f / 64.0f, cx = sinf(o) * 330.0f, cz = cosf(o) * 330.0f;
		computeVis(cx, cz, -sinf(o), -cosf(o), 1, vis);
		long a = 0, w = 0; rangesFor(cellAStart, cellACount, vis, &a); rangesFor(cellWStart, cellWCount, vis, &w);
		if (globalEnd + a + w > tw) tw = globalEnd + a + w;
	}
	printf("title orbit worst: %ld static vertices\n", tw);
	int fails = 0;
	if (over) { printf("FAIL: %ld views would draw more than the whole static city\n", over); fails++; }
	if (worst >= staticCount) { printf("FAIL: culling never reduces the frame (worst %ld of %d)\n", worst, staticCount); fails++; }
	printf("%s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
