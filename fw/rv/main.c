/*
 * main.c - composer firmware main loop (PicoRV32). The engine sequencer (cpu_sys.sv) holds
 * audio at each tick boundary until the CPU writes CR_TICK_DONE; it then renders the
 * tick's 48 samples and advances CR_TICK_LO/HI. Same order as model/render.c:
 * composer_init() at tick 0, then composer_tick() once per tick, before its samples.
 */
#include "../hal/hal.h"
#include "../composer/composer.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

int main(void)
{
    composer_init();
    for (;;) {
        composer_tick();
        uint32_t t = REG32(AP_MMIO_CTRL + CR_TICK_LO * 4u);
        REG32(AP_MMIO_CTRL + CR_TICK_DONE * 4u) = 1u;
        while (REG32(AP_MMIO_CTRL + CR_TICK_LO * 4u) == t) { }
    }
}
