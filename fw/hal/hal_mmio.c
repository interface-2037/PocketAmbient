/*
 * hal_mmio.c - RISC-V (PicoRV32) HAL: memory-mapped registers (fw/hal/regs.h).
 * Not compiled on the host. Voice/FX writes land in shadow registers that the
 * engine commits at the next tick boundary; status reads are tick snapshots.
 */
#include "hal.h"
#include "../ui/ui.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

void hal_vwrite(unsigned voice, unsigned reg, uint32_t val)
{
    REG32(AP_MMIO_VOICE + (voice * 32u + reg) * 4u) = val;
}

uint32_t hal_vstatus(unsigned voice)
{
    return REG32(AP_MMIO_VOICE + (voice * 32u + VR_STATUS) * 4u);
}

void hal_fxwrite(unsigned reg, uint32_t val) { REG32(AP_MMIO_FX + reg * 4u) = val; }
uint32_t hal_fxread(unsigned reg) { return REG32(AP_MMIO_FX + reg * 4u); }
uint32_t hal_ctrl_read(unsigned reg) { return REG32(AP_MMIO_CTRL + reg * 4u); }   /* clear-on-read in HW */
void hal_ctrl_write(unsigned reg, uint32_t val) { REG32(AP_MMIO_CTRL + reg * 4u) = val; }

uint64_t hal_ticks(void)
{
    uint32_t hi, lo;
    do {                                   /* re-read if the low word wrapped in between */
        hi = REG32(AP_MMIO_CTRL + CR_TICK_HI * 4u);
        lo = REG32(AP_MMIO_CTRL + CR_TICK_LO * 4u);
    } while (hi != REG32(AP_MMIO_CTRL + CR_TICK_HI * 4u));
    return ((uint64_t)hi << 32) | lo;
}

void hal_visual_event(uint32_t ev) { ui_event(ev); }   /* the UI runs on this CPU (fw/ui) */
void hal_trace(uint32_t kind, uint32_t a, uint32_t b, uint32_t c) { (void)kind; (void)a; (void)b; (void)c; }
