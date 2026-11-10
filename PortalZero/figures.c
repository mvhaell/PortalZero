// Folder browser for the SD card, plus loading/saving figures
//
// Figures are:
//   - files with exactly 1024 bytes (.sky, .bin, .dmp ...): raw figure data
//   - .nfc files from the Flipper Zero (text), read and written directly
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "ff.h"
#include "figures.h"
#include "nfc.h"

#define NFC_MAX_FILE  8192        // the .nfc of a 1K figure is about 4 KB

typedef struct {
    char    name[ENTRY_NAME_LEN];
    uint8_t type;
    uint8_t fav;            // in the favorites view: index into fav_paths, otherwise 0xFF
} entry_t;

static entry_t entries[BROWSER_MAX];
static int     count;
static int     figure_count;
static char    cur_dir[FIG_PATH_LEN] = "/";

static char    nfc_buf[NFC_MAX_FILE];

static char    fav_paths[FAV_MAX][FIG_PATH_LEN];
static int     fav_n;

static bool in_fav_view(void) { return strcmp(cur_dir, FAV_VIEW) == 0; }

// ---------------------------------------------------------------------------
static bool has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    return n > e && strcasecmp(name + n - e, ext) == 0;
}

static bool is_nfc(const char *name) { return has_ext(name, ".nfc"); }

static int cmp_entries(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->type != y->type) return (int)x->type - (int)y->type;   // back, folders, figures
    return strcasecmp(x->name, y->name);
}

static void join(char *out, size_t size, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, size, "/%s", name);
    else                       snprintf(out, size, "%s/%s", dir, name);
}

// Clean up leftovers of an interrupted save ("X.nfc.tmp"):
// if X.nfc is missing, the temp file becomes the real file, otherwise it is deleted.
static void recover_tmp_files(void) {
    DIR dir;
    FILINFO fno;
    char found[4][ENTRY_NAME_LEN];
    int n = 0;
    if (f_opendir(&dir, cur_dir) != FR_OK) return;
    while (n < 4 && f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
        if (!(fno.fattrib & AM_DIR) && has_ext(fno.fname, ".nfc.tmp") && strlen(fno.fname) < ENTRY_NAME_LEN)
            strcpy(found[n++], fno.fname);
    }
    f_closedir(&dir);
    for (int i = 0; i < n; i++) {
        char tmp[FIG_PATH_LEN], base[FIG_PATH_LEN];
        join(tmp, sizeof(tmp), cur_dir, found[i]);
        strcpy(base, tmp);
        base[strlen(base) - 4] = '\0';                  // strip ".tmp"
        if (f_stat(base, NULL) == FR_OK) f_unlink(tmp);
        else f_rename(tmp, base);
    }
}

// favorites view: "back" + all favorites that (still) exist
static bool rescan_favorites(void) {
    count = 0;
    figure_count = 0;
    strcpy(entries[0].name, "..");
    entries[0].type = ENTRY_UP;
    entries[0].fav = 0xFF;
    count = 1;
    for (int i = 0; i < fav_n && count < BROWSER_MAX; i++) {
        if (f_stat(fav_paths[i], NULL) != FR_OK) continue;   // file no longer exists
        const char *base = strrchr(fav_paths[i], '/');
        snprintf(entries[count].name, ENTRY_NAME_LEN, "%s", base ? base + 1 : fav_paths[i]);
        entries[count].type = ENTRY_FIGURE;
        entries[count].fav = (uint8_t)i;
        count++;
        figure_count++;
    }
    qsort(entries, count, sizeof(entry_t), cmp_entries);
    return true;
}

