/*
 * Host stand-ins for the libctru / citro3d / citro2d functions the game calls. They include the REAL headers, so the
 * compiler checks every signature against the declarations the 3DS build uses. A function the game starts calling that
 * is not defined here is a LINK error (the tests are linked without --unresolved-symbols=ignore-all).
 *
 * The stubs are deliberately dumb: they let the game logic run on a PC. Rendering, audio output, input and timing are
 * not simulated. Test hooks (g_host_*) let the tests choose what the "hardware" does.
 */
#include <3ds.h>
#include <citro3d.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vshader_shbin.h"

const uint8_t vshader_shbin[452] __attribute__((aligned(4))) = {0};
const uint8_t vshader_shbin_end[1] = {0};

/* ---- hooks ---- */
int    g_host_ndsp_ok    = 1;        /* ndspInit succeeds (DSP firmware present) */
int    g_host_thread_ok  = 1;        /* threadCreate succeeds */
size_t g_host_linear_now = 0, g_host_linear_peak = 0;   /* linearAlloc accounting */
int    g_host_linear_fail_over = 0;  /* >0: linearAlloc fails once the total would exceed this many bytes */
u32    g_host_keys_down = 0, g_host_keys_held = 0;
int    g_host_frames_to_run = 0;     /* aptMainLoop returns 1 this many times, then 0 */
u32    g_host_ndsp_wavebufs = 0;
int    g_host_scans = 0;             /* hidScanInput() calls so far */
int    g_host_frames_ended = 0;      /* C3D_FrameEnd() calls so far */
void (*g_host_scan_cb)(int scan) = NULL;   /* called from hidScanInput(): scripts keys / sticks / touch */
void (*g_host_frame_cb)(void) = NULL;      /* called from C3D_FrameEnd(): inspect the frame that was just built */
s16 g_host_circle_dx = 0, g_host_circle_dy = 0;
u16 g_host_touch_x = 0, g_host_touch_y = 0;

/* ---- linear memory (accounted so the memory audit is real; linearFree really frees, so a leak shows up under ASAN) ---- */
void* linearAlloc(size_t size) {
	if (g_host_linear_fail_over > 0 && g_host_linear_now + size > (size_t)g_host_linear_fail_over) return NULL;
	char* raw = (char*)malloc(size + 48);
	if (!raw) return NULL;
	char* p = (char*)(((uintptr_t)raw + 16 + 15) & ~(uintptr_t)15);   /* 16-byte aligned like the real one */
	((char**)p)[-1] = raw;
	((size_t*)p)[-2] = size;
	g_host_linear_now += size;
	if (g_host_linear_now > g_host_linear_peak) g_host_linear_peak = g_host_linear_now;
	return p;
}
void linearFree(void* ptr) {
	if (!ptr) return;
	char* p = (char*)ptr;
	g_host_linear_now -= ((size_t*)p)[-2];
	free(((char**)p)[-1]);
}

/* ---- apt / hid / system ---- */
bool aptMainLoop(void) { if (g_host_frames_to_run > 0) { g_host_frames_to_run--; return true; } return false; }
void hidScanInput(void) { if (g_host_scan_cb) g_host_scan_cb(g_host_scans); g_host_scans++; }
u32  hidKeysDown(void) { return g_host_keys_down; }
u32  hidKeysHeld(void) { return g_host_keys_held; }
void hidCircleRead(circlePosition* pos) { pos->dx = g_host_circle_dx; pos->dy = g_host_circle_dy; }
void hidTouchRead(touchPosition* pos) { pos->px = g_host_touch_x; pos->py = g_host_touch_y; }
void irrstCstickRead(circlePosition* pos) { pos->dx = 0; pos->dy = 0; }
u64  osGetTime(void) { static u64 t = 1700000000000ULL; return t += 16; }
void osSetSpeedupEnable(bool enable) { (void)enable; }
void svcSleepThread(s64 ns) { (void)ns; }
Result svcGetThreadPriority(s32* out, Handle handle) { (void)handle; *out = 0x30; return 0; }
/* Library-internal linear allocations, mirrored from the real library sources so the memory audit counts them:
 *  gfxInitDefault  libctru gfx.c: BGR8 framebuffers in LINEAR memory, double buffered: top 400x240x3 x2, bottom 320x240x3 x2
 *  C3D_Init        citro3d base.c:86: the GPU command buffer
 *  C2D_Init        citro2d base.c:31-36: 4 vertices (32 B each) and 6 u16 indices per object
 *  texture upload  citro3d texture.c:77: linearAlloc(total size of all mip levels) unless the texture is placed in VRAM
 * Render targets (colour + depth) are in VRAM and are not counted here. */
