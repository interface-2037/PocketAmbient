/* sched.h - tick-based event queue (binary min-heap, fixed capacity) */
#ifndef AP_SCHED_H
#define AP_SCHED_H

#include <stdint.h>

#define SCHED_CAP 256

typedef struct {
    uint64_t when;      /* tick */
    uint32_t seq;       /* insertion order, breaks ties FIFO */
    uint8_t  type;
    uint8_t  prio;      /* higher = more important; lowest is dropped when full */
    uint8_t  owner;     /* mode generation, for bulk cancel */
    uint8_t  pad;
    uint32_t a, b;
} ev_t;

typedef struct {
    ev_t     h[SCHED_CAP];
    uint32_t n;
    uint32_t seq;
    uint32_t dropped;
} sched_t;

enum { SCHED_OK = 0, SCHED_REPLACED = 1, SCHED_DROPPED = -1 };

void sched_init(sched_t *s);
int  sched_push(sched_t *s, uint64_t when, uint8_t type, uint8_t prio, uint8_t owner,
                uint32_t a, uint32_t b);
int  sched_pop_due(sched_t *s, uint64_t now, ev_t *out);   /* 1 if an event was popped */
void sched_cancel_owner(sched_t *s, uint8_t owner);
static inline uint32_t sched_count(const sched_t *s) { return s->n; }

/* wrap-safe ordering: valid while the two times are < 2^63 ticks apart */
static inline int tick_before(uint64_t a, uint64_t b) { return (int64_t)(a - b) < 0; }

#endif
