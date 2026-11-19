/* osk: on-screen keyboard for a device with a D-pad and four buttons only.  Pure state + drawing:
 * the caller feeds directions and buttons and reads the result; nothing here touches files or processes.
 *
 *   D-pad   move over the grid (wraps around at every edge)
 *   A       type the key under the cursor / press the action key
 *   B       delete the last character; on an empty field: cancel
 *   SELECT  next layer: abc -> ABC -> #+=
 *   START   OK
 * Layers 0/1 hold the digits and letters plus a few common punctuation marks, layer 2 holds every other
 * printable ASCII character, and Space is on the action row, so all of 0x20..0x7e can be typed. */
#ifndef OLPC_OSK_H
#define OLPC_OSK_H
#include <stddef.h>

enum osk_mode { OSK_KEY_WPA, OSK_KEY_WEP, OSK_SSID };
enum osk_result { OSK_NONE, OSK_OK, OSK_CANCEL };
#define OSK_TEXT_MAX 66
#define OSK_COLS 10
#define OSK_ROWS 5            /* 4 character rows + the action row */
#define OSK_NACT 6

struct osk {
	enum osk_mode mode;
	char prompt[96];
	char text[OSK_TEXT_MAX];
	char warn[80];                    /* why OK was refused, until the next edit */
	int len, layer, row, col, ocol;   /* ocol = wanted column, kept while moving vertically */
	int hidden;
	int maxlen;
};

void osk_init(struct osk *o, enum osk_mode mode, const char *prompt);
void osk_wipe(struct osk *o);                       /* zeroes the typed text (never optimised away) */
/* dx/dy = -1, 0, +1; wraps around */
void osk_move(struct osk *o, int dx, int dy);
enum osk_result osk_press(struct osk *o);           /* A */
enum osk_result osk_back(struct osk *o);            /* B */
void osk_next_layer(struct osk *o);                 /* SELECT */
/* START: OK when the text is valid; otherwise OSK_NONE and *why says what is wrong (may be NULL) */
enum osk_result osk_ok(struct osk *o, const char **why);

/* 1 when the text is acceptable for the mode; msg gets the status line ("12 characters - need 8 to 63") */
int  osk_check(enum osk_mode mode, const char *text, int len, char *msg, size_t cap);
/* the character shown on a grid cell, 0 for an empty cell (row < 4) */
char osk_cell_char(int layer, int row, int col);
int  osk_row_len(int layer, int row);

void osk_draw(const struct osk *o, unsigned short sel);   /* the whole screen below the title bar */
void osk_draw_cell(const struct osk *o, unsigned short sel, int row, int col);
void osk_draw_field(const struct osk *o);                 /* field, length line and layer tabs */
#endif