static void* s_lib_fb[2], *s_lib_cmd, *s_lib_vtx, *s_lib_idx;
void gfxInitDefault(void) { s_lib_fb[0] = linearAlloc(2 * 400 * 240 * 3); s_lib_fb[1] = linearAlloc(2 * 320 * 240 * 3); }
void gfxExit(void) { linearFree(s_lib_fb[0]); linearFree(s_lib_fb[1]); s_lib_fb[0] = s_lib_fb[1] = NULL; }
Result romfsMountSelf(const char* name) { (void)name; return 0; }
Result romfsUnmount(const char* name) { (void)name; return 0; }

/* ---- threads ---- */
static int s_fake_thread;
Thread threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int affinity, bool detached) {
	(void)entrypoint; (void)arg; (void)stack_size; (void)prio; (void)affinity; (void)detached;
	return g_host_thread_ok ? (Thread)&s_fake_thread : NULL;
}
Result threadJoin(Thread thread, u64 timeout_ns) { (void)thread; (void)timeout_ns; return 0; }
void   threadFree(Thread thread) { (void)thread; }

/* ---- ndsp / dsp ---- */
Result ndspInit(void) { return g_host_ndsp_ok ? 0 : -1; }
void   ndspExit(void) {}
void   ndspSetOutputMode(ndspOutputMode mode) { (void)mode; }
void   ndspChnReset(int id) { (void)id; }
void   ndspChnSetInterp(int id, ndspInterpType type) { (void)id; (void)type; }
void   ndspChnSetRate(int id, float rate) { (void)id; (void)rate; }
void   ndspChnSetFormat(int id, u16 format) { (void)id; (void)format; }
void   ndspChnSetMix(int id, float mix[12]) { (void)id; (void)mix; }
void   ndspChnWaveBufAdd(int id, ndspWaveBuf* buf) { (void)id; (void)buf; g_host_ndsp_wavebufs++; }
void   ndspChnWaveBufClear(int id) { (void)id; }
Result DSP_FlushDataCache(const void* address, u32 size) { (void)address; (void)size; return 0; }
Result GSPGPU_FlushDataCache(const void* adr, u32 size) { (void)adr; (void)size; return 0; }

/* ---- shader ---- */
static DVLB_s s_dvlb; static DVLE_s s_dvle;
DVLB_s* DVLB_ParseFile(u32* shbinData, u32 shbinSize) { (void)shbinData; (void)shbinSize; s_dvlb.numDVLE = 1; s_dvlb.DVLE = &s_dvle; return &s_dvlb; }
void    DVLB_Free(DVLB_s* dvlb) { (void)dvlb; }
Result  shaderProgramInit(shaderProgram_s* sp) { memset(sp, 0, sizeof *sp); return 0; }
Result  shaderProgramFree(shaderProgram_s* sp) { (void)sp; return 0; }
Result  shaderProgramSetVsh(shaderProgram_s* sp, DVLE_s* dvle) { (void)sp; (void)dvle; return 0; }
s8      shaderInstanceGetUniformLocation(shaderInstance_s* si, const char* name) { (void)si; (void)name; static s8 n; return n++; }

