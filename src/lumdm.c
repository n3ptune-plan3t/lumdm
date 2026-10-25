// lumdm - a tiny, pretty, TTY-based display manager for X11 and Wayland sessions.
// Language: C23 (ISO/IEC 9899:2024).  Only dependency: libpam (+ libm).
// Optional: system TrueType/OpenType fonts on the Linux console (src/vtfont.h, stb_truetype),
// ASCII wallpapers (src/scenes.h).
// Config: /etc/lumdm.conf (system) then ~/.config/lumdm.conf (per-user, restricted).
// Build: cc -std=c23 -O2 -s -o lumdm lumdm.c -lpam -lm  (GCC 13: use -std=c2x)
// Try the UI without root/PAM:  ./lumdm --preview     Check your config:  ./lumdm --check [user]
// Preview a font:  ./lumdm --font-test [name|path] [px]
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <math.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <pwd.h>
#include <security/pam_appl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define ESC "\x1b"
#define CONF "/etc/lumdm.conf"
#define STATE_DIR "/var/lib/lumdm"
#define STATE STATE_DIR "/state"
enum {
    MAX_SESS = 128,
    NAME_LEN = 64,
    CMD_LEN = 256,
    USER_LEN = 64,
    PW_LEN = 256,
    NFKEY = 12,
};

typedef struct { char act[CMD_LEN]; char label[24]; unsigned char src; } FKey;   // src: 0 default, 1 system, 2 user

typedef enum { K_X11, K_WAYLAND, K_SHELL } Kind;
typedef struct {
    char id[NAME_LEN], name[NAME_LEN], desk[NAME_LEN], exec[CMD_LEN];
    Kind kind;
} Session;

// ---------------------------------------------------------------- themes ----
// Eight roles. On the Linux console they are loaded into palette slots 0-7, so
// the 16-colour VT gets the exact theme colours; elsewhere we emit truecolor.
enum { R_BG, R_BG2, R_PANEL, R_FIELD, R_FG, R_DIM, R_ACCENT, R_ERR };
typedef struct { const char *name; unsigned c[8]; } Theme;
static const Theme THEMES[] = {
    {"mocha",   {0x11111b, 0x2a2b3d, 0x1e1e2e, 0x313244, 0xcdd6f4, 0x7f849c, 0xcba6f7, 0xf38ba8}},
    {"tokyo",   {0x16161e, 0x2f334d, 0x1a1b26, 0x292e42, 0xc0caf5, 0x565f89, 0x7aa2f7, 0xf7768e}},
    {"nord",    {0x242933, 0x3b4252, 0x2e3440, 0x3b4252, 0xeceff4, 0x7b88a1, 0x88c0d0, 0xbf616a}},
    {"gruvbox", {0x1d2021, 0x3c3836, 0x282828, 0x3c3836, 0xebdbb2, 0x928374, 0xfabd2f, 0xfb4934}},
    {"rose",    {0x191724, 0x2a273f, 0x1f1d2e, 0x26233a, 0xe0def4, 0x6e6a86, 0xebbcba, 0xeb6f92}},
};
#define NTHEMES (int)(sizeof THEMES / sizeof *THEMES)

// The stock Linux console font lacks rounded corners and some symbols.
typedef struct { const char *tl, *tr, *bl, *br, *h, *v, *l, *r, *dia, *dot, *bar; } Glyphs;
static const Glyphs GT = {"╭", "╮", "╰", "╯", "─", "│", "‹", "›", "◆", "•", "▌"};
static const Glyphs GV = {"┌", "┐", "└", "┘", "─", "│", "«", "»", "♦", "•", "▌"};

// ----------------------------------------------------------------- state ----
static struct {
    Session s[MAX_SESS];
    int ns, sel, focus, theme, rows, cols, vtn, scale;
    char user[USER_LEN], pw[PW_LEN], msg[160], last_sess[NAME_LEN];
    char xserver[128], xinit[128], cfg_user[USER_LEN];
    int numlock;                       // -1 keep, 0 off, 1 on
    int theme_override_val;            // last theme explicitly chosen by the user
    int wp, wp_override_val;           // wallpaper (WP_*), and the last one picked with the F-key
    bool theme_override, wp_override;
    FKey fk[NFKEY];
    struct { int theme, numlock, wp; FKey fk[NFKEY]; } base;   // system-level values
    bool err, showpw, vt, preview, check;
    bool dirty;                        // background + bars need a full redraw (the panel is redrawn every key)
} g = {.focus = 1, .rows = 24, .cols = 80, .vtn = 1, .numlock = -1, .scale = 100,
       .xserver = "/usr/bin/X", .xinit = "/usr/bin/xinit"};

static volatile sig_atomic_t quit_flag, winch_flag;
static char **g_argv;
static void set_msg(bool err, const char *m);
static void get_size(void);

#include "vtfont.h"

// ---------------------------------------------------------- output buffer ----
static char ob[1 << 17];
static size_t on;

static void put(const char *f, ...) {
    va_list a;
    va_start(a, f);
    int n = vsnprintf(ob + on, sizeof ob - on, f, a);
    va_end(a);
    if (n > 0) on = on + (size_t)n < sizeof ob ? on + (size_t)n : sizeof ob - 1;
}
static void flush(void) {
    for (size_t o = 0; o < on;) {
        ssize_t w = write(1, ob + o, on - o);
        if (w < 0) { if (errno == EINTR) continue; break; }
        o += (size_t)w;
    }
    on = 0;
}
static void at(int r, int c) { put(ESC "[%d;%dH", r, c); }
static void rep(const char *s, int n) { while (n-- > 0) put("%s", s); }
static void spaces(int n) { rep(" ", n); }

static void color(int role, bool bg) {
    if (g.vt) { put(ESC "[%dm", (bg ? 40 : 30) + role); return; }
    unsigned c = THEMES[g.theme].c[role];
    put(ESC "[%d;2;%u;%u;%um", bg ? 48 : 38, c >> 16, (c >> 8) & 255, c & 255);
}
static unsigned lerp(unsigned a, unsigned b, int t) {
    unsigned r = 0;
    for (int sh = 16; sh >= 0; sh -= 8) {
        int x = (a >> sh) & 255, y = (b >> sh) & 255;
        r |= (unsigned)(x + (y - x) * t / 100) << sh;
    }
    return r;
}
static int u8len(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}
// Print s in exactly w columns. al: -1 left, 0 centre, 1 right.
static void fit(const char *s, int w, int al) {
    if (w <= 0) return;
    int l = u8len(s);
    const char *e = s + strlen(s);
    if (l > w) {
        const char *p = s;
        for (int n = 0; *p && n < w; n++) {
            p++;
            while (((unsigned char)*p & 0xC0) == 0x80) p++;
        }
        e = p;
        l = w;
    }
    int pad = w - l, left = al < 0 ? 0 : al == 0 ? pad / 2 : pad;
    spaces(left);
    put("%.*s", (int)(e - s), s);
    spaces(pad - left);
}

// -------------------------------------------------------------- terminal ----
static struct termios saved_tio;
static bool have_tio;
static char tty_path[PATH_MAX];