bool browser_rescan(void) {
    DIR dir;
    FILINFO fno;
    if (in_fav_view()) return rescan_favorites();
    count = 0;
    figure_count = 0;
    recover_tmp_files();
    if (f_opendir(&dir, cur_dir) != FR_OK) return false;

    if (strcmp(cur_dir, "/") != 0) {
        strcpy(entries[count].name, "..");
        entries[count].fav = 0xFF;
        entries[count++].type = ENTRY_UP;
    }
    // show the "Favorites" folder at the top of the Skylanders folder as soon as there are any
    if (fav_n > 0 && strcasecmp(cur_dir, FIGURES_DIR) == 0) {
        strcpy(entries[count].name, "\x7F Favorites");
        entries[count].fav = 0xFF;
        entries[count++].type = ENTRY_FAV;
    }
    while (count < BROWSER_MAX) {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & (AM_HID | AM_SYS)) continue;
        if (fno.fname[0] == '.') continue;                 // ".", "..", macOS "._" files
        if (strlen(fno.fname) >= ENTRY_NAME_LEN) continue; // name too long
        if (strlen(cur_dir) + 1 + strlen(fno.fname) >= FIG_PATH_LEN) continue;

        if (fno.fattrib & AM_DIR) {
            if (strcasecmp(fno.fname, "System Volume Information") == 0) continue;
            entries[count].type = ENTRY_DIR;
        } else if (fno.fsize == 1024 || (is_nfc(fno.fname) && fno.fsize < NFC_MAX_FILE)) {
            entries[count].type = ENTRY_FIGURE;
            figure_count++;
        } else {
            continue;
        }
        strcpy(entries[count].name, fno.fname);
        entries[count].fav = 0xFF;
        count++;
    }
    f_closedir(&dir);
    qsort(entries, count, sizeof(entry_t), cmp_entries);
    return true;
}

bool browser_open(const char *dir) {
    char old[FIG_PATH_LEN];
    strcpy(old, cur_dir);
    snprintf(cur_dir, sizeof(cur_dir), "%s", dir);
    if (browser_rescan()) return true;
    strcpy(cur_dir, old);
    browser_rescan();
    return false;
}

const char *browser_dir(void) { return cur_dir; }

const char *browser_dir_name(void) {
    if (in_fav_view()) return "\x7F Favorites";
    if (strcmp(cur_dir, "/") == 0) return "SD card";
    const char *slash = strrchr(cur_dir, '/');
    return slash ? slash + 1 : cur_dir;
}

int browser_count(void) { return count; }
int browser_figure_count(void) { return figure_count; }

entry_type_t browser_type(int idx) {
    return (idx >= 0 && idx < count) ? (entry_type_t)entries[idx].type : ENTRY_FIGURE;
}

const char *browser_name(int idx) {
    return (idx >= 0 && idx < count) ? entries[idx].name : "";
}

void browser_path(int idx, char *out, size_t size) {
    if (in_fav_view() && idx >= 0 && idx < count && entries[idx].fav != 0xFF) {
        snprintf(out, size, "%s", fav_paths[entries[idx].fav]);
        return;
    }
    join(out, size, cur_dir, browser_name(idx));
}

bool browser_in_favorites(void) { return in_fav_view(); }

bool browser_is_fav(int idx) {
    if (browser_type(idx) != ENTRY_FIGURE || fav_n == 0) return false;
    char path[FIG_PATH_LEN];
    browser_path(idx, path, sizeof(path));
    return fav_is(path);
}

