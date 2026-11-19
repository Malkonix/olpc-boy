/* olpc-menu: pure logic (ROM discovery, covers, key repeat, state file, text helpers).
 * No framebuffer or evdev here: everything is unit-tested on the host (selftest.c). */
#ifndef OLPC_MENU_LIB_H
#define OLPC_MENU_LIB_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define NSYS 3
extern const char *const SYS_DIR[NSYS];     /* "gb" "gbc" "nes": directory and olpc-run system name */
extern const char *const SYS_TITLE[NSYS];   /* "Game Boy" "Game Boy Color" "NES" */

struct rom {
	char *name;       /* display name: file name without its last extension */
	char *file;       /* file name */
	char *path;       /* absolute path */
	char *romsdir;    /* the "roms" directory it was found in */
	char *sysdir;     /* the system directory as spelled on disk */
	off_t size;
};
struct romlist { struct rom *v; int n, cap; };

/* bases: colon-separated directories ("/mnt:/media").  Each base is searched at depth 1
 * (and depth 2 below /media-like bases) for a directory named roms (any case) that holds
 * gb / gbc / nes directories.  Fills lists[NSYS] (cleared first); returns the total. */
int  rom_scan(const char *bases, struct romlist lists[NSYS]);
void rom_free(struct romlist lists[NSYS]);
/* "<romsdir>/../covers/<sysdir lowercased>/<name>.obc" */
int  cover_path(const struct rom *r, char *out, size_t cap);

#define COVER_MAX 480
/* Reads an OBC1 cover into dst (COVER_MAX*COVER_MAX RGB565 values, row stride = *w).
 * 0 on success; -1 on any problem (missing, bad magic, size 0 or > COVER_MAX, short file). */
int  cover_load(const char *path, uint16_t *dst, int *w, int *h);

/* ---- key repeat for UP/DOWN, driven by the held state ------------------------------------ */
#define REP_DELAY_MS   300     /* first repeat */
#define REP_FAST_MS     83     /* ~12 steps/s */
#define REP_PAGE_AFTER 1500    /* held this long: page steps */
#define REP_PAGE_MS    200
struct rep { int dir; uint64_t t0, next; };
void rep_start(struct rep *r, int dir, uint64_t now_ms);   /* dir = -1 or +1; caller does the first step */
void rep_stop(struct rep *r);
/* Signed number of rows to move right now (0 = nothing due). */
int  rep_tick(struct rep *r, uint64_t now_ms, int page);
/* Milliseconds until the next step is due, -1 when no repeat is running. */
int  rep_timeout(const struct rep *r, uint64_t now_ms);

/* ---- remembered selection ---------------------------------------------------------------- */
int  state_write(const char *path, int sys, const char *file);   /* atomic (tmp + rename) */
int  state_read(const char *path, int *sys, char *file, size_t cap);
/* Same, plus the "In-game indicators" switch (a line "overlay=0" or "overlay=1", absent in older files):
 * overlay = 0 or 1 to write the line, -1 to leave it out; *overlay is -1 when the file has no valid line. */
int  state_write_opt(const char *path, int sys, const char *file, int overlay);
int  state_read_opt(const char *path, int *sys, char *file, size_t cap, int *overlay);

/* ---- text -------------------------------------------------------------------------------- */
/* UTF-8 (invalid bytes taken as Latin-1) to Latin-1 bytes; characters beyond U+00FF are folded
 * to ASCII lookalikes or '?'.  Returns the length. */
size_t utf8_to_l1(const char *s, unsigned char *out, size_t cap);
/* Shortens s (Latin-1) in place to at most maxch characters, ending in "..." when cut. */
void   fit_text(unsigned char *s, int maxch);
/* Word-wraps s into lines of at most maxch characters (the last line is cut with "..." when
 * more than maxlines would be needed).  Lines are written to out[i] (each cap bytes); returns n. */
int    wrap_text(const unsigned char *s, int maxch, char out[][96], int maxlines);

/* status parsers (status_parse, wifi_status_parse, wireless_level, clock_text, tzcfg_parse...) live in ../common/status.h */
#include "../common/status.h"
#endif