// When a session leader exits, the kernel vhangup()s the console: our fd 0/1 go dead.
// Reopen the tty by path (stderr is left alone: it may be the journal).
static bool reopen_tty(void) {
    if (!tty_path[0]) return false;
    int fd = open(tty_path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return false;
    dup2(fd, 0);
    dup2(fd, 1);
    if (fd > 2) close(fd);
    return true;
}

static void palette(void) {
    g.dirty = true;                    // theme colours changed: repaint the scene
    if (!g.vt) return;
    for (int i = 0; i < 16; i++) put(ESC "]P%X%06X", i, THEMES[g.theme].c[i & 7]);
}
// Console NumLock: KDGKBLED packs current flags (bits 0-2) and defaults (bits 4-6).
static void apply_numlock(void) {
    if (!g.vt || g.numlock < 0) return;
    char f = 0;
    if (ioctl(0, KDGKBLED, &f) != 0) return;
    int v = (unsigned char)f;
    v = g.numlock ? (v | 0x22) : (v & ~0x22);
    ioctl(0, KDSKBLED, v);
}

static void term_init(void) {
    if (tcgetattr(0, &saved_tio) == 0) have_tio = true;
    struct termios t = saved_tio;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    if (g.vt) {
        ioctl(0, KDSETMODE, KD_TEXT);
        ioctl(0, KDSKBMODE, K_UNICODE);        // undo raw/off mode left by a crashed compositor
        struct vt_mode m = {.mode = VT_AUTO};
        ioctl(0, VT_SETMODE, &m);
        palette();
        apply_numlock();
        font_apply(g.scale);
    }
    put(ESC "[?25l");
    flush();
    get_size();
    g.dirty = true;
}
static void term_reset(void) {
    if (g.vt) font_restore();           // a tty shell gets the normal console font back
    if (have_tio) tcsetattr(0, TCSAFLUSH, &saved_tio);
    put(ESC "[0m" ESC "[2J" ESC "[H" ESC "[?25h");
    if (g.vt) put(ESC "]R");
    flush();
}
static void get_size(void) {
    struct winsize w;
    if (ioctl(1, TIOCGWINSZ, &w) == 0 && w.ws_row > 4 && w.ws_col > 20) { g.rows = w.ws_row; g.cols = w.ws_col; }
}
static bool caps_on(void) {
    char f = 0;
    return g.vt && ioctl(0, KDGKBLED, &f) == 0 && (f & 4);
}

// ------------------------------------------------------------ small info ----
static void battery(char *out, size_t n) {
    out[0] = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (strncmp(e->d_name, "BAT", 3)) continue;
        char p[PATH_MAX], st[32] = "";
        int cap = -1;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fscanf(f, "%d", &cap) != 1) cap = -1; fclose(f); }
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
        if ((f = fopen(p, "r"))) { if (!fgets(st, sizeof st, f)) st[0] = 0; fclose(f); }
        if (cap >= 0) snprintf(out, n, "battery %d%%%s", cap, st[0] == 'C' ? " +" : "");
        break;
    }
    closedir(d);
}

static const char *fk_label(const FKey *k) {
    static char buf[CMD_LEN];
    if (k->label[0]) return k->label;
    if (!strcmp(k->act, "poweroff")) return "shutdown";
    if (!strcmp(k->act, "showpw")) return "show password";
    if (!strcmp(k->act, "scale+")) return "zoom in";
    if (!strcmp(k->act, "scale-")) return "zoom out";
    if (!strcmp(k->act, "scale0")) return "zoom reset";
    if (!strncmp(k->act, "exec:", 5)) {            // show the program name, not just "run"
        const char *c = k->act + 5;
        while (*c == ' ') c++;
        size_t n = strcspn(c, " \t");
        const char *base = c;
        for (const char *q = c; q < c + n; q++) if (*q == '/') base = q + 1;
        snprintf(buf, sizeof buf, "%.*s", (int)(c + n - base), base);
        return buf[0] ? buf : "run";
    }
    return k->act;
}

// -------------------------------------------------------------- wallpaper ----
// Theme-based background; the scenes themselves live in scenes.h.
static int bg_t(int r) {
    int d = g.rows - 3;
    int t = (r - 2) * 100 / (d > 0 ? d : 1);
    return t * t / 100;
}
static unsigned bg_row(int r) {
    const Theme *T = &THEMES[g.theme];
    return lerp(T->c[R_BG], T->c[R_BG2], bg_t(r));
}
static void bg_cell(int r) {
    if (g.vt) { color(R_BG, true); return; }
    unsigned c = bg_row(r);
    put(ESC "[48;2;%u;%u;%um", c >> 16, (c >> 8) & 255, c & 255);
}
static unsigned bg_hash(int r, int c) {
    unsigned h = (unsigned)r * 73856093u ^ (unsigned)c * 19349663u ^ 0x9e3779b9u;
    h ^= h >> 13;
    h *= 0x85ebca6bu;
    h ^= h >> 16;
    return h;
}
// Geometry of the login panel (zoom applies to it, unless a real font size does the zooming).
static struct { int y0, x0, w, h; } pg;
static void panel_geom(void) {
    int sc = font_active() ? 100 : g.scale;
    int W = 50 * sc / 100;
    if (W > g.cols - 4) W = g.cols - 4;
    if (W < 24) W = g.cols;
    int H_ = 12 * sc / 100, maxH = g.rows - 3;
    if (maxH < 9) maxH = g.rows > 2 ? g.rows : 2;
    if (H_ > maxH) H_ = maxH;
    if (H_ < 9 && maxH >= 9) H_ = 9;
    int y0 = (g.rows - H_) / 2 + 1;
    if (y0 < 2) y0 = 2;
    pg.y0 = y0; pg.x0 = (g.cols - W) / 2 + 1; pg.w = W; pg.h = H_;
}
#include "scenes.h"

// ------------------------------------------------------------------ draw ----
static const char *fk_short(const FKey *k) {
    if (k->label[0]) return k->label;
    static const struct { const char *a, *s; } m[] = {{"poweroff", "off"}, {"reboot", "reboot"}, {"suspend", "sleep"},
        {"hibernate", "hibernate"}, {"theme", "theme"}, {"showpw", "pw"}, {"reload", "reload"}, {"restart", "restart"},
        {"stop", "stop"}, {"scale+", "zoom+"}, {"scale-", "zoom-"}, {"scale0", "zoom0"}, {"wallpaper", "wall"}};
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) if (!strcmp(k->act, m[i].a)) return m[i].s;
    return fk_label(k);
}
static bool fk_on(const FKey *k) { return k->act[0] && strcmp(k->act, "none"); }
static int fk_w(int i, int pass) {
    const FKey *k = &g.fk[i];
    return (i < 9 ? 2 : 3) + 1 + u8len(pass ? fk_short(k) : fk_label(k)) + (pass ? 2 : 3);
}

