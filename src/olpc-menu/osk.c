/* osk: on-screen keyboard, see osk.h */
#include <stdio.h>
#include <string.h>
#include "netui.h"
#include "osk.h"

#define CW 108                /* cell pitch */
#define CH 84
#define GX 60                 /* grid origin */
#define GY 300
#define AW 180                /* action cell pitch */

static const char *const LAYER[3][4] = {
	{ "1234567890", "qwertyuiop", "asdfghjkl.", "zxcvbnm-_@" },
	{ "1234567890", "QWERTYUIOP", "ASDFGHJKL.", "ZXCVBNM-_@" },
	{ "!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~" },
};
static const char *const LAYER_NAME[3] = { "abc", "ABC", "#+=" };

char osk_cell_char(int layer, int row, int col)
{
	const char *s;

	if (layer < 0 || layer > 2 || row < 0 || row > 3 || col < 0) return 0;
	s = LAYER[layer][row];
	return col < (int)strlen(s) ? s[col] : 0;
}

int osk_row_len(int layer, int row)
{
	if (row == 4) return OSK_NACT;
	if (layer < 0 || layer > 2 || row < 0 || row > 3) return 0;
	return (int)strlen(LAYER[layer][row]);
}

void osk_wipe(struct osk *o)
{
	volatile char *p = o->text;
	size_t i;

	for (i = 0; i < sizeof o->text; i++) p[i] = 0;
	o->len = 0;
}

void osk_init(struct osk *o, enum osk_mode mode, const char *prompt)
{
	memset(o, 0, sizeof *o);
	o->mode = mode;
	snprintf(o->prompt, sizeof o->prompt, "%s", prompt ? prompt : "");
	o->maxlen = mode == OSK_SSID ? 32 : mode == OSK_KEY_WEP ? 26 : 64;
	o->row = 1; o->col = o->ocol = 0;           /* starts on "q": letters are what most keys need first */
}

static int is_hex(const char *s, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
	return n > 0;
}

int osk_check(enum osk_mode mode, const char *text, int len, char *msg, size_t cap)
{
	const char *pl = len == 1 ? "" : "s";

	switch (mode) {
	case OSK_SSID:
		if (len < 1) { snprintf(msg, cap, "Type the network name (1 to 32 characters)"); return 0; }
		snprintf(msg, cap, "%d of 32 characters - OK", len);
		return 1;
	case OSK_KEY_WEP:
		if (len == 0) { snprintf(msg, cap, "Type the WEP key (5 or 13 characters, or 10 / 26 hex digits)"); return 0; }
		if (len == 5 || len == 13 || ((len == 10 || len == 26) && is_hex(text, len))) { snprintf(msg, cap, "%d character%s - OK", len, pl); return 1; }
		snprintf(msg, cap, "%d character%s - need 5 or 13 (or 10 / 26 hex digits)", len, pl);
		return 0;
	default:
		if (len == 0) { snprintf(msg, cap, "Type the password (8 to 63 characters)"); return 0; }
		if (len >= 8 && len <= 63) { snprintf(msg, cap, "%d characters - OK", len); return 1; }
		if (len == 64) {
			if (is_hex(text, len)) { snprintf(msg, cap, "64 hex digits - OK"); return 1; }
			snprintf(msg, cap, "64 characters are only valid as hex digits (0-9 a-f)");
			return 0;
		}
		snprintf(msg, cap, "%d character%s - need 8 to 63", len, pl);
		return 0;
	}
}

static void clamp_col(struct osk *o)
{
	int n = osk_row_len(o->layer, o->row);

	if (o->row == 4) return;
	o->col = o->ocol < n ? o->ocol : n - 1;
}

void osk_move(struct osk *o, int dx, int dy)
{
	if (dx) {
		int n = osk_row_len(o->layer, o->row);

		o->col = (o->col + dx + n) % n;
		o->ocol = o->row == 4 ? (o->col * 10 + 5) / OSK_NACT : o->col;
	}
	if (dy) {
		int nr = (o->row + dy + OSK_ROWS) % OSK_ROWS;

		if (o->row < 4 && nr == 4) o->col = o->ocol * OSK_NACT / 10;
		o->row = nr;
		clamp_col(o);
	}
	o->warn[0] = 0;
}

void osk_next_layer(struct osk *o)
{
	o->layer = (o->layer + 1) % 3;
	if (o->row < 4) clamp_col(o);
	o->warn[0] = 0;
}

static void type(struct osk *o, char c)
{
	if (o->len >= o->maxlen) return;
	o->text[o->len++] = c;
	o->text[o->len] = 0;
	o->warn[0] = 0;
}

enum osk_result osk_back(struct osk *o)
{
	o->warn[0] = 0;
	if (o->len == 0) return OSK_CANCEL;
	o->text[--o->len] = 0;
	return OSK_NONE;
}

enum osk_result osk_ok(struct osk *o, const char **why)
{
	char m[96];

