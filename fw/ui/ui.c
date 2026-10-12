/*
 * ui.c - the per-mode screens (DECISIONS D42, mockups in docs/ui/, tools/ui/mockup.py).
 *
 * Runs on the composer CPU in the time it would otherwise spend waiting for the engine
 * (fw/rv/main.c), draws a whole frame into the back buffer and swaps at vsync. It only reads
 * the composer's state (composer_state()) and never touches engine or clear-on-read
 * registers, so the audio is identical with or without it.
 *
 * Visuals setting: Full = everything, Minimal = text only, Off = black. Y (overlay) adds an
 * info box: full seed, uptime and the engine diagnostics. Nothing flashes faster than 1 Hz.
 */
#include "ui.h"
#include "fb.h"
#include "../composer/cint.h"

#define CR(r) (*(volatile uint32_t *)(uintptr_t)(AP_MMIO_CTRL + (r) * 4u))

enum { BG, LINE, TXT, HI };                           /* palette roles, as in the mockups */
enum { S_BREAK = 2 };                                 /* mode_hush.c section */

static const uint32_t PAL_DRIFT[4]  = {0x02040A, 0x286EFF, 0x78E6C8, 0xE65032};
static const uint32_t PAL_TIDES[4]  = {0x02080E, 0x285AC8, 0xF0C85A, 0xE6F0FF};
static const uint32_t PAL_HUSH[4]   = {0x2848A0, 0x78C8D2, 0x96F0DC, 0xC8FFF0};
static const uint32_t PAL_BREAK[4]  = {0xB23418, 0xF0C8B4, 0xFAF0E6, 0xFFAA78};
static const uint32_t PAL_MOTHER[4] = {0x000602, 0x1E8C46, 0x46FF82, 0xC8FFD2};
static const uint32_t PAL_OFF[4]    = {0, 0, 0, 0};

/* ------------------------------------------------------------------ visual events */
#define EVQ 16
static uint32_t evq[EVQ];
static unsigned ev_w, ev_r;

void ui_event(uint32_t ev)
{
    if (ev_w - ev_r < EVQ) evq[ev_w++ % EVQ] = ev;
}

/* TIDES ring blooms: start tick and pan */
#define BLOOMS 6
static uint64_t bloom_t[BLOOMS];
static uint16_t bloom_pan[BLOOMS];
static unsigned bloom_i;

/* ------------------------------------------------------------------ small text helpers */
static char *put_u(char *p, uint32_t v, int digits)        /* zero-padded decimal */
{
    char t[10];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n < digits) t[n++] = '0';
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static char *put_hex(char *p, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) *p++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15u];
    *p = 0;
    return p;
}

