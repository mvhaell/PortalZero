// Read Flipper Zero .nfc files (MIFARE Classic 1K) and write game progress back.
// Pure text processing without hardware, so it can also be tested on a PC.
#ifndef NFC_H
#define NFC_H

#include <stddef.h>
#include <stdint.h>

#define NFC_BLOCKS      64
#define NFC_DATA_SIZE   (NFC_BLOCKS * 16)

typedef enum {
    NFC_OK = 0,          // all 64 blocks complete
    NFC_INCOMPLETE = 1,  // data blocks with "??" (incompletely read)
    NFC_ERROR = -1       // not a 1K figure / blocks missing
} nfc_result_t;

// Convert the text of an .nfc file into 1024 bytes. "??" becomes 0.
nfc_result_t nfc_parse(const char *text, size_t len, uint8_t data[NFC_DATA_SIZE]);

// Create a new .nfc file: all lines stay the same, only the data blocks are
// replaced with the current game progress. Sector trailers (blocks 3, 7, 11, ...)
// with the keys stay unchanged. emit() is called for every piece of text and
// returns 0 on error. Returns 1 = ok, 0 = error.
typedef int (*nfc_emit_fn)(const char *s, size_t n, void *ctx);
int nfc_rewrite(const char *text, size_t len, const uint8_t data[NFC_DATA_SIZE],
                nfc_emit_fn emit, void *ctx);

#endif
