/*
 * furi_stub.h — Minimal Furi OS stubs for host-side unit testing.
 * Replaces Flipper-specific types/macros so protocol + expansion code
 * compiles and runs on any x86 Linux/macOS host with no hardware.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#define _POSIX_C_SOURCE 199309L
#include <time.h>

/* Logging — goes to stdout during tests */
#define FURI_LOG_T(tag, fmt, ...) printf("[T][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define FURI_LOG_D(tag, fmt, ...) printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define FURI_LOG_I(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define FURI_LOG_W(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define FURI_LOG_E(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)

/* Assertions */
#define furi_assert(x)      assert(x)
#define furi_check(x)       assert(x)
#define UNUSED(x)           (void)(x)

/* Memory */
#define furi_alloc(size)    calloc(1, size)

/* Timestamp (ms) */
static inline uint64_t furi_get_tick(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Minimal FuriString stub (protocol code uses C strings directly) */
typedef struct { char* buf; size_t len; } FuriString;
static inline FuriString* furi_string_alloc(void) {
    FuriString* s = calloc(1, sizeof(FuriString)); return s;
}
static inline void furi_string_free(FuriString* s) { free(s->buf); free(s); }
static inline const char* furi_string_get_cstr(const FuriString* s) { return s->buf ? s->buf : ""; }
