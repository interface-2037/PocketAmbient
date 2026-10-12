/*
 * hal.h - Thin hardware abstraction used by the composer. Two implementations:
 *   hal_mmio.c  - RISC-V firmware, memory-mapped registers
 *   hal_model.c - host build, drives model/synth_model.c directly
 *
 * Timing contract (keeps host == RTL bit-exact): the composer runs once per
 * 1 ms tick. Status reads return the engine state as of the tick boundary and
 * all writes made during a tick take effect together at the first sample of
 * the next 48-sample block.
 */
#ifndef AP_HAL_H
#define AP_HAL_H

#include <stdint.h>
#include "regs.h"

void     hal_vwrite(unsigned voice, unsigned reg, uint32_t val);
uint32_t hal_vstatus(unsigned voice);
void     hal_fxwrite(unsigned reg, uint32_t val);
uint32_t hal_fxread(unsigned reg);
uint32_t hal_ctrl_read(unsigned reg);
void     hal_ctrl_write(unsigned reg, uint32_t val);
uint64_t hal_ticks(void);
void     hal_visual_event(uint32_t ev);

/* Debug/analysis trace. No-op on hardware. */
enum {
    TR_NOTE_ON = 1,   /* a=voice b=pitch(cents) c=max_len_s<<16|owner<<8|algo */
    TR_NOTE_OFF = 2,  /* a=voice */
    TR_KEY = 3,       /* a=tonic pc b=centre degree */
    TR_MODE = 4,      /* a=mode b=gen */
    TR_RESEED = 5,    /* a=seed */
    TR_STATE = 6,     /* a=composer state */
    TR_CHORD = 7,     /* a=chord index */
    TR_LOOP = 8,      /* a=loop b=event (0 new,1 mutate,2 retire) c=period ms */
    TR_STEAL = 9,     /* a=voice */
    TR_FORCE_OFF = 10,/* a=voice: safety gate-off for a note past its max length */
    TR_MARK = 11      /* intentional dynamics: a=1 stinger, a=2 swell (b = length ms) */
};
void hal_trace(uint32_t kind, uint32_t a, uint32_t b, uint32_t c);

#endif
