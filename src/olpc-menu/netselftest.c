/* netselftest: unit checks of the on-screen keyboard logic (osk.c) and a key-sequence planner for the scripted UI
 * tests of the network screen.
 *   netselftest                          run the unit checks
 *   netselftest --plan START_MS GAP_MS TOKEN...
 *       prints olpc-menu --script lines ("<ms> key RIGHT tap") that move the keyboard cursor over the
 *       shortest D-pad/SELECT path to type each token and press A.  A token is text:STRING or act:NAME
 *       (NAME = layer space del hide cancel ok).  The cursor starts where osk_init puts it (override with
 *       a leading start:LAYER,ROW,COL,OCOL token). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netui.h"
#include "osk.h"

/* osk.c draws through these; the unit checks never draw */
static struct gfx fakeg = { NULL, 1200, 1200, 900 };
static struct netui_host fakehost = { &fakeg, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
const struct netui_host *nd_host(void) { return &fakehost; }
void nd_fill(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
void nd_frame(int x, int y, int w, int h, int t, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)t; (void)c; }
int  nd_utext(int x, int y, int sc, uint16_t fg, const char *s, int maxch) { (void)x; (void)y; (void)sc; (void)fg; (void)s; (void)maxch; return 0; }
void nd_ctext(int cx, int y, int sc, uint16_t fg, const char *s) { (void)cx; (void)y; (void)sc; (void)fg; (void)s; }
void nd_rtext(int rx, int y, int sc, uint16_t fg, const char *s) { (void)rx; (void)y; (void)sc; (void)fg; (void)s; }
void nd_hint(const char *s) { (void)s; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails = 1; } } while (0)

enum { M_UP, M_DOWN, M_LEFT, M_RIGHT, M_SEL, M_A, NMOVE };
static const char *const MOVE_KEY[NMOVE] = { "UP", "DOWN", "LEFT", "RIGHT", "SELECT", "A" };

static void apply(struct osk *o, int m)
{
	switch (m) {
	case M_UP: osk_move(o, 0, -1); break;
	case M_DOWN: osk_move(o, 0, 1); break;
	case M_LEFT: osk_move(o, -1, 0); break;
	case M_RIGHT: osk_move(o, 1, 0); break;
	case M_SEL: osk_next_layer(o); break;
	}
}

static int skey(const struct osk *o) { return ((o->layer * 5 + o->row) * 10 + o->col) * 10 + o->ocol; }

/* shortest path from o to a cell showing the wanted character (act = -1) or the action column act on row 4 */
static int plan(const struct osk *from, char want, int act, int *path, int cap)
{
	static int prev[3 * 5 * 10 * 10], pm[3 * 5 * 10 * 10], q[3 * 5 * 10 * 10 + 1];
	struct osk st[3 * 5 * 10 * 10];
	int qh = 0, qt = 0, i, k;

	for (i = 0; i < 3 * 5 * 10 * 10; i++) prev[i] = -2;
	st[skey(from)] = *from;
	prev[skey(from)] = -1;
	q[qt++] = skey(from);
	while (qh < qt) {
		int cur = q[qh++];
		struct osk *o = &st[cur];
		int hit = act >= 0 ? (o->row == 4 && o->col == act) : (o->row < 4 && osk_cell_char(o->layer, o->row, o->col) == want);

		if (hit) {
			int n = 0, p, tmp[400];

			for (p = cur; prev[p] >= 0; p = prev[p]) tmp[n++] = pm[p];
			if (n + 1 > cap) return -1;
			for (i = 0; i < n; i++) path[i] = tmp[n - 1 - i];
			path[n++] = M_A;
			return n;
		}
		for (k = 0; k < 5; k++) {
			struct osk nx = *o;
			int nk;

			apply(&nx, k);
			nk = skey(&nx);
			if (prev[nk] != -2) continue;
			prev[nk] = cur; pm[nk] = k;
			st[nk] = nx;
			q[qt++] = nk;
		}
	}
	return -1;
}

static int act_col(const char *n)
{
	static const char *const N[] = { "layer", "space", "del", "hide", "cancel", "ok" };
	int i;

	for (i = 0; i < 6; i++) if (!strcmp(n, N[i])) return i;
	return -1;
}