/* ---- citro3d ---- */
C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
bool     C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];
static C3D_RenderTarget s_target; static C3D_TexEnv s_texenv[6];
bool C3D_Init(size_t cmdBufSize) { s_lib_cmd = linearAlloc(cmdBufSize); return s_lib_cmd != NULL; }
void C3D_Fini(void) { linearFree(s_lib_cmd); s_lib_cmd = NULL; }
bool C3D_FrameBegin(u8 flags) { (void)flags; return true; }
void C3D_FrameEnd(u8 flags) { (void)flags; g_host_frames_ended++; if (g_host_frame_cb) g_host_frame_cb(); }
bool C3D_FrameDrawOn(C3D_RenderTarget* target) { (void)target; return true; }
void C3D_FrameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth) { (void)fb; (void)clearBits; (void)clearColor; (void)clearDepth; }
C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt) { (void)width; (void)height; (void)colorFmt; (void)depthFmt; return &s_target; }
void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags) { (void)target; (void)screen; (void)side; (void)transferFlags; }
void C3D_BindProgram(shaderProgram_s* program) { (void)program; }
void C3D_SetAttrInfo(C3D_AttrInfo* info) { (void)info; }
void C3D_SetBufInfo(C3D_BufInfo* info) { (void)info; }
void AttrInfo_Init(C3D_AttrInfo* info) { memset(info, 0, sizeof *info); }
int  AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, GPU_FORMATS format, int count) { (void)info; (void)regId; (void)format; (void)count; return 0; }
void BufInfo_Init(C3D_BufInfo* info) { memset(info, 0, sizeof *info); }
int  BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64 permutation) { (void)info; (void)data; (void)stride; (void)attribCount; (void)permutation; return 0; }
void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size) { (void)primitive; (void)first; (void)size; }
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask) { (void)enable; (void)function; (void)writemask; }
void C3D_CullFace(GPU_CULLMODE mode) { (void)mode; }
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr, GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha) { (void)colorEq; (void)alphaEq; (void)srcClr; (void)dstClr; (void)srcAlpha; (void)dstAlpha; }
C3D_TexEnv* C3D_GetTexEnv(int id) { return &s_texenv[id < 0 ? 0 : (id > 5 ? 5 : id)]; }
bool C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p) {
	(void)cube;
	memset(tex, 0, sizeof *tex);
	if (p.format != GPU_RGBA8) return false;                       /* the only format the game uses; size = 4 bytes per texel */
	u32 size = 4u * (u32)p.width * (u32)p.height;
	if (p.onVram) return true;
	tex->data = linearAlloc(C3D_TexCalcTotalSize(size, p.maxLevel));
	return tex->data != NULL;
}
void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE face, int level) { (void)tex; (void)data; (void)face; (void)level; }
void C3D_TexGenerateMipmap(C3D_Tex* tex, GPU_TEXFACE face) { (void)tex; (void)face; }
void C3D_TexBind(int unitId, C3D_Tex* tex) { (void)unitId; (void)tex; }
void C3D_TexDelete(C3D_Tex* tex) { linearFree(tex->data); tex->data = NULL; }

/* matrix helpers the game calls as real functions (the other Mtx_* are static inline in the header) */
void Mtx_Translate(C3D_Mtx* mtx, float x, float y, float z, bool bRightSide) { (void)bRightSide; mtx->r[0].w += x; mtx->r[1].w += y; mtx->r[2].w += z; }
void Mtx_RotateX(C3D_Mtx* mtx, float angle, bool bRightSide) { (void)mtx; (void)angle; (void)bRightSide; }
void Mtx_RotateY(C3D_Mtx* mtx, float angle, bool bRightSide) { (void)mtx; (void)angle; (void)bRightSide; }
void Mtx_OrthoTilt(C3D_Mtx* mtx, float left, float right, float bottom, float top, float near, float far, bool isLeftHanded) { (void)left; (void)right; (void)bottom; (void)top; (void)near; (void)far; (void)isLeftHanded; Mtx_Identity(mtx); }
void Mtx_PerspTilt(C3D_Mtx* mtx, float fovy, float aspect, float near, float far, bool isLeftHanded) { (void)fovy; (void)aspect; (void)near; (void)far; (void)isLeftHanded; Mtx_Identity(mtx); }

