/*
 * rvsim.c - runs the real composer firmware (build/fw/composer.bin) on a small RV32IMC
 * instruction-set simulator, with the MMIO block wired to the bit-exact synth model.
 * Proves the RISC-V build of the composer behaves exactly like the host build
 * (model/render.c) before any RTL is involved, and measures the CPU work per tick.
 *
 *   rvsim --fw build/fw/composer.bin|src/fpga/core/cpu/composer.hex --mode drift --seed 123 --seconds 60 --out x.raw
 *
 * Options (same meaning and defaults as render): --mode --seed --density --brightness
 *   --reverb --sleep --seconds/--minutes --press SEC:BTN[,...] --no-autostart
 *   --set SEC:NAME=VAL[,...] (menu changes, model/setscript.h)   --out FILE (raw s16le stereo)   --quiet
 *   --profile N (PC histogram of tick N on stderr)   --eng-cycles N (FIFO estimate, default 700)
 * Prints instructions per tick (max / mean), an estimate of PicoRV32 cycles and the
 * deepest stack use.
 *
 * MMIO behaviour mirrors cpu_sys.sv: voice/FX writes go straight to the engine (the engine
 * is idle while the CPU runs a tick), CR_BTN_EVENTS and CR_CMD clear on read, a write to
 * CR_TICK_DONE renders the tick's 48 samples and advances the tick counter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "synth_model.h"
#include "setscript.h"

#define RAM_BYTES 65536u

static uint8_t  ram[RAM_BYTES];
static uint32_t x[32], pc;
static sm_t     g_sm;
static uint32_t ctrl[CR_NUM_REGS];
static uint64_t tick, dur_ms;
static FILE    *out;
static uint32_t min_sp = RAM_BYTES, top_sp;
static int64_t  prof_tick = -1;               /* --profile N: PC histogram of tick N */
static uint32_t prof[RAM_BYTES / 2];

/* per-tick statistics: instructions and an estimated PicoRV32 cycle count */
static uint64_t ins_tick, cyc_tick, ins_max, cyc_max, ins_sum, cyc_sum, max_tick_at;
/* audio FIFO model: CPU tick then 48 engine samples, all serial; the I2S side takes one frame
 * every 1546.875 clocks. max_lag is how far production falls behind real time, in clocks:
 * the FIFO must be pre-filled with at least that much audio (DECISIONS D48). */
static uint64_t eng_cycles = 700, prod_cycles;
static double   max_lag;

typedef struct { uint64_t tick; uint32_t mask; } press_t;
static press_t presses[4096];
static int npress, pi_next;
static setscript_t sets[4096];
static int nset, si_next;

