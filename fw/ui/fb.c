/*
 * fb.c - drawing primitives for the back framebuffer (fb.h). Single pixels go through the
 * hardware's pixel window (it does the read-modify-write); spans of whole words go through
 * the word window. Every primitive calls fb_yield() first, so drawing can be suspended.
 */
#include "fb.h"
#include "../hal/regs.h"

#define FBW(i)      (*(volatile uint32_t *)(uintptr_t)(AP_FB_WORDS + (uint32_t)(i) * 4u))
#define FBPX(x, y)  (*(volatile uint32_t *)(uintptr_t)(AP_FB_PIXEL + ((((uint32_t)(y) << 9) | (uint32_t)(x)) << 2)))
#define VC(r)       (*(volatile uint32_t *)(uintptr_t)(AP_MMIO_VIDEO + (r) * 4u))

extern const uint8_t FONT5X7[64][7];

static uint32_t rep(unsigned c) { return (c & 3u) * 0x55555555u; }

void fb_clear(unsigned c)
{
    uint32_t w = rep(c);
    for (unsigned i = 0; i < AP_FB_WPL * AP_FB_H; i++) {
        if ((i & 255u) == 0) fb_yield();
        FBW(i) = w;
    }
}

void fb_px(int x, int y, unsigned c)
{
    if ((unsigned)x < AP_FB_W && (unsigned)y < AP_FB_H) FBPX(x, y) = c;
}

void fb_hline(int x0, int x1, int y, unsigned c)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    fb_yield();
    if ((unsigned)y >= AP_FB_H || x1 < 0 || x0 >= (int)AP_FB_W) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= (int)AP_FB_W) x1 = AP_FB_W - 1;
    while (x0 <= x1 && (x0 & 15)) FBPX(x0++, y) = c;                 /* to a word boundary */
    uint32_t w = rep(c), base = (uint32_t)y * AP_FB_WPL;
    while (x0 + 15 <= x1) { FBW(base + (uint32_t)x0 / 16u) = w; x0 += 16; }
    while (x0 <= x1) FBPX(x0++, y) = c;
}

void fb_vline(int x, int y0, int y1, unsigned c)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    fb_yield();
    for (int y = y0; y <= y1; y++) fb_px(x, y, c);
}

void fb_fill(int x0, int y0, int x1, int y1, unsigned c)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) fb_hline(x0, x1, y, c);
}

void fb_rect(int x0, int y0, int x1, int y1, unsigned c)
{
    fb_hline(x0, x1, y0, c);
    fb_hline(x0, x1, y1, c);
    fb_vline(x0, y0, y1, c);
    fb_vline(x1, y0, y1, c);
}

void fb_line(int x0, int y0, int x1, int y1, unsigned c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    fb_yield();
    for (;;) {
        fb_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fb_dotted_h(int x0, int x1, int y, unsigned c, int step, int size)
{
    for (int x = x0; x < x1; x += step) fb_fill(x, y, x + size - 1, y + size - 1, c);
}

/* quarter-wave sine, Q14, 65 entries (0..64 = 0..90 degrees) */
static const int16_t SIN_Q[65] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
    6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003,
    11297, 11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256,
    14449, 14635, 14811, 14978, 15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986, 16069,
    16143, 16207, 16261, 16305, 16340, 16364, 16379, 16384};

int32_t fb_sin(int a)
{
    a &= 255;
    if (a < 64) return SIN_Q[a];
    if (a < 128) return SIN_Q[128 - a];
    if (a < 192) return -SIN_Q[a - 128];
    return -SIN_Q[256 - a];
}

int32_t fb_cos(int a) { return fb_sin(a + 64); }

void fb_arc(int cx, int cy, int rx, int ry, int a0, int a1, int seg, unsigned c)
{
    int px = cx + (int)((rx * fb_cos(a0)) >> 14), py = cy - (int)((ry * fb_sin(a0)) >> 14);
    for (int a = a0 + seg; a <= a1; a += seg) {
        int x = cx + (int)((rx * fb_cos(a)) >> 14), y = cy - (int)((ry * fb_sin(a)) >> 14);
        fb_line(px, py, x, y, c);
        px = x;
        py = y;
    }
}

int fb_text_w(const char *s, int scale, int spacing)
{
    int n = 0;
    while (s[n]) n++;
    return n * (6 + spacing) * scale;
}

int fb_text(int x, int y, const char *s, unsigned c, int scale, int spacing)
{
    for (; *s; s++) {
        fb_yield();
        int ch = *s;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch == '|') ch = '!';                               /* the font keeps '|' in the '!' slot */
        if (ch < 32 || ch > 95) ch = 32;
        const uint8_t *g = FONT5X7[ch - 32];
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (g[r] & (0x10 >> k)) {
                    int px = x + (k + 1) * scale, py = y + (r + 1) * scale;
                    if (scale == 1) fb_px(px, py, c);
                    else fb_fill(px, py, px + scale - 1, py + scale - 1, c);
                }
        x += (6 + spacing) * scale;
    }
    return x;
}

void fb_palette(const uint32_t pal[4])
{
    for (unsigned i = 0; i < 4; i++) VC(VC_PAL0 + i) = pal[i];
}

void fb_swap(void) { VC(VC_SWAP) = 1u; }
int  fb_swap_pending(void) { return (int)(VC(VC_STATUS) & 1u); }