static char *put_s(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_hms(char *p, uint32_t s)
{
    p = put_u(p, s / 3600u, 2);
    *p++ = ':';
    p = put_u(p, s / 60u % 60u, 2);
    *p++ = ':';
    return put_u(p, s % 60u, 2);
}

static char *put_frac2(char *p, int32_t q15)               /* 0.00 .. 1.00 */
{
    if (q15 < 0) q15 = 0;
    uint32_t h = ((uint32_t)q15 * 100u + 16384u) >> 15;
    p = put_u(p, h / 100u, 1);
    *p++ = '.';
    return put_u(p, h % 100u, 2);
}

static const char *NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *put_note(char *p, int32_t midi)
{
    if (midi <= 0) return put_s(p, "--");
    p = put_s(p, NOTE[midi % 12]);
    return put_u(p, (uint32_t)(midi / 12 - 1), 1);
}

/* chord name: root note + quality, from the chord's pitch classes */
static char *put_chord(char *p, hkey_t k, chord_t ch, int roman)
{
    static const char *RN[7] = {"I", "II", "III", "IV", "V", "VI", "VII"};
    int8_t pcs[4];
    int n = chord_pcs(k, ch, pcs);
    int third = n > 1 ? (pcs[1] - pcs[0] + 12) % 12 : 4;
    int sev = n > 3 ? (pcs[3] - pcs[0] + 12) % 12 : -1;
    if (roman) {
        int d = ch.deg % 7;
        if (d < 0) d += 7;
        p = put_s(p, RN[d]);
        *p++ = ' ';
    } else {
        p = put_s(p, NOTE[pcs[0] % 12]);
    }
    if (ch.type == CH_SUS2) return put_s(p, "SUS2");
    if (ch.type == CH_SUS4) return put_s(p, "SUS4");
    p = put_s(p, third == 3 ? "MIN" : (roman ? "MAJ" : ""));
    if (ch.type == CH_ADD9) return put_s(p, "ADD9");
    if (sev == 11) return put_s(p, third == 3 ? "MAJ7" : (roman ? "7" : "MAJ7"));
    if (sev == 10) return put_s(p, "7");
    return p;
}

/* ------------------------------------------------------------------ shared layout */
static void frame2(int x0, int y0, int x1, int y1, unsigned c, int dbl)
{
    fb_rect(x0, y0, x1, y1, c);
    if (dbl) fb_rect(x0 + 2, y0 + 2, x1 - 2, y1 - 2, c);
}

static void cross(int x, int y, unsigned c)
{
    fb_hline(x - 9, x + 9, y - 1, c);
    fb_hline(x - 9, x + 9, y + 1, c);
    fb_vline(x - 1, y - 9, y + 9, c);
    fb_vline(x + 1, y - 9, y + 9, c);
}

static void boxed(int x, int y, const char *label, const char *value)
{
    fb_text(x, y, label, TXT, 1, 1);
    fb_rect(x, y + 10, x + 80, y + 22, LINE);
    fb_text(x + 3, y + 12, value, TXT, 1, 1);
}

static uint32_t elapsed_s(const comp_t *c)
{
    return c->state == CS_TITLE ? 0u : (uint32_t)((c->now - c->play_start) / 1000u);
}

static void readouts(const comp_t *c, const char *xl, const char *xv)
{
    char b[24];
    int x = 232, y = 12;
    fb_rect(x - 4, 6, 315, 283, LINE);
    put_hms(b, elapsed_s(c));       boxed(x, y, "ELAPSED", b); y += 30;
    put_u(b, c->density, 1);        boxed(x, y, "DENSITY", b); y += 30;
    put_u(b, c->bright, 1);         boxed(x, y, "BRIGHT", b);  y += 30;
    put_hex(b, c->sess_seed & 0xFFFFu, 4); boxed(x, y, "SEED", b); y += 30;
    boxed(x, y, xl, xv);            y += 30;
    put_u(put_s(b, "# "), c->mode + 1u, 1); boxed(x, y, "SYSTEM", b);
    static const char *SL[4] = {"OFF", "30 MIN", "60 MIN", "90 MIN"};
    fb_text(x, 260, "SLEEP", TXT, 1, 1);
    fb_rect(x, 270, x + 80, 280, LINE);
    fb_text(x + 3, 271, SL[c->sleep_sel & 3u], TXT, 1, 1);
}

static void base(const comp_t *c, const char *title, const char *xl, const char *xv, int full, int crosses)
{
    frame2(2, 2, 317, 285, LINE, 1);
    fb_rect(6, 6, 224, 32, LINE);
    fb_text(12, 11, title, TXT, 2, 2);
    readouts(c, xl, xv);
    fb_rect(6, 36, 224, 283, LINE);
    if (full && crosses) {
        cross(30, 60, LINE);
        cross(200, 60, LINE);
        cross(30, crosses, LINE);
        cross(200, crosses, LINE);
    }
}

static void press_a(const comp_t *c, int y, unsigned col)
{
    if (c->state == CS_TITLE && (c->now / 1000u) % 2u == 0) {
        const char *s = "PRESS A";
        fb_text(115 - fb_text_w(s, 2, 1) / 2, y, s, col, 2, 1);
    }
}

/* ------------------------------------------------------------------ DRIFT */
static void drift(const comp_t *c, int full)
{
    const drift_t *d = &c->m.d;
    char x[16], b[48], *p;
    put_frac2(x, d->tension);
    base(c, "DRIFT", "TENSION", x, full, 244);
    if (full) {
        int cx = 115, cy = 158, r = 84;
        int rot = (int)(c->now / 400u) & 15;                      /* slow turn: 20/256 turn per 6.4 s */
        for (int lon = -48 + rot; lon <= 48; lon += 16) {         /* near-side meridians */
            int sx = (int)((r * fb_sin(lon + 16)) >> 14);
            int px = cx, py = cy - r;
            for (int lat = 60; lat >= -64; lat -= 4) {
                int xx = cx + (int)((sx * fb_cos(lat)) >> 14), yy = cy - (int)((r * fb_sin(lat)) >> 14);
                if (lat != 60) fb_line(px, py, xx, yy, TXT);
                px = xx;
                py = yy;
            }
        }
        for (int lat = -40; lat <= 40; lat += 14) {               /* parallels */
            int y = cy - (int)((r * fb_sin(lat)) >> 14), rr = (int)((r * fb_cos(lat)) >> 14) * 95 / 100;
            fb_hline(cx - rr, cx + rr, y, TXT);
        }
        uint32_t s = c->sess_seed;                                /* contour rings: shape from the seed */
        int ox = cx - 22 + (int)((8 * fb_sin((int)(c->now / 1500u))) >> 14);
        int oy = cy - 32 + (int)((5 * fb_cos((int)(c->now / 2100u))) >> 14);
        for (int k = 0; k < 5; k++) {
            int rad = 14 + k * 7, px = 0, py = 0;
            for (int t = 0; t <= 256; t += 8) {
                s = s * 1664525u + 1013904223u;
                int rr = rad + (int)(s >> 30) - 1 + (int)((d->tension >> 13) & 1);
                int xx = ox + (int)((rr * 4 * fb_cos(t)) >> 16) * 1, yy = oy + (int)((rr * 9 * fb_sin(t)) >> 18);
                if (t) fb_line(px, py, xx, yy, HI);
                px = xx;
                py = yy;
            }
        }
    }
    p = put_s(b, "STRINGS ");
    p = put_u(p, d->nstr, 1);
    p = put_s(p, "  HORN ");
    p = put_note(p, d->horn_left > 0 ? d->horn_p : 0);
    p = put_s(p, "  DRONE ");
    put_note(p, d->drone_off ? 0 : d->drone_p);
    fb_text(12, 262, b, TXT, 1, 0);
    press_a(c, 140, HI);
}

/* ------------------------------------------------------------------ TIDES */
static void tides(const comp_t *c, int full)
{
    const tides_t *t = &c->m.t;
    char x[16], b[24], *p;
    put_chord(x, t->key, CHORD_VOCAB[t->chord % CHORD_VOCAB_N], 1);
    base(c, "TIDES", "CHORD", c->state == CS_TITLE ? "--" : x, 0, 0);
    int cx = 110, cy = 160;
    if (full) {
        for (int gx = 14; gx < 222; gx += 12) fb_vline(gx, 40, 280, LINE);
        for (int gy = 40; gy < 281; gy += 12) fb_hline(10, 221, gy, LINE);
        static const int R[3] = {70, 56, 30};
        for (int i = 0; i < 3; i++) fb_arc(cx, cy, R[i], R[i], 0, 256, 8, TXT);
        for (int a = 0; a < 256; a += 32)
            fb_line(cx + (int)((30 * fb_cos(a)) >> 14), cy - (int)((30 * fb_sin(a)) >> 14),
                    cx + (int)((56 * fb_cos(a)) >> 14), cy - (int)((56 * fb_sin(a)) >> 14), TXT);
        fb_rect(cx - 8, cy - 40, cx + 8, cy + 40, TXT);
        fb_rect(cx - 40, cy - 8, cx + 40, cy + 8, TXT);
        fb_rect(172, 110, 200, 210, TXT);                          /* side gauge: lead pitch */
        for (int gy = 116; gy < 206; gy += 8) fb_hline(168, 172, gy, TXT);
        if (t->lead_p > 0) {
            int ly = 206 - (int)((t->lead_p - 60) * 3);
            if (ly < 114) ly = 114;
            if (ly > 206) ly = 206;
            fb_fill(175, ly - 1, 197, ly + 1, HI);
        }
        for (int i = 0; i < BLOOMS; i++) {                         /* rings bloom on each note */
            uint64_t age = c->now - bloom_t[i];
            if (!bloom_t[i] || age > 2400u) continue;
            int rr = 8 + (int)(age / 30u);
            int bx = cx + ((int)bloom_pan[i] - 16384) / 400;
            fb_arc(bx, cy, rr, rr, 0, 256, 16, age < 1200u ? HI : TXT);
        }
    }
    uint32_t el = elapsed_s(c);
    p = put_hms(b, el);
    *p++ = ':';
    put_u(p, (uint32_t)(c->now % 1000u) * 30u / 1000u, 2);         /* frames, 30 per second */
    fb_text(118, 44, b, HI, 1, 1);
    press_a(c, 250, HI);
}

/* ------------------------------------------------------------------ HUSH */
static void hush(const comp_t *c, int full, int brk)
{
    const hush_t *h = &c->m.h;
    char b[48], *p;
    fb_text(16, 14, "HUSH", TXT, 3, 1);
    fb_text(196, 22, "LOFI CTR", TXT, 1, 2);
    fb_dotted_h(14, 306, 46, TXT, 4, 2);
    if (brk) {
        fb_text(40, 92, "BREAK", TXT, 5, 1);
    } else {
        if (full) {
            int shift = (int)(h->bar & 3u);
            for (int i = 0, y = 60; y < 160; i++, y += 10) {
                if (y == 100 || y == 110) continue;
                int x0 = 40 + ((i + shift) % 3) * 8;
                fb_dotted_h(x0, 270 - ((i + shift) % 2) * 16, y, LINE, 5, 3);
                fb_hline(24, 27, y + 1, LINE);
                fb_hline(292, 295, y + 1, LINE);
            }
        }
        p = b;
        int n = h->prog_len ? h->prog_len : 0;
        if (n > 3) n = 3;
        for (int i = 0; i < n; i++) {
            chord_t ch = {(int8_t)h->prog[i], CH_SEV};
            if (i) p = put_s(p, " | ");
            p = put_chord(p, h->key, ch, 0);
        }
        if (c->state != CS_TITLE) fb_text(115 - fb_text_w(b, 1, 1) / 2 + 45, 101, b, HI, 1, 1);
    }
    fb_dotted_h(14, 306, 172, TXT, 4, 2);
    uint32_t beat = 0;
    if (c->state != CS_TITLE && h->bpm && c->now > h->t0)
        beat = (uint32_t)((c->now - h->t0) * h->bpm / 60000u) & 3u;
    for (int i = 0; i < 4; i++) {
        int x = 40 + i * 64;
        if (c->state != CS_TITLE && (uint32_t)i == beat) fb_fill(x, 186, x + 40, 198, TXT);
        else fb_rect(x, 186, x + 40, 198, TXT);
    }
    put_u(b, h->bpm, 3);
    fb_text(16, 222, b, TXT, 4, 1);
    fb_text(120, 236, "BPM", TXT, 2, 1);
    put_u(put_s(b, "DR "), c->density, 1);
    fb_text(200, 222, b, TXT, 3, 1);
    put_u(put_s(b, "BAR "), h->bar, 1);
    fb_text(16, 268, b, HI, 1, 1);
    press_a(c, 100, HI);
}

/* ------------------------------------------------------------------ MOTHER */
static void mother(const comp_t *c, int full)
{
    const mother_t *o = &c->m.o;
    char x[16], b[40], *p;
    put_frac2(x, o->activity);
    base(c, "MOTHER", "ACTIVITY", x, full, 0);
    if (full) { cross(30, 258, LINE); cross(200, 258, LINE); }
    int y = 46;
    fb_text(14, y, c->state == CS_TITLE ? "INTERFACE READY" : "INTERFACE ACTIVE", HI, 1, 1);
    y += 24;
    fb_text(14, y, "ENVIRON CTR", TXT, 1, 1); y += 12;
    fb_text(14, y, "  COOLANT FLOW", TXT, 1, 1);
    fb_text(14 + 17 * 7, y, o->flow_left[0] > 0 || o->flow_left[1] > 0 ? "SURGE" : "NOMINAL", TXT, 1, 1); y += 12;
    fb_text(14, y, "  RELAY BANK 4", TXT, 1, 1);
    fb_text(14 + 17 * 7, y, o->relay_left ? "CYCLING" : "IDLE", TXT, 1, 1); y += 12;
    fb_text(14, y, "  DRIVE 2", TXT, 1, 1);
    static const char *DP[4] = {"IDLE", "SPIN UP", "SEEK", "SPIN DN"};
    fb_text(14 + 17 * 7, y, DP[o->drive_phase & 3u], TXT, 1, 1); y += 24;
    p = put_s(b, "THERMAL  -");
    p = put_u(p, 12u + (uint32_t)(o->activity >> 13), 2);
    p = put_s(p, ".");
    put_u(p, (uint32_t)(o->activity >> 9) % 10u, 1);
    fb_text(14, y, b, TXT, 1, 1); y += 12;
    fb_text(14, y, o->alert ? "HULL     CONTACT" : "HULL     STABLE", o->alert ? HI : TXT, 1, 1); y += 24;
    const char *q = o->alert ? "TRACKING" : "AWAITING INQUIRY";
    int ex = fb_text(14, y, q, TXT, 1, 1);
    if ((c->now / 500u) % 2u == 0) fb_fill(ex + 2, y + 1, ex + 8, y + 9, HI);   /* 1 Hz cursor */
    if (full && o->alert) {                                        /* motion tracker, only in an alert */
        int cx = 116, cy = 236;
        static const int R[3] = {14, 28, 40};
        for (int i = 0; i < 3; i++) fb_arc(cx, cy, R[i], R[i], 36, 92, 4, LINE);
        int a = 40 + (int)(c->now / 40u) % 50;
        fb_line(cx, cy, cx + (int)((40 * fb_cos(a)) >> 14), cy - (int)((40 * fb_sin(a)) >> 14), TXT);
        int dr = 6 + (int)(((int64_t)o->distance * 34) >> 15);
        fb_fill(cx + (dr * 3) / 4 - 1, cy - dr * 2 / 3 - 1, cx + (dr * 3) / 4 + 2, cy - dr * 2 / 3 + 2, HI);
    }
    press_a(c, 200, HI);
}

/* ------------------------------------------------------------------ overlay (Y) */
static void overlay(const comp_t *c)
{
    char b[32], *p;
    int x = 20, y = 150;
    fb_fill(x - 6, y - 6, x + 196, y + 82, BG);
    fb_rect(x - 6, y - 6, x + 196, y + 82, HI);
    p = put_s(b, "SEED   ");
    put_hex(p, c->sess_seed, 8);
    fb_text(x, y, b, HI, 1, 1); y += 12;
    put_hms(put_s(b, "UPTIME "), (uint32_t)(c->now / 1000u));
    fb_text(x, y, b, HI, 1, 1); y += 12;
    put_u(put_s(b, "CPU MAX  "), CR(CR_DIAG_CPU_MAX), 6);
    fb_text(x, y, b, HI, 1, 1); y += 12;
    put_u(put_s(b, "FIFO MIN "), CR(CR_DIAG_FIFO) & 0xFFFFu, 4);
    fb_text(x, y, b, HI, 1, 1); y += 12;
    put_u(put_s(b, "UNDERRUN "), CR(CR_DIAG_UNDERRUN), 5);
    fb_text(x, y, b, HI, 1, 1); y += 12;
    uint32_t g = CR(CR_DIAG_FRAME);
    p = put_u(put_s(b, "FRAME    "), g >> 16, 4);
    p = put_s(p, "-");
    put_u(p, g & 0xFFFFu, 4);
    fb_text(x, y, b, HI, 1, 1);
}

/* ------------------------------------------------------------------ frame scheduling */
static uint64_t last_frame;
static int      started;

int ui_due(void)
{
    const comp_t *c = composer_state();
    return !fb_swap_pending() && (!started || c->now - last_frame >= UI_FRAME_MS);
}

void ui_frame(void)
{
    const comp_t *c = composer_state();
    while (ev_r != ev_w) {                                         /* collect visual events */
        uint32_t ev = evq[ev_r++ % EVQ];
        if ((ev >> 24) == 1u) {
            bloom_t[bloom_i % BLOOMS] = c->now ? c->now : 1;
            bloom_pan[bloom_i % BLOOMS] = (uint16_t)(ev & 0xFFFFu);
            bloom_i++;
        }
    }
    started = 1;
    last_frame = c->now;
    int brk = c->mode == MODE_HUSH && c->m.h.section == S_BREAK && c->state != CS_TITLE;
    if (c->visuals == 2) {
        fb_palette(PAL_OFF);
        fb_clear(BG);
        fb_swap();
        return;
    }
    static const uint32_t *const PAL[4] = {PAL_DRIFT, PAL_TIDES, PAL_HUSH, PAL_MOTHER};
    fb_palette(brk ? PAL_BREAK : PAL[c->mode & 3u]);
    fb_clear(BG);
    int full = c->visuals == 0;
    switch (c->mode) {
    case MODE_DRIFT: drift(c, full); break;
    case MODE_TIDES: tides(c, full); break;
    case MODE_HUSH:  hush(c, full, brk); break;
    default:         mother(c, full); break;
    }
    if (c->overlay) overlay(c);
    fb_swap();
}
