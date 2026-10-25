// vtfont.h - use an installed TrueType/OpenType font on the Linux console.
// Included by lumdm.c (single translation unit).
//
// The kernel console only understands 1-bit bitmap fonts, so a TTF/OTF has to be
// rasterised first. That runs in a forked child that has dropped to "nobody" with
// tight rlimits (stb_truetype must not be trusted with hostile files), and the
// finished 256-glyph bitmap comes back through a pipe. The parent only does ioctls.
//
// The original console font + unicode map are saved first and put back before a
// session starts / when lumdm exits, so a tty login shell gets its normal font.
#include <sys/mman.h>
#include <sys/resource.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wtype-limits"
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#pragma GCC diagnostic pop

enum { FONT_MIN = 8, FONT_MAX = 32, FONT_SLOTS = 256, FONT_ROWS = 32, FONT_BPR = 4 };

// What comes back from the rasteriser child.
struct FontBlob {
    int ok, cw, ch;
    char path[PATH_MAX];
    char err[96];
    unsigned short cp[FONT_SLOTS];                          // unicode of each glyph slot (0 = unused)
    unsigned char bits[FONT_SLOTS * FONT_ROWS * FONT_BPR];  // per glyph: 32 rows of ceil(cw/8) bytes
};

static struct {
    char want[CMD_LEN];     // font = ... (name or absolute path)
    char path[PATH_MAX];    // resolved file, cached after the first successful load
    char err[96];           // last failure, shown on screen once
    int size;               // font_size: cell height in px at 100% zoom
    int cur_px;             // size currently loaded, 0 = console's own font
    int cw, ch;
    bool orig_ok;
    struct console_font_op orig;
    unsigned char *orig_data;
    struct unipair *orig_map;
    unsigned orig_n;
} fnt = {.size = 16};

static bool font_active(void) { return fnt.cur_px > 0; }

// ------------------------------------------------------------ font lookup ----
static void fnorm(char *d, size_t n, const char *s) {       // lower-case alphanumerics only
    size_t i = 0;
    for (; *s && i + 1 < n; s++) if (isalnum((unsigned char)*s)) d[i++] = (char)tolower((unsigned char)*s);
    d[i] = 0;
}
static bool has_style(const char *s) {
    static const char *const k[] = {"bold", "italic", "oblique", "light", "thin", "black", "heavy", "extra",
                                    "semi", "demi", "condensed", "narrow", "wide", "medium", "slanted", "wght"};
    for (size_t i = 0; i < sizeof k / sizeof *k; i++) if (strstr(s, k[i])) return true;
    return false;
}
static int fscore(const char *base, const char *w) {         // lower is better
    size_t wl = strlen(w);
    if (!strcmp(base, w)) return 0;
    if (!strncmp(base, w, wl)) {
        const char *rest = base + wl;
        if (!strcmp(rest, "regular") || !strcmp(rest, "book") || !strcmp(rest, "roman")) return 1;
        return (has_style(rest) ? 60 : 10) + (int)strlen(rest);
    }
    if (strstr(base, w)) return (has_style(base) ? 160 : 100) + (int)strlen(base);
    return 1 << 20;
}

struct FScan { char w[96]; int best; char path[PATH_MAX]; int nfiles; bool fam; };