	if (osk_check(o->mode, o->text, o->len, m, sizeof m)) return OSK_OK;
	snprintf(o->warn, sizeof o->warn, "%.78s", m);
	if (why) *why = o->warn;
	return OSK_NONE;
}

enum osk_result osk_press(struct osk *o)
{
	if (o->row < 4) {
		char c = osk_cell_char(o->layer, o->row, o->col);

		if (c) type(o, c);
		return OSK_NONE;
	}
	switch (o->col) {
	case 0: osk_next_layer(o); break;
	case 1: type(o, ' '); break;
	case 2: osk_back(o); break;                       /* Del never cancels */
	case 3: o->hidden = !o->hidden; break;
	case 4: return OSK_CANCEL;
	default: return osk_ok(o, NULL);
	}
	return OSK_NONE;
}

/* ---------------------------------------------------------------------------- drawing */

static void cell_rect(int row, int col, int *x, int *y, int *w, int *h)
{
	*y = GY + row * CH;
	*h = CH;
	if (row == 4) { *x = GX + col * AW; *w = AW; }
	else { *x = GX + col * CW; *w = CW; }
}

void osk_draw_cell(const struct osk *o, unsigned short sel, int row, int col)
{
	int x, y, w, h, on = row == o->row && col == o->col;
	char b[8];
	const char *label = NULL;
	unsigned short fg = NC_TEXT;

	cell_rect(row, col, &x, &y, &w, &h);
	nd_fill(x, y, w, h, NC_BG);
	x += 3; y += 3; w -= 6; h -= 6;
	if (!on) nd_fill(x, y, w, h, NC_BORDER);
	nd_fill(x + (on ? 0 : 2), y + (on ? 0 : 2), w - (on ? 0 : 4), h - (on ? 0 : 4), on ? sel : NC_BOX);
	if (row < 4) {
		char c = osk_cell_char(o->layer, row, col);

		if (!c) return;
		b[0] = c; b[1] = 0;
		label = b;
		nd_ctext(x + w / 2, y + (h - 48) / 2, 3, fg, label);
		return;
	}
	switch (col) {
	case 0: label = LAYER_NAME[(o->layer + 1) % 3]; break;
	case 1: label = "Space"; break;
	case 2: label = "Del"; break;
	case 3: label = o->hidden ? "Show" : "Hide"; break;
	case 4: label = "Cancel"; fg = NC_WARN; break;
	default: label = "OK"; fg = NC_OK; break;
	}
	nd_ctext(x + w / 2, y + (h - 32) / 2, 2, on ? NC_TEXT : fg, label);
}

void osk_draw_field(const struct osk *o)
{
	const struct netui_host *h = nd_host();
	int x = GX, y = 140, w = h->g->w - 2 * GX, maxch = (w - 28) / 16, i, tx, start;
	char shown[OSK_TEXT_MAX + 2], m[96];
	int ok;

	nd_fill(0, y - 6, h->g->w, GY - y + 6, NC_BG);
	nd_fill(x - 3, y - 3, w + 6, 66, NC_BORDER);
	nd_fill(x, y, w, 60, NC_PANEL);
	start = o->len > maxch - 1 ? o->len - (maxch - 1) : 0;
	for (i = start; i < o->len; i++) shown[i - start] = o->hidden ? '*' : o->text[i];
	shown[o->len - start] = 0;
	tx = nd_utext(x + 14, y + 14, 2, NC_TEXT, shown, 0);
	nd_fill(x + 14 + tx + 2, y + 14, 12, 4, NC_DIM);               /* the cursor */
	nd_fill(x + 14 + tx + 2, y + 42, 12, 4, NC_DIM);
	ok = osk_check(o->mode, o->text, o->len, m, sizeof m);
	if (o->warn[0]) nd_utext(x, y + 74, 2, NC_WARN, o->warn, 0);
	else nd_utext(x, y + 74, 2, ok ? NC_OK : o->len ? NC_WARN : NC_DIM, m, 0);
	for (i = 0; i < 3; i++) {                                      /* layer tabs */
		int tx0 = x + i * 112;
		nd_fill(tx0, y + 116, 104, 40, i == o->layer ? NC_BORDER : NC_PANEL);
		nd_ctext(tx0 + 52, y + 124, 2, i == o->layer ? NC_TEXT : NC_DIM, LAYER_NAME[i]);
	}
	nd_rtext(x + w, y + 124, 2, NC_DIM, o->hidden ? "text hidden" : "text shown");
}

void osk_draw(const struct osk *o, unsigned short sel)
{
	const struct netui_host *h = nd_host();
	int r, c;

	nd_fill(0, 72, h->g->w, h->g->h - 56 - 72, NC_BG);
	nd_utext(GX, 92, 2, NC_TEXT, o->prompt, (h->g->w - 2 * GX) / 16);
	osk_draw_field(o);
	for (r = 0; r < OSK_ROWS; r++)
		for (c = 0; c < osk_row_len(o->layer, r); c++) osk_draw_cell(o, sel, r, c);
	nd_hint("D-pad move   A type   B delete   SELECT layer   START ok");
}