static int do_plan(int argc, char **argv)
{
	long ms = atol(argv[0]), gap = atol(argv[1]);
	struct osk o;
	int i;

	osk_init(&o, OSK_KEY_WPA, "x");
	for (i = 2; i < argc; i++) {
		const char *t = argv[i];

		if (!strncmp(t, "start:", 6)) {
			sscanf(t + 6, "%d,%d,%d,%d", &o.layer, &o.row, &o.col, &o.ocol);
		} else if (!strncmp(t, "text:", 5)) {
			const char *c;

			for (c = t + 5; *c; c++) {
				int path[400], n, k;

				n = *c == ' ' ? plan(&o, 0, 1, path, 400) : plan(&o, *c, -1, path, 400);
				if (n < 0) { fprintf(stderr, "cannot reach '%c'\n", *c); return 1; }
				for (k = 0; k < n; k++) {
					printf("%ld key %s tap\n", ms, MOVE_KEY[path[k]]);
					ms += gap;
					if (path[k] != M_A) apply(&o, path[k]);
				}
				if (osk_press(&o) != OSK_NONE) return 1;
			}
		} else if (!strcmp(t, "sel")) {
			printf("%ld key SELECT tap\n", ms);
			ms += gap;
			osk_next_layer(&o);
		} else if (!strncmp(t, "dump:", 5)) {
			ms += 300;
			printf("%ld dump %s\n", ms, t + 5);
			ms += 300;
		} else if (!strncmp(t, "act:", 4)) {
			int path[400], n, k, a = act_col(t + 4);

			if (a < 0) return 1;
			n = plan(&o, 0, a, path, 400);
			if (n < 0) return 1;
			for (k = 0; k < n; k++) {
				printf("%ld key %s tap\n", ms, MOVE_KEY[path[k]]);
				ms += gap;
				if (path[k] != M_A) apply(&o, path[k]);
			}
			if (a == 3 || a == 0 || a == 1 || a == 2) osk_press(&o);
		}
	}
	printf("# next free time %ld\n", ms);
	return 0;
}

