#include "harmony.h"

static const int8_t MAJ[7] = {0, 2, 4, 5, 7, 9, 11};
static const int8_t TYPE_DEGS[5][4] = {
    {0, 2, 4, -1},   /* triad */
    {0, 2, 4, 6},    /* seventh */
    {0, 2, 4, 1},    /* add9 (9th = one degree up) */
    {0, 1, 4, -1},   /* sus2 */
    {0, 3, 4, -1},   /* sus4 */
};

const chord_t CHORD_VOCAB[CHORD_VOCAB_N] = {
    {0, CH_TRIAD}, {0, CH_ADD9}, {0, CH_SEV}, {0, CH_SUS2}, {0, CH_SUS4},
    {1, CH_SEV}, {2, CH_SEV}, {3, CH_SEV}, {3, CH_ADD9}, {5, CH_SEV},
    {5, CH_ADD9}, {4, CH_SUS4}, {3, CH_SUS2}, {5, CH_TRIAD},
};

static int pmod(int x, int m) { x %= m; return x < 0 ? x + m : x; }

int key_pc_of_degree(hkey_t k, int deg) { return pmod(k.tonic + MAJ[pmod(deg, 7)], 12); }

int key_has_pc(hkey_t k, int pc)
{
    for (int d = 0; d < 7; d++)
        if (key_pc_of_degree(k, d) == pmod(pc, 12)) return 1;
    return 0;
}

int key_centre_pc(hkey_t k) { return key_pc_of_degree(k, k.centre); }

static int centre_ok(int c) { return c == 0 || c == 1 || c == 3 || c == 4 || c == 5; }

hkey_t key_neighbor(hkey_t k, prng_t *r)
{
    /* moves: fifth up, fifth down, relative major/minor, other modal centre */
    static const uint32_t w[4] = {3, 3, 3, 1};
    hkey_t n = k;
    switch (prng_weighted(r, w, 4)) {
    case 0: n.tonic = (int8_t)pmod(k.tonic + 7, 12); break;
    case 1: n.tonic = (int8_t)pmod(k.tonic + 5, 12); break;
    case 2: n.centre = (int8_t)(k.centre == 5 ? 0 : 5); break;
    default: {
        static const int8_t cs[3] = {1, 3, 4};
        int8_t c;
        do { c = cs[prng_below(r, 3)]; } while (c == k.centre);
        n.centre = c;
        break;
    }
    }
    return n;
}

int key_is_neighbor(hkey_t a, hkey_t b)
{
    if (!centre_ok(a.centre) || !centre_ok(b.centre)) return 0;
    if (a.centre == b.centre)
        return pmod(b.tonic - a.tonic, 12) == 7 || pmod(b.tonic - a.tonic, 12) == 5;
    return a.tonic == b.tonic;
}

int chord_pcs(hkey_t k, chord_t c, int8_t out[4])
{
    int root = k.centre + c.deg, n = 0;
    for (int i = 0; i < 4; i++) {
        int d = TYPE_DEGS[c.type][i];
        if (d < 0) break;
        out[n++] = (int8_t)key_pc_of_degree(k, root + d);
    }
    return n;
}

int chord_valid(hkey_t k, chord_t c)
{
    int root = k.centre + c.deg;
    int r = key_pc_of_degree(k, root);
    if (pmod(key_pc_of_degree(k, root + 4) - r, 12) != 7) return 0;   /* perfect fifth */
    if (c.type == CH_SUS2 || c.type == CH_ADD9)
        if (pmod(key_pc_of_degree(k, root + 1) - r, 12) != 2) return 0;
    if (c.type == CH_SUS4)
        if (pmod(key_pc_of_degree(k, root + 3) - r, 12) != 5) return 0;
    return 1;
}

int chord_common_tones(hkey_t k, chord_t a, chord_t b)
{
    int8_t pa[4], pb[4];
    int na = chord_pcs(k, a, pa), nb = chord_pcs(k, b, pb), ct = 0;
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++)
            if (pa[i] == pb[j]) { ct++; break; }
    return ct;
}

int chord_next(hkey_t k, int cur, int32_t tension, prng_t *r)
{
    uint32_t w[CHORD_VOCAB_N];
    int cur_ok = cur >= 0 && cur < CHORD_VOCAB_N && chord_valid(k, CHORD_VOCAB[cur]);
    uint32_t home_mul = 6 - (uint32_t)(((tension + 32768) * 4) >> 16);    /* 6..2 */
    for (int j = 0; j < CHORD_VOCAB_N; j++) {
        w[j] = 0;
        if (j == cur || !chord_valid(k, CHORD_VOCAB[j])) continue;
        int ct = cur_ok ? chord_common_tones(k, CHORD_VOCAB[cur], CHORD_VOCAB[j]) : 1;
        uint32_t base = 2 + 3 * (uint32_t)(ct * ct);
        w[j] = base * (CHORD_VOCAB[j].deg == 0 ? home_mul : 3);
    }
    int n = prng_weighted(r, w, CHORD_VOCAB_N);
    return n < 0 ? 0 : n;
}

int chord_home(hkey_t k, int cur, prng_t *r)
{
    uint32_t w[CHORD_VOCAB_N];
    for (int j = 0; j < CHORD_VOCAB_N; j++)
        w[j] = (j != cur && CHORD_VOCAB[j].deg == 0 && chord_valid(k, CHORD_VOCAB[j])) ? 1 : 0;
    int n = prng_weighted(r, w, CHORD_VOCAB_N);
    return n < 0 ? 0 : n;
}

