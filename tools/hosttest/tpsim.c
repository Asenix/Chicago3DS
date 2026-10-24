/*
 * Target Practice end to end on the real game logic: the "player" fires its three shots near the bull on its turn, the three
 * computer shooters take theirs, five rounds run, and the match must finish with sane scores, a place, and no stuck state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define main game_main
#include "../../source/main.c"
#undef main

int main(void) {
	vbo = (Vertex*)malloc(sizeof(Vertex) * MAX_VERTS); buildCity(); staticCount = vcount; initNav(); initCars(); initPeds(); initCrates();
	setupMode(M_TP);
	int f = 0, cool = 0, shots = 0;
	while (f < 60000 && gState != GS_OVER) {
		plHp = 1e9f;
		if (gState == GS_PLAY && tpTurn == TP_PL && tpPhase == 1 && tpShotsLeft > 0 && cool-- <= 0) {
			float cam[3] = {gPlX, 1.62f, gPlZ}, fwd[3] = {0.002f * rnd(), (0.38f + 0.1f * rnd()) / 17.0f, -1.0f}, muz[3] = {gPlX, 1.4f, gPlZ - 0.9f};
			float l = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + 1.0f);
			for (int i = 0; i < 3; i++) fwd[i] /= l;
			tpPlayerShot(cam, fwd, muz); cool = 20; shots++;
		}
		updateMode(gPlX, gPlZ, 0); updateFx(); f++;
	}
	printf("match over after %d frames (%.0f s): scores %d %d %d %d, place %d, won %d, draw %d, player shots %d\n", f, f / 60.0, tpScore[0], tpScore[1], tpScore[2], tpScore[3], gPlace, gWon, gDraw, shots);
	int fails = 0;
	if (gState != GS_OVER) { printf("FAIL: the match never finished (state %d, round %d, turn %d, phase %d)\n", gState, tpRound, tpTurn, tpPhase); fails++; }
	for (int i = 0; i < 4; i++) if (tpScore[i] < 0 || tpScore[i] > 10 * 3 * TP_ROUNDS) { printf("FAIL: score %d for lane %d out of range\n", tpScore[i], i); fails++; }
	if (gPlace < 1 || gPlace > 4) { printf("FAIL: place %d\n", gPlace); fails++; }
	if (shots != 3 * TP_ROUNDS) { printf("FAIL: expected %d player shots, took %d\n", 3 * TP_ROUNDS, shots); fails++; }
	// and the geometry the range draws must fit the buffer
	gCamX = gPlX; gCamZ = gPlZ; gCullR = 140.0f; gCone = 0;
	vcount = staticCount; drawDynamic(); drawTpRoom(10); drawLoot(10); drawStorm();
	printf("range dynamic geometry: %d vertices (room for %d)\n", vcount - staticCount, MAX_VERTS - staticCount);
	if (gVtxOver) { printf("FAIL: range overran the buffer (%u vertices refused)\n", gVtxOver); fails++; }
	printf("%s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
