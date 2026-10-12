/* fb.h - drawing into the back framebuffer (320 x 288, 2 bpp, 4-colour palette; regs.h). */
#ifndef AP_FB_DRAW_H
#define AP_FB_DRAW_H

#include <stdint.h>

void fb_clear(unsigned c);
void fb_px(int x, int y, unsigned c);
void fb_fill(int x0, int y0, int x1, int y1, unsigned c);          /* inclusive corners */
void fb_hline(int x0, int x1, int y, unsigned c);
void fb_vline(int x, int y0, int y1, unsigned c);
void fb_line(int x0, int y0, int x1, int y1, unsigned c);
void fb_rect(int x0, int y0, int x1, int y1, unsigned c);          /* outline */
void fb_dotted_h(int x0, int x1, int y, unsigned c, int step, int size);
/* ellipse arc: centre, radii, angles in 1/256 turns [a0, a1), `seg` 1/256-turn steps */
void fb_arc(int cx, int cy, int rx, int ry, int a0, int a1, int seg, unsigned c);
/* text in the square 5x7 font (8x8 cells), `scale` 1..6, `spacing` extra cells of air.
 * Returns the x after the last character. */
int  fb_text(int x, int y, const char *s, unsigned c, int scale, int spacing);
int  fb_text_w(const char *s, int scale, int spacing);
/* sin / cos of an angle in 1/256 turns, Q14 */
int32_t fb_sin(int a);
int32_t fb_cos(int a);

/* provided by the firmware main loop: may suspend drawing until the next tick is done */
void fb_yield(void);

void fb_palette(const uint32_t pal[4]);
void fb_swap(void);                /* show the back buffer at the next vsync */
int  fb_swap_pending(void);

#endif
