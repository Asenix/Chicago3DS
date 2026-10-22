#define main game_main
#include "../../source/main.c"
#undef main
#include <stdlib.h>
// usage: driver out.bin mode camx camy camz yaw pitch [lineup]
int main(int argc, char** argv) {
	vbo = (Vertex*)malloc(sizeof(Vertex) * MAX_VERTS);
	buildCity();
	staticCount = vcount;
	printf("static vertices: %d of %d (dynamic room %d)\n", staticCount, MAX_VERTS, MAX_VERTS - staticCount);
	printf("buildings/trees/fountains: %d of %d, ground patches: %d of %d\n", nBld, MAX_BLD, nPatch, MAX_PATCH);
	int nb = 0, nt = 0, nf = 0, ns = 0, cnt[16] = {0};
	for (int i = 0; i < nBld; i++) { if (bldg[i].kind == 0) nb++; else if (bldg[i].kind == 1) nt++; else if (bldg[i].kind == 2) nf++; else ns++; }
	for (int i = 0; i < GRID; i++) for (int j = 0; j < GRID; j++) cnt[blkType[i][j]]++;
	printf("buildings %d trees %d fountains %d solids %d | blocks: tower %d low %d mid %d park %d plaza %d houses %d mixed %d comm %d indus %d dock %d lot %d court %d rail %d fwy %d\n", nb, nt, nf, ns, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[BT_HOUSES], cnt[BT_MIXED], cnt[BT_COMM], cnt[BT_INDUS], cnt[BT_DOCK], cnt[BT_LOT], cnt[BT_COURT], cnt[BT_RAIL], cnt[BT_FWY]);
	printf("features %d of %d, doors %d, lamps %d, dropped %d, bucket overflow %d\n", nFeat, MAX_FEAT, nDoor, nLamp, gDropped, bkOverflow);
	for (int r = 0; r < RM_N; r++) { int c = 0; for (int d = 0; d < nDoor; d++) c += doors[d].room == r; printf("  doors to %s: %d\n", roomName[r], c); }
	int mx = 0; for (int c = 0; c < CELLS; c++) { int v = cellACount[c] + cellWCount[c]; if (v > mx) mx = v; }
	printf("largest district: %d vertices; global (ground+skyline) %d\n", mx, globalEnd);
	float hmax = 0; for (int i = 0; i < nBld; i++) if (bldg[i].kind == 0 && bldg[i].h > hmax) hmax = bldg[i].h;
	printf("tallest building %.1f m\n", hmax);
	// overlap check: building footprints vs each other and trees
	int overlaps = 0;
	for (int i = 0; i < nBld; i++) for (int j = i + 1; j < nBld; j++) {
		const Building *a = &bldg[i], *b = &bldg[j];
		if (a->x0 < b->x1 - 0.01f && a->x1 > b->x0 + 0.01f && a->z0 < b->z1 - 0.01f && a->z1 > b->z0 + 0.01f) {
			overlaps++; if (overlaps < 8) printf("  overlap: #%d kind%d (%.1f,%.1f) vs #%d kind%d (%.1f,%.1f)\n", i, a->kind, a->x0, a->z0, j, b->kind, b->x0, b->z0);
		}
	}
	printf("footprint overlaps: %d\n", overlaps);
	// anything blocking the road / sidewalk? (building footprint must stay inside its lot)
	int outside = 0;
	for (int i = 0; i < nBld; i++) {
		const Building* a = &bldg[i];
		float cx = (a->x0 + a->x1) * 0.5f, cz = (a->z0 + a->z1) * 0.5f;
		float bx = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * floorf((cx + CITY_H) / ROAD_SP);
		float bz = -CITY_H + ROAD_SP * 0.5f + ROAD_SP * floorf((cz + CITY_H) / ROAD_SP);
		if (a->x0 < bx - LOT_H - 0.01f || a->x1 > bx + LOT_H + 0.01f || a->z0 < bz - LOT_H - 0.01f || a->z1 > bz + LOT_H + 0.01f) { outside++; if (outside < 6) printf("  outside lot: #%d kind%d [%.1f %.1f %.1f %.1f] block (%.0f,%.0f)\n", i, a->kind, a->x0, a->z0, a->x1, a->z1, bx, bz); }
	}
	printf("footprints outside their lot: %d\n", outside);
	if (argc < 8) return 0;
	initCars(); initPeds(); initCrates();
	if (argc > 8) { // lineup: every pedestrian in a row facing the camera
		for (int i = 0; i < N_PEDS; i++) {
			Ped* p = &peds[i];
			p->axis = 1; p->road = 3; p->side = 1; p->dir = 1; p->walk = 1; p->panic = 0; p->dead = 0;
			p->s = 0.0f; p->off = (-9.5f + 1.0f * (float)i) - SW_OFF; p->ph = (float)i * 0.9f;
		}
		for (int i = 0; i < N_CARS; i++) cars[i].s = 400.0f;
	}
	gCamX = atof(argv[3]); gCamZ = atof(argv[5]); gCullR = 500.0f; gCone = 0;
	drawDynamic();
	FILE* f = fopen(argv[1], "wb");
	fwrite(vbo, sizeof(Vertex), vcount, f);
	fclose(f);
	printf("dumped %d vertices (%d dynamic)\n", vcount, vcount - staticCount);
	return 0;
}
