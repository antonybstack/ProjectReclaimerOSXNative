// glproto.h -- shared-memory protocol between the loader (x86_64, Rosetta) and the native GL helper (arm64).
// See shim/docs/gl-helper.md. Included by glproxy.c (loader) and glhelper.c (helper); plain C, no OS headers.
#pragma once
#include <stdint.h>
#include "glproto_ops.h"

// protocol opcodes (GL calls are GLOP_FIRST_GL and up, glproto_ops.h)
enum {
    GLOP_WRAP = 1,       // filler: the rest of the ring until its end is unused
    GLOP_STAGE,          // [total_lo, total_hi, offset_lo, offset_hi, bytes...] append to the staging buffer
    GLOP_SYNC,           // [seq] reply with seq once everything before it has run (Finish waits for the GPU too)
    GLOP_HELLO,          // [protocol version]
    GLOP_PQ_PUBLISH,     // [fbo, vw, vh, frame_lo, frame_hi] copy the back buffer into a present slot
    GLOP_QUIT,
};
#define GLPROTO_VERSION 1

// a command: [op | (payload words << 8)] then the payload. Payload words < 2^24 (64 MB); bigger data is staged.
#define GLCMD(op,nw) ((uint32_t)(op) | ((uint32_t)(nw) << 8))
#define GLCMD_OP(h)  ((h) & 0xFFu)
#define GLCMD_NW(h)  ((h) >> 8)

// data argument that follows inline: [bytes] then ceil(bytes/4) words; bytes == GLDATA_STAGED -> the staging buffer
// (filled by GLOP_STAGE commands just before); bytes == GLDATA_NULL -> a NULL pointer; GLDATA_OFFSET -> the next two
// words are an offset into the bound pixel/array buffer
#define GLDATA_NULL    0xFFFFFFFFu
#define GLDATA_STAGED  0xFFFFFFFEu
#define GLDATA_OFFSET  0xFFFFFFFDu
#define GLDATA_INLINE_MAX (8u << 20)   // bigger uploads are staged in pieces of this size

// object namespaces (names are allocated by the loader, mapped by the helper)
enum { NS_TEX, NS_BUF, NS_FBO, NS_RBO, NS_OBJ, NS_SMP, NS_VAO, NS_PIPE, NS_QRY, NS_SYNC, NS_COUNT };

#define GLP_RING_WORDS   (16u << 20)   // 64 MB command ring
#define GLP_REPLY_BYTES  (64u << 20)   // round-trip replies (ReadPixels, GetTexImage, logs, ...)
#define GLP_QUERIES      (1u << 18)    // occlusion query result table
#define GLP_SLOTS        4             // present slots (IOSurfaces)

typedef struct {
    // ring positions in words, monotonic; producer = the loader thread holding the render context
    volatile uint64_t head; uint64_t pad0[7];
    volatile uint64_t tail; uint64_t pad1[7];
    // helper idles on its wake pipe when this is set (the loader writes a byte after publishing)
    volatile uint32_t helper_sleeping; uint32_t pad2[15];
    // round trips: the loader sends GLOP_SYNC/ a custom request with seq; the helper fills reply then sets reply_seq
    volatile uint64_t reply_seq; volatile uint32_t reply_len; volatile int32_t reply_status; uint64_t pad3[6];
    // occlusion queries: the loader bumps qgen[id] at BeginQuery, the helper sets qval[id] then qdone[id]=that gen
    volatile uint32_t qgen[GLP_QUERIES]; volatile uint32_t qdone[GLP_QUERIES]; volatile uint64_t qval[GLP_QUERIES];
    // present slots: the helper fills a slot, then publishes it as latest (pq_seq++), and writes the present pipe;
    // the loader's present thread marks the slot it is drawing in pq_reading
    volatile uint32_t slot_surface[GLP_SLOTS], slot_w[GLP_SLOTS], slot_h[GLP_SLOTS]; volatile uint64_t slot_frame[GLP_SLOTS];
    volatile int32_t pq_latest, pq_reading; volatile uint64_t pq_seq; uint64_t pad4[6];
    // statistics (helper)
    volatile uint64_t cmds, gl_errors; volatile double busy_s, idle_s;
    uint64_t pad5[4];
    uint32_t ring[GLP_RING_WORDS];
    uint8_t reply[GLP_REPLY_BYTES];
} GlpShared;
