// lumdm - a tiny, pretty, TTY-based display manager for X11 and Wayland sessions.
// Language: C23 (ISO/IEC 9899:2024).  Only dependency: libpam.
// Config: /etc/lumdm.conf (system) then ~/.config/lumdm.conf (per-user, restricted).
// Build: cc -std=c23 -O2 -s -o lumdm lumdm.c -lpam      (GCC 13: use -std=c2x)
// Try the UI without root/PAM:  ./lumdm --preview
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
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
constexpr int MAX_SESS = 128;
constexpr int NAME_LEN = 64;
constexpr int CMD_LEN = 256;
constexpr int USER_LEN = 64;
constexpr int PW_LEN = 256;
constexpr int NFKEY = 12;

typedef struct { char act[CMD_LEN]; char label[24]; } FKey;

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
    int ns, sel, focus, theme, rows, cols, vtn;
    char user[USER_LEN], pw[PW_LEN], msg[160], last_sess[NAME_LEN];
    char xserver[128], xinit[128], cfg_user[USER_LEN];
    int numlock;                       // -1 keep, 0 off, 1 on
    FKey fk[NFKEY];
    struct { int theme, numlock; FKey fk[NFKEY]; } base;   // system-level values
    bool err, showpw, vt, preview;
} g = {.focus = 1, .rows = 24, .cols = 80, .vtn = 1, .numlock = -1,
       .xserver = "/usr/bin/X", .xinit = "/usr/bin/xinit"};

static volatile sig_atomic_t quit_flag, winch_flag;

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
    }
    put(ESC "[?25l");
    flush();
}
static void term_reset(void) {
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
    if (k->label[0]) return k->label;
    if (!strcmp(k->act, "poweroff")) return "shutdown";
    if (!strcmp(k->act, "showpw")) return "show password";
    if (!strncmp(k->act, "exec:", 5)) return "run";
    return k->act;
}

// ------------------------------------------------------------------ draw ----
static void draw(void) {
    const Theme *T = &THEMES[g.theme];
    const Glyphs *G = g.vt ? &GV : &GT;
    char host[64] = "localhost", bat[48], when[96];
    gethostname(host, sizeof host - 1);
    battery(bat, sizeof bat);
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(when, sizeof when, "%A, %d %B  \xc2\xb7  %H:%M", &tm);

    put(ESC "[?25l" ESC "[0m");
    // Background: smooth gradient (truecolor) or dithered blocks (VT).
    static const char *dither[] = {" ", "░", "▒", "▓"};
    for (int r = 2; r < g.rows; r++) {
        at(r, 1);
        int t = (r - 2) * 100 / (g.rows - 3 > 0 ? g.rows - 3 : 1);
        t = t * t / 100;
        if (g.vt) {
            color(R_BG, true);
            color(R_BG2, false);
            rep(dither[t < 45 ? 0 : t < 65 ? 1 : t < 85 ? 2 : 3], g.cols);
        } else {
            unsigned c = lerp(T->c[R_BG], T->c[R_BG2], t);
            put(ESC "[48;2;%u;%u;%um", c >> 16, (c >> 8) & 255, c & 255);
            spaces(g.cols);
        }
    }
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
    // Bottom bar with key hints.
    at(g.rows, 1);
    color(R_PANEL, true);
    spaces(g.cols);
    at(g.rows, 2);
    int used = 1;
    for (int i = 0; i < NFKEY; i++) {
        const FKey *k = &g.fk[i];
        if (!k->act[0] || !strcmp(k->act, "none")) continue;
        char kn[8];
        snprintf(kn, sizeof kn, "F%d", i + 1);
        const char *lb = fk_label(k);
        int w = (int)strlen(kn) + 1 + u8len(lb) + 3;
        if (used + w > g.cols) break;
        color(R_ACCENT, false);
        put("%s", kn);
        color(R_DIM, false);
        put(" %s   ", lb);
        used += w;
    }

    // Panel.
    int W = g.cols - 4 < 50 ? g.cols - 4 : 50, H_ = 12;
    if (W < 24) W = g.cols;
    int y0 = (g.rows - H_) / 2 + 1, x0 = (g.cols - W) / 2 + 1;
    if (y0 < 2) y0 = 2;
    for (int i = 0; i < H_; i++) {
        at(y0 + i, x0);
        color(R_PANEL, true);
        color(R_DIM, false);
        if (i == 0) { put("%s", G->tl); rep(G->h, W - 2); put("%s", G->tr); }
        else if (i == H_ - 1) { put("%s", G->bl); rep(G->h, W - 2); put("%s", G->br); }
        else { put("%s", G->v); spaces(W - 2); put("%s", G->v); }
    }
    int iw = W - 4;
    at(y0 + 2, x0 + 2);
    color(R_PANEL, true);
    color(R_FG, false);
    if (!g.vt) put(ESC "[1m");
    fit("Welcome back", iw, 0);
    put(ESC "[22m");
    at(y0 + 3, x0 + 2);
    color(R_DIM, false);
    fit(when, iw, 0);

    // Fields.
    static const char *lab[] = {"session", "user", "password"};
    int vw = iw - 12, curcol = 0;
    for (int k = 0; k < 3; k++) {
        bool f = g.focus == k;
        int y = y0 + 5 + k;
        at(y, x0 + 2);
        color(R_FIELD, true);
        color(f ? R_ACCENT : R_FIELD, false);
        put("%s ", G->bar);
        color(f ? R_ACCENT : R_DIM, false);
        fit(lab[k], 9, -1);
        color(R_FG, false);
        if (k == 0) {
            fit(g.s[g.sel].name, vw - 5, -1);
            color(R_DIM, false);
            put(" %s %s ", G->l, G->r);
        } else if (k == 1) {
            fit(g.user, vw, -1);
        } else {
            int n = u8len(g.pw);
            if (g.showpw) fit(g.pw, vw, -1);
            else { int d = n < vw ? n : vw; rep(G->dot, d); spaces(vw - d); }
            if (f) curcol = x0 + 2 + 11 + (n < vw - 1 ? n : vw - 1);
        }
        put(" ");
    }
    if (g.focus == 1) curcol = x0 + 2 + 11 + (u8len(g.user) < vw - 1 ? u8len(g.user) : vw - 1);

    // Message line.
    at(y0 + 9, x0 + 2);
    color(R_PANEL, true);
    const char *m = g.msg;
    bool e = g.err;
    if (!m[0] && caps_on()) { m = "caps lock is on"; e = true; }
    color(e ? R_ERR : R_DIM, false);
    fit(m, iw, 0);

    if (g.focus > 0) { at(y0 + 5 + g.focus, curcol); color(R_FIELD, true); color(R_FG, false); put(ESC "[?25h"); }
    flush();
}

