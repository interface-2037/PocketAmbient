#include "sched.h"

static int ev_less(const ev_t *a, const ev_t *b)
{
    if (a->when != b->when) return tick_before(a->when, b->when);
    return (int32_t)(a->seq - b->seq) < 0;
}

static void swap(ev_t *a, ev_t *b) { ev_t t = *a; *a = *b; *b = t; }

static void sift_up(sched_t *s, uint32_t i)
{
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (!ev_less(&s->h[i], &s->h[p])) break;
        swap(&s->h[i], &s->h[p]);
        i = p;
    }
}

static void sift_down(sched_t *s, uint32_t i)
{
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < s->n && ev_less(&s->h[l], &s->h[m])) m = l;
        if (r < s->n && ev_less(&s->h[r], &s->h[m])) m = r;
        if (m == i) break;
        swap(&s->h[i], &s->h[m]);
        i = m;
    }
}

static void remove_at(sched_t *s, uint32_t i)
{
    s->n--;
    if (i == s->n) return;
    s->h[i] = s->h[s->n];
    sift_down(s, i);
    sift_up(s, i);
}

void sched_init(sched_t *s) { s->n = 0; s->seq = 0; s->dropped = 0; }

int sched_push(sched_t *s, uint64_t when, uint8_t type, uint8_t prio, uint8_t owner,
               uint32_t a, uint32_t b)
{
    int ret = SCHED_OK;
    if (s->n == SCHED_CAP) {
        /* full: evict the lowest-priority (latest among equals) event if the new one beats it */
        uint32_t lo = 0;
        for (uint32_t i = 1; i < s->n; i++) {
            if (s->h[i].prio < s->h[lo].prio ||
                (s->h[i].prio == s->h[lo].prio && ev_less(&s->h[lo], &s->h[i])))
                lo = i;
        }
        s->dropped++;
        if (prio <= s->h[lo].prio) return SCHED_DROPPED;
        remove_at(s, lo);
        ret = SCHED_REPLACED;
    }
    ev_t *e = &s->h[s->n];
    e->when = when; e->seq = s->seq++; e->type = type; e->prio = prio;
    e->owner = owner; e->pad = 0; e->a = a; e->b = b;
    sift_up(s, s->n++);
    return ret;
}

int sched_pop_due(sched_t *s, uint64_t now, ev_t *out)
{
    if (!s->n || tick_before(now, s->h[0].when)) return 0;
    *out = s->h[0];
    remove_at(s, 0);
    return 1;
}

void sched_cancel_owner(sched_t *s, uint8_t owner)
{
    uint32_t i = 0;
    while (i < s->n) {
        if (s->h[i].owner == owner) remove_at(s, i);
        else i++;
    }
}
