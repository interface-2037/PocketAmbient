/* hal_model.h - host-only controls for the model HAL (renderer / tests) */
#ifndef AP_HAL_MODEL_H
#define AP_HAL_MODEL_H

#include <stdint.h>
#include "../../model/synth_model.h"

typedef void (*halm_trace_fn)(void *ctx, uint64_t tick, uint32_t kind,
                              uint32_t a, uint32_t b, uint32_t c);

void     halm_bind(sm_t *sm);
void     halm_set_tick(uint64_t t);
void     halm_ctrl_set(unsigned reg, uint32_t val);
uint32_t halm_ctrl_get(unsigned reg);
void     halm_press(uint32_t mask);
void     halm_set_trace(halm_trace_fn fn, void *ctx);
uint32_t halm_visual_count(void);

#endif
