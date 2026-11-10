// Folder browser for the SD card, plus loading/saving figures
#ifndef FIGURES_H
#define FIGURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FIGURES_DIR      "/Skylanders"   // start folder
#define BROWSER_MAX      150             // entries per folder
#define ENTRY_NAME_LEN   96
#define FIG_PATH_LEN     256

typedef enum { ENTRY_UP, ENTRY_FAV, ENTRY_DIR, ENTRY_FIGURE } entry_type_t;   // ENTRY_FAV = "Favorites" folder

typedef enum {
    FIG_OK = 0,
    FIG_INCOMPLETE = 1,   // .nfc incompletely read - loaded, may not work
    FIG_ERROR = -1
} fig_result_t;

// open and read a folder (entries: "back", folders, figures)
bool         browser_open(const char *dir);
bool         browser_rescan(void);
const char  *browser_dir(void);                // current path, e.g. "/Skylanders/Giants"
const char  *browser_dir_name(void);           // last part only, "SD card" for /
int          browser_count(void);
int          browser_figure_count(void);       // figures only (no folders)
entry_type_t browser_type(int idx);
const char  *browser_name(int idx);
bool         browser_enter(int idx, int *new_sel);   // folder or "back"
void         browser_path(int idx, char *out, size_t size);

fig_result_t figure_load(const char *path, uint8_t data[1024]);
bool         figure_save(const char *path, const uint8_t data[1024]);

// Remember the last placed figures: ONE small hidden file /.portalzero_last
// (one path per line, the most recently placed figure is last). It is only
// rewritten when something actually changed.
#define LAST_FILE     "/.portalzero_last"
#define LAST_FILE_PTPE "/.ptpe_last"             // name used by the older PTPE firmware - taken over automatically
#define LAST_FILE_OLD "/ptpe_last.txt"           // old visible version - gets deleted
bool last_save(char paths[][FIG_PATH_LEN], int count);
int  last_load(char paths[][FIG_PATH_LEN], int max);   // number of paths read

// Favorites: ONE small hidden file /.portalzero_fav (one path per line).
// The /Skylanders folder shows a "Favorites" folder at the top as soon as there are any.
#define FAV_FILE  "/.portalzero_fav"
#define FAV_FILE_PTPE "/.ptpe_fav"              // name used by the older PTPE firmware - taken over automatically
#define FAV_MAX   32
#define FAV_VIEW  "/\x01" "Favorites"      // internal path of the favorites view (\x01 never occurs in file names)
int  fav_load(void);                            // call after mounting the SD card
int  fav_toggle(const char *path);              // 1 = now a favorite, 0 = removed, -1 = full/error
bool fav_is(const char *path);
int  fav_count(void);
bool browser_is_fav(int idx);
bool browser_in_favorites(void);

#endif
