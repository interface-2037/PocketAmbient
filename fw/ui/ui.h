/* ui.h - per-mode screens drawn by the composer CPU between ticks (ui.c). */
#ifndef AP_UI_H
#define AP_UI_H

#include <stdint.h>

#define UI_FRAME_MS 33u          /* ~30 frames per second */

void ui_event(uint32_t ev);      /* visual event from the composer (hal_visual_event) */
int  ui_due(void);               /* a new frame may be drawn now */
void ui_frame(void);             /* draw one frame into the back buffer and swap */

#endif
