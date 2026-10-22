#pragma once
#include "3ds.h"
typedef struct { float m[16]; } C3D_Mtx;
typedef struct { int x; } C3D_Tex, C3D_RenderTarget, C3D_AttrInfo, C3D_BufInfo, C3D_TexEnv;
typedef struct { void* vertexShader; } shaderProgram_s;
typedef struct { void* DVLE; } DVLB2;
