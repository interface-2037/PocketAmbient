/*
 * regs.h - Register map shared by the composer firmware, the bit-exact C
 * model and (later) the RTL. This file is the contract between them; see
 * docs/REGISTER_MAP.md for the prose version. Keep both in sync.
 */
#ifndef AP_REGS_H
#define AP_REGS_H

#define AP_FS            48000u   /* output sample rate */
#define AP_TICK_SAMPLES  48u      /* composer tick = 1 ms */
#define AP_NUM_VOICES    32u
#define AP_KS_FIRST      28u      /* voices 28..31 are pluck (Karplus-Strong) capable */
#define AP_KS_SLOTS      4u
#define AP_KS_BUF        2048u    /* samples per pluck delay line (BRAM) */

/* ---- CPU memory map (section 3.4) ---- */
#define AP_MMIO_VOICE    0x10000000u  /* 32 voices x 32 words */
#define AP_MMIO_FX       0x10001000u  /* 64 words */
#define AP_MMIO_CTRL     0x10002000u  /* 64 words */
#define AP_MMIO_VISUAL   0x10003000u  /* write = push visual event */

/* ---- Voice registers (word index inside a voice's 32-word block) ---- */
#define VR_GATE       0   /* bit0 gate, bit1 trigger strobe (self-clearing) */
#define VR_ALGO       1   /* ALGO_* */
#define VR_PINC0      2   /* u32 phase increment, osc 0 (carrier) */
#define VR_PINC1      3   /* u32 phase increment, osc 1 (detune / modulator / harmonic) */
#define VR_FM_INDEX   4   /* U4.12 FM peak index, 1.0 = one full cycle (2*pi rad). slewed */
#define VR_FM_SUS     5   /* U4.12 FM index floor after mod envelope decays */
#define VR_MOD_DECAY  6   /* rate code, FM index envelope decay */
#define VR_ATTACK     7   /* rate code */
#define VR_DECAY      8   /* rate code */
#define VR_SUSTAIN    9   /* U1.15 sustain level (32768 = 1.0) */
#define VR_RELEASE    10  /* rate code */
#define VR_CUTOFF     11  /* U0.16 SVF frequency coefficient f = 2 sin(pi fc / 96000). slewed */
#define VR_RESO       12  /* U2.14 SVF damping q (16384 = 1.0; 2.0 = no resonance) */
#define VR_FMODE      13  /* FMODE_* */
#define VR_AMP        14  /* U1.15 output level. slewed */
#define VR_PAN        15  /* 0..32768, 16384 = centre, equal-power. slewed */
#define VR_SEND_REV   16  /* U1.15 */
#define VR_SEND_ECHO  17  /* U1.15 */
#define VR_WAVE       18  /* U1.15 PAD: saw->tri morph. SUB: osc1 level */
#define VR_NOISE      19  /* NOISE_* */
#define VR_DUST_RATE  20  /* u16 dust trigger probability per sample, /65536 */
#define VR_KS_LEN     21  /* U16.16 pluck loop delay in samples */
#define VR_KS_DAMP    22  /* U1.15 pluck loop gain */
#define VR_KS_BRIGHT  23  /* U0.16 excitation low-pass coefficient */
#define VR_PENV       24  /* u32 extra osc-0 phase increment at mod-envelope peak (pitch drop) */
#define VR_VIB_RATE   25  /* u32 vibrato LFO phase increment */
#define VR_VIB_DEPTH  26  /* U0.16 vibrato depth target, fraction of the increment (379 = 10 cents) */
#define VR_VIB_DELAY  27  /* rate code: depth ramps in from 0 after each trigger (0 = instant) */
#define VR_GLIDE      28  /* rate code: oscillator increments slew to PINC0/1 (0 = instant) */
#define VR_SLEW       29  /* rate code: cutoff AND amp slew (0 = default ~21 ms) */
#define VR_CUT_SLEW   VR_SLEW
#define VR_FENV       30  /* U0.16 cutoff boost x amp envelope level */
#define VR_STATUS     31  /* RO: [1:0] env state, [31:16] env level (U0.16) */

#define GATE_ON       1u
#define GATE_TRIG     2u

#define ALGO_PAD      0   /* dual band-limited saw/tri wavetable */
#define ALGO_FM       1   /* 2-op FM, osc1 modulates osc0 */
#define ALGO_NOISE    2   /* white / pink / dust */
#define ALGO_KS       3   /* Karplus-Strong pluck (voices 28..31 only) */
#define ALGO_SUB      4   /* sine + sine (harmonic) */
#define ALGO_PULSE    5   /* band-limited pulse = saw(p) - saw(p + width), width = VR_WAVE */

#define FMODE_LP      0
#define FMODE_BP      1
#define FMODE_HP      2
#define FMODE_BYPASS  3

