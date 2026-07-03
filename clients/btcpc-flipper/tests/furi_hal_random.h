/* furi_hal_random.h — stub for host-side tests */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Replace Flipper TRNG with /dev/urandom for host testing */
#include <stdio.h>
static inline void furi_hal_random_fill_buf(uint8_t* buf, size_t len) {
    FILE* f = fopen("/dev/urandom", "rb");
    if(f) { (void)!fread(buf, 1, len, f); fclose(f); }
}

static inline uint32_t furi_hal_random_get(void) {
    uint32_t v = 0;
    furi_hal_random_fill_buf((uint8_t*)&v, sizeof(v));
    return v;
}