// ------------------------------------------------------- config & session ----
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
}
static void set_theme(const char *n) {
    for (int i = 0; i < NTHEMES; i++) if (!strcmp(THEMES[i].name, n)) { g.theme = i; return; }
}
enum { CFG_SYSTEM, CFG_USER, CFG_STATE };
static bool is_builtin(const char *a) {
    static const char *b[] = {"none", "poweroff", "reboot", "suspend", "hibernate", "theme", "showpw"};
    for (size_t i = 0; i < sizeof b / sizeof *b; i++) if (!strcmp(a, b[i])) return true;
    return false;
}
// System file: everything, incl. exec:<cmd> (runs as root - file is root-owned).
// User file: theme, numlock, fN, fN_label - built-in actions only, never exec:.
static void parse_stream(FILE *f, int mode) {
    char l[CMD_LEN + 64];
    while (fgets(l, sizeof l, f)) {
        trim(l);
        char *p = l;
        while (isspace((unsigned char)*p)) p++;
        char *eq = strchr(p, '=');
        if (*p == '#' || !eq) continue;
        *eq = 0;
        char *k = p, *v = eq + 1;
        trim(k);
        while (isspace((unsigned char)*v)) v++;
        if (strncmp(v, "exec:", 5)) { char *c = strstr(v, " #"); if (c) *c = 0; trim(v); }
        if (mode == CFG_STATE) {
            if (!strcmp(k, "user")) snprintf(g.user, sizeof g.user, "%s", v);
            else if (!strcmp(k, "session")) snprintf(g.last_sess, sizeof g.last_sess, "%s", v);
        } else if (!strcmp(k, "theme")) set_theme(v);
        else if (!strcmp(k, "numlock")) g.numlock = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
        else if (mode == CFG_SYSTEM && !strcmp(k, "xserver")) snprintf(g.xserver, sizeof g.xserver, "%s", v);
        else if (mode == CFG_SYSTEM && !strcmp(k, "xinit")) snprintf(g.xinit, sizeof g.xinit, "%s", v);
        else if (k[0] == 'f') {
            char *end;
            long n = strtol(k + 1, &end, 10);
            if (end == k + 1 || n < 1 || n > NFKEY) continue;
            FKey *fk = &g.fk[n - 1];
            if (!*end) {
                if (is_builtin(v) || (mode == CFG_SYSTEM && !strncmp(v, "exec:", 5) && v[5]))
                    snprintf(fk->act, sizeof fk->act, "%s", v);
            } else if (!strcmp(end, "_label")) snprintf(fk->label, sizeof fk->label, "%s", v);
        }
    }
}
static void load_kv(const char *path, int mode) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    parse_stream(f, mode);
    fclose(f);
}
// ~/.config/lumdm.conf of the typed user, read before login - so be paranoid:
// no symlinks, regular file, owned by that user, small, not world-writable.
static void load_user_conf(const char *user) {
    struct passwd *pw = getpwnam(user);
    if (!pw) return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/.config/lumdm.conf", pw->pw_dir);
    int fd = open(p, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != pw->pw_uid || st.st_size > 4096 || (st.st_mode & S_IWOTH)) {
        close(fd);
        return;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return; }
    parse_stream(f, CFG_USER);
    fclose(f);
}
static void defaults(void) {
    static const char *d[] = {"poweroff", "reboot", "suspend", "theme", "showpw"};
    for (int i = 0; i < 5; i++) snprintf(g.fk[i].act, sizeof g.fk[i].act, "%s", d[i]);
}
// Re-layer: defaults < /etc/lumdm.conf < ~/.config/lumdm.conf of the selected user.
static void refresh_user_conf(void) {
    if (!strcmp(g.user, g.cfg_user)) return;
    snprintf(g.cfg_user, sizeof g.cfg_user, "%s", g.user);
    g.theme = g.base.theme;
    g.numlock = g.base.numlock;
    memcpy(g.fk, g.base.fk, sizeof g.fk);
    if (g.user[0]) load_user_conf(g.user);
    palette();
    apply_numlock();
}
static void save_state(void) {
    if (g.preview) return;
    mkdir(STATE_DIR, 0755);
    FILE *f = fopen(STATE, "w");
    if (!f) return;
    fprintf(f, "user=%s\nsession=%s\n", g.user, g.s[g.sel].name);
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
static void run_action(const char *a) {
    if (!*a || !strcmp(a, "none")) return;
    if (!strcmp(a, "theme")) { g.theme = (g.theme + 1) % NTHEMES; palette(); }
    else if (!strcmp(a, "showpw")) g.showpw = !g.showpw;
    else if (!strncmp(a, "exec:", 5)) run_cmd(a + 5);       // only ever loaded from /etc/lumdm.conf
    else power(a);                                          // poweroff | reboot | suspend | hibernate
}

// ----------------------------------------------------------------- input ----
enum { KEY_UP = 1000, KEY_DOWN, KEY_RIGHT, KEY_LEFT, KEY_DEL, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
       KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12 };
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
    case 127: case 8: if (field) bs(field); break;
    case 0x03: case 0x1b: if (g.preview) quit_flag = 1; break;   // preview: Esc / Ctrl-C quits
    case 0x15: if (field) field[0] = 0; break;                    // Ctrl-U
    default:
        if (k >= 32 && k != 127 && k < 256 && field) add(field, cap, k);
    }
    if (g.focus != 1) refresh_user_conf();   // username committed -> layer that user's config
}

