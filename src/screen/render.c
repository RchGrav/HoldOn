#include "hold/config.h"
#include "hold/screen.h"

#include "font_data.h"

/* Snapshot -> PNG. A framebuffer of 8x16-pixel cells (times scale), an
 * optional coordinate ruler around it, a cursor ring and a mouse reticle on
 * top, and a small self-contained PNG encoder (fixed-Huffman deflate). */

typedef struct {
    uint8_t r, g, b;
} rgb;

static const rgb THEME_FG = {0xd8, 0xda, 0xdf};
static const rgb THEME_BG = {0x16, 0x18, 0x1d};
static const rgb RULER_BG = {0x2a, 0x2e, 0x37};
static const rgb RULER_FG = {0xf2, 0xf2, 0xf2};
static const rgb CURSOR_MARK = {0xff, 0x3b, 0x30}; /* red: the cursor's row/column */
static const rgb MOUSE_MARK = {0x00, 0xe5, 0xff};  /* cyan: the last mouse event */

struct fb {
    int w, h;
    uint8_t *px; /* PNG scanlines: per row, one filter byte then w RGB pixels */
};

/* Row y's first pixel; the byte before it is the row's PNG filter type. */
static inline uint8_t *fb_row(const struct fb *fb, int y) {
    return fb->px + (size_t)y * ((size_t)fb->w * 3 + 1) + 1;
}

static void put(struct fb *fb, int x, int y, rgb c) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) return;
    uint8_t *p = fb_row(fb, y) + (size_t)x * 3;
    p[0] = c.r;
    p[1] = c.g;
    p[2] = c.b;
}

static void blend(struct fb *fb, int x, int y, rgb c, double a) {
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h || a <= 0) return;
    if (a > 1) a = 1;
    uint8_t *p = fb_row(fb, y) + (size_t)x * 3;
    p[0] = (uint8_t)(p[0] + (c.r - p[0]) * a + 0.5);
    p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a + 0.5);
    p[2] = (uint8_t)(p[2] + (c.b - p[2]) * a + 0.5);
}

static void fill(struct fb *fb, int x, int y, int w, int h, rgb c) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) put(fb, i, j, c);
}

static void fill_alpha(struct fb *fb, int x, int y, int w, int h, rgb c, double a) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) blend(fb, i, j, c, a);
}

/* ---- glyphs ---------------------------------------------------------------- */