#define NOISE_WHITE   0
#define NOISE_PINK    1
#define NOISE_DUST    2   /* white noise gated by random decaying bursts */
#define NOISE_PDUST   3   /* pink noise gated by random decaying bursts (softer top end) */

#define ENV_IDLE      0
#define ENV_ATTACK    1
#define ENV_DECAY     2   /* decays towards sustain and holds there */
#define ENV_RELEASE   3

/* ---- FX / master registers ---- */
#define FX_REV_GAIN0     0   /* 0..7: U0.16 per-line feedback gain */
#define FX_REV_DAMP      8   /* U0.16 one-pole coefficient (65535 = no damping) */
#define FX_REV_WET       9   /* U1.15 */
#define FX_REV_PREDELAY  10  /* samples, 1..AP_REV_PRE_MAX */
#define FX_ECHO_FB       11  /* U1.15 */
#define FX_ECHO_DAMP     12  /* U0.16 */
#define FX_ECHO_WET      13  /* U1.15 */
#define FX_ECHO_LEN_L    14  /* samples, 1..AP_ECHO_MAX */
#define FX_ECHO_LEN_R    15
#define FX_ECHO_TO_REV   16  /* U1.15 */
#define FX_DRY           17  /* U1.15 */
#define FX_MASTER_TARGET 18  /* U1.23 fade gain target (8388608 = unity) */
#define FX_MASTER_STEP   19  /* gain change per sample (linear ramp; gain is squared) */
#define FX_TRIM          20  /* U2.14 master trim, pre-limiter */
#define FX_ECHO_MOD_DEPTH 21 /* tape wobble depth, U8.8 samples (0 = off) */
#define FX_ECHO_MOD_RATE 22  /* tape wobble LFO phase increment */
#define FX_STATUS_GAIN   31  /* RO: current master gain */
#define FX_NUM_REGS      32

/* ---- Reverb / echo memory layout (external SRAM, 16-bit words) ---- */
#define AP_SRAM_WORDS    131072u
#define AP_REV_LINES     8
#define AP_REV_MOD_DEPTH 16      /* samples of modulation on lines 0 and 5 */
#define AP_REV_PRE_MAX   4800u
#define AP_ECHO_MAX      36000u
static const unsigned AP_REV_LEN[AP_REV_LINES] = {
    1601, 2207, 2999, 4111, 5639, 7717, 10589, 14519 };
static const unsigned AP_AP_LEN[4] = { 142, 107, 379, 277 };

/* ---- Control / status block ---- */
#define CR_MODE          0   /* menu: 0 Drift 1 Tides 2 Hush 3 Mother */
#define CR_DENSITY       1   /* menu: 1..5 */
#define CR_BRIGHT        2   /* menu: 1..5 */
#define CR_REVERB        3   /* menu: 0 Low 1 Med 2 High */
#define CR_VISUALS       4   /* menu: 0 Full 1 Minimal 2 Off */
#define CR_SEED_LOCK     5   /* menu: 0/1 */
#define CR_SEED          6   /* menu: u32 */
#define CR_SLEEP         7   /* menu: 0 Off 1 30min 2 60min 3 90min */
#define CR_CMD           8   /* bit0 reseed (clear-on-read) */
#define CR_SETTINGS_SEQ  9   /* increments whenever the bridge writes a menu setting */
#define CR_BTN_EVENTS    10  /* pressed-edge bitmap, clear-on-read */
#define CR_RTC_EPOCH     11
#define CR_TICK_LO       12
#define CR_TICK_HI       13
#define CR_TICK_DONE     14  /* CPU write: this tick's composer work is done, render its samples */
#define CR_TICK_CYCLES   15  /* RO, hardware: CPU cycles used by the previous tick (diagnostic) */
/* status written by the composer, read by video/bridge */
#define CR_ST_STATE      16
#define CR_ST_MODE       17
#define CR_ST_ELAPSED_S  18
#define CR_ST_SEED       19
#define CR_ST_DENSITY    20
#define CR_ST_BRIGHT     21
#define CR_ST_VISUALS    22
#define CR_ST_OVERLAY    23
#define CR_ST_REVERB     24
#define CR_ST_SLEEP      25
#define CR_NUM_REGS      32

/* button bits (match APF cont1_key ordering where possible) */
#define BTN_UP     (1u << 0)
#define BTN_DOWN   (1u << 1)
#define BTN_LEFT   (1u << 2)
#define BTN_RIGHT  (1u << 3)
#define BTN_A      (1u << 4)
#define BTN_B      (1u << 5)
#define BTN_X      (1u << 6)
#define BTN_Y      (1u << 7)
#define BTN_L      (1u << 8)
#define BTN_R      (1u << 9)
#define BTN_SELECT (1u << 14)
#define BTN_START  (1u << 15)

#endif
