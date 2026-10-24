/* Host-only shim: the real libctru headers include newlib's <sys/lock.h>, which glibc does not have. */
#pragma once
typedef int _LOCK_T;
typedef struct { int a, b, c; } _LOCK_RECURSIVE_T;