static void draw_scene(void) {
    const Glyphs *G = g.vt && !font_active() ? &GV : &GT;
    char host[64] = "localhost", bat[48];
    gethostname(host, sizeof host - 1);
    battery(bat, sizeof bat);
    put(ESC "[?25l" ESC "[0m");
    // Background: smooth gradient (truecolor) or dithered blocks (VT).
    static const char *dither[] = {" ", "░", "▒", "▓"};
    for (int r = 2; r < g.rows; r++) {
        at(r, 1);
        int t = bg_t(r);
        if (g.vt) {
            color(R_BG, true);
            color(R_BG2, false);
            rep(dither[t < 45 ? 0 : t < 65 ? 1 : t < 85 ? 2 : 3], g.cols);
        } else {
            unsigned c = bg_row(r);
            put(ESC "[48;2;%u;%u;%um", c >> 16, (c >> 8) & 255, c & 255);
            spaces(g.cols);
        }
    }
    build_wallpaper();
    emit_wallpaper();
    flush();
    // Top bar.
    at(1, 1);
    color(R_PANEL, true);
    spaces(g.cols);
    at(1, 2);
    color(R_ACCENT, false);
    put("%s ", G->dia);
    color(R_FG, false);
    put("%.*s", g.cols / 2, host);
    int bl = u8len(bat);
    if (bl && g.cols > bl + 4) { at(1, g.cols - bl); color(R_DIM, false); put("%s", bat); }
    // Bottom bar: full labels if they fit, else short ones, else as many keys as fit.
    at(g.rows, 1);
    color(R_PANEL, true);
    spaces(g.cols);
    at(g.rows, 2);
    int pass = 0;
    for (; pass < 2; pass++) {
        int total = 1;
        for (int i = 0; i < NFKEY; i++) if (fk_on(&g.fk[i])) total += fk_w(i, pass);
        if (total <= g.cols) break;
    }
    bool trunc = pass == 2;
    if (trunc) pass = 1;
    int used = 1;
    for (int i = 0; i < NFKEY; i++) {
        const FKey *k = &g.fk[i];
        if (!fk_on(k)) continue;
        if (used + fk_w(i, pass) > g.cols - (trunc ? 4 : 0)) {
            if (trunc) { color(R_DIM, false); put("..."); }
            break;
        }
        char kn[8];
        snprintf(kn, sizeof kn, "F%d", i + 1);
        color(R_ACCENT, false);
        put("%s", kn);
        color(R_DIM, false);
        put(pass ? " %s  " : " %s   ", pass ? fk_short(k) : fk_label(k));
        used += fk_w(i, pass);
    }
    flush();
}

static void draw_panel(void) {
    const Glyphs *G = g.vt && !font_active() ? &GV : &GT;
    char when[96];
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%A, %d %B  \xc2\xb7  %H:%M", &tm);
    put(ESC "[?25l" ESC "[0m");
    int W = pg.w, H_ = pg.h, y0 = pg.y0, x0 = pg.x0;
    for (int i = 0; i < H_; i++) {
        at(y0 + i, x0);
        color(R_PANEL, true);
        color(R_DIM, false);
        if (i == 0) { put("%s", G->tl); rep(G->h, W - 2); put("%s", G->tr); }
        else if (i == H_ - 1) { put("%s", G->bl); rep(G->h, W - 2); put("%s", G->br); }
        else { put("%s", G->v); spaces(W - 2); put("%s", G->v); }
    }
    int padx = W / 25 < 2 ? 2 : W / 25;
    int iw = W - 2 * padx;
    int titleY = y0 + 2 * H_ / 12, dateY = titleY + 1;
    int fieldY = y0 + 5 * H_ / 12, msgY = y0 + 9 * H_ / 12;
    at(titleY, x0 + padx);
    color(R_PANEL, true);
    color(R_FG, false);
    if (!g.vt) put(ESC "[1m");
    fit("Welcome back", iw, 0);
    put(ESC "[22m");
    at(dateY, x0 + padx);
    color(R_DIM, false);
    fit(when, iw, 0);

    // Fields.
    static const char *lab[] = {"session", "user", "password"};
    int vw = iw - 12, curcol = 0;
    for (int k = 0; k < 3; k++) {
        bool f = g.focus == k;
        int y = fieldY + k;
        at(y, x0 + padx);
        color(R_FIELD, true);
        color(f ? R_ACCENT : R_FIELD, false);
        put("%s ", G->bar);
        color(f ? R_ACCENT : R_DIM, false);
        fit(lab[k], 9, -1);
        color(R_FG, false);
        if (k == 0) {
            fit(g.s[g.sel].name, vw - 5 < 0 ? 0 : vw - 5, -1);
            color(R_DIM, false);
            put(" %s %s ", G->l, G->r);
        } else if (k == 1) {
            fit(g.user, vw, -1);
        } else {
            int n = u8len(g.pw);
            if (g.showpw) fit(g.pw, vw, -1);
            else { int d = n < vw ? n : vw; rep(G->dot, d); spaces(vw - d); }
            if (f) curcol = x0 + padx + 11 + (n < vw - 1 ? n : vw - 1);
        }
        put(" ");
    }
    if (g.focus == 1) curcol = x0 + padx + 11 + (u8len(g.user) < vw - 1 ? u8len(g.user) : vw - 1);

    // Message line.
    at(msgY, x0 + padx);
    color(R_PANEL, true);
    const char *m = g.msg;
    bool e = g.err;
    if (!m[0] && caps_on()) { m = "caps lock is on"; e = true; }
    color(e ? R_ERR : R_DIM, false);
    fit(m, iw, 0);

    if (g.focus > 0) { at(fieldY + g.focus, curcol); color(R_FIELD, true); color(R_FG, false); put(ESC "[?25h"); }
    flush();
}


static void draw(void) {
    panel_geom();
    if (g.dirty) { g.dirty = false; draw_scene(); }
    draw_panel();
}