static void on_sig(int s) { if (s == SIGWINCH) winch_flag = 1; else quit_flag = 1; }

int main(int argc, char **argv) {
    g.preview = argc > 1 && !strcmp(argv[1], "--preview");
    if (!g.preview && geteuid() != 0) { fputs("lumdm: must run as root (or use --preview)\n", stderr); return 1; }
    if (!isatty(0) || !isatty(1)) { fputs("lumdm: stdin/stdout must be a terminal\n", stderr); return 1; }

    const char *term = getenv("TERM");
    const char *tty = ttyname(0);
    if (tty) snprintf(tty_path, sizeof tty_path, "%s", tty);
    int vtn = 0;
    if (tty && sscanf(tty, "/dev/tty%d", &vtn) == 1 && vtn > 0) g.vtn = vtn;
    g.vt = !g.preview && vtn > 0 && term && !strcmp(term, "linux");

    defaults();
    load_kv(CONF, CFG_SYSTEM);
    g.base.theme = g.theme;
    g.base.numlock = g.numlock;
    memcpy(g.base.fk, g.fk, sizeof g.fk);
    load_sessions();
    load_kv(STATE, CFG_STATE);
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
    draw();
    while (!quit_flag) {
        int to = (60 - (int)(time(nullptr) % 60)) * 1000;   // wake once a minute for the clock
        struct pollfd p = {0, POLLIN, 0};
        int r = poll(&p, 1, to);
        if (winch_flag) { winch_flag = 0; get_size(); }
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
            get_size();
            term_init();
        }
        draw();
    }
    term_reset();
    return 0;
    }
}