static bool font_ext(const char *n) {
    size_t l = strlen(n);
    return l > 4 && (!strcasecmp(n + l - 4, ".ttf") || !strcasecmp(n + l - 4, ".otf") || !strcasecmp(n + l - 4, ".ttc"));
}
static void name_norm(const stbtt_fontinfo *f, int id, char *out, size_t n) {
    int len = 0;
    out[0] = 0;
    const char *s = stbtt_GetFontNameString(f, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP,
                                            STBTT_MS_LANG_ENGLISH, id);
    char tmp[128];
    size_t k = 0;
    if (s && len > 0) {                                   // UTF-16BE: keep the ASCII range
        for (int i = 0; i + 1 < len && k + 1 < sizeof tmp; i += 2) if (!s[i]) tmp[k++] = s[i + 1];
    } else if ((s = stbtt_GetFontNameString(f, &len, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
                                            STBTT_MAC_LANG_ENGLISH, id)) && len > 0) {
        for (int i = 0; i < len && k + 1 < sizeof tmp; i++) tmp[k++] = s[i];
    }
    tmp[k] = 0;
    fnorm(out, n, tmp);
}
static void fscan_dir(const char *dir, int depth, struct FScan *fs) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && fs->nfiles < 6000 && !(fs->fam && fs->best == 0);) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 5) fscan_dir(p, depth + 1, fs); continue; }
        if (!S_ISREG(st.st_mode) || !font_ext(e->d_name)) continue;
        fs->nfiles++;
        if (!fs->fam) {
            char base[PATH_MAX];
            snprintf(base, sizeof base, "%.*s", (int)strlen(e->d_name) - 4, e->d_name);
            char nb[128];
            fnorm(nb, sizeof nb, base);
            int sc = fscore(nb, fs->w);
            if (sc < fs->best || (sc == fs->best && strlen(p) < strlen(fs->path))) { fs->best = sc; snprintf(fs->path, sizeof fs->path, "%s", p); }
        } else if (fs->best > 0 && st.st_size > 64 && st.st_size < (64 << 20)) {
            int fd = open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            void *m = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (m == MAP_FAILED) continue;
            stbtt_fontinfo f;
            int off = stbtt_GetFontOffsetForIndex(m, 0);
            if (off >= 0 && stbtt_InitFont(&f, m, off)) {
                char fam[96], sub[48], full[96];
                name_norm(&f, 1, fam, sizeof fam);
                name_norm(&f, 2, sub, sizeof sub);
                name_norm(&f, 4, full, sizeof full);
                bool reg = !strcmp(sub, "regular") || !strcmp(sub, "book") || !strcmp(sub, "roman");
                if (!strcmp(full, fs->w) || (reg && !strcmp(fam, fs->w))) { fs->best = 0; snprintf(fs->path, sizeof fs->path, "%s", p); }
            }
            munmap(m, (size_t)st.st_size);
            if (fs->best == 0) break;
        }
    }
    closedir(d);
}
static bool font_find(const char *want, char *out, size_t n) {
    static const char *const dirs[] = {"/usr/share/fonts", "/usr/local/share/fonts", "/usr/share/X11/fonts",
                                       "/run/current-system/sw/share/X11/fonts", "/usr/lib/X11/fonts", nullptr};
    static const char *const mono[] = {"dejavusansmono", "liberationmono", "notosansmono", "ubuntumono",
                                       "jetbrainsmono", "firacode", "hack", "cousine", "sourcecodepro", nullptr};
    char w[96];
    fnorm(w, sizeof w, want);
    if (!w[0]) return false;
    const char *single[2] = {w, nullptr};
    const char *const *list = !strcmp(w, "monospace") || !strcmp(w, "mono") ? mono : single;
    for (int i = 0; list[i]; i++) {
        struct FScan fs = {.best = 1 << 20};
        snprintf(fs.w, sizeof fs.w, "%s", list[i]);
        for (int pass = 0; pass < 2; pass++) {                // 1: by file name, 2: by the name inside the font
            fs.fam = pass == 1;
            fs.best = 1 << 20;
            fs.path[0] = 0;
            for (int di = 0; dirs[di]; di++) fscan_dir(dirs[di], 0, &fs);
            if (fs.best < (1 << 20)) { snprintf(out, n, "%s", fs.path); return true; }
        }
    }
    return false;
}

// ----------------------------------------------------------- rasterising ----
enum { K_REPL, K_H, K_V, K_RTL, K_RTR, K_RBL, K_RBR, K_SQTL, K_SQTR, K_SQBL, K_SQBR, K_LHALF, K_RHALF, K_FULL,
       K_UHALF, K_DHALF, K_LIGHT, K_MED, K_DARK, K_DIAMOND, K_BULLET, K_LCHEV, K_RCHEV };