// ------------------------------------------------------- config & session ----
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}
static bool set_theme(const char *n) {
    for (int i = 0; i < NTHEMES; i++) if (!strcmp(THEMES[i].name, n)) { g.theme = i; return true; }
    return false;
}
static bool set_wallpaper(const char *n) {
    if (!strcmp(n, "random")) { g.wp = (int)((unsigned)time(nullptr) / 7 % 3); return true; }
    for (int i = 0; i < NWP; i++) if (!strcmp(WP_NAMES[i], n)) { g.wp = i; return true; }
    return false;
}
enum { CFG_SYSTEM, CFG_USER, CFG_STATE };
static bool is_builtin(const char *a) {
    static const char *b[] = {"none", "poweroff", "reboot", "suspend", "hibernate", "theme", "showpw", "wallpaper",
                              "reload", "restart", "stop", "scale+", "scale-", "scale0"};
    for (size_t i = 0; i < sizeof b / sizeof *b; i++) if (!strcmp(a, b[i])) return true;
    return false;
}
// Case-insensitive, with a few friendly aliases. exec:<cmd> is left alone.
static const char *canon_action(char *v) {
    if (!strncmp(v, "exec:", 5)) return v;
    for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!strcmp(v, "shutdown") || !strcmp(v, "halt")) return "poweroff";
    if (!strcmp(v, "sleep")) return "suspend";
    if (!strcmp(v, "zoom+") || !strcmp(v, "zoomin")) return "scale+";
    if (!strcmp(v, "zoom-") || !strcmp(v, "zoomout")) return "scale-";
    return v;
}
static const char *cfg_file;
static int cfg_line;
static void diag(const char *fmt, ...) {          // --check only: say why a line was not used
    if (!g.check) return;
    va_list a;
    va_start(a, fmt);
    printf("  ignored  %s:%d: ", cfg_file, cfg_line);
    vprintf(fmt, a);
    putchar('\n');
    va_end(a);
}
// System file: everything, incl. exec:<cmd> (runs as root - file is root-owned) and font settings.
// User file: theme, wallpaper, numlock, fN, fN_label - built-in actions only, never exec:.
// Keys are case-insensitive; "key = value  # comment" (blank before '#') is allowed.
static void parse_stream(FILE *f, int mode, const char *name) {
    char l[CMD_LEN + 64];
    cfg_file = name;
    cfg_line = 0;
    while (fgets(l, sizeof l, f)) {
        cfg_line++;
        trim(l);
        char *p = l;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) { diag("no '=' in \"%s\"", p); continue; }
        *eq = 0;
        char *k = p, *v = eq + 1;
        trim(k);
        for (char *q = k; *q; q++) *q = (char)tolower((unsigned char)*q);
        while (isspace((unsigned char)*v)) v++;
        if (strncmp(v, "exec:", 5)) {
            for (char *c = v; *c; c++) if (*c == '#' && c > v && (c[-1] == ' ' || c[-1] == '\t')) { *c = 0; break; }
            trim(v);
        }
        if (!strcmp(k, "theme") || !strcmp(k, "wallpaper"))
            for (char *c = v; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (mode == CFG_STATE) {
            if (!strcmp(k, "user")) snprintf(g.user, sizeof g.user, "%s", v);
            else if (!strcmp(k, "session")) snprintf(g.last_sess, sizeof g.last_sess, "%s", v);
            else if (!strcmp(k, "theme")) {
                if (set_theme(v)) { g.theme_override = true; g.theme_override_val = g.theme; }
            } else if (!strcmp(k, "wallpaper")) {
                if (strcmp(v, "random") && set_wallpaper(v)) { g.wp_override = true; g.wp_override_val = g.wp; }
            } else if (!strcmp(k, "scale")) {
                char *end;
                long n = strtol(v, &end, 10);
                if (end != v) g.scale = n < 60 ? 60 : n > 160 ? 160 : (int)n;
            }
        } else if (!strcmp(k, "theme")) { if (!set_theme(v)) diag("unknown theme \"%s\"", v); }
        else if (!strcmp(k, "wallpaper")) { if (!set_wallpaper(v)) diag("unknown wallpaper \"%s\" (festival ocean forest none random)", v); }
        else if (!strcmp(k, "numlock")) g.numlock = !strcasecmp(v, "on") ? 1 : !strcasecmp(v, "off") ? 0 : -1;
        else if (mode == CFG_SYSTEM && !strcmp(k, "xserver")) snprintf(g.xserver, sizeof g.xserver, "%s", v);
        else if (mode == CFG_SYSTEM && !strcmp(k, "xinit")) snprintf(g.xinit, sizeof g.xinit, "%s", v);
        else if (mode == CFG_SYSTEM && !strcmp(k, "font")) { snprintf(fnt.want, sizeof fnt.want, "%s", v); fnt.path[0] = 0; }
        else if (mode == CFG_SYSTEM && !strcmp(k, "font_size")) {
            char *end;
            long n = strtol(v, &end, 10);
            if (end != v) fnt.size = n < FONT_MIN ? FONT_MIN : n > FONT_MAX ? FONT_MAX : (int)n;
            else diag("font_size needs a number");
        } else if (k[0] == 'f' && isdigit((unsigned char)k[1])) {
            char *end;
            long n = strtol(k + 1, &end, 10);
            if (n < 1 || n > NFKEY) { diag("no such key %s (F1-F%d)", k, NFKEY); continue; }
            FKey *fk = &g.fk[n - 1];
            if (!*end) {
                const char *a = canon_action(v);
                if (is_builtin(a) || (mode == CFG_SYSTEM && !strncmp(v, "exec:", 5) && v[5])) {
                    snprintf(fk->act, sizeof fk->act, "%s", a);
                    fk->src = mode == CFG_SYSTEM ? 1 : 2;
                } else if (!strncmp(v, "exec:", 5)) diag("exec: is only allowed in /etc/lumdm.conf");
                else diag("unknown action \"%s\"", v);
            } else if (!strcmp(end, "_label")) snprintf(fk->label, sizeof fk->label, "%s", v);
            else diag("unknown key \"%s\"", k);
        } else diag("unknown key \"%s\"%s", k, !strcmp(k, "font") || !strcmp(k, "font_size") || !strcmp(k, "xserver") || !strcmp(k, "xinit") ? " (system file only)" : "");
    }
}
static void load_kv(const char *path, int mode) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    parse_stream(f, mode, path);
    fclose(f);
}
// ~/.config/lumdm.conf of the typed user, read before login. A child drops to that user and reads the
// file with their own permissions, so symlinked dotfiles (stow, chezmoi...) work and cannot be abused to
// read other files as root. Only recognised keys are parsed; size is capped.
enum { UC_OK, UC_NONE, UC_NOTREG, UC_BIG, UC_DENIED, UC_ERR };
static const char *const UC_WHY[] = {"", "", "not a regular file", "larger than 4 KiB", "permission denied", "unreadable"};
static int slurp(const char *path, int out) {
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? UC_NONE : errno == EACCES || errno == EPERM ? UC_DENIED : UC_ERR;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) return UC_NOTREG;
    if (st.st_size > 4096) return UC_BIG;
    char b[4097];
    ssize_t n = read(fd, b, sizeof b);
    if (n < 0) return UC_ERR;
    if (n > 4096) return UC_BIG;
    for (ssize_t o = 0; o < n;) {
        ssize_t w = write(out, b + o, (size_t)(n - o));
        if (w < 0) { if (errno == EINTR) continue; return UC_ERR; }
        o += w;
    }
    return UC_OK;
}
static int load_user_conf(const char *user) {
    struct passwd *pw = getpwnam(user);
    if (!pw) return UC_NONE;
    char p[PATH_MAX], buf[4097];
    snprintf(p, sizeof p, "%s/.config/lumdm.conf", pw->pw_dir);
    int pf[2];
    if (pipe2(pf, O_CLOEXEC)) return UC_ERR;
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) { close(pf[0]); close(pf[1]); return UC_ERR; }
    if (pid == 0) {
        close(pf[0]);
        if (geteuid() == 0 && (initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid))) _exit(UC_ERR);
        _exit(slurp(p, pf[1]));
    }
    close(pf[1]);
    size_t len = 0;
    ssize_t n;
    while (len < sizeof buf - 1 && (n = read(pf[0], buf + len, sizeof buf - 1 - len)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        len += (size_t)n;
    }
    close(pf[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : UC_ERR;
    if (rc == UC_OK && len) {
        FILE *f = fmemopen(buf, len, "r");
        if (f) { parse_stream(f, CFG_USER, p); fclose(f); }
    }
    return rc;
}
// Re-layer: /etc/lumdm.conf < ~/.config/lumdm.conf of the selected user.
// A theme/wallpaper chosen at runtime (or restored from state) wins over both.
static void refresh_user_conf(void) {
    if (!strcmp(g.user, g.cfg_user)) return;
    snprintf(g.cfg_user, sizeof g.cfg_user, "%s", g.user);
    g.theme = g.base.theme;
    g.numlock = g.base.numlock;
    g.wp = g.base.wp;
    memcpy(g.fk, g.base.fk, sizeof g.fk);
    if (g.user[0]) {
        int rc = load_user_conf(g.user);
        if (rc >= UC_NOTREG) {
            char m[160];
            snprintf(m, sizeof m, "~/.config/lumdm.conf ignored: %s", UC_WHY[rc]);
            set_msg(true, m);
        }
    }
    if (g.theme_override) g.theme = g.theme_override_val;
    if (g.wp_override) g.wp = g.wp_override_val;
    palette();
    apply_numlock();
}
static void save_state(void) {
    if (g.preview) return;
    mkdir(STATE_DIR, 0755);
    FILE *f = fopen(STATE, "w");
    if (!f) return;
    fprintf(f, "user=%s\nsession=%s\nscale=%d\n", g.user, g.s[g.sel].name, g.scale);
    if (g.theme_override) fprintf(f, "theme=%s\n", THEMES[g.theme_override_val].name);
    if (g.wp_override) fprintf(f, "wallpaper=%s\n", WP_NAMES[g.wp_override_val]);
    fclose(f);
}
static void clean_exec(char *dst, const char *src) {
    size_t n = 0;
    for (; *src && n < CMD_LEN - 1; src++) {
        if (*src == '%') { if (src[1] == '%') dst[n++] = '%'; if (src[1]) src++; continue; }
        dst[n++] = *src;
    }
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}
static void scan_dir(const char *dir, Kind kind) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && g.ns < MAX_SESS - 1;) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 8, ".desktop")) continue;
        char path[PATH_MAX], l[512];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        Session s = {.kind = kind};
        snprintf(s.id, sizeof s.id, "%.*s", (int)n - 8, e->d_name);
        bool hide = false;
        while (fgets(l, sizeof l, f)) {
            trim(l);
            if (!strncmp(l, "Name=", 5)) snprintf(s.name, sizeof s.name, "%s", l + 5);
            else if (!strncmp(l, "Exec=", 5)) clean_exec(s.exec, l + 5);
            else if (!strncmp(l, "DesktopNames=", 13)) {
                snprintf(s.desk, sizeof s.desk, "%s", l + 13);
                for (char *c = s.desk; *c; c++) if (*c == ';') *c = ':';
                trim(s.desk);
                size_t dl = strlen(s.desk);
                if (dl && s.desk[dl - 1] == ':') s.desk[dl - 1] = 0;
            } else if (!strcmp(l, "Hidden=true") || !strcmp(l, "NoDisplay=true")) hide = true;
            else if (l[0] == '[' && strcmp(l, "[Desktop Entry]")) break;
        }
        fclose(f);
        if (hide || !s.name[0] || !s.exec[0]) continue;
        if (!s.desk[0]) snprintf(s.desk, sizeof s.desk, "%s", s.id);
        if (!strcasestr(s.name, "wayland") && !strcasestr(s.name, "x11") && !strcasestr(s.name, "xorg")) {
            char tmp[NAME_LEN];
            snprintf(tmp, sizeof tmp, "%s (%s)", s.name, kind == K_WAYLAND ? "Wayland" : "X11");
            snprintf(s.name, sizeof s.name, "%s", tmp);
        }
        g.s[g.ns++] = s;
    }
    closedir(d);
}
static int cmp_sess(const void *a, const void *b) {
    return strcasecmp(((const Session *)a)->name, ((const Session *)b)->name);
}
static void load_sessions(void) {
    scan_dir("/usr/share/wayland-sessions", K_WAYLAND);
    scan_dir("/usr/local/share/wayland-sessions", K_WAYLAND);
    scan_dir("/usr/share/xsessions", K_X11);
    scan_dir("/usr/local/share/xsessions", K_X11);
    qsort(g.s, (size_t)g.ns, sizeof *g.s, cmp_sess);
    Session sh = {.kind = K_SHELL};
    snprintf(sh.id, sizeof sh.id, "shell");
    snprintf(sh.name, sizeof sh.name, "Shell (tty)");
    g.s[g.ns++] = sh;
}
// Re-read the font after a config/zoom change; the window size follows.
static void font_sync(void) {
    if (!g.vt) return;
    font_apply(g.scale);
    get_size();
    g.dirty = true;
}
// Re-read /etc/lumdm.conf and the selected user's overrides without dropping input.
static void reload_config(void) {
    char sel[NAME_LEN] = "", oldfont[CMD_LEN];
    if (g.ns > 0 && g.sel >= 0 && g.sel < g.ns) snprintf(sel, sizeof sel, "%s", g.s[g.sel].name);
    snprintf(oldfont, sizeof oldfont, "%s", fnt.want);
    int oldsize = fnt.size;

    memset(g.fk, 0, sizeof g.fk);
    g.theme = 0;
    g.wp = WP_FESTIVAL;
    g.numlock = -1;
    fnt.want[0] = 0;
    fnt.size = 16;
    snprintf(g.xserver, sizeof g.xserver, "/usr/bin/X");
    snprintf(g.xinit, sizeof g.xinit, "/usr/bin/xinit");
    load_kv(CONF, CFG_SYSTEM);
    g.base.theme = g.theme;
    g.base.numlock = g.numlock;
    g.base.wp = g.wp;
    memcpy(g.base.fk, g.fk, sizeof g.fk);
    if (strcmp(oldfont, fnt.want) || oldsize != fnt.size) { font_restore(); fnt.path[0] = 0; }

    g.ns = 0;
    load_sessions();
    g.sel = 0;
    if (sel[0]) for (int i = 0; i < g.ns; i++) if (!strcmp(g.s[i].name, sel)) { g.sel = i; break; }
    g.cfg_user[0] = 0;
    refresh_user_conf();
    palette();
    apply_numlock();
    font_sync();
    set_msg(false, fnt.err[0] ? fnt.err : "configuration reloaded");
    g.err = fnt.err[0] != 0;
}