// Enter a folder or go up one level. *new_sel = entry to select afterwards.
bool browser_enter(int idx, int *new_sel) {
    if (idx < 0 || idx >= count) return false;
    *new_sel = 0;

    if (entries[idx].type == ENTRY_DIR) {
        char path[FIG_PATH_LEN];
        join(path, sizeof(path), cur_dir, entries[idx].name);
        return browser_open(path);
    }
    if (entries[idx].type == ENTRY_FAV) return browser_open(FAV_VIEW);
    if (entries[idx].type == ENTRY_UP && in_fav_view()) {
        // from the favorites back to the Skylanders folder, select "Favorites"
        if (!browser_open(FIGURES_DIR)) return false;
        for (int i = 0; i < count; i++) if (entries[i].type == ENTRY_FAV) { *new_sel = i; break; }
        return true;
    }
    if (entries[idx].type == ENTRY_UP) {
        char child[ENTRY_NAME_LEN] = "";
        char parent[FIG_PATH_LEN];
        strcpy(parent, cur_dir);
        char *slash = strrchr(parent, '/');
        if (!slash) return false;
        snprintf(child, sizeof(child), "%s", slash + 1);
        if (slash == parent) parent[1] = '\0';   // "/Skylanders" -> "/"
        else *slash = '\0';
        if (!browser_open(parent)) return false;
        // select the folder we came from
        for (int i = 0; i < count; i++)
            if (entries[i].type == ENTRY_DIR && strcmp(entries[i].name, child) == 0) { *new_sel = i; break; }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Loading

static bool read_text(const char *path, size_t *len) {
    FIL fp;
    UINT br = 0;
    if (f_open(&fp, path, FA_READ) != FR_OK) return false;
    FSIZE_t size = f_size(&fp);
    bool ok = size < NFC_MAX_FILE &&
              f_read(&fp, nfc_buf, (UINT)size, &br) == FR_OK && br == size;
    f_close(&fp);
    *len = br;
    return ok;
}

fig_result_t figure_load(const char *path, uint8_t data[1024]) {
    if (is_nfc(path)) {
        size_t len;
        if (!read_text(path, &len)) return FIG_ERROR;
        nfc_result_t r = nfc_parse(nfc_buf, len, data);
        if (r == NFC_ERROR) return FIG_ERROR;
        return r == NFC_INCOMPLETE ? FIG_INCOMPLETE : FIG_OK;
    }
    FIL fp;
    UINT br = 0;
    if (f_open(&fp, path, FA_READ) != FR_OK) return FIG_ERROR;
    FRESULT res = f_read(&fp, data, 1024, &br);
    f_close(&fp);
    return (res == FR_OK && br == 1024) ? FIG_OK : FIG_ERROR;
}

// ---------------------------------------------------------------------------
// Saving

static int emit_file(const char *s, size_t n, void *ctx) {
    UINT bw = 0;
    return f_write((FIL *)ctx, s, (UINT)n, &bw) == FR_OK && bw == n;
}

// .nfc: write to a temp file first, then swap. This way a complete file
// always remains, even after a power loss.
static bool save_nfc(const char *path, const uint8_t data[1024]) {
    size_t len;
    if (!read_text(path, &len)) return false;
    uint8_t check[1024];
    if (nfc_parse(nfc_buf, len, check) == NFC_ERROR) return false;   // don't touch the file

    char tmp[FIG_PATH_LEN + 8];
    if (strlen(path) + 4 >= FIG_PATH_LEN) return false;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FIL fp;
    if (f_open(&fp, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    int ok = nfc_rewrite(nfc_buf, len, data, emit_file, &fp);
    if (f_close(&fp) != FR_OK) ok = 0;
    if (!ok) { f_unlink(tmp); return false; }

    if (f_unlink(path) != FR_OK) { f_unlink(tmp); return false; }
    return f_rename(tmp, path) == FR_OK;
}

bool figure_save(const char *path, const uint8_t data[1024]) {
    if (is_nfc(path)) return save_nfc(path, data);
    FIL fp;
    UINT bw = 0;
    if (f_open(&fp, path, FA_WRITE | FA_OPEN_EXISTING) != FR_OK) return false;
    FRESULT res = f_write(&fp, data, 1024, &bw);
    if (f_close(&fp) != FR_OK) res = FR_DISK_ERR;
    return res == FR_OK && bw == 1024;
}

// ---------------------------------------------------------------------------
// Remember the last placed figures

// last written content - identical content is not written again
static char     last_written[2048];
static uint32_t last_written_len = 0xFFFFFFFF;

bool last_save(char paths[][FIG_PATH_LEN], int count) {
    static char text[2048];
    size_t len = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(paths[i]);
        if (len + n + 1 >= sizeof(text)) break;
        memcpy(text + len, paths[i], n);
        len += n;
        text[len++] = '\n';
    }
    if (len == last_written_len && memcmp(text, last_written, len) == 0) return true;   // unchanged

    FIL fp;
    UINT bw = 0;
    if (f_open(&fp, LAST_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    bool ok = f_write(&fp, text, (UINT)len, &bw) == FR_OK && bw == len;
    if (f_close(&fp) != FR_OK) ok = false;
    f_chmod(LAST_FILE, AM_HID, AM_HID);          // hidden (invisible on Mac/PC)
    if (ok) { memcpy(last_written, text, len); last_written_len = (uint32_t)len; }
    return ok;
}

// Take over a file from the older PTPE firmware (same SD card) under the new name
static void take_over(const char *old_name, const char *new_name) {
    if (f_stat(new_name, NULL) != FR_OK && f_stat(old_name, NULL) == FR_OK) f_rename(old_name, new_name);
}

int last_load(char paths[][FIG_PATH_LEN], int max) {
    FIL fp;
    char line[FIG_PATH_LEN + 4];
    int count = 0;
    f_unlink(LAST_FILE_OLD);                     // clean up leftover of the old version
    take_over(LAST_FILE_PTPE, LAST_FILE);
    if (f_open(&fp, LAST_FILE, FA_READ) != FR_OK) return 0;
    while (count < max && f_gets(line, sizeof(line), &fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] != '/' || n >= FIG_PATH_LEN) continue;   // invalid line
        strcpy(paths[count++], line);
    }
    f_close(&fp);
    // remember as "already written" so an unchanged state is not rewritten
    size_t len = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(paths[i]);
        if (len + n + 1 >= sizeof(last_written)) break;
        memcpy(last_written + len, paths[i], n);
        len += n;
        last_written[len++] = '\n';
    }
    last_written_len = (uint32_t)len;
    return count;
}

// ---------------------------------------------------------------------------
// Favorites

static bool fav_save(void) {
    FIL fp;
    UINT bw;
    if (f_open(&fp, FAV_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    bool ok = true;
    for (int i = 0; i < fav_n && ok; i++) {
        size_t n = strlen(fav_paths[i]);
        ok = f_write(&fp, fav_paths[i], (UINT)n, &bw) == FR_OK && bw == n &&
             f_write(&fp, "\n", 1, &bw) == FR_OK && bw == 1;
    }
    if (f_close(&fp) != FR_OK) ok = false;
    f_chmod(FAV_FILE, AM_HID, AM_HID);           // hidden
    return ok;
}

int fav_load(void) {
    FIL fp;
    char line[FIG_PATH_LEN + 4];
    fav_n = 0;
    take_over(FAV_FILE_PTPE, FAV_FILE);
    if (f_open(&fp, FAV_FILE, FA_READ) != FR_OK) return 0;
    while (fav_n < FAV_MAX && f_gets(line, sizeof(line), &fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] != '/' || n >= FIG_PATH_LEN || fav_is(line)) continue;
        strcpy(fav_paths[fav_n++], line);
    }
    f_close(&fp);
    return fav_n;
}

bool fav_is(const char *path) {
    for (int i = 0; i < fav_n; i++) if (strcmp(fav_paths[i], path) == 0) return true;
    return false;
}

int fav_count(void) { return fav_n; }

int fav_toggle(const char *path) {
    if (strlen(path) >= FIG_PATH_LEN) return -1;
    for (int i = 0; i < fav_n; i++) {
        if (strcmp(fav_paths[i], path) == 0) {               // present -> remove
            for (int k = i; k < fav_n - 1; k++) strcpy(fav_paths[k], fav_paths[k + 1]);
            fav_n--;
            if (!fav_save()) return -1;
            return 0;
        }
    }
    if (fav_n >= FAV_MAX) return -1;                         // full
    strcpy(fav_paths[fav_n++], path);
    if (!fav_save()) { fav_n--; return -1; }
    return 1;
}