static const struct { unsigned short cp; unsigned char kind; } SPEC[] = {
    {0xFFFD, K_REPL},  {0x2500, K_H},     {0x2502, K_V},    {0x256D, K_RTL},   {0x256E, K_RTR},    {0x2570, K_RBL},
    {0x256F, K_RBR},   {0x250C, K_SQTL},  {0x2510, K_SQTR}, {0x2514, K_SQBL},  {0x2518, K_SQBR},   {0x258C, K_LHALF},
    {0x2590, K_RHALF}, {0x2588, K_FULL},  {0x2580, K_UHALF}, {0x2584, K_DHALF}, {0x2591, K_LIGHT}, {0x2592, K_MED},
    {0x2593, K_DARK},  {0x25C6, K_DIAMOND}, {0x2022, K_BULLET}, {0x2039, K_LCHEV}, {0x203A, K_RCHEV},
};
#define NSPEC (int)(sizeof SPEC / sizeof *SPEC)   // slots 0..NSPEC-1 are the synthesised glyphs

static void gpix(struct FontBlob *b, int slot, int x, int y) {
    int bpr = (b->cw + 7) / 8;
    if (x < 0 || y < 0 || x >= b->cw || y >= b->ch) return;
    b->bits[(size_t)slot * FONT_ROWS * bpr + (size_t)y * bpr + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
}
static void gline(struct FontBlob *b, int slot, float x0, float y0, float x1, float y1, int t) {
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0)) * 2) + 1;
    for (int i = 0; i <= n; i++) {
        float x = x0 + (x1 - x0) * (float)i / (float)n, y = y0 + (y1 - y0) * (float)i / (float)n;
        for (int a = 0; a < t; a++) for (int c = 0; c < t; c++) gpix(b, slot, (int)x + a - (t - 1) / 2, (int)y + c - (t - 1) / 2);
    }
}
// Corner piece: arms go sx (+1 right / -1 left) and sy (+1 down / -1 up), joined by an arc of radius rad.
static void gcorner(struct FontBlob *b, int slot, int sx, int sy, float rad, int t) {
    int xl = (b->cw - t) / 2, yl = (b->ch - t) / 2;
    float cx = (float)xl + (float)t / 2, cy = (float)yl + (float)t / 2;
    float acx = cx + (float)sx * rad, acy = cy + (float)sy * rad, half = (float)t / 2 + 0.001f;
    for (int y = 0; y < b->ch; y++) for (int x = 0; x < b->cw; x++) {
        float px = (float)x + 0.5f, py = (float)y + 0.5f, d = 1e9f;
        bool ha = (px - acx) * (float)sx >= 0, va = (py - acy) * (float)sy >= 0;
        if (ha) d = fminf(d, fabsf(py - cy));
        if (va) d = fminf(d, fabsf(px - cx));
        float lim = half;
        if (!ha && !va) {
            if (rad <= 0) continue;
            d = fabsf(hypotf(px - acx, py - acy) - rad);
            lim = half + 0.25f;
        }
        if (d <= lim) gpix(b, slot, x, y);
    }
}
static void gsynth(struct FontBlob *b, int slot, int kind) {
    int cw = b->cw, ch = b->ch;
    int t = (int)lroundf((float)ch / 14.0f);
    if (t < 1) t = 1;
    int xl = (cw - t) / 2, yl = (ch - t) / 2;
    float rad = fminf(fminf((float)xl + (float)t / 2, (float)(cw - xl) - (float)t / 2),
                      fminf((float)yl + (float)t / 2, (float)(ch - yl) - (float)t / 2));
    rad = floorf(rad);
    float mx = (float)cw / 2, my = (float)ch / 2;
    switch (kind) {
    case K_H: for (int x = 0; x < cw; x++) for (int a = 0; a < t; a++) gpix(b, slot, x, yl + a); break;
    case K_V: for (int y = 0; y < ch; y++) for (int a = 0; a < t; a++) gpix(b, slot, xl + a, y); break;
    case K_RTL: gcorner(b, slot, +1, +1, rad, t); break;
    case K_RTR: gcorner(b, slot, -1, +1, rad, t); break;
    case K_RBL: gcorner(b, slot, +1, -1, rad, t); break;
    case K_RBR: gcorner(b, slot, -1, -1, rad, t); break;
    case K_SQTL: gcorner(b, slot, +1, +1, 0, t); break;
    case K_SQTR: gcorner(b, slot, -1, +1, 0, t); break;
    case K_SQBL: gcorner(b, slot, +1, -1, 0, t); break;
    case K_SQBR: gcorner(b, slot, -1, -1, 0, t); break;
    case K_LHALF: for (int y = 0; y < ch; y++) for (int x = 0; x < cw / 2; x++) gpix(b, slot, x, y); break;
    case K_RHALF: for (int y = 0; y < ch; y++) for (int x = cw / 2; x < cw; x++) gpix(b, slot, x, y); break;
    case K_UHALF: for (int y = 0; y < ch / 2; y++) for (int x = 0; x < cw; x++) gpix(b, slot, x, y); break;
    case K_DHALF: for (int y = ch / 2; y < ch; y++) for (int x = 0; x < cw; x++) gpix(b, slot, x, y); break;
    case K_FULL: for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) gpix(b, slot, x, y); break;
    case K_LIGHT: for (int y = 0; y < ch; y += 2) for (int x = (y / 2) % 2 * 2; x < cw; x += 4) gpix(b, slot, x, y); break;
    case K_MED: for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) if (!((x + y) & 1)) gpix(b, slot, x, y); break;
    case K_DARK: for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) if (!((x & 1) && (y & 1))) gpix(b, slot, x, y); break;
    case K_DIAMOND: {
        float hw = fminf((float)cw / 2 - 0.5f, (float)ch / 4);
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++)
            if (fabsf((float)x + 0.5f - mx) + fabsf((float)y + 0.5f - my) <= hw) gpix(b, slot, x, y);
        break;
    }
    case K_BULLET: {
        float rr_ = fmaxf(1.5f, fminf((float)cw, (float)ch) * 0.27f);
        for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++)
            if (hypotf((float)x + 0.5f - mx, (float)y + 0.5f - my) <= rr_) gpix(b, slot, x, y);
        break;
    }
    case K_LCHEV: case K_RCHEV: {
        float a = (float)cw * 0.30f, c2 = (float)cw * 0.70f, y0 = (float)ch * 0.30f, y1 = (float)ch * 0.70f;
        if (kind == K_LCHEV) { gline(b, slot, c2, y0, a, my, t); gline(b, slot, a, my, c2, y1, t); }
        else { gline(b, slot, a, y0, c2, my, t); gline(b, slot, c2, my, a, y1, t); }
        break;
    }
    case K_REPL:
        for (int x = 1; x < cw - 1; x++) { gpix(b, slot, x, 2); gpix(b, slot, x, ch - 3); }
        for (int y = 2; y < ch - 2; y++) { gpix(b, slot, 1, y); gpix(b, slot, cw - 2, y); }
        break;
    }
}
static bool gtext(struct FontBlob *b, const stbtt_fontinfo *f, float scale, int base, int slot, int cp) {
    int gi = stbtt_FindGlyphIndex(f, cp);
    if (!gi) return false;
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(f, gi, &adv, &lsb);
    stbtt_GetGlyphBitmapBox(f, gi, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w < 256 && h < 256) {
        unsigned char *bm = calloc((size_t)w * (size_t)h, 1);
        if (!bm) return false;
        stbtt_MakeGlyphBitmap(f, bm, w, h, w, scale, scale, gi);
        int bx = x0 + (int)floorf(((float)b->cw - (float)adv * scale) / 2 + 0.5f), by = base + y0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (bm[y * w + x] >= 96) gpix(b, slot, bx + x, by + y);
        free(bm);
    }
    return true;
}

