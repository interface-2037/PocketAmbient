/*
 * hal_model.c - Host HAL: composer talks straight to the C synth model.
 * The renderer calls composer_tick() between 48-sample blocks, which gives the
 * same tick-boundary semantics as the hardware's shadow-register commit.
 */
#include "hal.h"
#include "hal_model.h"

static sm_t      *g_sm;
static uint64_t   g_tick;
static uint32_t   g_ctrl[CR_NUM_REGS];
static halm_trace_fn g_trace;
static void      *g_trace_ctx;
static uint32_t   g_visual_count;

void halm_bind(sm_t *sm) { g_sm = sm; }
void halm_set_tick(uint64_t t)
{
    g_tick = t;
    g_ctrl[CR_TICK_LO] = (uint32_t)t;
    g_ctrl[CR_TICK_HI] = (uint32_t)(t >> 32);
}
void halm_ctrl_set(unsigned reg, uint32_t val) { if (reg < CR_NUM_REGS) g_ctrl[reg] = val; }
uint32_t halm_ctrl_get(unsigned reg) { return reg < CR_NUM_REGS ? g_ctrl[reg] : 0; }
void halm_press(uint32_t mask) { g_ctrl[CR_BTN_EVENTS] |= mask; }
void halm_set_trace(halm_trace_fn fn, void *ctx) { g_trace = fn; g_trace_ctx = ctx; }
uint32_t halm_visual_count(void) { return g_visual_count; }

void hal_vwrite(unsigned voice, unsigned reg, uint32_t val) { sm_vwrite(g_sm, voice, reg, val); }
uint32_t hal_vstatus(unsigned voice) { return sm_vread(g_sm, voice, VR_STATUS); }
void hal_fxwrite(unsigned reg, uint32_t val) { sm_fxwrite(g_sm, reg, val); }
uint32_t hal_fxread(unsigned reg) { return sm_fxread(g_sm, reg); }

uint32_t hal_ctrl_read(unsigned reg)
{
    if (reg >= CR_NUM_REGS) return 0;
    uint32_t v = g_ctrl[reg];
    if (reg == CR_BTN_EVENTS || reg == CR_CMD) g_ctrl[reg] = 0;   /* clear-on-read */
    return v;
}
void hal_ctrl_write(unsigned reg, uint32_t val) { if (reg < CR_NUM_REGS) g_ctrl[reg] = val; }
uint64_t hal_ticks(void) { return g_tick; }
void hal_visual_event(uint32_t ev) { (void)ev; g_visual_count++; }

void hal_trace(uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    if (g_trace) g_trace(g_trace_ctx, g_tick, kind, a, b, c);
}
