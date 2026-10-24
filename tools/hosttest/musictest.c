/*
 * Title-music source selection: romfs:/title.pcm, then the SD card, then the built-in theme, and "no sound" without DSP
 * firmware. The game's real initAudio()/openMusicFile()/updateAudio() run; only fopen() is redirected so "romfs:/" and
 * "sdmc:/" point at temp directories (HOST_ROMFS / HOST_SDMC) and the ndsp/thread calls are the host stubs.
 * usage: musictest <romfs|sdmc|none|tiny|nodsp|threadfail|readfail>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static FILE* host_fopen(const char* path, const char* mode);
#define fopen host_fopen
#define main game_main
#include "../../source/main.c"
#undef main
#undef fopen
extern int g_host_ndsp_ok, g_host_thread_ok;
extern u32 g_host_ndsp_wavebufs;

static FILE* host_fopen(const char* path, const char* mode) {
	char buf[1024];
	const char* root = NULL;
	if (!strncmp(path, "romfs:/", 7)) { root = getenv("HOST_ROMFS"); path += 7; }
	else if (!strncmp(path, "sdmc:/", 6)) { root = getenv("HOST_SDMC"); path += 6; }
	else return fopen(path, mode);
	if (!root) return NULL;
	snprintf(buf, sizeof buf, "%s/%s", root, path);
	return fopen(buf, mode);
}

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char** argv) {
	const char* c = argc > 1 ? argv[1] : "none";
	if (!strcmp(c, "nodsp")) g_host_ndsp_ok = 0;
	if (!strcmp(c, "threadfail")) g_host_thread_ok = 0;
	initAudio();
	printf("case %-10s -> gMusicMode %d  sndOK %d  theme buffer %s  thread %s\n", c, gMusicMode, sndOK, themeSnd.data ? "yes" : "no", musicThread ? "yes" : "no");
	if (!strcmp(c, "romfs") || !strcmp(c, "sdmc")) {
		CHECK(gMusicMode == 1, "title.pcm should stream (mode %d)", gMusicMode);
		CHECK(musicThread != NULL, "no music thread");
		CHECK(themeSnd.data == NULL, "theme should not be started");
		musicFail = 1; updateAudio(0, 0, 0.0f, 0.0f);   // the reader thread reports a read error later
		CHECK(gMusicMode == 2 && themeSnd.data, "read failure must fall back to the built-in theme (mode %d)", gMusicMode);
	} else if (!strcmp(c, "none") || !strcmp(c, "tiny") || !strcmp(c, "threadfail")) {
		CHECK(gMusicMode == 2, "built-in theme expected (mode %d)", gMusicMode);
		CHECK(themeSnd.data != NULL, "theme not synthesised");
		CHECK(g_host_ndsp_wavebufs > 0, "theme not queued on a channel");
		musicWant = 1;
		for (int i = 0; i < 300; i++) updateAudio(0, 0, 0.0f, 0.0f);
		CHECK(themeGain > 0.5f, "theme never fades in on the title screen (gain %.2f)", themeGain);
	} else if (!strcmp(c, "nodsp")) {
		CHECK(gMusicMode == 3 && !sndOK, "no DSP firmware: expected silent mode 3 (mode %d)", gMusicMode);
		updateAudio(1, 0, 0.0f, 0.0f);   // must not touch audio state
		playSfx(SFX_PISTOL, 1.0f);       // nor crash
	}
	printf("%s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}