static void font_build(struct FontBlob *b, const char *want, const char *known, int px, int force_w) {
    if (known && known[0]) snprintf(b->path, sizeof b->path, "%s", known);
    else if (strchr(want, '/')) snprintf(b->path, sizeof b->path, "%s", want);
    else if (!font_find(want, b->path, sizeof b->path)) { snprintf(b->err, sizeof b->err, "font \"%.60s\" not found", want); return; }
    int fd = open(b->path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 64 || st.st_size > (128 << 20)) {
        snprintf(b->err, sizeof b->err, "cannot read %.70s", b->path);
        if (fd >= 0) close(fd);
        return;
    }
    unsigned char *data = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    stbtt_fontinfo f;
    int off = data == MAP_FAILED ? -1 : stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0 || !stbtt_InitFont(&f, data, off)) { snprintf(b->err, sizeof b->err, "not a usable font: %.60s", b->path); return; }

    int asc, desc, gap, adv, lsb;
    stbtt_GetFontVMetrics(&f, &asc, &desc, &gap);
    float scale = stbtt_ScaleForPixelHeight(&f, (float)px);
    stbtt_GetCodepointHMetrics(&f, 'M', &adv, &lsb);
    int cw = force_w ? force_w : (int)lroundf((float)adv * scale);
    cw = cw < 5 ? 5 : cw > 32 ? 32 : cw;
    b->cw = cw;
    b->ch = px;
    int base = (int)lroundf((float)asc * scale);

    for (int i = 0; i < NSPEC; i++) { b->cp[i] = SPEC[i].cp; gsynth(b, i, SPEC[i].kind); }
    for (int cp = 0x20; cp <= 0x7E; cp++) if (gtext(b, &f, scale, base, cp, cp)) b->cp[cp] = (unsigned short)cp;
    for (int cp = 0xA0; cp <= 0xFF; cp++) if (gtext(b, &f, scale, base, cp, cp)) b->cp[cp] = (unsigned short)cp;
    b->cp[0x20] = 0x20;                                     // slot 0x20 is what the console clears with: keep it blank
    b->ok = 1;
}