// ------------------------------------------------------------------- PAM ----
static int conv_fn(int n, const struct pam_message **m, struct pam_response **resp, void *) {
    struct pam_response *r = calloc((size_t)n, sizeof *r);
    if (!r) return PAM_BUF_ERR;
    for (int i = 0; i < n; i++) {
        if (m[i]->msg_style == PAM_PROMPT_ECHO_OFF) r[i].resp = strdup(g.pw);
        else if (m[i]->msg_style == PAM_PROMPT_ECHO_ON) r[i].resp = strdup(g.user);
    }
    *resp = r;
    return PAM_SUCCESS;
}

// ------------------------------------------------------- session launching ----
static int free_display(void) {
    char p[64];
    for (int n = 0; n < 64; n++) {
        snprintf(p, sizeof p, "/tmp/.X11-unix/X%d", n);
        if (access(p, F_OK) == 0) continue;
        snprintf(p, sizeof p, "/tmp/.X%d-lock", n);
        if (access(p, F_OK) != 0) return n;
    }
    return 9;
}
static void put16(FILE *f, unsigned v) { fputc((int)(v >> 8) & 255, f); fputc((int)v & 255, f); }
static void xauth_entry(FILE *f, unsigned fam, const char *host, int dpy, const unsigned char ck[16]) {
    char num[16];
    snprintf(num, sizeof num, "%d", dpy);
    static const char name[] = "MIT-MAGIC-COOKIE-1";
    put16(f, fam);
    put16(f, (unsigned)strlen(host));
    fputs(host, f);
    put16(f, (unsigned)strlen(num));
    fputs(num, f);
    put16(f, sizeof name - 1);
    fwrite(name, 1, sizeof name - 1, f);
    put16(f, 16);
    fwrite(ck, 1, 16, f);
}
static bool write_xauth(const char *path, int dpy) {
    unsigned char ck[16];
    char host[256] = "localhost";
    if (getrandom(ck, sizeof ck, 0) != (ssize_t)sizeof ck) return false;
    gethostname(host, sizeof host - 1);
    umask(077);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    xauth_entry(f, 256, host, dpy, ck);   // FamilyLocal
    xauth_entry(f, 65535, "", dpy, ck);   // FamilyWild: survives hostname changes
    return fclose(f) == 0;
}