static void unit(void)
{
	struct osk o;
	int l, r, c, i;
	char seen[128], m[120];

	/* every printable ASCII character can be typed: letters/digits on layers 0 and 1, the rest on layer 2, space on the action row */
	memset(seen, 0, sizeof seen);
	for (l = 0; l < 3; l++)
		for (r = 0; r < 4; r++)
			for (c = 0; c < osk_row_len(l, r); c++) {
				char ch = osk_cell_char(l, r, c);
				if (ch) seen[(int)ch] = 1;
			}
	seen[' '] = 1;
	for (i = 0x20; i < 0x7f; i++) CHECK(seen[i], "character 0x%02x (%c) is not reachable", i, i);
	CHECK(osk_cell_char(0, 3, 12) == 0 && osk_cell_char(2, 3, 5) == 0, "empty cells report 0");

	/* planner reaches every typeable character */
	osk_init(&o, OSK_KEY_WPA, "p");
	for (i = 0x21; i < 0x7f; i++) {
		int path[400];
		CHECK(plan(&o, (char)i, -1, path, 400) > 0, "no key path to %c", i);
	}

	/* wrap-around */
	osk_init(&o, OSK_KEY_WPA, "p");
	o.row = 0; o.col = 0; o.ocol = 0;
	osk_move(&o, -1, 0);
	CHECK(o.col == 9 && o.row == 0, "left wraps to the last column (col %d)", o.col);
	osk_move(&o, 1, 0);
	CHECK(o.col == 0, "right wraps to the first column");
	osk_move(&o, 0, -1);
	CHECK(o.row == 4, "up from the first row wraps to the action row");
	osk_move(&o, 0, 1);
	CHECK(o.row == 0, "down from the action row wraps to the first row");
	osk_move(&o, 0, -1); osk_move(&o, -1, 0);
	CHECK(o.row == 4 && o.col == OSK_NACT - 1, "left wraps on the action row");

	/* short last row of the symbol layer: the column is clamped and comes back */
	osk_init(&o, OSK_KEY_WPA, "p");
	o.layer = 2; o.row = 2; o.col = o.ocol = 8;
	osk_move(&o, 0, 1);
	CHECK(o.row == 3 && o.col == 1, "short row clamps the column (col %d)", o.col);
	osk_move(&o, 0, -1);
	CHECK(o.row == 2 && o.col == 8, "the wanted column comes back (col %d)", o.col);

	/* typing, backspace, cancel on empty */
	osk_init(&o, OSK_KEY_WPA, "p");
	o.row = 1; o.col = 0;
	CHECK(osk_press(&o) == OSK_NONE && o.len == 1 && o.text[0] == 'q', "A types q");
	CHECK(osk_back(&o) == OSK_NONE && o.len == 0, "B deletes");
	CHECK(osk_back(&o) == OSK_CANCEL, "B on an empty field cancels");
	o.row = 4; o.col = 1;
	osk_press(&o);
	CHECK(o.len == 1 && o.text[0] == ' ', "Space key types a space");
	o.col = 2; osk_press(&o);
	CHECK(o.len == 0, "Del key deletes and never cancels");
	o.col = 4;
	CHECK(osk_press(&o) == OSK_CANCEL, "Cancel key cancels");
	o.col = 3; osk_press(&o);
	CHECK(o.hidden == 1, "Hide key hides");
	osk_press(&o);
	CHECK(o.hidden == 0, "Show key shows again");
	osk_next_layer(&o); osk_next_layer(&o); osk_next_layer(&o);
	CHECK(o.layer == 0, "three SELECTs come back to the first layer");

	/* maximum length per mode */
	osk_init(&o, OSK_KEY_WPA, "p");
	o.row = 1; o.col = 0;
	for (i = 0; i < 100; i++) osk_press(&o);
	CHECK(o.len == 64, "WPA field stops at 64 characters (%d)", o.len);
	osk_init(&o, OSK_SSID, "p");
	o.row = 1;
	for (i = 0; i < 100; i++) osk_press(&o);
	CHECK(o.len == 32, "SSID field stops at 32 characters (%d)", o.len);
	osk_init(&o, OSK_KEY_WEP, "p");
	o.row = 1;
	for (i = 0; i < 100; i++) osk_press(&o);
	CHECK(o.len == 26, "WEP field stops at 26 characters (%d)", o.len);

	/* validation */
	CHECK(!osk_check(OSK_KEY_WPA, "", 0, m, sizeof m), "empty WPA key invalid");
	CHECK(!osk_check(OSK_KEY_WPA, "1234567", 7, m, sizeof m) && strstr(m, "need 8 to 63"), "7 chars invalid: %s", m);
	CHECK(osk_check(OSK_KEY_WPA, "12345678", 8, m, sizeof m) && strstr(m, "OK"), "8 chars valid: %s", m);
	{
		char k63[70], k64[70], k64x[70];
		memset(k63, 'a', 63); k63[63] = 0;
		memset(k64, 'a', 64); k64[64] = 0;
		memset(k64x, 'a', 64); k64x[64] = 0; k64x[10] = 'z';
		CHECK(osk_check(OSK_KEY_WPA, k63, 63, m, sizeof m), "63 chars valid");
		CHECK(osk_check(OSK_KEY_WPA, k64, 64, m, sizeof m), "64 hex digits valid");
		CHECK(!osk_check(OSK_KEY_WPA, k64x, 64, m, sizeof m), "64 non-hex invalid");
	}
	CHECK(osk_check(OSK_KEY_WEP, "abcde", 5, m, sizeof m), "WEP 5 valid");
	CHECK(osk_check(OSK_KEY_WEP, "abcdefghijklm", 13, m, sizeof m), "WEP 13 valid");
	CHECK(osk_check(OSK_KEY_WEP, "0123456789", 10, m, sizeof m), "WEP 10 hex valid");
	CHECK(!osk_check(OSK_KEY_WEP, "abcdefghij", 10, m, sizeof m), "WEP 10 non-hex invalid");
	CHECK(!osk_check(OSK_KEY_WEP, "abcdef", 6, m, sizeof m), "WEP 6 invalid");
	CHECK(!osk_check(OSK_SSID, "", 0, m, sizeof m) && osk_check(OSK_SSID, "x", 1, m, sizeof m), "SSID needs 1+ characters");

	/* OK refused with a reason, accepted when valid */
	osk_init(&o, OSK_KEY_WPA, "p");
	o.row = 1; o.col = 0;
	osk_press(&o);
	{
		const char *why = NULL;
		CHECK(osk_ok(&o, &why) == OSK_NONE && why && strstr(why, "need 8"), "OK refused for a 1-character key");
	}
	for (i = 0; i < 7; i++) osk_press(&o);
	CHECK(osk_ok(&o, NULL) == OSK_OK, "OK accepted for 8 characters");

	/* wipe */
	osk_wipe(&o);
	for (i = 0; i < OSK_TEXT_MAX; i++) if (o.text[i]) break;
	CHECK(i == OSK_TEXT_MAX && o.len == 0, "wipe clears the whole buffer");
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "--plan")) return do_plan(argc - 2, argv + 2);
	unit();
	puts(fails ? "netselftest: FAILED" : "netselftest: all checks passed");
	return fails;
}
