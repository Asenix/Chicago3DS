#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result;
#define R_FAILED(r) ((r) < 0)
typedef void* Thread;
typedef struct { s16 dx, dy; } circlePosition;
typedef struct { u16 px, py; } touchPosition;
typedef struct { void* data_vaddr; u32 nsamples; bool looping; int status; } ndspWaveBuf;
enum { NDSP_WBUF_DONE = 3 };
enum { NDSP_OUTPUT_STEREO, NDSP_INTERP_LINEAR, NDSP_FORMAT_STEREO_PCM16, NDSP_FORMAT_MONO_PCM16 };
#define KEY_A 1
#define KEY_B 2
#define KEY_SELECT 4
#define KEY_START 8
#define KEY_DUP 16
#define KEY_DDOWN 32
#define KEY_DLEFT 64
#define KEY_DRIGHT 128
#define KEY_X 256
#define KEY_Y 512
#define KEY_L 1024
#define KEY_R 2048
#define KEY_ZL 4096
#define KEY_ZR 8192
#define KEY_TOUCH 16384
#define CUR_THREAD_HANDLE 0
#define U64_MAX 0xFFFFFFFFFFFFFFFFULL
Result ndspInit(void); void ndspExit(void);
void ndspSetOutputMode(int); void ndspChnReset(int); void ndspChnSetInterp(int,int);
void ndspChnSetRate(int,float); void ndspChnSetFormat(int,int); void ndspChnSetMix(int,float*);
void ndspChnWaveBufClear(int); void ndspChnWaveBufAdd(int, ndspWaveBuf*);
#define DSP_FlushDataCache(a,b) ((void)0)
#define GSPGPU_FlushDataCache(a,b) ((void)0)
void* linearAlloc(size_t); void linearFree(void*);
Thread threadCreate(void(*)(void*), void*, size_t, s32, s32, bool);
void threadJoin(Thread, u64); void threadFree(Thread);
void svcSleepThread(s64); Result svcGetThreadPriority(s32*, int);
typedef struct { void* DVLE; } DVLB_s;
#include "consts.h"
u64 osGetTime(void);