[[noreturn]] static void session_child(const struct passwd *pw, const Session *s, char **penv, int dpy) {
    setsid();
    if (g.vt) ioctl(0, TIOCSCTTY, 1);
    if (initgroups(pw->pw_name, pw->pw_gid) || setgid(pw->pw_gid) || setuid(pw->pw_uid)) _exit(1);
    if (chdir(pw->pw_dir)) (void)!chdir("/");
    char lang[64] = "C.UTF-8";
    if (getenv("LANG")) snprintf(lang, sizeof lang, "%s", getenv("LANG"));
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh", 1);
    setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
    setenv("LANG", lang, 1);
    setenv("TERM", "linux", 1);
    for (char **e = penv; e && *e; e++) putenv(strdup(*e));
    if (!getenv("XDG_RUNTIME_DIR")) {
        char rt[64];
        snprintf(rt, sizeof rt, "/run/user/%u", pw->pw_uid);
        if (access(rt, F_OK) == 0) setenv("XDG_RUNTIME_DIR", rt, 1);
    }
    const char *shell = getenv("SHELL"), *base = strrchr(shell, '/');
    char a0[80], cmd[CMD_LEN + 320];
    snprintf(a0, sizeof a0, "-%s", base ? base + 1 : shell);   // leading '-' => login shell

    if (s->kind == K_SHELL) {
        execl(shell, a0, (char *)nullptr);
    } else if (s->kind == K_WAYLAND) {
        snprintf(cmd, sizeof cmd, "exec %s", s->exec);
        execl(shell, a0, "-c", cmd, (char *)nullptr);
    } else {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        char xa[PATH_MAX], disp[16], vtarg[16];
        snprintf(xa, sizeof xa, rt ? "%s/lumdm.xauth" : "%s/.Xauthority", rt ? rt : pw->pw_dir);
        if (!write_xauth(xa, dpy)) _exit(1);
        snprintf(disp, sizeof disp, ":%d", dpy);
        snprintf(vtarg, sizeof vtarg, "vt%d", g.vtn);
        const char *nl = g.numlock < 0 ? "" : g.numlock ? "command -v numlockx >/dev/null 2>&1 && numlockx on; "
                                                        : "command -v numlockx >/dev/null 2>&1 && numlockx off; ";
        snprintf(cmd, sizeof cmd, "%s[ -r \"$HOME/.xprofile\" ] && . \"$HOME/.xprofile\"; exec %s", nl, s->exec);
        setenv("XAUTHORITY", xa, 1);
        char *argv[20];
        int n = 0;
        argv[n++] = "xinit"; argv[n++] = "/bin/sh"; argv[n++] = "-l"; argv[n++] = "-c"; argv[n++] = cmd;
        argv[n++] = "--"; argv[n++] = g.xserver; argv[n++] = disp;
        if (g.vt) argv[n++] = vtarg;
        argv[n++] = "-auth"; argv[n++] = xa; argv[n++] = "-nolisten"; argv[n++] = "tcp"; argv[n] = nullptr;
        execv(g.xinit, argv);
    }
    _exit(127);
}

static void pam_env(pam_handle_t *ph, const char *fmt, ...) {
    char b[128];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    pam_putenv(ph, b);
}

static void set_msg(bool err, const char *m) { g.err = err; snprintf(g.msg, sizeof g.msg, "%s", m); }

static void do_login(void) {
    if (g.preview) { set_msg(false, "preview mode - login disabled"); return; }
    if (!g.user[0]) { set_msg(true, "enter a username"); g.focus = 1; return; }
    set_msg(false, "signing in...");
    draw();

    pam_handle_t *ph = nullptr;
    struct pam_conv conv = {conv_fn, nullptr};
    int r = pam_start("lumdm", g.user, &conv, &ph);
    if (r == PAM_SUCCESS) r = pam_authenticate(ph, 0);
    if (r == PAM_SUCCESS) r = pam_acct_mgmt(ph, 0);
    if (r != PAM_SUCCESS) {
        set_msg(true, r == PAM_NEW_AUTHTOK_REQD ? "password expired - use passwd" : "login incorrect");
        if (ph) pam_end(ph, r);
        explicit_bzero(g.pw, sizeof g.pw);
        draw();
        struct timespec ts = {1, 0};
        nanosleep(&ts, nullptr);
        return;
    }
    const Session *s = &g.s[g.sel];
    pam_setcred(ph, PAM_ESTABLISH_CRED);
    pam_env(ph, "XDG_SESSION_CLASS=user");
    pam_env(ph, "XDG_SEAT=seat0");
    pam_env(ph, "XDG_VTNR=%d", g.vtn);
    pam_env(ph, "XDG_SESSION_TYPE=%s", s->kind == K_X11 ? "x11" : s->kind == K_WAYLAND ? "wayland" : "tty");
    pam_env(ph, "XDG_SESSION_DESKTOP=%s", s->id);
    pam_env(ph, "XDG_CURRENT_DESKTOP=%s", s->desk);
    if (pam_open_session(ph, 0) != PAM_SUCCESS) {
        set_msg(true, "could not open session");
        pam_setcred(ph, PAM_DELETE_CRED);
        pam_end(ph, 0);
        return;
    }
    struct passwd *pw = getpwnam(g.user);
    char **penv = pam_getenvlist(ph);
    explicit_bzero(g.pw, sizeof g.pw);
    save_state();
    int dpy = s->kind == K_X11 ? free_display() : 0;

    term_reset();
    pid_t pid = fork();
    if (pid == 0) {
        if (!pw) _exit(1);
        session_child(pw, s, penv, dpy);
    }
    int st = 0;
    bool killed = false;
    while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        if (quit_flag && !killed) { kill(pid, SIGTERM); killed = true; }
    }
    pam_close_session(ph, 0);
    pam_setcred(ph, PAM_DELETE_CRED);
    pam_end(ph, PAM_SUCCESS);
    if (penv) { for (char **e = penv; *e; e++) free(*e); free(penv); }

    reopen_tty();
    get_size();
    term_init();
    set_msg(false, "");
    g.focus = 2;
}

static void power(const char *what) {
    set_msg(false, "please wait...");
    draw();
    pid_t p = fork();
    if (p == 0) {
        bool sd = access("/run/systemd/system", F_OK) == 0;
        if (!strcmp(what, "suspend") || !strcmp(what, "hibernate")) {
            if (sd) execlp("systemctl", "systemctl", what, (char *)nullptr);
            execlp("loginctl", "loginctl", what, (char *)nullptr);
            execlp(what[0] == 'h' ? "ZZZ" : "zzz", what[0] == 'h' ? "ZZZ" : "zzz", (char *)nullptr);
        } else {
            if (sd) execlp("systemctl", "systemctl", what, (char *)nullptr);
            execlp(what, what, (char *)nullptr);
        }
        _exit(1);
    }
    if (p > 0) waitpid(p, nullptr, 0);
    set_msg(false, "");
}