int key_pentatonic(hkey_t k, int8_t out[5])
{
    static const int8_t MAJP[5] = {0, 2, 4, 7, 9}, MINP[5] = {0, 3, 5, 7, 10};
    int minor = k.centre == 1 || k.centre == 5;
    int base = minor ? key_centre_pc(k) : k.tonic;
    for (int i = 0; i < 5; i++) out[i] = (int8_t)pmod(base + (minor ? MINP[i] : MAJP[i]), 12);
    return 5;
}

/* ---------------------------------------------------------------- voicing */

static int nearest_pc(int p, int pc, int lo, int hi, prng_t *r)
{
    int best = -1, bd = 1000;
    for (int q = lo; q <= hi; q++) {
        if (pmod(q, 12) != pc) continue;
        int d = q > p ? q - p : p - q;
        if (d < bd || (d == bd && (prng_next(r) & 1))) { bd = d; best = q; }
    }
    return best < 0 ? p : best;
}

static int nearest_chord_tone(int p, const int8_t *pcs, int npc, int lo, int hi, prng_t *r)
{
    int best = -1, bd = 1000;
    for (int q = lo; q <= hi; q++) {
        int ok = 0;
        for (int i = 0; i < npc; i++) ok |= pmod(q, 12) == pcs[i];
        if (!ok) continue;
        int d = q > p ? q - p : p - q;
        if (d < bd || (d == bd && (prng_next(r) & 1))) { bd = d; best = q; }
    }
    return best < 0 ? p : best;
}

static void sort_i32(int32_t *a, int n)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && a[j - 1] > a[j]; j--) {
            int32_t t = a[j]; a[j] = a[j - 1]; a[j - 1] = t;
        }
}

/* no unisons: move each duplicate to the unused chord tone nearest its reference pitch */
static void fix_unisons(int32_t *up, const int32_t *ref, int n, const int8_t *pcs, int npc,
                        int lo, int hi)
{
    for (int i = 1; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < i; j++) dup |= up[j] == up[i];
        if (!dup) continue;
        int best = up[i], bd = 1000;
        for (int q = lo - VL_HEADROOM; q <= hi + VL_HEADROOM; q++) {   /* crowded window */
            int ok = 0, used = 0;
            for (int t = 0; t < npc; t++) ok |= pmod(q, 12) == pcs[t];
            for (int j = 0; j < n; j++) used |= j != i && up[j] == q;
            if (!ok || used) continue;
            int d = q > ref[i] ? q - ref[i] : ref[i] - q;
            if (d < bd) { bd = d; best = q; }
        }
        up[i] = best;
    }
}

void voice_lead(const int8_t *pcs, int npc, int bass_prev,
                const int32_t *prev, int nprev, int n_upper,
                int lo, int hi, int bass_lo, int bass_hi,
                int32_t *out, prng_t *r)
{
    int32_t *up = out + 1;
    int bp = bass_prev > 0 ? bass_prev : (bass_lo + bass_hi) / 2;
    out[0] = nearest_pc(bp, pcs[0], bass_lo, bass_hi, r);

    if (nprev <= 0) {
        int32_t ref[8] = {0};
        int cur = lo + (int)prng_below(r, 5);
        for (int i = 0; i < n_upper && i < 8; i++) {
            int pc = pcs[(i + 1) % npc];
            int q = cur;
            while (pmod(q, 12) != pc) q++;
            if (q > hi) q = nearest_pc(hi, pc, lo, hi, r);   /* may collide; fixed below */
            up[i] = ref[i] = q;
            cur = q + 3;
        }
        fix_unisons(up, ref, n_upper, pcs, npc, lo, hi);
        sort_i32(up, n_upper);
        return;
    }

    /* match the voice count: drop the top voices, or add above the top */
    int32_t tmp[8];
    int n = nprev > 8 ? 8 : nprev;
    for (int i = 0; i < n; i++) tmp[i] = prev[i];
    sort_i32(tmp, n);
    while (n > n_upper) n--;
    while (n < n_upper) { tmp[n] = tmp[n - 1] + 4 > hi ? tmp[0] - 4 : tmp[n - 1] + 4; n++; }

    for (int i = 0; i < n; i++) up[i] = nearest_chord_tone(tmp[i], pcs, npc, lo, hi, r);

    /* coverage: every chord tone except possibly the root (the bass has it) */
    int first_req = n_upper >= npc ? 0 : 1;
    for (int m = first_req; m < npc; m++) {
        int have = 0;
        for (int i = 0; i < n; i++) have |= pmod(up[i], 12) == pcs[m];
        if (have) continue;
        int bi = -1, bcost = 1000, bq = 0;
        for (int i = 0; i < n; i++) {
            int dup = pmod(up[i], 12) == pcs[0] && first_req;   /* root is spare */
            for (int j = 0; j < n; j++)
                if (j != i && pmod(up[j], 12) == pmod(up[i], 12)) dup = 1;
            if (!dup) continue;
            int q = nearest_pc(tmp[i], pcs[m], lo, hi, r);
            int c = q > tmp[i] ? q - tmp[i] : tmp[i] - q;
            if (c < bcost) { bcost = c; bi = i; bq = q; }
        }
        if (bi >= 0) up[bi] = bq;
    }

    fix_unisons(up, tmp, n, pcs, npc, lo, hi);
    sort_i32(up, n);
}