static uint32_t btn_mask(const char *s)
{
    static const struct { const char *n; uint32_t m; } T[] = {
        {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
        {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R},
        {"SELECT", BTN_SELECT}, {"START", BTN_START}};
    for (unsigned i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcmp(s, T[i].n)) return T[i].m;
    fprintf(stderr, "unknown button %s\n", s);
    exit(2);
}

static void add_press(uint64_t ms, uint32_t mask)
{
    if (npress < (int)(sizeof presses / sizeof presses[0])) {
        presses[npress].tick = ms;
        presses[npress].mask = mask;
        npress++;
    }
}

static int cmp_press(const void *a, const void *b)
{
    const press_t *p = a, *q = b;
    return p->tick < q->tick ? -1 : p->tick > q->tick;
}

static void deliver_presses(void)
{
    while (pi_next < npress && presses[pi_next].tick <= tick) ctrl[CR_BTN_EVENTS] |= presses[pi_next++].mask;
    setscript_apply(sets, nset, &si_next, tick, ctrl);
}

static void fail(const char *what, uint32_t a)
{
    fprintf(stderr, "rvsim: %s 0x%08x at pc 0x%08x, tick %llu\n", what, a, pc, (unsigned long long)tick);
    exit(1);
}

/* ------------------------------------------------------------------ tick boundary */

static void end_tick(void)
{
    int16_t buf[AP_TICK_SAMPLES * 2];
    for (unsigned k = 0; k < AP_TICK_SAMPLES; k++) sm_process(&g_sm, &buf[2 * k], &buf[2 * k + 1]);
    if (out) fwrite(buf, sizeof buf, 1, out);
    if (ins_tick > ins_max) { ins_max = ins_tick; max_tick_at = tick; }
    if (cyc_tick > cyc_max) cyc_max = cyc_tick;
    ins_sum += ins_tick;
    cyc_sum += cyc_tick;
    prod_cycles += cyc_tick + AP_TICK_SAMPLES * eng_cycles;
    double lag = (double)prod_cycles - (double)(tick + 1) * AP_TICK_SAMPLES * 1546.875;
    if (lag > max_lag) max_lag = lag;
    ins_tick = cyc_tick = 0;
    tick++;
    deliver_presses();
}

/* ------------------------------------------------------------------ memory */

static uint32_t mmio_read(uint32_t a)
{
    if ((a & 3u) != 0) fail("unaligned MMIO read", a);
    if (a >= AP_MMIO_VOICE && a < AP_MMIO_VOICE + 0x1000u) {
        uint32_t w = (a - AP_MMIO_VOICE) >> 2;
        return sm_vread(&g_sm, w >> 5, w & 31u);
    }
    if (a >= AP_MMIO_FX && a < AP_MMIO_FX + 0x100u) return sm_fxread(&g_sm, (a - AP_MMIO_FX) >> 2);
    if (a >= AP_MMIO_CTRL && a < AP_MMIO_CTRL + 0x100u) {
        uint32_t r = (a - AP_MMIO_CTRL) >> 2;
        if (r == CR_TICK_LO) return (uint32_t)tick;
        if (r == CR_TICK_HI) return (uint32_t)(tick >> 32);
        if (r >= CR_NUM_REGS) return 0;
        uint32_t v = ctrl[r];
        if (r == CR_BTN_EVENTS || r == CR_CMD) ctrl[r] = 0;
        return v;
    }
    fail("read from unmapped address", a);
    return 0;
}

static void mmio_write(uint32_t a, uint32_t v)
{
    if ((a & 3u) != 0) fail("unaligned MMIO write", a);
    if (a >= AP_MMIO_VOICE && a < AP_MMIO_VOICE + 0x1000u) {
        uint32_t w = (a - AP_MMIO_VOICE) >> 2;
        sm_vwrite(&g_sm, w >> 5, w & 31u, v);
    } else if (a >= AP_MMIO_FX && a < AP_MMIO_FX + 0x100u) {
        sm_fxwrite(&g_sm, (a - AP_MMIO_FX) >> 2, v);
    } else if (a >= AP_MMIO_CTRL && a < AP_MMIO_CTRL + 0x100u) {
        uint32_t r = (a - AP_MMIO_CTRL) >> 2;
        if (r == CR_TICK_DONE) end_tick();
        else if (r >= CR_ST_STATE && r < CR_NUM_REGS) ctrl[r] = v;
    } else if (a != AP_MMIO_VISUAL) {
        fail("write to unmapped address", a);
    }
}

static uint32_t load(uint32_t a, unsigned size)
{
    if (a >= 0x10000000u) return mmio_read(a);
    if (a + size > RAM_BYTES || (a & (size - 1u))) fail("bad load", a);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint32_t)ram[a + i] << (8 * i);
    return v;
}

static void store(uint32_t a, uint32_t v, unsigned size)
{
    if (a >= 0x10000000u) { if (size != 4) fail("sub-word MMIO write", a); mmio_write(a, v); return; }
    if (a + size > RAM_BYTES || (a & (size - 1u))) fail("bad store", a);
    for (unsigned i = 0; i < size; i++) ram[a + i] = (uint8_t)(v >> (8 * i));
}

/* ------------------------------------------------------------------ RV32C -> RV32I */

