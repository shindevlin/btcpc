/*
 * test_expansion.c — Host-side unit tests for the expansion frame protocol.
 *
 * Tests the frame chunking/reassembly logic and state machine transitions
 * without any UART hardware. Uses a byte-pipe to simulate the serial link.
 */

#include "furi_stub.h"
#include "../protocol/btcpc_protocol.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0;
static int tests_pass = 0;

#define TEST(name)  static void test_##name(void)
#define RUN(name)  do { \
    tests_run++; printf("  %-50s", #name); \
    test_##name(); tests_pass++; printf("PASS\n"); \
} while(0)
#define ASSERT_EQ(a,b) do { \
    if((a)!=(b)){printf("FAIL\n    %lld != %lld at line %d\n",(long long)(a),(long long)(b),__LINE__);exit(1);} \
} while(0)
#define ASSERT_TRUE(x) ASSERT_EQ(!!(x), 1)

/* ── Expansion frame constants (mirrors expansion_protocol.h) ────────────── */
#define EXP_FRAME_HEARTBEAT  0x01
#define EXP_FRAME_STATUS     0x02
#define EXP_FRAME_BAUD_RATE  0x03
#define EXP_FRAME_DATA       0x05
#define EXP_MAX_DATA         64
#define EXP_ERROR_NONE       0x00

/* ── Pipe-based byte transport ───────────────────────────────────────────── */

typedef struct {
    uint8_t buf[65536];
    size_t  read_pos;
    size_t  write_pos;
} BytePipe;

static void pipe_write(BytePipe* p, const uint8_t* data, size_t len) {
    assert(p->write_pos + len < sizeof(p->buf));
    memcpy(p->buf + p->write_pos, data, len);
    p->write_pos += len;
}

static int pipe_read(BytePipe* p, uint8_t* out, size_t len) {
    size_t avail = p->write_pos - p->read_pos;
    size_t n = avail < len ? avail : len;
    memcpy(out, p->buf + p->read_pos, n);
    p->read_pos += n;
    return (int)n;
}

static size_t pipe_available(BytePipe* p) {
    return p->write_pos - p->read_pos;
}

/* ── Frame chunker (mirrors expansion.c logic) ───────────────────────────── */

static void chunk_and_write(BytePipe* pipe, const uint8_t* data, size_t len) {
    size_t offset = 0;
    while(offset < len) {
        size_t chunk = len - offset < EXP_MAX_DATA ? len - offset : EXP_MAX_DATA;
        uint8_t frame[2 + EXP_MAX_DATA];
        frame[0] = EXP_FRAME_DATA;
        frame[1] = (uint8_t)chunk;
        memcpy(frame + 2, data + offset, chunk);
        pipe_write(pipe, frame, 2 + chunk);
        offset += chunk;
    }
}

/* ── Reassembler (mirrors process_data_chunk logic) ─────────────────────── */

typedef struct {
    uint8_t buf[4096];
    size_t  len;
    uint8_t frames[16][4096];
    size_t  frame_lens[16];
    int     frame_count;
} Reassembler;

static void reassembler_feed(Reassembler* r, const uint8_t* chunk, size_t chunk_len) {
    assert(r->len + chunk_len < sizeof(r->buf));
    memcpy(r->buf + r->len, chunk, chunk_len);
    r->len += chunk_len;

    /* Discard leading bytes that can't start the magic sequence */
    while(r->len > 0 && r->buf[0] != 'B') {
        memmove(r->buf, r->buf + 1, r->len - 1);
        r->len--;
    }

    while(r->len >= 7) {
        if(r->buf[0] != 'B' || r->buf[1] != 'T' ||
           r->buf[2] != 'P' || r->buf[3] != 'C') {
            memmove(r->buf, r->buf + 1, r->len - 1);
            r->len--;
            continue;
        }
        uint16_t plen = (uint16_t)r->buf[5] | ((uint16_t)r->buf[6] << 8);
        size_t total  = 71 + plen;
        if(r->len < total) break;

        assert(r->frame_count < 16);
        memcpy(r->frames[r->frame_count], r->buf, total);
        r->frame_lens[r->frame_count] = total;
        r->frame_count++;
        memmove(r->buf, r->buf + total, r->len - total);
        r->len -= total;
    }
}

