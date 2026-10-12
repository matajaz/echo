// Bounded RIFF/PCM parsing and byte-fragment assembly, shared with host tests.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum { WAV_MORE, WAV_READY, WAV_INVALID } wav_result_t;
typedef struct { uint32_t rate, bytes; size_t offset; } wav_info_t;
static inline uint32_t wav_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint16_t wav_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline wav_result_t wav_header(const uint8_t *p, size_t n, wav_info_t *out) {
    if (!p || !out) return WAV_INVALID;
    if (n < 12) return WAV_MORE;
    if (memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return WAV_INVALID;
    uint32_t riff = wav_u32(p + 4);
    if (riff < 36) return WAV_INVALID;
    bool fmt = false;
    uint32_t rate = 0;
    for (size_t i = 12; ; ) {
        if (i > n || n - i < 8) return WAV_MORE;
        uint32_t len = wav_u32(p + i + 4);
        // Validate against declared RIFF size before any addition or skip.
        uint64_t end = (uint64_t)i + 8u + len + (len & 1u);
        if (end > (uint64_t)riff + 8u || end > SIZE_MAX) return WAV_INVALID;
        if (!memcmp(p + i, "data", 4)) {
            if (!fmt || (len & 1u)) return WAV_INVALID;
            *out = (wav_info_t){ .rate = rate, .bytes = len, .offset = i + 8 };
            return WAV_READY;
        }
        if ((uint64_t)n < end) return WAV_MORE;
        if (!memcmp(p + i, "fmt ", 4)) {
            if (fmt || len < 16) return WAV_INVALID;
            const uint8_t *f = p + i + 8;
            rate = wav_u32(f + 4);
            if (wav_u16(f) != 1 || wav_u16(f + 2) != 1 || wav_u16(f + 14) != 16 ||
                wav_u16(f + 12) != 2 || rate < 8000 || rate > 48000 ||
                wav_u32(f + 8) != rate * 2u) return WAV_INVALID;
            fmt = true;
        }
        i = (size_t)end;
    }
}
typedef struct { bool have_byte; uint8_t byte; } wav_fragment_t;
// Output capacity must be >= (n+1)/2 samples. No byte is lost across reads.
static inline size_t wav_samples(wav_fragment_t *f, const uint8_t *p, size_t n, int16_t *out) {
    size_t count = 0, i = 0;
    if (f->have_byte && n) {
        out[count++] = (int16_t)((uint16_t)f->byte | ((uint16_t)p[i++] << 8));
        f->have_byte = false;
    }
    while (n - i >= 2) { out[count++] = (int16_t)wav_u16(p + i); i += 2; }
    if (i < n) { f->byte = p[i]; f->have_byte = true; }
    return count;
}