static uint32_t enc_r(unsigned f7, unsigned rs2, unsigned rs1, unsigned f3, unsigned rd, unsigned op)
{ return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op; }
static uint32_t enc_i(int32_t imm, unsigned rs1, unsigned f3, unsigned rd, unsigned op)
{ return ((uint32_t)imm & 0xFFFu) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op; }
static uint32_t enc_s(int32_t imm, unsigned rs2, unsigned rs1, unsigned f3, unsigned op)
{ uint32_t u = (uint32_t)imm; return (u >> 5 & 0x7Fu) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | (u & 31u) << 7 | op; }
static uint32_t enc_b(int32_t imm, unsigned rs2, unsigned rs1, unsigned f3)
{
    uint32_t u = (uint32_t)imm;
    return (u >> 12 & 1u) << 31 | (u >> 5 & 0x3Fu) << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 |
           (u >> 1 & 0xFu) << 8 | (u >> 11 & 1u) << 7 | 0x63u;
}
static uint32_t enc_j(int32_t imm, unsigned rd)
{
    uint32_t u = (uint32_t)imm;
    return (u >> 20 & 1u) << 31 | (u >> 1 & 0x3FFu) << 21 | (u >> 11 & 1u) << 20 | (u >> 12 & 0xFFu) << 12 |
           rd << 7 | 0x6Fu;
}
static int32_t sext(uint32_t v, unsigned bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static uint32_t expand(uint32_t i)
{
    unsigned f3 = i >> 13 & 7u, rd = i >> 7 & 31u, rs2 = i >> 2 & 31u;
    unsigned rdp = 8 + (i >> 2 & 7u), rs1p = 8 + (i >> 7 & 7u);
    int32_t imm6 = sext((i >> 7 & 0x20u) | (i >> 2 & 0x1Fu), 6);
    uint32_t sh = (i >> 7 & 0x20u) | (i >> 2 & 0x1Fu);
    int32_t joff = sext((i >> 1 & 0x800u) | (i << 2 & 0x400u) | (i >> 1 & 0x300u) | (i << 1 & 0x80u) |
                        (i >> 1 & 0x40u) | (i << 3 & 0x20u) | (i >> 7 & 0x10u) | (i >> 2 & 0xEu), 12);
    int32_t boff = sext((i >> 4 & 0x100u) | (i << 1 & 0xC0u) | (i << 3 & 0x20u) | (i >> 7 & 0x18u) |
                        (i >> 2 & 0x6u), 9);
    uint32_t lw_off = (i >> 7 & 0x38u) | (i << 1 & 0x40u) | (i >> 4 & 0x4u);
    switch (i & 3u) {
    case 0:
        if (f3 == 0) {
            uint32_t nz = (i >> 7 & 0x30u) | (i >> 1 & 0x3C0u) | (i >> 4 & 0x4u) | (i >> 2 & 0x8u);
            if (!nz) return 0;
            return enc_i((int32_t)nz, 2, 0, rdp, 0x13);                       /* c.addi4spn */
        }
        if (f3 == 2) return enc_i((int32_t)lw_off, rs1p, 2, rdp, 0x03);       /* c.lw */
        if (f3 == 6) return enc_s((int32_t)lw_off, rdp, rs1p, 2, 0x23);       /* c.sw */
        return 0;
    case 1:
        switch (f3) {
        case 0: return enc_i(imm6, rd, 0, rd, 0x13);                          /* c.addi / c.nop */
        case 1: return enc_j(joff, 1);                                         /* c.jal */
        case 2: return enc_i(imm6, 0, 0, rd, 0x13);                           /* c.li */
        case 3:
            if (rd == 2) {                                                     /* c.addi16sp */
                int32_t im = sext((i >> 3 & 0x200u) | (i >> 2 & 0x10u) | (i << 1 & 0x40u) |
                                  (i << 4 & 0x180u) | (i << 3 & 0x20u), 10);
                return im ? enc_i(im, 2, 0, 2, 0x13) : 0;
            } else {                                                           /* c.lui */
                int32_t im = sext((i << 5 & 0x20000u) | (i << 10 & 0x1F000u), 18);
                return im ? (((uint32_t)im & 0xFFFFF000u) | rd << 7 | 0x37u) : 0;
            }
        case 4:
            switch (i >> 10 & 3u) {
            case 0: return enc_r(0x00, sh & 31u, rs1p, 5, rs1p, 0x13);        /* c.srli */
            case 1: return enc_r(0x20, sh & 31u, rs1p, 5, rs1p, 0x13);        /* c.srai */
            case 2: return enc_i(imm6, rs1p, 7, rs1p, 0x13);                  /* c.andi */
            default: {
                if (i >> 12 & 1u) return 0;
                static const unsigned f3s[4] = {0, 4, 6, 7};
                unsigned k = i >> 5 & 3u;
                return enc_r(k == 0 ? 0x20 : 0x00, rdp, rs1p, f3s[k], rs1p, 0x33);   /* sub xor or and */
            }
            }
        case 5: return enc_j(joff, 0);                                         /* c.j */
        case 6: return enc_b(boff, 0, rs1p, 0);                                /* c.beqz */
        default: return enc_b(boff, 0, rs1p, 1);                               /* c.bnez */
        }
    case 2:
        switch (f3) {
        case 0: return enc_r(0, sh & 31u, rd, 1, rd, 0x13);                   /* c.slli */
        case 2: return enc_i((int32_t)((i >> 7 & 0x20u) | (i >> 2 & 0x1Cu) | (i << 4 & 0xC0u)), 2, 2, rd, 0x03);
        case 4:
            if (!(i >> 12 & 1u)) return rs2 ? enc_r(0, rs2, 0, 0, rd, 0x33)   /* c.mv */
                                            : enc_i(0, rd, 0, 0, 0x67);        /* c.jr */
            if (!rs2) return rd ? enc_i(0, rd, 0, 1, 0x67) : 0x00100073u;      /* c.jalr / c.ebreak */
            return enc_r(0, rs2, rd, 0, rd, 0x33);                             /* c.add */
        case 6: return enc_s((int32_t)((i >> 7 & 0x3Cu) | (i >> 1 & 0xC0u)), rs2, 2, 2, 0x23);   /* c.swsp */
        default: return 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ execute */

/* rough PicoRV32 cycle counts (README, ENABLE_FAST_MUL, ENABLE_DIV, barrel shifter) */
enum { C_ALU = 3, C_MEM = 5, C_BR_T = 5, C_BR_N = 3, C_JAL = 3, C_JALR = 6, C_MUL = 6, C_DIV = 40 };

static void step(void)
{
    uint32_t i = load(pc, 2), len = 4;
    if ((i & 3u) != 3u) {
        uint32_t e = expand(i);
        if (!e) fail("illegal compressed instruction", i);
        i = e;
        len = 2;
    } else {
        i |= load(pc + 2, 2) << 16;
    }
    uint32_t op = i & 0x7Fu, rd = i >> 7 & 31u, f3 = i >> 12 & 7u, rs1 = i >> 15 & 31u, rs2 = i >> 20 & 31u;
    uint32_t f7 = i >> 25, a = x[rs1], b = x[rs2], npc = pc + len, r = 0;
    int32_t ii = (int32_t)i >> 20;
    int32_t is = ((int32_t)(i & 0xFE000000u) >> 20) | (int32_t)(i >> 7 & 31u);
    int32_t ib = ((int32_t)(i & 0x80000000u) >> 19) | (int32_t)((i & 0x80u) << 4) |
                 (int32_t)(i >> 20 & 0x7E0u) | (int32_t)(i >> 7 & 0x1Eu);
    int32_t ij = ((int32_t)(i & 0x80000000u) >> 11) | (int32_t)(i & 0xFF000u) |
                 (int32_t)(i >> 9 & 0x800u) | (int32_t)(i >> 20 & 0x7FEu);
    int wr = 1;
    unsigned cyc = C_ALU;
    switch (op) {
    case 0x37: r = i & 0xFFFFF000u; break;                                     /* lui */
    case 0x17: r = pc + (i & 0xFFFFF000u); break;                              /* auipc */
    case 0x6F: r = npc; npc = pc + (uint32_t)ij; cyc = C_JAL; break;           /* jal */
    case 0x67: r = npc; npc = (a + (uint32_t)ii) & ~1u; cyc = C_JALR; break;   /* jalr */
    case 0x63: {
        int t;
        switch (f3) {
        case 0: t = a == b; break;
        case 1: t = a != b; break;
        case 4: t = (int32_t)a < (int32_t)b; break;
        case 5: t = (int32_t)a >= (int32_t)b; break;
        case 6: t = a < b; break;
        case 7: t = a >= b; break;
        default: fail("illegal branch", i); t = 0;
        }
        if (t) npc = pc + (uint32_t)ib;
        cyc = t ? C_BR_T : C_BR_N;
        wr = 0;
        break;
    }
    case 0x03: {
        uint32_t ad = a + (uint32_t)ii;
        cyc = C_MEM;
        switch (f3) {
        case 0: r = (uint32_t)(int32_t)(int8_t)load(ad, 1); break;
        case 1: r = (uint32_t)(int32_t)(int16_t)load(ad, 2); break;
        case 2: r = load(ad, 4); break;
        case 4: r = load(ad, 1); break;
        case 5: r = load(ad, 2); break;
        default: fail("illegal load", i);
        }
        break;
    }
    case 0x23:
        cyc = C_MEM;
        wr = 0;
        if (f3 > 2) fail("illegal store", i);
        store(a + (uint32_t)is, b, 1u << f3);
        break;
    case 0x13: case 0x33: {
        int reg = op == 0x33;
        uint32_t c = reg ? b : (uint32_t)ii;
        if (reg && f7 == 1) {                                                  /* M extension */
            int64_t sa = (int32_t)a, sb = (int32_t)b;
            cyc = f3 < 4 ? C_MUL : C_DIV;
            switch (f3) {
            case 0: r = a * b; break;
            case 1: r = (uint32_t)((sa * sb) >> 32); break;
            case 2: r = (uint32_t)((sa * (int64_t)(uint64_t)b) >> 32); break;
            case 3: r = (uint32_t)(((uint64_t)a * b) >> 32); break;
            case 4: r = b == 0 ? ~0u : (a == 0x80000000u && b == ~0u) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
            case 5: r = b == 0 ? ~0u : a / b; break;
            case 6: r = b == 0 ? a : (a == 0x80000000u && b == ~0u) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
            default: r = b == 0 ? a : a % b; break;
            }
            break;
        }
        switch (f3) {
        case 0: r = (reg && f7 == 0x20) ? a - c : a + c; break;
        case 1: r = a << (c & 31u); break;
        case 2: r = (int32_t)a < (int32_t)c; break;
        case 3: r = a < c; break;
        case 4: r = a ^ c; break;
        case 5: r = (f7 & 0x20u) ? (uint32_t)((int32_t)a >> (c & 31u)) : a >> (c & 31u); break;
        case 6: r = a | c; break;
        default: r = a & c; break;
        }
        break;
    }
    case 0x0F: wr = 0; break;                                                  /* fence */
    default: fail("illegal instruction", i);
    }
    if (wr && rd) x[rd] = r;
    if (x[2] > top_sp) top_sp = x[2];
    if (x[2] && x[2] < min_sp) min_sp = x[2];
    if ((int64_t)tick == prof_tick && pc < RAM_BYTES) prof[pc >> 1]++;
    pc = npc;
    ins_tick++;
    cyc_tick += cyc;
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    const char *fw = "build/fw/composer.bin", *out_path = NULL;
    uint32_t mode = 0, seed = 1, density = 3, bright = 3, reverb = 1, sleep_sel = 0;
    int autostart = 1, quiet = 0;
    dur_ms = 10000;
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
#define ARG(n) (!strcmp(o, n) && ++i < argc)
        if (ARG("--fw")) fw = v;
        else if (ARG("--mode")) {
            static const char *M[4] = {"drift", "tides", "hush", "mother"};
            mode = 4;
            for (uint32_t k = 0; k < 4; k++) if (!strcmp(v, M[k])) mode = k;
            if (mode == 4) { fprintf(stderr, "unknown mode %s\n", v); return 2; }
        }
        else if (ARG("--seed")) seed = (uint32_t)strtoul(v, NULL, 0);
        else if (ARG("--density")) density = (uint32_t)atoi(v);
        else if (ARG("--brightness")) bright = (uint32_t)atoi(v);
        else if (ARG("--reverb")) reverb = (uint32_t)atoi(v);
        else if (ARG("--sleep")) sleep_sel = (uint32_t)atoi(v);
        else if (ARG("--seconds")) dur_ms = (uint64_t)(atof(v) * 1000.0 + 0.5);
        else if (ARG("--minutes")) dur_ms = (uint64_t)(atof(v) * 60000.0 + 0.5);
        else if (ARG("--out")) out_path = v;
        else if (ARG("--press")) {
            char *s = malloc(strlen(v) + 1);
            strcpy(s, v);
            for (char *t = strtok(s, ","); t; t = strtok(NULL, ",")) {
                char *c = strchr(t, ':');
                if (!c) { fprintf(stderr, "bad --press %s\n", t); return 2; }
                *c = 0;
                add_press((uint64_t)(atof(t) * 1000.0 + 0.5), btn_mask(c + 1));
            }
            free(s);
        }
        else if (ARG("--set")) nset = setscript_parse(v, sets, nset, 4096);
        else if (ARG("--profile")) prof_tick = atoll(v);
        else if (ARG("--eng-cycles")) eng_cycles = strtoull(v, NULL, 0);
        else if (!strcmp(o, "--no-autostart")) autostart = 0;
        else if (!strcmp(o, "--quiet")) quiet = 1;
        else { fprintf(stderr, "unknown/incomplete option %s (see header of model/rvsim.c)\n", o); return 2; }
#undef ARG
    }
    if (autostart) add_press(1, BTN_A);
    qsort(presses, (size_t)npress, sizeof presses[0], cmp_press);

    FILE *f = fopen(fw, "rb");
    if (!f) { perror(fw); return 2; }
    size_t n = 0, fl = strlen(fw);
    if (fl > 4 && !strcmp(fw + fl - 4, ".hex")) {          /* BRAM init file: one LE word per line */
        unsigned w;
        while (n + 4 <= RAM_BYTES && fscanf(f, "%x", &w) == 1) {
            for (int k = 0; k < 4; k++) ram[n + k] = (uint8_t)(w >> (8 * k));
            n += 4;
        }
    } else {
        n = fread(ram, 1, RAM_BYTES, f);
    }
    fclose(f);
    if (out_path && !(out = fopen(out_path, "wb"))) { perror(out_path); return 2; }

    sm_init(&g_sm);
    ctrl[CR_MODE] = mode;
    ctrl[CR_DENSITY] = density;
    ctrl[CR_BRIGHT] = bright;
    ctrl[CR_REVERB] = reverb;
    ctrl[CR_SLEEP] = sleep_sel;
    ctrl[CR_SEED_LOCK] = 1;
    ctrl[CR_SEED] = seed;
    deliver_presses();                       /* presses at tick 0 (render applies them before tick 0 too) */

    pc = 0;
    uint64_t guard = 0;
    while (tick < dur_ms) {
        uint64_t t0 = tick;
        step();
        guard = tick == t0 ? guard + 1 : 0;
        if (guard > 50000000u) fail("no tick completed in 50M instructions", 0);
    }
    if (out) fclose(out);
    if (prof_tick >= 0)
        for (uint32_t k = 0; k < RAM_BYTES / 2; k++)
            if (prof[k]) fprintf(stderr, "prof %08x %u\n", k * 2, prof[k]);
    if (!quiet)
        printf("rvsim: FIFO lead needed (engine %llu cycles/sample): %.0f clocks = %.1f frames\n",
               (unsigned long long)eng_cycles, max_lag, max_lag / 1546.875);
    if (!quiet)
        printf("rvsim: %zu-byte image, %llu ticks; instructions/tick max %llu (tick %llu) mean %.0f; "
               "est. PicoRV32 cycles/tick max %llu mean %.0f; stack used %u bytes\n",
               n, (unsigned long long)tick, (unsigned long long)ins_max, (unsigned long long)max_tick_at,
               (double)ins_sum / (double)tick, (unsigned long long)cyc_max, (double)cyc_sum / (double)tick,
               top_sp - min_sp);
    return 0;
}
