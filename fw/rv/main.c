/*
 * main.c - composer firmware main loop (PicoRV32). The engine sequencer (cpu_sys.sv) holds
 * audio at each tick boundary until the CPU writes CR_TICK_DONE; it then renders the
 * tick's 48 samples and advances CR_TICK_LO/HI. Same order as model/render.c:
 * composer_init() at tick 0, then composer_tick() once per tick, before its samples.
 *
 * While the engine renders, the CPU runs the screen code (fw/ui) as a coroutine on its own
 * stack. The UI hands control back between drawing primitives (fb_yield) as soon as the next
 * tick is ready, so composer_tick() is never held up by more than one primitive. The UI only
 * reads composer state, so the audio does not depend on it.
 */
#include "../hal/hal.h"
#include "../composer/composer.h"
#include "../ui/ui.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))
#define TICK_LO     REG32(AP_MMIO_CTRL + CR_TICK_LO * 4u)

typedef struct { uint32_t r[14]; } co_ctx_t;               /* ra, sp, s0-s11 (coro.S) */
void co_switch(co_ctx_t *save, const co_ctx_t *load);

static co_ctx_t main_ctx, ui_ctx;
static uint32_t ui_stack[384];                              /* 1.5 KB */
static uint32_t wait_tick;

/* called by the drawing code between primitives: give the CPU back once the tick is ready */
void fb_yield(void)
{
    if (TICK_LO != wait_tick) co_switch(&ui_ctx, &main_ctx);
}

static void ui_task(void)
{
    for (;;) {
        if (ui_due()) ui_frame();
        co_switch(&ui_ctx, &main_ctx);                      /* nothing to draw: idle */
    }
}

int main(void)
{
    ui_ctx.r[0] = (uint32_t)(uintptr_t)ui_task;
    ui_ctx.r[1] = (uint32_t)(uintptr_t)&ui_stack[384];
    composer_init();
    for (;;) {
        composer_tick();
        wait_tick = TICK_LO;
        REG32(AP_MMIO_CTRL + CR_TICK_DONE * 4u) = 1u;
        while (TICK_LO == wait_tick) co_switch(&main_ctx, &ui_ctx);
    }
}