static void run_cmd(const char *cmd) {
    set_msg(false, "please wait...");
    draw();
    pid_t p = fork();
    if (p == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)nullptr);
        _exit(127);
    }
    if (p > 0) waitpid(p, nullptr, 0);
    set_msg(false, "");
}
static void scale_step(int d) {
    int v = g.scale + d;
    if (v < 60) v = 60;
    if (v > 160) v = 160;
    if (v == g.scale) return;
    g.scale = v;
    save_state();
    font_sync();
    g.dirty = true;
    char m[32];
    snprintf(m, sizeof m, "ui scale %d%%", g.scale);
    set_msg(false, m);
}
static void scale_reset(void) {
    if (g.scale == 100) return;
    g.scale = 100;
    save_state();
    font_sync();
    g.dirty = true;
    set_msg(false, "ui scale 100%");
}
// Ask the init system to reload/restart/stop the lumdm service. Returns false when
// there is no service manager we recognise, so callers can fall back to local action.
static bool service_ctl(const char *verb) {
    set_msg(false, "please wait...");
    draw();
    pid_t p = fork();
    if (p == 0) {
        if (access("/run/systemd/system", F_OK) == 0)
            execlp("systemctl", "systemctl", verb, "lumdm", (char *)nullptr);
        else if (access("/run/runit/service/lumdm", F_OK) == 0)
            execlp("sv", "sv", verb, "/run/runit/service/lumdm", (char *)nullptr);
        else if (access("/var/service/lumdm", F_OK) == 0)
            execlp("sv", "sv", verb, "/var/service/lumdm", (char *)nullptr);
        else if (access("/etc/sv/lumdm", F_OK) == 0)
            execlp("sv", "sv", verb, "/etc/sv/lumdm", (char *)nullptr);
        _exit(1);
    }
    int st = 0;
    if (p > 0)
        while (waitpid(p, &st, 0) < 0 && errno == EINTR)
            if (quit_flag) return true;
    set_msg(false, "");
    return p > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}
static void restart_self(void) {
    term_reset();
    if (g_argv && g_argv[0]) execv("/proc/self/exe", g_argv);
    if (g_argv && g_argv[0]) execv(g_argv[0], g_argv);
    term_init();
    set_msg(true, "restart failed");
}
static void run_action(const char *a) {
    if (!*a || !strcmp(a, "none")) return;
    if (!strcmp(a, "theme")) {
        g.theme = (g.theme + 1) % NTHEMES;
        g.theme_override = true;
        g.theme_override_val = g.theme;
        save_state();
        palette();
    } else if (!strcmp(a, "wallpaper")) {
        g.wp = (g.wp + 1) % NWP;
        g.wp_override = true;
        g.wp_override_val = g.wp;
        g.dirty = true;
        save_state();
    } else if (!strcmp(a, "showpw")) g.showpw = !g.showpw;
    else if (!strcmp(a, "reload")) reload_config();
    else if (!strcmp(a, "restart")) {
        if (service_ctl("restart")) quit_flag = 1;
        else restart_self();
    } else if (!strcmp(a, "stop")) {
        service_ctl("stop");
        quit_flag = 1;
    } else if (!strcmp(a, "scale+")) scale_step(10);
    else if (!strcmp(a, "scale-")) scale_step(-10);
    else if (!strcmp(a, "scale0")) scale_reset();
    else if (!strncmp(a, "exec:", 5)) run_cmd(a + 5);       // only ever loaded from /etc/lumdm.conf
    else power(a);                                          // poweroff | reboot | suspend | hibernate
}

// ----------------------------------------------------------------- input ----
enum { KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_DEL, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
       KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_PPAGE, KEY_NPAGE };
static unsigned char ib[64];
static int in_n, in_p;

static int next_key(void) {
    if (in_p >= in_n) return 0;
    unsigned char c = ib[in_p++];
    if (c != 0x1b) return c;
    if (in_p >= in_n) return 0x1b;
    unsigned char d = ib[in_p++];
    if (d == 'O' && in_p < in_n) {
        unsigned char f = ib[in_p++];
        return f >= 'P' && f <= 'S' ? KEY_F1 + (f - 'P') : 0;
    }
    if (d != '[' || in_p >= in_n) return 0;
    unsigned char e = ib[in_p++];
    if (e == '[' && in_p < in_n) {                 // Linux console: ESC [ [ A..E
        unsigned char f = ib[in_p++];
        return f >= 'A' && f <= 'E' ? KEY_F1 + (f - 'A') : 0;
    }
    if (e >= 'A' && e <= 'D') return KEY_UP + (e - 'A');
    if (isdigit(e)) {
        int n = e - '0';
        while (in_p < in_n && isdigit(ib[in_p])) n = n * 10 + ib[in_p++] - '0';
        while (in_p < in_n && ib[in_p] != '~' && !isalpha(ib[in_p])) in_p++;
        if (in_p < in_n) in_p++;
        if (n == 3) return KEY_DEL;
        if (n == 5) return KEY_PPAGE;
        if (n == 6) return KEY_NPAGE;
        if (n >= 11 && n <= 15) return KEY_F1 + (n - 11);      // F1-F5
        if (n >= 17 && n <= 21) return KEY_F6 + (n - 17);      // F6-F10
        if (n == 23 || n == 24) return KEY_F11 + (n - 23);     // F11-F12
    }
    return 0;
}
static void bs(char *s) {
    size_t n = strlen(s);
    while (n && ((unsigned char)s[n - 1] & 0xC0) == 0x80) n--;
    if (n) n--;
    s[n] = 0;
}
static void add(char *s, size_t cap, int c) {
    size_t n = strlen(s);
    if (n + 1 < cap) { s[n] = (char)c; s[n + 1] = 0; }
}
static void cycle_session(int d) { g.sel = (g.sel + d + g.ns) % g.ns; }

static void on_key(int k) {
    char *field = g.focus == 1 ? g.user : g.focus == 2 ? g.pw : nullptr;
    size_t cap = g.focus == 1 ? sizeof g.user : sizeof g.pw;
    if (g.msg[0]) set_msg(false, "");
    if (k >= KEY_F1 && k <= KEY_F12) { run_action(g.fk[k - KEY_F1].act); return; }
    switch (k) {
    case '\r': case '\n':
        if (g.focus < 2) g.focus++;
        else do_login();
        break;
    case '\t': case KEY_DOWN: g.focus = (g.focus + 1) % 3; break;
    case KEY_UP: g.focus = (g.focus + 2) % 3; break;
    case KEY_RIGHT: cycle_session(1); break;
    case KEY_LEFT: cycle_session(-1); break;
    case KEY_PPAGE: scale_step(10); break;
    case KEY_NPAGE: scale_step(-10); break;
    case '+': case '=': if (field) add(field, cap, k); else scale_step(10); break;
    case '-': case '_': if (field) add(field, cap, k); else scale_step(-10); break;
    case '0': if (field) add(field, cap, k); else scale_reset(); break;
    case 127: case 8: if (field) bs(field); break;
    case 0x03: case 0x1b: if (g.preview) quit_flag = 1; break;   // preview: Esc / Ctrl-C quits
    case 0x15: if (field) field[0] = 0; break;                    // Ctrl-U
    default:
        if (k >= 32 && k != 127 && k < 256 && field) add(field, cap, k);
    }
    if (g.focus != 1) refresh_user_conf();   // username committed -> layer that user's config
}

static void on_sig(int s) { if (s == SIGWINCH) winch_flag = 1; else quit_flag = 1; }