[[noreturn]] static void font_child(const char *want, const char *known, int px, int force_w, int wfd) {
    static struct FontBlob b;
    memset(&b, 0, sizeof b);
    if (geteuid() == 0) {
        struct rlimit as = {512u << 20, 512u << 20}, cpu = {10, 10};
        setrlimit(RLIMIT_AS, &as);
        setrlimit(RLIMIT_CPU, &cpu);
        if (setgroups(0, nullptr) || setgid(65534) || setuid(65534)) snprintf(b.err, sizeof b.err, "cannot drop privileges");
        else font_build(&b, want, known, px, force_w);
    } else font_build(&b, want, known, px, force_w);
    for (size_t o = 0; o < sizeof b;) {
        ssize_t w = write(wfd, (char *)&b + o, sizeof b - o);
        if (w < 0) { if (errno == EINTR) continue; break; }
        o += (size_t)w;
    }
    _exit(0);
}
static bool font_load(const char *want, const char *known, int px, int force_w, struct FontBlob *b) {
    int p[2];
    memset(b, 0, sizeof *b);
    if (pipe2(p, O_CLOEXEC)) { snprintf(b->err, sizeof b->err, "pipe failed"); return false; }
    fflush(nullptr);
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); snprintf(b->err, sizeof b->err, "fork failed"); return false; }
    if (pid == 0) { close(p[0]); font_child(want, known, px, force_w, p[1]); }
    close(p[1]);
    size_t got = 0;
    ssize_t n;
    while (got < sizeof *b && (n = read(p[0], (char *)b + got, sizeof *b - got)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        got += (size_t)n;
    }
    close(p[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (got != sizeof *b) { memset(b, 0, sizeof *b); snprintf(b->err, sizeof b->err, "font helper crashed"); return false; }
    return b->ok;
}

// -------------------------------------------------------- talking to the VT ----
static bool font_save_orig(void) {
    if (fnt.orig_ok) return true;
    free(fnt.orig_data);
    free(fnt.orig_map);
    fnt.orig_data = calloc(512 * FONT_ROWS * FONT_BPR, 1);
    unsigned cap = 8192;
    fnt.orig_map = calloc(cap, sizeof *fnt.orig_map);
    if (!fnt.orig_data || !fnt.orig_map) return false;
    fnt.orig = (struct console_font_op){.op = KD_FONT_OP_GET, .width = 32, .height = 32, .charcount = 512, .data = fnt.orig_data};
    if (ioctl(0, KDFONTOP, &fnt.orig) < 0) return false;
    struct unimapdesc ud = {.entry_ct = (unsigned short)cap, .entries = fnt.orig_map};
    if (ioctl(0, GIO_UNIMAP, &ud) < 0) return false;
    fnt.orig_n = ud.entry_ct;
    fnt.orig_ok = true;
    return true;
}
static bool font_set(const struct FontBlob *b) {
    struct console_font_op op = {.op = KD_FONT_OP_SET, .width = (unsigned)b->cw, .height = (unsigned)b->ch,
                                 .charcount = FONT_SLOTS, .data = (unsigned char *)b->bits};
    if (ioctl(0, KDFONTOP, &op) < 0) return false;
    struct unipair up[FONT_SLOTS + 4];
    unsigned n = 0;
    for (int i = 0; i < FONT_SLOTS; i++) if (b->cp[i]) up[n++] = (struct unipair){b->cp[i], (unsigned short)i};
    up[n++] = (struct unipair){0x2666, 15};                 // black diamond suit -> same glyph as U+25C6
    struct unimapinit ui = {0};
    ioctl(0, PIO_UNIMAPCLR, &ui);
    struct unimapdesc ud = {.entry_ct = (unsigned short)n, .entries = up};
    ioctl(0, PIO_UNIMAP, &ud);
    return true;
}
static void font_restore(void) {
    if (!fnt.cur_px || !fnt.orig_ok) { fnt.cur_px = 0; return; }
    struct console_font_op op = fnt.orig;
    op.op = KD_FONT_OP_SET;
    op.data = fnt.orig_data;
    ioctl(0, KDFONTOP, &op);
    struct unimapinit ui = {0};
    ioctl(0, PIO_UNIMAPCLR, &ui);
    struct unimapdesc ud = {.entry_ct = (unsigned short)fnt.orig_n, .entries = fnt.orig_map};
    ioctl(0, PIO_UNIMAP, &ud);
    fnt.cur_px = 0;
}
static int font_px(int scale) {
    int v = fnt.size * scale / 100;
    return v < FONT_MIN ? FONT_MIN : v > FONT_MAX ? FONT_MAX : v;
}
// (Re)load the configured font at the size implied by the current zoom. Caller re-reads the window size.
static void font_apply(int scale) {
    fnt.err[0] = 0;
    if (!fnt.want[0]) return;
    int px = font_px(scale);
    if (fnt.cur_px == px) return;
    if (!font_save_orig()) { snprintf(fnt.err, sizeof fnt.err, "font: cannot read the console font"); return; }
    static struct FontBlob b;
    int fw = 0;
    for (int tries = 0; tries < 2; tries++) {
        if (!font_load(fnt.want, fnt.path, px, fw, &b)) { snprintf(fnt.err, sizeof fnt.err, "%s", b.err[0] ? b.err : "font failed"); return; }
        snprintf(fnt.path, sizeof fnt.path, "%s", b.path);
        if (font_set(&b)) { fnt.cur_px = px; fnt.cw = b.cw; fnt.ch = b.ch; return; }
        if (b.cw == 8) break;
        fw = 8;                                              // VGA text mode only does 8-pixel-wide cells
    }
    snprintf(fnt.err, sizeof fnt.err, "console refused the font (%s)", strerror(errno));
}
