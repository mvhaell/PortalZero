// Read Flipper Zero .nfc files (MIFARE Classic 1K) and write game progress back.
//
// Structure of such a file (excerpt):
//   Filetype: Flipper NFC device
//   Device type: Mifare Classic
//   Mifare Classic type: 1K
//   Block 0: 21 B4 89 41 5D 81 01 0F C4 01 00 00 00 00 00 12
//   Block 1: ...
//   Block 3: 4B 0B 20 10 7C CB 0F 0F 0F 69 ?? ?? ?? ?? ?? ??
#include <string.h>
#include "nfc.h"

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int is_trailer(int block) { return (block % 4) == 3; }

// does the range [p, end) contain the text s?
static int contains(const char *p, const char *end, const char *s) {
    size_t n = strlen(s);
    for (; p + n <= end; p++) if (memcmp(p, s, n) == 0) return 1;
    return 0;
}

// Checks whether the line [p, end) is a block line. Returns the block number (otherwise -1);
// *bytes then points to the part after the colon.
static int block_line(const char *p, const char *end, const char **bytes) {
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (end - p < 7 || strncmp(p, "Block", 5) != 0) return -1;
    p += 5;
    while (p < end && *p == ' ') p++;
    int n = 0, digits = 0;
    while (p < end && *p >= '0' && *p <= '9' && digits < 4) { n = n * 10 + (*p - '0'); p++; digits++; }
    if (digits == 0) return -1;
    while (p < end && *p == ' ') p++;
    if (p >= end || *p != ':') return -1;
    *bytes = p + 1;
    return n;
}

// Read 16 bytes from "XX XX ... XX". Returns 16 = ok, otherwise error.
// *unknown is set if "??" occurs.
static int parse_bytes(const char *p, const char *end, uint8_t out[16], int *unknown) {
    int count = 0;
    *unknown = 0;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) p++;
        if (p >= end) break;
        if (end - p < 2 || count >= 16) return -1;
        if (p[0] == '?' && p[1] == '?') {
            out[count++] = 0;
            *unknown = 1;
        } else {
            int h = hexval(p[0]), l = hexval(p[1]);
            if (h < 0 || l < 0) return -1;
            out[count++] = (uint8_t)(h * 16 + l);
        }
        p += 2;
        if (p < end && *p != ' ' && *p != '\t' && *p != '\r') return -1;
    }
    return count;
}

nfc_result_t nfc_parse(const char *text, size_t len, uint8_t data[NFC_DATA_SIZE]) {
    const char *p = text, *end = text + len;
    uint8_t found[NFC_BLOCKS];
    int incomplete = 0, is_classic = 0;
    memset(found, 0, sizeof(found));
    memset(data, 0, NFC_DATA_SIZE);

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;

        if (!is_classic && (contains(p, eol, "Mifare Classic") || contains(p, eol, "MIFARE Classic")))
            is_classic = 1;

        const char *bytes;
        int n = block_line(p, eol, &bytes);
        if (n >= 0) {
            if (n >= NFC_BLOCKS) return NFC_ERROR;          // not a 1K figure (e.g. 4K)
            int unknown;
            if (parse_bytes(bytes, eol, &data[n * 16], &unknown) != 16) return NFC_ERROR;
            if (unknown && !is_trailer(n)) incomplete = 1;  // missing keys are normal
            found[n] = 1;
        }
        p = eol + 1;
    }

    if (!is_classic) return NFC_ERROR;
    for (int i = 0; i < NFC_BLOCKS; i++) if (!found[i]) return NFC_ERROR;
    return incomplete ? NFC_INCOMPLETE : NFC_OK;
}

int nfc_rewrite(const char *text, size_t len, const uint8_t data[NFC_DATA_SIZE],
                nfc_emit_fn emit, void *ctx) {
    static const char hex[] = "0123456789ABCDEF";
    const char *p = text, *end = text + len;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *next = eol ? eol + 1 : end;
        if (!eol) eol = end;

        const char *bytes;
        int n = block_line(p, eol, &bytes);
        if (n >= 0 && n < NFC_BLOCKS && !is_trailer(n)) {
            // rewrite data block, keep line ending (\n or \r\n)
            char line[16 + 48];
            int k = 0;
            const char *prefix = "Block ";
            memcpy(line, prefix, 6); k = 6;
            if (n >= 10) line[k++] = (char)('0' + n / 10);
            line[k++] = (char)('0' + n % 10);
            line[k++] = ':';
            for (int i = 0; i < 16; i++) {
                uint8_t b = data[n * 16 + i];
                line[k++] = ' ';
                line[k++] = hex[b >> 4];
                line[k++] = hex[b & 15];
            }
            if (!emit(line, (size_t)k, ctx)) return 0;
            if (eol > p && eol[-1] == '\r') { if (!emit("\r", 1, ctx)) return 0; }
            if (next != eol) { if (!emit("\n", 1, ctx)) return 0; }
        } else {
            // copy all other lines (header, trailers) unchanged
            if (!emit(p, (size_t)(next - p), ctx)) return 0;
        }
        p = next;
    }
    return 1;
}