static int font_find(uint32_t cp) {
    int lo = 0, hi = HOLD_FONT_GLYPHS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_cp[mid] == cp) return mid;
        if (font_cp[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

/* First and last rows with ink in glyph g (16 rows); false if blank. */
static bool glyph_ink_rows(int g, int *top, int *bottom) {
    uint32_t off = font_off[g] & 0x7fffffffu;
    int bpr = (font_off[g] & 0x80000000u) ? 2 : 1;
    *top = -1;
    for (int row = 0; row < 16; row++) {
        bool ink = false;
        for (int k = 0; k < bpr; k++) ink |= font_bits[off + (size_t)row * bpr + k] != 0;
        if (!ink) continue;
        if (*top < 0) *top = row;
        *bottom = row;
    }
    return *top >= 0;
}

/* Unifont draws marks above at the top of the cell (placed for capitals);
 * lower one to sit two rows above its base letter's ink, as precomposed
 * letters do. Marks below, and marks over tall letters, stay put. */
static int combining_shift(uint32_t base, uint32_t mark) {
    int gb = font_find(base), gm = font_find(mark);
    int btop, bbot, mtop, mbot;
    if (gb < 0 || gm < 0 || !glyph_ink_rows(gb, &btop, &bbot) || !glyph_ink_rows(gm, &mtop, &mbot)) return 0;
    if (mbot >= 8) return 0; /* a mark below or through the letter */
    int shift = (btop - 2) - mbot;
    return shift > 0 ? shift : 0;
}

/* Draw a font glyph into `cells` terminal cells (1 or 2) with its top-left
 * at (x, y); returns false if absent. The terminal decides a character's
 * width, not the font: a 16-pixel glyph in a single cell (Unifont draws
 * Ethiopic, Indic, and some math letters wide) is squeezed into it by
 * merging pixel pairs, and an 8-pixel glyph in a double cell is centered,
 * so nothing spills into (and gets painted over by) the next cell. */
static bool draw_font_glyph(struct fb *fb, int x, int y, int s, int cells, uint32_t cp, rgb fg, bool bold) {
    int g = font_find(cp);
    if (g < 0) return false;
    uint32_t off = font_off[g] & 0x7fffffffu;
    bool wide = font_off[g] & 0x80000000u;
    int bpr = wide ? 2 : 1;
    bool squeeze = wide && cells == 1;
    int ox = !wide && cells == 2 ? x + 4 * s : x;
    int out_w = wide && !squeeze ? 16 : 8;
    for (int row = 0; row < 16; row++) {
        const unsigned char *bits = &font_bits[off + (size_t)row * bpr];
        for (int col = 0; col < out_w; col++) {
            int sc = squeeze ? col * 2 : col;
            bool ink = bits[sc / 8] & (0x80 >> (sc % 8));
            if (squeeze) ink |= (bits[(sc + 1) / 8] & (0x80 >> ((sc + 1) % 8))) != 0;
            if (!ink) continue;
            fill(fb, ox + col * s, y + row * s, s, s, fg);
            if (bold) fill(fb, ox + col * s + s, y + row * s, s, s, fg);
        }
    }
    return true;
}

/* Noto Emoji mask index for cp, or -1. */
static int emoji_find(uint32_t cp) {
    int lo = 0, hi = HOLD_EMOJI_GLYPHS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (emoji_cp[mid] == cp) return mid;
        if (emoji_cp[mid] < cp) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static double emoji_alpha(int e, int x, int y) {
    if (x < 0 || y < 0 || x >= HOLD_EMOJI_PX || y >= HOLD_EMOJI_PX) return 0;
    size_t i = (size_t)e * HOLD_EMOJI_PX * HOLD_EMOJI_PX + (size_t)y * HOLD_EMOJI_PX + (size_t)x;
    unsigned char b = emoji_mask[i / 2];
    return ((i & 1) ? (b & 15) : (b >> 4)) / 15.0;
}

/* Draw emoji e anti-aliased into the w x h box at (x, y): the 32x32 mask
 * resampled bilinearly (at scale 1 that is the average of each 2x2 block). */
static void draw_emoji(struct fb *fb, int x, int y, int w, int h, int e, rgb fg) {
    for (int ty = 0; ty < h; ty++) {
        double fy = (ty + 0.5) * HOLD_EMOJI_PX / h - 0.5;
        int y0 = (int)(fy < 0 ? fy - 1 : fy);
        double wy = fy - y0;
        for (int tx = 0; tx < w; tx++) {
            double fx = (tx + 0.5) * HOLD_EMOJI_PX / w - 0.5;
            int x0 = (int)(fx < 0 ? fx - 1 : fx);
            double wx = fx - x0;
            double a = (1 - wy) * ((1 - wx) * emoji_alpha(e, x0, y0) + wx * emoji_alpha(e, x0 + 1, y0)) +
                       wy * ((1 - wx) * emoji_alpha(e, x0, y0 + 1) + wx * emoji_alpha(e, x0 + 1, y0 + 1));
            blend(fb, x + tx, y + ty, fg, a);
        }
    }
}

/* Unicode default-ignorable code points (variation selectors, zero-width
 * joiner/space, bidi controls, tags, ...): terminals draw nothing for them,
 * but Unifont gives them visible labelled boxes. */
static bool default_ignorable(uint32_t cp) {
    return cp == 0x034F || cp == 0x061C || (cp >= 0x115F && cp <= 0x1160) || (cp >= 0x17B4 && cp <= 0x17B5) ||
           (cp >= 0x180B && cp <= 0x180F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x206F) || cp == 0x3164 || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF ||
           cp == 0xFFA0 || (cp >= 0xFFF0 && cp <= 0xFFF8) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
           (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0000 && cp <= 0xE0FFF);
}

/* U+2500..U+257F as up/down/left/right weights: 0 none, 1 light, 2 heavy,
 * 3 double. NULL = not a straight-line glyph (diagonals): use the font. */
static const char *const box_weights[0x80] = {
    "0011", "0022", "1100", "2200", "0011", "0022", "1100", "2200", "0011", "0022", "1100", "2200",
    "0101", "0102", "0201", "0202", "0110", "0120", "0210", "0220", "1001", "1002", "2001", "2002",
    "1010", "1020", "2010", "2020", "1101", "1102", "2101", "1201", "2201", "2102", "1202", "2202",
    "1110", "1120", "2110", "1210", "2210", "2120", "1220", "2220", "0111", "0121", "0112", "0122",
    "0211", "0221", "0212", "0222", "1011", "1021", "1012", "1022", "2011", "2021", "2012", "2022",
    "1111", "1121", "1112", "1122", "2111", "1211", "2211", "2121", "2112", "1221", "1212", "2122",
    "1222", "2221", "2212", "2222", "0011", "0022", "1100", "2200", "0033", "3300", "0103", "0301",
    "0303", "0130", "0310", "0330", "1003", "3001", "3003", "1030", "3010", "3030", "1103", "3301",
    "3303", "1130", "3310", "3330", "0133", "0311", "0333", "1033", "3011", "3033", "1133", "3311",
    "3333", "0101", "0110", "1010", "1001", NULL,   NULL,   NULL,   "0010", "1000", "0001", "0100",
    "0020", "2000", "0002", "0200", "0012", "1200", "0021", "2100",
};

static void box_segment(struct fb *fb, int x, int y, int cw, int ch, int s, int dir, int weight, rgb fg) {
    if (!weight) return;
    int t = weight == 2 ? 2 * s : s;
    int cx = x + (cw - t) / 2, cy = y + (ch - t) / 2;
    int offs[2] = {0, 0};
    int nlines = 1;
    if (weight == 3) {
        nlines = 2;
        offs[0] = -s;
        offs[1] = s;
    }
    for (int k = 0; k < nlines; k++) {
        int o = offs[k] * (weight == 3 ? 1 : 0);
        switch (dir) {
        case 0: fill(fb, cx + o, y, t, cy - y + t, fg); break;                 /* up */
        case 1: fill(fb, cx + o, cy, t, y + ch - cy, fg); break;               /* down */
        case 2: fill(fb, x, cy + o, cx - x + t, t, fg); break;                 /* left */
        case 3: fill(fb, cx, cy + o, x + cw - cx, t, fg); break;               /* right */
        }
    }
}

/* No libm: the static and cross-compiled release builds link without -lm. */
static double sqrt_newton(double v) {
    if (v <= 0) return 0;
    double x = v > 1 ? v : 1;
    for (int i = 0; i < 30; i++) x = 0.5 * (x + v / x);
    return x;
}

/* Rounded corners U+256D..U+2570 (╭ ╮ ╯ ╰): a quarter circle, anti-aliased,
 * the thickness of a light line, joining the cell's two edge midpoints the
 * straight box lines meet, then straight down/up to the far edge. */
static void draw_arc(struct fb *fb, int x, int y, int cw, int ch, int s, uint32_t cp, rgb fg) {
    bool right = cp == 0x256D || cp == 0x2570, down = cp == 0x256D || cp == 0x256E;
    int t = s;
    int lx = x + (cw - t) / 2, ly = y + (ch - t) / 2; /* the straight lines' top-left */
    double xc = lx + t / 2.0, yc = ly + t / 2.0;       /* their centre lines */
    double r = right ? x + cw - xc : xc - x;          /* reach the side edge exactly */
    double ax = right ? xc + r : xc - r, ay = down ? yc + r : yc - r;
    if (down) fill(fb, lx, (int)(ay + 0.5), t, y + ch - (int)(ay + 0.5), fg);
    else fill(fb, lx, y, t, (int)(ay + 0.5) - y, fg);
    int x0 = (int)(right ? xc - t : ax - 1), x1 = (int)(right ? ax + 1 : xc + t);
    int y0 = (int)(down ? yc - t : ay - 1), y1 = (int)(down ? ay + 1 : yc + t);
    for (int py = y0; py <= y1; py++) {
        for (int px = x0; px <= x1; px++) {
            double dx = px + 0.5 - ax, dy = py + 0.5 - ay;
            if ((right ? dx > 0.5 : dx < -0.5) || (down ? dy > 0.5 : dy < -0.5)) continue; /* other quadrants */
            double off = sqrt_newton(dx * dx + dy * dy) - r;
            double a = t / 2.0 + 0.5 - (off < 0 ? -off : off);
            if (a > 0) blend(fb, px, py, fg, a > 1 ? 1 : a);
        }
    }
}

/* Procedural glyphs: box drawing, blocks, shades, Braille. */
static bool draw_procedural(struct fb *fb, int x, int y, int cw, int ch, int s, uint32_t cp, rgb fg) {
    if (cp >= 0x256D && cp <= 0x2570) {
        draw_arc(fb, x, y, cw, ch, s, cp, fg);
        return true;
    }
    if (cp >= 0x2500 && cp <= 0x257f && box_weights[cp - 0x2500]) {
        const char *w = box_weights[cp - 0x2500];
        for (int d = 0; d < 4; d++) box_segment(fb, x, y, cw, ch, s, d, w[d] - '0', fg);
        return true;
    }
    if (cp >= 0x2580 && cp <= 0x259f) {
        int hw = cw / 2, hh = ch / 2;
        switch (cp) {
        case 0x2580: fill(fb, x, y, cw, hh, fg); return true;
        case 0x2588: fill(fb, x, y, cw, ch, fg); return true;
        case 0x2590: fill(fb, x + hw, y, cw - hw, ch, fg); return true;
        case 0x2591: fill_alpha(fb, x, y, cw, ch, fg, 0.25); return true;
        case 0x2592: fill_alpha(fb, x, y, cw, ch, fg, 0.5); return true;
        case 0x2593: fill_alpha(fb, x, y, cw, ch, fg, 0.75); return true;
        case 0x2594: fill(fb, x, y, cw, ch / 8, fg); return true;
        case 0x2595: fill(fb, x + cw - cw / 8, y, cw / 8, ch, fg); return true;
        default: break;
        }
        if (cp >= 0x2581 && cp <= 0x2587) {
            int h = ch * (int)(cp - 0x2580) / 8;
            fill(fb, x, y + ch - h, cw, h, fg);
            return true;
        }
        if (cp >= 0x2589 && cp <= 0x258f) {
            int w = cw * (int)(0x2590 - cp) / 8;
            fill(fb, x, y, w, ch, fg);
            return true;
        }
        /* quadrants: bits UL UR LL LR */
        static const uint8_t quad[10] = {0x2, 0x1, 0x8, 0xb, 0x9, 0xe, 0xd, 0x4, 0x6, 0x7};
        uint8_t q = quad[cp - 0x2596];
        if (q & 0x8) fill(fb, x, y, hw, hh, fg);
        if (q & 0x4) fill(fb, x + hw, y, cw - hw, hh, fg);
        if (q & 0x2) fill(fb, x, y + hh, hw, ch - hh, fg);
        if (q & 0x1) fill(fb, x + hw, y + hh, cw - hw, ch - hh, fg);
        return true;
    }
    if (cp >= 0x2800 && cp <= 0x28ff) {
        /* A 2x4 grid: each dot centred in its half-width column and
         * quarter-height row, so neighbouring dots never touch. */
        static const int dot_col[8] = {0, 0, 0, 1, 1, 1, 0, 1};
        static const int dot_row[8] = {0, 1, 2, 0, 1, 2, 3, 3};
        int d = cw / 4 > 0 ? cw / 4 : 1;
        int colw = cw / 2, rowh = ch / 4;
        for (int b = 0; b < 8; b++) {
            if (!((cp - 0x2800) & (1u << b))) continue;
            int dx = x + dot_col[b] * colw + (colw - d) / 2;
            int dy = y + dot_row[b] * rowh + (rowh - d) / 2;
            fill(fb, dx, dy, d, d, fg);
        }
        return true;
    }
    return false;
}

static void draw_missing(struct fb *fb, int x, int y, int cw, int ch, int s, rgb fg) {
    int m = s;
    fill(fb, x + m, y + 2 * s, cw - 2 * m, s, fg);
    fill(fb, x + m, y + ch - 3 * s, cw - 2 * m, s, fg);
    fill(fb, x + m, y + 2 * s, s, ch - 4 * s, fg);
    fill(fb, x + cw - m - s, y + 2 * s, s, ch - 4 * s, fg);
}

static void draw_text(struct fb *fb, int x, int y, int s, const char *txt, rgb fg) {
    for (; *txt; txt++, x += 8 * s) (void)draw_font_glyph(fb, x, y, s, 1, (unsigned char)*txt, fg, false);
}

/* ---- cells ------------------------------------------------------------------ */

static rgb cell_fg(const struct hold_cell *c) {
    return (c->flags & HOLD_CELL_FG_DEFAULT) ? THEME_FG : (rgb){c->fg[0], c->fg[1], c->fg[2]};
}

static rgb cell_bg(const struct hold_cell *c) {
    return (c->flags & HOLD_CELL_BG_DEFAULT) ? THEME_BG : (rgb){c->bg[0], c->bg[1], c->bg[2]};
}

static void draw_cell(struct fb *fb, const struct hold_snapshot *snap, int r, int c, int ox, int oy, int s, bool cursor) {
    const struct hold_cell *cell = &snap->cells[(size_t)r * snap->cols + c];
    if (cell->width == 0) return; /* painted by the wide cell to its left */
    int cw = 8 * s, ch = 16 * s;
    int span = cell->width == 2 && c + 1 < snap->cols ? 2 : 1;
    rgb fg = cell_fg(cell), bg = cell_bg(cell);
    if (cell->attrs & HOLD_ATTR_DIM) /* faint: the foreground halfway to the background, as xterm draws it */
        fg = (rgb){(uint8_t)((fg.r + bg.r + 1) / 2), (uint8_t)((fg.g + bg.g + 1) / 2), (uint8_t)((fg.b + bg.b + 1) / 2)};
    if (cell->attrs & HOLD_ATTR_REVERSE) {
        rgb t = fg;
        fg = bg;
        bg = t;
    }
    if (cursor) {
        rgb t = fg;
        fg = bg;
        bg = t;
    }
    if (cell->attrs & HOLD_ATTR_CONCEAL) fg = bg;
    int x = ox + c * cw, y = oy + r * ch;
    fill(fb, x, y, cw * span, ch, bg);
    uint32_t cp = cell->ch[0];
    int emoji = span == 2 ? emoji_find(cp) : -1;
    if (emoji >= 0) {
        draw_emoji(fb, x, y, cw * 2, ch, emoji, fg); /* its variation selector / modifiers add nothing drawable */
    } else if (cp && cp != ' ' && !default_ignorable(cp)) {
        bool bold = cell->attrs & HOLD_ATTR_BOLD;
        if (!draw_procedural(fb, x, y, cw * span, ch, s, cp, fg)) {
            if (draw_font_glyph(fb, x, y, s, span, cp, fg, bold)) {
                for (int k = 1; k < HOLD_CELL_CHARS && cell->ch[k]; k++)
                    if (!default_ignorable(cell->ch[k]))
                        (void)draw_font_glyph(fb, x, y + combining_shift(cp, cell->ch[k]) * s, s, span, cell->ch[k], fg,
                                              bold);
            } else {
                draw_missing(fb, x, y, cw * span, ch, s, fg);
            }
        }
    }
    if (cell->attrs & HOLD_ATTR_UNDERLINE) fill(fb, x, y + ch - 2 * s, cw * span, s, fg);
    if (cell->attrs & HOLD_ATTR_STRIKE) fill(fb, x, y + ch / 2, cw * span, s, fg);
}

/* ---- overlays --------------------------------------------------------------- */

/* Newton square root: keeps the static binary free of libm. */
/* Anti-aliased ring: radius 0.75 x cell height, stroke ~2.5 px x scale. */
static void draw_ring(struct fb *fb, double cx, double cy, double radius, double stroke, rgb c) {
    int x0 = (int)(cx - radius - stroke - 1), x1 = (int)(cx + radius + stroke + 1);
    int y0 = (int)(cy - radius - stroke - 1), y1 = (int)(cy + radius + stroke + 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            double d = sqrt_newton(dx * dx + dy * dy);
            double e = d - radius;
            double a = stroke / 2 + 0.5 - (e < 0 ? -e : e);
            blend(fb, x, y, c, a);
        }
    }
}

static void draw_reticle(struct fb *fb, int x, int y, int cw, int ch, int s, rgb c) {
    /* outline the cell, plus short crosshair arms outside it */
    fill(fb, x - s, y - s, cw + 2 * s, s, c);
    fill(fb, x - s, y + ch, cw + 2 * s, s, c);
    fill(fb, x - s, y - s, s, ch + 2 * s, c);
    fill(fb, x + cw, y - s, s, ch + 2 * s, c);
    int arm = 4 * s;
    fill(fb, x + cw / 2 - s / 2, y - s - arm, s, arm, c);
    fill(fb, x + cw / 2 - s / 2, y + ch + s, s, arm, c);
    fill(fb, x - s - arm, y + ch / 2 - s / 2, arm, s, c);
    fill(fb, x + cw + s, y + ch / 2 - s / 2, arm, s, c);
}

static int digits_of(int n) {
    int d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

/* Top: labels every 10 columns above a white rule with ┬ ticks (long at 10s,
 * medium at 5s). Left: every row number beside a rule with ├ ticks. The
 * cursor's column and row are marked in red, the last mouse event in cyan.
 * The bands end at bl/bt; a one-cell blank gutter separates them from the
 * viewport at lm/tm, and every tick stays aligned with its column or row. */
static void draw_ruler(struct fb *fb, const struct hold_snapshot *snap, int lm, int tm, int bl, int bt, int s,
                       bool mouse) {
    int cw = 8 * s, ch = 16 * s, tick = 8 * s;
    fill(fb, 0, 0, lm + snap->cols * cw, bt, RULER_BG);
    fill(fb, 0, 0, bl, tm + snap->rows * ch, RULER_BG);
    int axis_y = bt - tick - s, axis_x = bl - tick - s;
    fill(fb, lm, axis_y, snap->cols * cw, s, RULER_FG);
    fill(fb, axis_x, tm, s, snap->rows * ch, RULER_FG);
    char num[16];
    for (int c = 0; c < snap->cols; c++) {
        int len = c % 10 == 0 ? tick : c % 5 == 0 ? tick * 3 / 4 : tick / 2;
        int tx = lm + c * cw + cw / 2 - s / 2;
        fill(fb, tx, axis_y, s, len + s, RULER_FG);
        if (c % 10 == 0) {
            snprintf(num, sizeof(num), "%d", c);
            draw_text(fb, lm + c * cw, axis_y - ch, s, num, RULER_FG);
        }
    }
    for (int r = 0; r < snap->rows; r++) {
        int len = r % 10 == 0 ? tick : r % 5 == 0 ? tick * 3 / 4 : tick / 2;
        int ty = tm + r * ch + ch / 2 - s / 2;
        fill(fb, axis_x, ty, len + s, s, RULER_FG);
        snprintf(num, sizeof(num), "%d", r);
        draw_text(fb, axis_x - s - (int)strlen(num) * cw, tm + r * ch, s, num, RULER_FG);
    }
    struct {
        bool on;
        int row, col;
        rgb c;
    } marks[2] = {
        {mouse, snap->meta.mouse_row, snap->meta.mouse_col, MOUSE_MARK},
        {true, snap->cursor_row, snap->cursor_col, CURSOR_MARK},
    };
    for (int m = 0; m < 2; m++) {
        if (!marks[m].on) continue;
        rgb c = marks[m].c;
        int col = marks[m].col < snap->cols ? marks[m].col : snap->cols - 1;
        int row = marks[m].row < snap->rows ? marks[m].row : snap->rows - 1;
        int tx = lm + col * cw + cw / 2 - s;
        fill(fb, tx, axis_y - s, 2 * s, tick + 2 * s, c);
        snprintf(num, sizeof(num), "%d", col);
        int lw = (int)strlen(num) * cw;
        int lx = lm + col * cw + cw / 2 - lw / 2;
        if (lx < lm) lx = lm;
        if (lx + lw > fb->w) lx = fb->w - lw;
        /* A marker label wins: erase any regular label it would collide with. */
        for (int k = 0; k < snap->cols; k += 10) {
            int kx = lm + k * cw, kw = digits_of(k) * cw;
            if (kx < lx + lw + s && kx + kw > lx - s) fill(fb, kx, axis_y - ch, kw, ch - s, RULER_BG);
        }
        fill(fb, lx - s, axis_y - ch, lw + 2 * s, ch - s, RULER_BG);
        draw_text(fb, lx, axis_y - ch, s, num, c);
        int ty = tm + row * ch + ch / 2 - s;
        fill(fb, axis_x - s, ty, tick + 2 * s, 2 * s, c);
        snprintf(num, sizeof(num), "%d", row);
        lw = (int)strlen(num) * cw;
        fill(fb, 0, tm + row * ch, axis_x - s, ch, RULER_BG);
        draw_text(fb, axis_x - s - lw, tm + row * ch, s, num, c);
    }
}

/* ---- PNG encoding ------------------------------------------------------------ */

static uint32_t crc_table[256];

static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

struct bitw {
    uint8_t *out;
    size_t len, cap;
    uint32_t acc;
    int nbits;
    bool oom;
};

static void bw_byte(struct bitw *w, uint8_t b) {
    if (w->len == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 65536;
        uint8_t *p = realloc(w->out, cap);
        if (!p) {
            w->oom = true;
            return;
        }
        w->out = p;
        w->cap = cap;
    }
    w->out[w->len++] = b;
}

static void bw_bits(struct bitw *w, uint32_t v, int n) { /* LSB-first */
    w->acc |= v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        bw_byte(w, (uint8_t)w->acc);
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

static void bw_huff(struct bitw *w, uint32_t code, int n) { /* Huffman codes go MSB-first */
    uint32_t rev = 0;
    for (int i = 0; i < n; i++) rev |= ((code >> i) & 1) << (n - 1 - i);
    bw_bits(w, rev, n);
}

static void emit_literal(struct bitw *w, int sym) {
    if (sym < 144) bw_huff(w, 0x30 + sym, 8);
    else if (sym < 256) bw_huff(w, 0x190 + sym - 144, 9);
    else if (sym < 280) bw_huff(w, sym - 256, 7);
    else bw_huff(w, 0xc0 + sym - 280, 8);
}

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void emit_match(struct bitw *w, int len, int dist) {
    int li = 28;
    while (len_base[li] > len) li--;
    emit_literal(w, 257 + li);
    bw_bits(w, (uint32_t)(len - len_base[li]), len_extra[li]);
    int di = 29;
    while (dist_base[di] > dist) di--;
    bw_huff(w, (uint32_t)di, 5);
    bw_bits(w, (uint32_t)(dist - dist_base[di]), dist_extra[di]);
}

#define HASH_BITS 15
#define WINDOW 32768
#define MAX_CHAIN 48

/* zlib stream: one final fixed-Huffman block with greedy LZ77 matching. */
static int zlib_compress(const uint8_t *in, size_t n, uint8_t **out, size_t *out_len) {
    struct bitw w = {0};
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS);
    int32_t *prev = malloc(sizeof(int32_t) * WINDOW);
    if (!head || !prev) {
        free(head);
        free(prev);
        return -1;
    }
    for (size_t i = 0; i < (1u << HASH_BITS); i++) head[i] = -1;
    bw_byte(&w, 0x78);
    bw_byte(&w, 0x01);
    bw_bits(&w, 1, 1); /* BFINAL */
    bw_bits(&w, 1, 2); /* fixed Huffman */
    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 3 <= n) {
            uint32_t h = ((uint32_t)in[i] << 10 ^ (uint32_t)in[i + 1] << 5 ^ in[i + 2]) & ((1u << HASH_BITS) - 1);
            int32_t cand = head[h];
            int chain = MAX_CHAIN;
            while (cand >= 0 && chain-- > 0 && i - (size_t)cand <= WINDOW - 1) {
                size_t max = n - i < 258 ? n - i : 258;
                size_t l = 0;
                while (l < max && in[cand + l] == in[i + l]) l++;
                if ((int)l > best_len) {
                    best_len = (int)l;
                    best_dist = (int)(i - (size_t)cand);
                    if (l == max) break;
                }
                cand = prev[cand % WINDOW];
            }
            prev[i % WINDOW] = head[h];
            head[h] = (int32_t)i;
        }
        if (best_len >= 3) {
            emit_match(&w, best_len, best_dist);
            for (size_t k = 1; k < (size_t)best_len && i + k + 3 <= n; k++) {
                size_t p = i + k;
                uint32_t h = ((uint32_t)in[p] << 10 ^ (uint32_t)in[p + 1] << 5 ^ in[p + 2]) & ((1u << HASH_BITS) - 1);
                prev[p % WINDOW] = head[h];
                head[h] = (int32_t)p;
            }
            i += (size_t)best_len;
        } else {
            emit_literal(&w, in[i]);
            i++;
        }
    }
    emit_literal(&w, 256);
    if (w.nbits) bw_bits(&w, 0, 8 - w.nbits);
    uint32_t a = 1, b = 0;
    for (size_t k = 0; k < n; k++) {
        a = (a + in[k]) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = b << 16 | a;
    for (int k = 3; k >= 0; k--) bw_byte(&w, (uint8_t)(adler >> (8 * k)));
    free(head);
    free(prev);
    if (w.oom) {
        free(w.out);
        return -1;
    }
    *out = w.out;
    *out_len = w.len;
    return 0;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t hdr[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n,
                      (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3]};
    uint32_t crc = crc_update(0xffffffffu, hdr + 4, 4);
    crc = crc_update(crc, data, n) ^ 0xffffffffu;
    uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    if (fwrite(hdr, 1, 8, f) != 8 || (n && fwrite(data, 1, n, f) != n) || fwrite(tail, 1, 4, f) != 4) return -1;
    return 0;
}

/* Filters the framebuffer in place (it is laid out as PNG scanlines), so the
 * image is held once, not twice: a 1000x1000 screen is 384 MB of pixels. */
static int write_png(struct fb *fb, FILE *f) {
    crc_init();
    size_t stride = (size_t)fb->w * 3 + 1;
    for (int y = 0; y < fb->h; y++) {
        uint8_t *row = fb_row(fb, y);
        row[-1] = 1; /* Sub filter: flat runs become zeros; right to left keeps the left neighbor intact */
        for (size_t i = (size_t)fb->w * 3; i-- > 3;) row[i] = (uint8_t)(row[i] - row[i - 3]);
    }
    uint8_t *z = NULL;
    size_t zn = 0;
    int rc = zlib_compress(fb->px, stride * fb->h, &z, &zn);
    if (rc != 0) return -1;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    uint8_t ihdr[13] = {(uint8_t)(fb->w >> 24), (uint8_t)(fb->w >> 16), (uint8_t)(fb->w >> 8), (uint8_t)fb->w,
                        (uint8_t)(fb->h >> 24), (uint8_t)(fb->h >> 16), (uint8_t)(fb->h >> 8), (uint8_t)fb->h,
                        8, 2, 0, 0, 0};
    rc = (fwrite(sig, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) == 0 && write_chunk(f, "IDAT", z, zn) == 0 &&
          write_chunk(f, "IEND", NULL, 0) == 0)
             ? 0
             : -1;
    free(z);
    return rc;
}

/* ---- entry point --------------------------------------------------------------- */

void hold_render_options_init(struct hold_render_options *o) {
    memset(o, 0, sizeof(*o));
    o->ruler = true;
    o->mouse_marker = true;
    o->footer = true;
    o->scale = 0;
}

int hold_snapshot_write_png(const struct hold_snapshot *snap, const struct hold_render_options *o, FILE *f) {
    int s = o->scale >= 1 && o->scale <= 4 ? o->scale : (snap->cols <= 100 ? 2 : 1);
    int cw = 8 * s, ch = 16 * s;
    int lm = 0, tm = 0;
    int bl = 0, bt = 0; /* ruler band extents; the viewport starts one cell past them */
    if (o->ruler) {
        bl = digits_of(snap->rows > 0 ? snap->rows - 1 : 0) * cw + 10 * s;
        bt = ch + 10 * s;
        lm = bl + cw;
        tm = bt + ch;
    }
    /* The footer: CURSOR, SIZE, and (after a mouse event) MOUSE, three spaces
     * apart, filling lines no wider than the screen - a narrow screen wraps
     * it instead of stretching the image beside the terminal. */
    char seg[3][48] = {"", "", ""};
    int nseg = 0, seg_line[3] = {0, 0, 0}, seg_col[3] = {0, 0, 0}, lines = 0, foot_chars = 0;
    if (o->footer) {
        snprintf(seg[nseg++], sizeof(seg[0]), "CURSOR: col %u, row %u", snap->cursor_col, snap->cursor_row);
        snprintf(seg[nseg++], sizeof(seg[0]), "SIZE: %ux%u", snap->cols, snap->rows);
        if (o->mouse_marker && snap->meta.has_mouse)
            snprintf(seg[nseg++], sizeof(seg[0]), "MOUSE: col %u, row %u", snap->meta.mouse_col, snap->meta.mouse_row);
        int avail = snap->cols, col = 0;
        for (int i = 0; i < nseg; i++)
            if ((int)strlen(seg[i]) > avail) avail = (int)strlen(seg[i]);
        lines = 1;
        for (int i = 0; i < nseg; i++) {
            int len = (int)strlen(seg[i]);
            if (col > 0 && col + 3 + len > avail) {
                lines++;
                col = 0;
            }
            if (col > 0) col += 3;
            seg_line[i] = lines - 1;
            seg_col[i] = col;
            col += len;
            if (col > foot_chars) foot_chars = col;
        }
    }
    int foot_w = foot_chars * cw;
    int view_w = lm + snap->cols * cw;
    /* footer: a one-cell blank gutter, then its lines */
    struct fb fb = {view_w > lm + foot_w ? view_w : lm + foot_w, tm + snap->rows * ch + (o->footer ? (1 + lines) * ch : 0),
                    NULL};
    fb.px = malloc(((size_t)fb.w * 3 + 1) * fb.h);
    if (!fb.px) return -1;
    fill(&fb, 0, 0, fb.w, fb.h, THEME_BG);
    bool show_cursor = snap->modes.cursor_visible;
    for (int r = 0; r < snap->rows; r++)
        for (int c = 0; c < snap->cols; c++)
            draw_cell(&fb, snap, r, c, lm, tm, s, show_cursor && r == snap->cursor_row && c == snap->cursor_col);
    bool mouse = o->mouse_marker && snap->meta.has_mouse;
    if (mouse) {
        int mr = snap->meta.mouse_row < snap->rows ? snap->meta.mouse_row : snap->rows - 1;
        int mc = snap->meta.mouse_col < snap->cols ? snap->meta.mouse_col : snap->cols - 1;
        draw_reticle(&fb, lm + mc * cw, tm + mr * ch, cw, ch, s, MOUSE_MARK);
    }
    if (o->cursor_ring) {
        draw_ring(&fb, lm + (snap->cursor_col + 0.5) * cw, tm + (snap->cursor_row + 0.5) * ch, 0.75 * ch, 2.5 * s,
                  CURSOR_MARK);
    }
    if (o->ruler) draw_ruler(&fb, snap, lm, tm, bl, bt, s, mouse);
    for (int i = 0; i < nseg; i++)
        draw_text(&fb, lm + seg_col[i] * cw, tm + snap->rows * ch + (1 + seg_line[i]) * ch, s, seg[i],
                  i == 2 ? MOUSE_MARK : RULER_FG);
    int rc = write_png(&fb, f);
    free(fb.px);
    return rc;
}
