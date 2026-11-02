/* SPDX-License-Identifier: AGPL-3.0-only */
/* Minimal stand-in for SDL3 so database drivers can be built headless for tests. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef uint64_t Uint64; typedef uint32_t Uint32; typedef uint8_t Uint8; typedef int32_t Sint32;
typedef int SDL_Keycode; typedef unsigned short SDL_Keymod;
typedef struct SDL_Window SDL_Window; typedef struct SDL_Renderer SDL_Renderer; typedef struct SDL_Cursor SDL_Cursor;
typedef union SDL_Event SDL_Event; typedef Uint32 SDL_WindowID;
static inline char *SDL_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static inline void SDL_free(void *p) { free(p); }
static inline const char *SDL_getenv(const char *k) { return getenv(k); }
static inline Uint64 SDL_GetPerformanceCounter(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (Uint64)ts.tv_sec * 1000000000ull + ts.tv_nsec; }
static inline Uint64 SDL_GetPerformanceFrequency(void) { return 1000000000ull; }
#define SDL_Log(...) fprintf(stderr, __VA_ARGS__)