// ./lumdm --check [user]: what will the login screen actually use, and why was a line ignored?
static int do_check(const char *user) {
    g.check = true;
    FILE *f = fopen(CONF, "r");
    printf("system   %s  %s\n", CONF, f ? "found" : "MISSING - built-in defaults only");
    if (f) { fclose(f); load_kv(CONF, CFG_SYSTEM); }
    g.base.theme = g.theme; g.base.wp = g.wp; g.base.numlock = g.numlock;
    memcpy(g.base.fk, g.fk, sizeof g.fk);
    if (user) {
        struct passwd *pw = getpwnam(user);
        if (!pw) printf("user     %s  (no such user)\n", user);
        else {
            printf("user     %s/.config/lumdm.conf\n", pw->pw_dir);
            int rc = load_user_conf(user);
            printf("  status: %s\n", rc == UC_OK ? "loaded" : rc == UC_NONE ? "not present" : UC_WHY[rc]);
            if (rc >= UC_NOTREG) puts("  -> the login screen would show this and ignore the file");
        }
    } else puts("user     (pass a username to check their ~/.config/lumdm.conf)");
    printf("\ntheme=%s  wallpaper=%s  numlock=%s\n", THEMES[g.theme].name, WP_NAMES[g.wp], g.numlock < 0 ? "keep" : g.numlock ? "on" : "off");
    if (fnt.want[0]) {
        static struct FontBlob b;
        bool ok = font_load(fnt.want, "", fnt.size, 0, &b);
        printf("font     \"%s\" size %d -> ", fnt.want, fnt.size);
        if (ok) printf("%s (cell %dx%d)\n", b.path, b.cw, b.ch);
        else printf("FAILED: %s\n", b.err);
        puts("         (applies on the Linux console only, not in --preview or a terminal emulator)");
    } else puts("font     (console default)");
    puts("\nfunction keys, as the bottom bar shows them:");
    for (int i = 0; i < NFKEY; i++) {
        const FKey *k = &g.fk[i];
        if (!fk_on(k)) { printf("  F%-2d (off)\n", i + 1); continue; }
        printf("  F%-2d %-14s %-22s [%s]\n", i + 1, fk_label(k), k->act, k->src == 2 ? "user file" : k->src == 1 ? "system file" : "default");
    }
    int need = 1;
    for (int i = 0; i < NFKEY; i++) if (fk_on(&g.fk[i])) need += fk_w(i, 0);
    printf("\nbar needs %d columns for full labels (short labels are used on narrower screens)\n", need);
    return 0;
}
static int u8dec(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    int c = *s++, n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (n) c &= 0x3F >> n;
    while (n-- > 0 && (*s & 0xC0) == 0x80) c = (c << 6) | (*s++ & 0x3F);
    *p = (const char *)s;
    return c;
}
// ./lumdm --font-test [name|path] [px]: rasterise a font and draw the console bitmaps as text.
static int do_font_test(int argc, char **argv) {
    load_kv(CONF, CFG_SYSTEM);
    const char *want = argc > 2 ? argv[2] : fnt.want[0] ? fnt.want : "monospace";
    int px = argc > 3 ? atoi(argv[3]) : fnt.size;
    px = px < FONT_MIN ? FONT_MIN : px > FONT_MAX ? FONT_MAX : px;
    static struct FontBlob b;
    if (!font_load(want, "", px, 0, &b)) { fprintf(stderr, "lumdm: %s\n", b.err); return 1; }
    printf("%s  cell %dx%d\n", b.path, b.cw, b.ch);
    static const char *const lines[] = {"Welcome back", "Hello, World 0123", "user @#%&* ~", "\xe2\x95\xad\xe2\x94\x80\xe2\x94\x80\xe2\x95\xae \xe2\x97\x86 \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 \xe2\x80\xb9\xe2\x80\xba \xe2\x96\x8c\xe2\x96\x91\xe2\x96\x92\xe2\x96\x93"};
    int bpr = (b.cw + 7) / 8;
    for (size_t li = 0; li < sizeof lines / sizeof *lines; li++) {
        putchar('\n');
        for (int y = 0; y < b.ch; y++) {
            for (const char *t = lines[li]; *t;) {
                int cp = u8dec(&t), slot = 0;
                for (int i = 0; i < FONT_SLOTS; i++) if (b.cp[i] == cp) { slot = i; break; }
                const unsigned char *row = b.bits + (size_t)slot * FONT_ROWS * bpr + (size_t)y * bpr;
                for (int x = 0; x < b.cw; x++) fputs(row[x >> 3] & (0x80 >> (x & 7)) ? "\xe2\x96\x88" : " ", stdout);
            }
            putchar('\n');
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    g_argv = argv;
    if (argc > 1 && !strcmp(argv[1], "--check")) return do_check(argc > 2 ? argv[2] : nullptr);
    if (argc > 1 && !strcmp(argv[1], "--font-test")) return do_font_test(argc, argv);
    g.preview = argc > 1 && !strcmp(argv[1], "--preview");
    if (!g.preview && geteuid() != 0) { fputs("lumdm: must run as root (or use --preview)\n", stderr); return 1; }
    if (!isatty(0) || !isatty(1)) { fputs("lumdm: stdin/stdout must be a terminal\n", stderr); return 1; }

    const char *term = getenv("TERM");
    const char *tty = ttyname(0);
    if (tty) snprintf(tty_path, sizeof tty_path, "%s", tty);
    int vtn = 0;
    if (tty && sscanf(tty, "/dev/tty%d", &vtn) == 1 && vtn > 0) g.vtn = vtn;
    g.vt = !g.preview && vtn > 0 && term && !strcmp(term, "linux");

    load_kv(CONF, CFG_SYSTEM);
    g.base.theme = g.theme;
    g.base.numlock = g.numlock;
    g.base.wp = g.wp;
    memcpy(g.base.fk, g.fk, sizeof g.fk);
    load_sessions();
    load_kv(STATE, CFG_STATE);
    if (g.wp_override) g.wp = g.wp_override_val;
    for (int i = 0; i < g.ns; i++) if (!strcmp(g.s[i].name, g.last_sess)) g.sel = i;
    g.focus = g.user[0] ? 2 : 1;
    refresh_user_conf();

    struct sigaction sa = {.sa_handler = on_sig};
    sigemptyset(&sa.sa_mask);
    const int sigs[] = {SIGTERM, SIGINT, SIGWINCH};
    for (size_t i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, nullptr);
    signal(SIGHUP, SIG_IGN);       // tty hangups are handled by reopening, not by dying
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    get_size();
    term_init();
    if (fnt.err[0]) set_msg(true, fnt.err);
    draw();
    while (!quit_flag) {
        int to = (60 - (int)(time(nullptr) % 60)) * 1000;   // wake once a minute for the clock
        struct pollfd p = {0, POLLIN, 0};
        int r = poll(&p, 1, to);
        if (winch_flag) { winch_flag = 0; get_size(); g.dirty = true; }
        if (r == 0) g.dirty = true;                          // minute tick: clock, battery
        if (r > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(0, ib, sizeof ib);
            in_n = n > 0 ? (int)n : 0;
            in_p = 0;
            for (int k; (k = next_key());) on_key(k);
            if (g.preview && quit_flag) break;
        } else if (r > 0 && (p.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            struct timespec ts = {0, 200 * 1000 * 1000};   // avoid a busy loop while recovering
            nanosleep(&ts, nullptr);
            if (!reopen_tty()) break;
            term_init();
        }
        draw();
    }
    term_reset();
    return 0;
}