/* ---- citro2d ---- */
static char s_tb[64];
static struct C2D_TextBuf_s* s_textbuf = (struct C2D_TextBuf_s*)s_tb;
bool C2D_Init(size_t maxObjects) { s_lib_vtx = linearAlloc(4 * maxObjects * 32); s_lib_idx = linearAlloc(6 * maxObjects * 2); return s_lib_vtx && s_lib_idx; }
void C2D_Fini(void) { linearFree(s_lib_vtx); linearFree(s_lib_idx); s_lib_vtx = s_lib_idx = NULL; }
void C2D_Prepare(void) {}
void C2D_Flush(void) {}
void C2D_SceneSize(u32 width, u32 height, bool tilt) { (void)width; (void)height; (void)tilt; }
C3D_RenderTarget* C2D_CreateScreenTarget(gfxScreen_t screen, gfx3dSide_t side) { (void)screen; (void)side; return &s_target; }
void C2D_TargetClear(C3D_RenderTarget* target, u32 color) { (void)target; (void)color; }
bool C2D_DrawTriangle(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float x2, float y2, u32 clr2, float depth) { (void)x0; (void)y0; (void)clr0; (void)x1; (void)y1; (void)clr1; (void)x2; (void)y2; (void)clr2; (void)depth; return true; }
bool C2D_DrawLine(float x0, float y0, u32 clr0, float x1, float y1, u32 clr1, float thickness, float depth) { (void)x0; (void)y0; (void)clr0; (void)x1; (void)y1; (void)clr1; (void)thickness; (void)depth; return true; }
bool C2D_DrawRectangle(float x, float y, float z, float w, float h, u32 clr0, u32 clr1, u32 clr2, u32 clr3) { (void)x; (void)y; (void)z; (void)w; (void)h; (void)clr0; (void)clr1; (void)clr2; (void)clr3; return true; }
bool C2D_DrawEllipse(float x, float y, float z, float w, float h, u32 clr0, u32 clr1, u32 clr2, u32 clr3) { (void)x; (void)y; (void)z; (void)w; (void)h; (void)clr0; (void)clr1; (void)clr2; (void)clr3; return true; }
C2D_TextBuf C2D_TextBufNew(size_t maxGlyphs) { (void)maxGlyphs; return s_textbuf; }
void C2D_TextBufDelete(C2D_TextBuf buf) { (void)buf; }
void C2D_TextBufClear(C2D_TextBuf buf) { (void)buf; }
const char* C2D_TextParse(C2D_Text* text, C2D_TextBuf buf, const char* str) {
	memset(text, 0, sizeof *text);
	text->buf = buf; text->width = 6.0f * (float)strlen(str); text->lines = 1; text->words = 1;
	return str + strlen(str);
}
void C2D_TextOptimize(const C2D_Text* text) { (void)text; }
void C2D_TextGetDimensions(const C2D_Text* text, float scaleX, float scaleY, float* outWidth, float* outHeight) {
	if (outWidth) *outWidth = text->width * scaleX;
	if (outHeight) *outHeight = 30.0f * scaleY;
}
void C2D_DrawText(const C2D_Text* text, u32 flags, float x, float y, float z, float scaleX, float scaleY, ...) { (void)text; (void)flags; (void)x; (void)y; (void)z; (void)scaleX; (void)scaleY; }

/* ---- console / vblank (used by the "not enough memory" screen) ---- */
static PrintConsole s_console;
int g_host_console_inits = 0;
PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console) { (void)screen; g_host_console_inits++; return console ? console : &s_console; }
void gspWaitForEvent(GSPGPU_Event id, bool nextEvent) { (void)id; (void)nextEvent; }