static void drain_pipe_to_reassembler(BytePipe* pipe, Reassembler* r) {
    while(pipe_available(pipe) > 0) {
        uint8_t byte;
        if(pipe_read(pipe, &byte, 1) == 1) {
            /* Simulates receiving data frame: type + len + data */
            if(byte == EXP_FRAME_DATA) {
                uint8_t dlen;
                pipe_read(pipe, &dlen, 1);
                uint8_t dbuf[EXP_MAX_DATA];
                pipe_read(pipe, dbuf, dlen);
                reassembler_feed(r, dbuf, dlen);
            }
        }
    }
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

TEST(small_frame_fits_one_chunk) {
    /* A heartbeat frame (71 + 13 = 84 bytes) fits in one 64-byte expansion chunk
     * for the header, then a second chunk for payload. Total 2 data frames. */
    BytePipe pipe = {0};
    Reassembler r = {0};

    /* Build a minimal BTCPC frame manually */
    uint8_t frame[84];
    memset(frame, 0, sizeof(frame));
    frame[0]='B'; frame[1]='T'; frame[2]='P'; frame[3]='C';
    frame[4] = 0x05; /* heartbeat */
    frame[5] = 13;   /* payload len LE */
    frame[6] = 0;
    /* sig bytes 7..70 = 0 */
    /* payload bytes 71..83: battery=75, rest 0 */
    frame[71] = 75;

    chunk_and_write(&pipe, frame, sizeof(frame));
    drain_pipe_to_reassembler(&pipe, &r);

    ASSERT_EQ(r.frame_count, 1);
    ASSERT_EQ(r.frame_lens[0], 84);
    ASSERT_EQ(r.frames[0][71], 75); /* payload preserved */
}

TEST(large_frame_reassembled_from_multiple_chunks) {
    /* A SubGHz census frame: 71 + 106 = 177 bytes → 3 expansion data frames */
    BytePipe pipe = {0};
    Reassembler r = {0};

    uint8_t frame[177];
    memset(frame, 0, sizeof(frame));
    frame[0]='B'; frame[1]='T'; frame[2]='P'; frame[3]='C';
    frame[4] = 0x08; /* census */
    frame[5] = 106;  frame[6] = 0; /* payload_len LE */
    /* fill payload with pattern */
    for(int i = 0; i < 106; i++) frame[71 + i] = (uint8_t)(i & 0xFF);

    chunk_and_write(&pipe, frame, sizeof(frame));

    /* Count expansion data frames written: ceil(177/64) = 3 */
    int data_frames = 0;
    size_t pos = 0;
    while(pos < pipe.write_pos) {
        if(pipe.buf[pos] == EXP_FRAME_DATA) {
            uint8_t len = pipe.buf[pos + 1];
            pos += 2 + len;
            data_frames++;
        } else {
            pos++;
        }
    }
    ASSERT_EQ(data_frames, 3);

    drain_pipe_to_reassembler(&pipe, &r);
    ASSERT_EQ(r.frame_count, 1);
    ASSERT_EQ(r.frame_lens[0], 177);

    /* Verify payload bytes preserved */
    for(int i = 0; i < 106; i++) {
        ASSERT_EQ(r.frames[0][71 + i], (uint8_t)(i & 0xFF));
    }
}

TEST(two_frames_in_sequence) {
    BytePipe pipe = {0};
    Reassembler r = {0};

    for(int f = 0; f < 2; f++) {
        uint8_t frame[84];
        memset(frame, 0, sizeof(frame));
        frame[0]='B'; frame[1]='T'; frame[2]='P'; frame[3]='C';
        frame[4] = 0x05;
        frame[5] = 13; frame[6] = 0;
        frame[71] = (uint8_t)(f + 1); /* distinguish frames */
        chunk_and_write(&pipe, frame, sizeof(frame));
    }

    drain_pipe_to_reassembler(&pipe, &r);
    ASSERT_EQ(r.frame_count, 2);
    ASSERT_EQ(r.frames[0][71], 1);
    ASSERT_EQ(r.frames[1][71], 2);
}

TEST(garbage_bytes_skipped) {
    Reassembler r = {0};
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00};
    reassembler_feed(&r, garbage, sizeof(garbage));
    ASSERT_EQ(r.frame_count, 0);
    ASSERT_EQ(r.len, 0); /* all consumed/discarded */
}

TEST(frame_after_garbage) {
    Reassembler r = {0};

    /* garbage first, then a valid frame */
    uint8_t garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
    reassembler_feed(&r, garbage, sizeof(garbage));

    uint8_t frame[84];
    memset(frame, 0, sizeof(frame));
    frame[0]='B'; frame[1]='T'; frame[2]='P'; frame[3]='C';
    frame[4]=0x05; frame[5]=13; frame[6]=0;
    frame[71]=42;
    reassembler_feed(&r, frame, sizeof(frame));

    ASSERT_EQ(r.frame_count, 1);
    ASSERT_EQ(r.frames[0][71], 42);
}

TEST(max_payload_frame) {
    /* BTCPC_MAX_PAYLOAD = 472; total = 71 + 472 = 543 bytes → 9 chunks */
    BytePipe pipe = {0};
    Reassembler r = {0};

    size_t total_size = 71 + BTCPC_MAX_PAYLOAD;
    uint8_t* frame = calloc(1, total_size);
    frame[0]='B'; frame[1]='T'; frame[2]='P'; frame[3]='C';
    frame[4] = 0x01;
    frame[5] = (uint8_t)(BTCPC_MAX_PAYLOAD & 0xFF);
    frame[6] = (uint8_t)(BTCPC_MAX_PAYLOAD >> 8);
    for(size_t i = 0; i < BTCPC_MAX_PAYLOAD; i++) frame[71+i] = (uint8_t)(i & 0xFF);

    chunk_and_write(&pipe, frame, total_size);
    drain_pipe_to_reassembler(&pipe, &r);

    ASSERT_EQ(r.frame_count, 1);
    ASSERT_EQ(r.frame_lens[0], total_size);
    for(size_t i = 0; i < BTCPC_MAX_PAYLOAD; i++) {
        ASSERT_EQ(r.frames[0][71+i], (uint8_t)(i & 0xFF));
    }
    free(frame);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("\nbtcpc_expansion host-side tests\n");
    printf("══════════════════════════════════════════════════════\n");

    RUN(small_frame_fits_one_chunk);
    RUN(large_frame_reassembled_from_multiple_chunks);
    RUN(two_frames_in_sequence);
    RUN(garbage_bytes_skipped);
    RUN(frame_after_garbage);
    RUN(max_payload_frame);

    printf("══════════════════════════════════════════════════════\n");
    printf("%d/%d tests passed\n\n", tests_pass, tests_run);
    return tests_pass == tests_run ? 0 : 1;
}
