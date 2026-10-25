// scenes.h - ASCII wallpapers for lumdm. Included by lumdm.c (single translation unit).
//
// Scenes are drawn on a character canvas, then written out once; colours come from the
// active theme roles (R_FG / R_DIM / R_ACCENT / R_ERR) so every theme recolours every scene.
// Sprites are plain string arrays: edit them freely. Mirrored sprites flip / \ ( ) < > ` ' too.
// Everything is ASCII, so it also looks right on the stock console font.
enum { WP_FESTIVAL, WP_OCEAN, WP_FOREST, WP_NONE, NWP };
static const char *const WP_NAMES[NWP] = {"festival", "ocean", "forest", "none"};

// ---------------------------------------------------------------- canvas ----
typedef struct { char ch; unsigned char role; } Cell;   // ch == 0: transparent
static Cell *cv;
static size_t cv_cap;
static unsigned rs = 1;
static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }
static int rr(int n) { return n > 0 ? (int)(rnd() % (unsigned)n) : 0; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool cv_reset(void) {
    size_t n = (size_t)g.rows * (size_t)g.cols;
    if (n > cv_cap) {
        Cell *p = realloc(cv, n * sizeof *cv);
        if (!p) return false;
        cv = p;
        cv_cap = n;
    }
    memset(cv, 0, n * sizeof *cv);
    return true;
}
static void cv_put(int r, int c, char ch, int role) {   // rows 1 and g.rows belong to the bars
    if (!ch || r < 2 || r > g.rows - 1 || c < 1 || c > g.cols) return;
    Cell *x = &cv[(size_t)(r - 1) * (size_t)g.cols + (size_t)(c - 1)];
    x->ch = ch;
    x->role = (unsigned char)role;
}
static void cv_str(int r, int c, const char *s, int role) {
    for (; *s; s++, c++) if (*s != ' ') cv_put(r, c, *s, role);
}

typedef struct { const char *const *row; int h; } Spr;
#define SPR(a) {a, (int)(sizeof(a) / sizeof *(a))}
static char mchar(char ch) {
    switch (ch) {
    case '/': return '\\';  case '\\': return '/';
    case '(': return ')';   case ')': return '(';
    case '<': return '>';   case '>': return '<';
    case '[': return ']';   case ']': return '[';
    case '{': return '}';   case '}': return '{';
    case '`': return '\'';  case '\'': return '`';
    default: return ch;
    }
}
static int spr_w(const Spr *s) {
    int w = 0;
    for (int i = 0; i < s->h; i++) w = imax(w, (int)strlen(s->row[i]));
    return w;
}
// Top-left at (r,c). Spaces between a row's first and last glyph are opaque (they hide what is behind).
static void spr_draw(const Spr *s, int r, int c, int role, bool mir) {
    int w = spr_w(s);
    if (w > 95) return;
    for (int i = 0; i < s->h; i++) {
        char t[96];
        int l = (int)strlen(s->row[i]);
        for (int x = 0; x < w; x++) {
            char ch = x < l ? s->row[i][x] : ' ';
            t[mir ? w - 1 - x : x] = mir ? mchar(ch) : ch;
        }
        int a = 0, b = w - 1;
        while (a <= b && t[a] == ' ') a++;
        while (b >= a && t[b] == ' ') b--;
        for (int x = a; x <= b; x++) cv_put(r + i, c + x, t[x], role);
    }
}
static bool hits_panel(int r, int c, int h, int w, int m) {   // does a box touch the login panel?
    return r - m < pg.y0 + pg.h && r + h + m > pg.y0 && c - m < pg.x0 + pg.w && c + w + m > pg.x0;
}

// ---------------------------------------------------------------- common ----
static void wp_stars(int r0, int r1, int dens) {
    for (int r = r0; r <= r1; r++)
        for (int c = 2; c <= g.cols; c++) {
            unsigned h = bg_hash(r, c);
            if ((int)(h & 255) >= dens) continue;
            unsigned k = (h >> 8) & 7;
            cv_put(r, c, k == 0 ? '*' : k < 3 ? '+' : '.', k == 0 ? R_FG : k < 3 ? R_ACCENT : R_DIM);
        }
}
static const char *const MOON_A[] = {" .-. ", "(   )", " `-' "};
static const Spr MOON = SPR(MOON_A);
// Try a few spots, take the first one the login panel does not cover.
static bool put_free(const Spr *s, const int (*cand)[2], int n, int role, bool mir) {
    int w = spr_w(s);
    for (int i = 0; i < n; i++) {
        if (cand[i][0] < 2 || cand[i][1] < 1 || hits_panel(cand[i][0], cand[i][1], s->h, w, 1)) continue;
        spr_draw(s, cand[i][0], cand[i][1], role, mir);
        return true;
    }
    return false;
}

typedef struct { int c, h, sl; } Peak;                   // sl: 1/4 row lost per column
static int ridge_h(const Peak *p, int n, int c, int seed) {
    int best = 0;
    for (int i = 0; i < n; i++) best = imax(best, p[i].h - abs(c - p[i].c) * p[i].sl / 4);
    if (best > 2) best += (int)(bg_hash(c / 2, (unsigned)seed) & 1);
    return best;
}
// One mountain range standing on row `base`. far: outline only (dim); near: shaded, snow-capped peaks.
static void mountains(int base, int maxh, int seed, bool far) {
    Peak pk[32];
    int n = clampi(g.cols / 22 + 2, 3, 32);
    rs = (unsigned)seed * 2654435761u + 977u;
    for (int i = 0; i < n; i++) {
        pk[i].c = (i * g.cols) / n + rr(imax(1, g.cols / n));
        pk[i].h = maxh * 55 / 100 + rr(maxh * 45 / 100 + 1);
        pk[i].sl = 2 + rr(3);
    }
    for (int c = 1; c <= g.cols; c++) {
        int h = imin(ridge_h(pk, n, c, seed), base - 2);
        int hp = ridge_h(pk, n, c - 1, seed), hn = ridge_h(pk, n, c + 1, seed);
        if (h <= 0) continue;
        bool snow = h * 100 > maxh * 78;
        for (int d = 0; d < h; d++) {
            int y = base - h + 1 + d;
            if (d == 0) {
                char ch = h > hp && h > hn ? '^' : hn > h ? '/' : hn < h ? '\\' : h > hp ? '^' : '-';
                cv_put(y, c, ch, far ? R_DIM : snow ? R_FG : R_ACCENT);
            } else if (!far) {
                bool lit = hn > h;                       // rising side catches the light
                cv_put(y, c, d == 1 && snow ? '\'' : lit ? ':' : '#', d == 1 && snow ? R_FG : R_DIM);
            }
        }
    }
}

// Pine: tiered triangle with a trunk. base = trunk row, h = total rows.
static void pine(int base, int c, int h, int edge, int fill) {
    int fol = h - 1, nt = fol >= 10 ? 3 : fol >= 5 ? 2 : 1, th = (fol + nt - 1) / nt;
    cv_put(base, c, '|', edge);
    for (int i = 0; i < fol; i++) {
        int k = i / th, j = i % th, half = j + k, y = base - fol + i;
        if (half == 0) { cv_put(y, c, '^', edge); continue; }
        for (int x = -half + 1; x < half; x++) cv_put(y, c + x, (x + j) & 1 ? '^' : ' ', fill);
        cv_put(y, c - half, '/', edge);
        cv_put(y, c + half, '\\', edge);
    }
}
// Broadleaf: ellipse of foliage on a trunk.
static void oak(int base, int c, int h, int w, int edge, int fill) {
    int fol = h - 2;
    cv_put(base, c, '|', edge);
    cv_put(base - 1, c, '|', edge);
    for (int i = 0; i < fol; i++) {
        float t = fol > 1 ? ((float)i - (float)(fol - 1) / 2) / ((float)fol / 2) : 0;
        int half = (int)lroundf((float)w * sqrtf(fmaxf(0, 1 - t * t)));
        int y = base - 2 - (fol - 1) + i;
        for (int x = -half + 1; x < half; x++) cv_put(y, c + x, ((x * 7 + i * 3) & 3) ? ' ' : '@', fill);
        cv_put(y, c - half, '(', edge);
        cv_put(y, c + half, ')', edge);
    }
}

// ------------------------------------------------------------- festival ----
static const char *const P_STAND[] = {" o ", "/|\\", "/ \\"};
static const char *const P_CHEER[] = {"\\o/", " | ", "/ \\"};
static const char *const P_WAVE[] = {" o/", "/| ", "/ \\"};
static const char *const P_ARMS[] = {" o ", "<|>", "/ \\"};
static const char *const P_KID[] = {" o ", "/|\\"};
static const char *const P_PAIR[] = {" o   o ", "/|\\_/|\\", "/ \\ / \\"};
static const Spr PEOPLE[] = {SPR(P_STAND), SPR(P_CHEER), SPR(P_WAVE), SPR(P_ARMS), SPR(P_KID), SPR(P_STAND)};
static const Spr PAIR = SPR(P_PAIR);
static const char *const T_SMALL[] = {"  /\\  ", " /  \\ ", "/_/\\_\\"};
static const char *const T_BIG[] = {"    |>    ", "   /^\\    ", "  /   \\   ", " /     \\  ", "/__ /\\ __\\"};
static const Spr TENT_S = SPR(T_SMALL), TENT_B = SPR(T_BIG);
static const char *const FIRE_A[] = {"  ) (  ", " ( ) ) ", " )(_(( ", "\\_/|\\_/"};
static const Spr FIRE = SPR(FIRE_A);
static const char *const LANT_A[] = {" ^ ", "(#)"};
static const Spr LANT = SPR(LANT_A);

static void firework(int r, int c, int rad, int role) {
    for (int s = 1; s <= rad; s++) {
        bool tip = s == rad;
        cv_put(r - s, c, tip ? '*' : '|', role);
        cv_put(r + s, c, tip ? '*' : '|', role);
        cv_put(r, c - 2 * s, tip ? '*' : '-', role);
        cv_put(r, c + 2 * s, tip ? '*' : '-', role);
        cv_put(r - s, c - 2 * s, tip ? '*' : '\\', role);
        cv_put(r - s, c + 2 * s, tip ? '*' : '/', role);
        cv_put(r + s, c - 2 * s, tip ? '*' : '/', role);
        cv_put(r + s, c + 2 * s, tip ? '*' : '\\', role);
    }
    cv_put(r, c, '*', R_FG);
    cv_put(r - rad - 1, c, '.', R_DIM);
    cv_put(r + rad + 1, c, '.', R_DIM);
    cv_put(r, c - 2 * rad - 2, '.', R_DIM);
    cv_put(r, c + 2 * rad + 2, '.', R_DIM);
}
static void bunting(int r1, int c1, int r2, int c2, int sag) {
    int n = c2 - c1, prev = r1;
    if (n < 6 || n > 26) return;                          // only between neighbouring tents
    for (int c = c1 + 1; c < c2; c++) {
        int t = c - c1, y = r1 + (r2 - r1) * t / n + sag * 4 * t * (n - t) / (n * n);
        cv_put(y, c, y < prev ? '/' : y > prev ? '\\' : '-', R_DIM);
        if (t % 3 == 0) cv_put(y + 1, c, 'v', (t / 3) & 1 ? R_ACCENT : R_ERR);
        prev = y;
    }
}
static bool *occ;
static bool span_free(int c0, int w) {
    for (int c = c0; c < c0 + w; c++) if (c < 1 || c > g.cols || occ[c]) return false;
    return true;
}
static void span_mark(int c0, int w) {
    for (int c = c0; c < c0 + w; c++) if (c >= 1 && c <= g.cols) occ[c] = true;
}
static void person(int c, int fy, int role) {
    const Spr *p = &PEOPLE[rr(6)];
    spr_draw(p, fy - p->h + 1, c, role, rr(2));
}

static void wp_festival(void) {
    int H = g.rows, W = g.cols;
    bool big = H >= 34;
    int fh = big ? 5 : 3, fy = H - 2, mb = fy - fh - 1;
    if (H < 14 || mb < 7) { wp_stars(2, H - 2, 12); return; }
    int maxh = clampi(H / 3, 5, 18);
    maxh = imin(maxh, mb - 4);

    wp_stars(2, mb - 2, 13);
    {   // moon
        int cand[][2] = {{3, W * 3 / 4}, {3, W / 6}, {4, W * 9 / 10}, {3, W / 2}};
        put_free(&MOON, cand, 4, R_FG, false);
    }
    rs = 4242;                                            // fireworks: one or two bursts in the open sky
    int rad = clampi(H / 9, 2, 5);
    for (int i = 0; i < 3; i++) {
        int c = i == 0 ? W / 7 : i == 1 ? W * 6 / 7 : W / 2, r = 3 + rad + rr(imax(1, mb / 4));
        if (!hits_panel(r - rad - 1, c - 2 * rad - 2, 2 * rad + 3, 4 * rad + 5, 1)) firework(r, c, rad, i == 1 ? R_ERR : R_ACCENT);
    }
    for (int i = 0; i < clampi(W / 14, 3, 12); i++) {     // sky lanterns drifting up
        int r = 3 + rr(imax(1, mb - maxh - 3)), c = 3 + rr(W - 8);
        if (!hits_panel(r, c, LANT.h, 3, 1)) spr_draw(&LANT, r, c, R_ERR, false);
    }

    mountains(mb, maxh * 2 / 3, 2, true);
    mountains(mb, maxh, 1, false);

    for (int c = 1; c <= W; c++) {                        // meadow
        unsigned h = bg_hash(fy + 1, c);
        if ((h & 7) < 3) cv_put(fy + 1, c, ",.'\""[(h >> 4) & 3], R_DIM);
        if (((h >> 9) & 31) == 0) cv_put(fy, c, ((h >> 14) & 1) ? '*' : 'v', R_ACCENT);
    }

    occ = calloc((size_t)W + 2, 1);
    if (!occ) return;
    rs = 777;
    int tops[64][2], nt = 0;
    int nfire = W >= 110 ? 2 : 1;
    int fires[2];
    for (int i = 0; i < nfire; i++) {                     // bonfire circles first
        fires[i] = nfire == 1 ? W * 3 / 10 : W * (2 + 5 * i) / 10 - 3;
        span_mark(fires[i] - 11, 29);
    }
    for (int c = 2 + rr(4); c < W - 6;) {
        if (occ[c]) { c++; continue; }
        int k = rr(100);
        const Spr *t = big ? &TENT_B : &TENT_S;
        int tw = spr_w(t);
        if (k < 26 && span_free(c, tw + 1) && nt < 60) {
            spr_draw(t, fy - t->h + 1, c, R_ACCENT, rr(2));
            tops[nt][0] = fy - t->h; tops[nt++][1] = c + tw / 2;
            span_mark(c, tw + 1);
            c += tw + 1 + rr(3);
        } else if (k < 52 && span_free(c, 12)) {
            int n = 2 + rr(3);
            for (int i = 0; i < n; i++) person(c + i * 4, fy, i & 1 ? R_FG : R_ACCENT);
            span_mark(c, n * 4);
            c += n * 4 + rr(4);
        } else if (k < 68 && span_free(c, 9)) {
            spr_draw(&PAIR, fy - PAIR.h + 1, c, R_FG, false);
            span_mark(c, 9);
            c += 9 + rr(4);
        } else c += 2 + rr(5);
    }
    for (int i = 0; i + 1 < nt; i++) bunting(tops[i][0], tops[i][1], tops[i + 1][0], tops[i + 1][1], 1 + big);
    for (int i = 0; i < nfire; i++) {                     // people around the fire, drawn last so they stay on top
        int c = fires[i];
        spr_draw(&FIRE, fy - FIRE.h + 1, c, R_ERR, false);
        cv_put(fy - FIRE.h, c + 3, '.', R_ERR);
        static const int off[] = {-10, -6, 9, 13, -3 + 0, 7};
        for (int j = 0; j < 4; j++) person(c + off[j], fy, j & 1 ? R_ACCENT : R_FG);
    }
    free(occ);
    occ = nullptr;
}

// ---------------------------------------------------------------- ocean ----
static const char *const DOL_L_A[] = {"         _.--._         ", "      ,-'   /|  `-.     ",
                                      "   ,-'     / |      `.   ", "  /  .--. /  |   o    `. ",
                                      " <__/    `'--'`-.___.-'`>"};
static const char *const DOL_M_A[] = {"      ,--.      ", "   ,-'  /|`-.   ", " ,'    / | o `. ", "<_.-'`'`-'`--.-'>"};
static const char *const DOL_S_A[] = {"   _,-._   ", " ,'  /| `. ", "<_,-'`-.o`>"};
static const Spr DOL[3] = {SPR(DOL_L_A), SPR(DOL_M_A), SPR(DOL_S_A)};
static const char *const BOAT_A[] = {"    |\\    ", "    | \\   ", "   /|__\\  ", "  /_|___\\ ", " \\_______/"};
static const Spr BOAT = SPR(BOAT_A);
static const char *const FIN_A[] = {"  /|   ", "_/ |__ "};
static const Spr FIN = SPR(FIN_A);

static void wp_ocean(void) {
    int H = g.rows, W = g.cols;
    if (H < 12) { wp_stars(2, H - 2, 12); return; }
    int hz = clampi(2 + (H - 2) * 40 / 100, 5, H - 6);
    wp_stars(2, hz - 1, 14);
    {
        int cand[][2] = {{3, W * 3 / 4}, {3, W / 8}, {3, W * 9 / 10}, {4, W / 2}};
        if (!put_free(&MOON, cand, 4, R_FG, false)) cv_put(3, W * 3 / 4, 'O', R_FG);
    }
    rs = 31337;
    for (int i = 0; i < clampi(W / 25, 2, 8); i++) {      // gulls
        int r = 3 + rr(imax(1, hz - 4)), c = 3 + rr(W - 8);
        if (!hits_panel(r, c, 1, 3, 1)) cv_str(r, c, rr(2) ? "\\v/" : "v", R_DIM);
    }
    {
        int c = W - 13, cand[][2] = {{hz - BOAT.h + 1, c}, {hz - BOAT.h + 1, 3}};
        put_free(&BOAT, cand, 2, R_FG, false);
    }
    for (int c = 1; c <= W; c++) if (bg_hash(hz, c) & 3) cv_put(hz, c, '-', R_DIM);

    int mc = W * 3 / 4;                                   // moon glitter column
    for (int r = hz + 1; r <= H - 1; r++) {
        int d = r - hz;
        for (int c = 1 + rr(4); c <= W;) {
            int len = 2 + rr(3 + d / 3);
            bool glitter = abs(c + len / 2 - mc) <= 1 + d / 2;
            for (int i = 0; i < len; i++) {
                char ch = i == 0 && rr(5) == 0 ? ',' : len > 4 && i == len / 2 ? '-' : '~';
                cv_put(r, c + i, ch, glitter ? R_FG : ch == ',' ? R_ACCENT : R_DIM);
            }
            c += len + 2 + rr(3 + imax(0, 8 - d));
        }
    }
    int size = H >= 36 ? 0 : H >= 26 ? 1 : 2;
    int nd = clampi(W / 45, 1, 5);
    for (int i = 0; i < nd; i++) {
        bool mir = rr(2);
        for (int tries = 0; tries < 24; tries++) {
            const Spr *s = &DOL[imin(2, size + (tries >= 12))];
            int w = spr_w(s), c = 2 + rr(imax(1, W - w - 3)), bot = hz + 2 + rr(imax(1, H - 2 - hz - 1));
            int r = bot - s->h + 1;
            if (r < 3 || bot > H - 2 || hits_panel(r, c, s->h, w, 1)) continue;
            for (int k = 0; k < 5; k++) cv_put(r + rr(s->h + 1), c - 2 + rr(w + 4), ".'o,"[rr(4)], R_ACCENT);   // spray
            spr_draw(s, r, c, R_FG, mir);
            break;
        }
    }
    for (int i = 0; i < clampi(W / 40, 1, 4); i++) {      // fins cutting the surface
        int r = H - 3 - rr(imax(1, (H - hz) / 3)), c = 2 + rr(imax(1, W - 10));
        if (r > hz + 1 && !hits_panel(r, c, FIN.h, 7, 1)) spr_draw(&FIN, r, c, R_DIM, rr(2));
    }
}

// --------------------------------------------------------------- forest ----
static const char *const OWL_A[] = {"{o,o}", "|)__)", "-\"-\"-"};
static const char *const RABBIT_A[] = {" (\\_/) ", " (o.o) ", "(\")_(\")"};
static const char *const DEER_A[] = {"         Y/ ", "    ,---.\\| ", "  _/     (o>", " (_ ____ /  ", "   || ||    "};
static const char *const FOX_A[] = {" /\\_/\\        ", "( o.o )  ___  ", " > ^ <__/   `~", "  |_|_|      "};
static const char *const BEAR_A[] = {" (\\___/) ", " (o   o) ", "  \\ v /  ", " /|   |\\ ", "(_|___|_)"};
static const Spr OWL = SPR(OWL_A), RABBIT = SPR(RABBIT_A), DEER = SPR(DEER_A), FOX = SPR(FOX_A), BEAR = SPR(BEAR_A);

static void wp_forest(void) {
    int H = g.rows, W = g.cols;
    if (H < 14) { wp_stars(2, H - 2, 12); return; }
    int fy = H - 2, hz = fy - 3;
    wp_stars(2, hz - 4, 12);
    {
        int cand[][2] = {{3, W * 7 / 10}, {3, W / 5}, {3, W * 9 / 10}, {4, W / 2}};
        put_free(&MOON, cand, 4, R_FG, false);
    }
    rs = 2024;
    for (int i = 0; i < clampi(W / 28, 2, 7); i++) {      // birds
        int r = 3 + rr(imax(1, hz / 3)), c = 3 + rr(W - 8);
        if (!hits_panel(r, c, 1, 3, 1)) cv_str(r, c, rr(2) ? "\\v/" : "v", R_DIM);
    }
    mountains(hz - 1, clampi(H / 5, 3, 8), 5, true);      // distant hills
    int hh = clampi(H / 7, 3, 5);
    for (int c = 2; c < W - 3; c += 3 + rr(4)) pine(hz, c, hh, R_DIM, R_DIM);              // far treeline
    int mh = clampi(H / 4, 5, 10);
    for (int c = 4 + rr(6); c < W - 4; c += 9 + rr(10)) {                                   // middle row
        if (rr(5)) pine(hz + 1, c, mh - rr(3), R_ACCENT, R_DIM);
        else oak(hz + 1, c, mh - 2, 2 + mh / 5, R_ACCENT, R_DIM);
    }
    int nh = clampi(H * 55 / 100, 8, 20);                 // big trees frame the scene
    int tcol[4] = {W / 14, W / 6, W - W / 14, W - W / 6 - 2};
    for (int i = 0; i < 4; i++) {
        int h = nh - (i & 1) * 3;
        if (i & 1) oak(fy, tcol[i], h - 4, 2 + h / 6, R_FG, R_ACCENT);
        else pine(fy, tcol[i], h, R_FG, R_ACCENT);
    }
    for (int c = 1; c <= W; c++)                           // forest floor
        for (int r = hz + 1; r <= H - 1; r++) {
            unsigned h = bg_hash(r, c);
            if ((h & 15) < (unsigned)(2 + (r - hz))) cv_put(r, c, ",.'\"`"[(h >> 5) % 5], R_DIM);
            if (r == fy + 1 && ((h >> 9) & 7) == 0) cv_put(r, c, 'w', R_ACCENT);
        }
    {   // an owl on a low branch of the nearest big tree on the left
        int bc = tcol[0] + 2, br = fy - nh / 2;
        if (!hits_panel(br - OWL.h, bc, OWL.h + 1, 7, 1)) {
            cv_str(br, tcol[0] + 1, "=======", R_ACCENT);
            spr_draw(&OWL, br - OWL.h, bc + 1, R_FG, false);
        }
    }
    struct { const Spr *s; int role; } an[] = {{&DEER, R_FG}, {&FOX, R_FG}, {&RABBIT, R_FG}, {&BEAR, R_FG}, {&RABBIT, R_FG}};
    int na = W >= 100 ? 5 : W >= 70 ? 4 : 3;
    bool used[5] = {0};
    bool *taken = calloc((size_t)W + 2, 1);
    if (!taken) return;
    for (int i = 0; i < na; i++) {
        for (int tries = 0; tries < 30; tries++) {
            int k = rr(5);
            if (used[k] && tries < 20) continue;
            if (an[k].s == &BEAR && H < 26) continue;
            int w = spr_w(an[k].s), c = 2 + rr(imax(1, W - w - 3)), r = fy - an[k].s->h + 1;
            bool near_tree = false;
            for (int t = 0; t < 4; t++) if (abs(c + w / 2 - tcol[t]) < 8 + w / 2) near_tree = true;
            bool clash = false;
            for (int x = c - 2; x < c + w + 2; x++) if (x >= 1 && x <= W && taken[x]) clash = true;
            if (near_tree || clash || hits_panel(r, c, an[k].s->h, w, 1)) continue;
            spr_draw(an[k].s, r, c, an[k].role, rr(2));
            for (int x = c; x < c + w && x <= W; x++) taken[x] = true;
            used[k] = true;
            break;
        }
    }
    free(taken);
    for (int i = 0; i < clampi(W / 7, 4, 24); i++) {       // fireflies
        int r = hz - 2 + rr(imax(1, H - hz)), c = 2 + rr(W - 3);
        if (r > 2 && r < H - 1 && !hits_panel(r, c, 1, 1, 0)) cv_put(r, c, rr(2) ? '*' : '.', R_ACCENT);
    }
}

// ------------------------------------------------------------- build/emit ----
static void build_wallpaper(void) {
    if (g.wp == WP_NONE || g.rows < 10 || g.cols < 30 || !cv_reset()) { if (cv) memset(cv, 0, cv_cap * sizeof *cv); return; }
    switch (g.wp) {
    case WP_OCEAN: wp_ocean(); break;
    case WP_FOREST: wp_forest(); break;
    default: wp_festival(); break;
    }
}
static void emit_wallpaper(void) {
    if (!cv || g.wp == WP_NONE || g.rows < 10 || g.cols < 30) return;
    for (int r = 2; r <= g.rows - 1; r++) {
        const Cell *row = cv + (size_t)(r - 1) * (size_t)g.cols;
        int cur = -1, next = 0;
        bool bgset = false;
        for (int c = 1; c <= g.cols; c++) {
            const Cell *x = &row[c - 1];
            if (!x->ch) continue;
            if (!bgset) { bg_cell(r); bgset = true; }
            if (next != c) at(r, c);
            if (x->role != cur) { color(x->role, false); cur = x->role; }
            put("%c", x->ch);
            next = c + 1;
            if (on > sizeof ob - 512) flush();
        }
    }
}
