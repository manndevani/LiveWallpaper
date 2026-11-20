/* png.c - writes an RGBA PNG with no library: zlib "stored" blocks (no
 * compression). Only used by -snapshot, so size doesn't matter. */
#include "lw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    c ^= 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t b[4];
    be32(b, n);
    fwrite(b, 1, 4, f);
    uint32_t c = crc(0, (const uint8_t *)type, 4);
    if (n) c = crc(c, data, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    be32(b, c);
    fwrite(b, 1, 4, f);
}

bool png_write(const char *path, const uint8_t *rgba, int w, int h)
{
    size_t row = (size_t)w * 4 + 1, raw_n = row * (size_t)h;
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_n), *z = malloc(z_n);
    FILE *f = fopen(path, "wb");
    if (!raw || !z || !f) { free(raw); free(z); if (f) fclose(f); return false; }

    for (int y = 0; y < h; y++) {
        raw[y * row] = 0; /* filter: none */
        memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    uint8_t *o = z;
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *o++ = off + n == raw_n;
        *o++ = n & 255; *o++ = n >> 8;
        *o++ = ~n & 255; *o++ = (~n >> 8) & 255;
        memcpy(o, raw + off, n);
        o += n;
    }
    be32(o, (b << 16) | a);
    o += 4;

    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)w);
    be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    fwrite(sig, 1, 8, f);
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)(o - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return true;
}
