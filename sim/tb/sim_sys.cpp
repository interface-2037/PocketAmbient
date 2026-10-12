// sim_sys.cpp - Phase 4 full-system simulation (Verilator): cpu_sys running the real composer
// firmware (core/cpu/composer.hex) drives the audio engine; every sample that enters the
// audio FIFO is written out, to compare bit-for-bit with model/render (sim/run_sys.sh).
//
//   obj_dir/Vcpu_sys --mode drift --seed 123 --seconds 60 --out x.raw [--press SEC:BTN,...]
//                    [--set SEC:NAME=VAL,...] [--no-autostart] [--fast] [--rd 10 --wr 8]
//                    [--frames SEC,... --frame-prefix PATH]   (scanned-out video frames as PPM)
//
// Run from src/fpga (tables and firmware load relative to it, like Quartus).
// SRAM: behavioural, ack after --rd / --wr cycles (default 10 / 8 = sram_ctrl at (6,5)),
// random power-up contents. Frame ticks: 48 kHz from 74.25 MHz (1546.875 clocks), or with
// --fast as often as the FIFO allows (same samples, about 2x faster).
// Buttons follow render's timing: a press at tick t is visible to composer_tick(t), so the
// edge is pulsed while the tick counter is t - 1 (presses at tick 0 are not supported).
// Menu changes (--set, model/setscript.h) drive the cfg_* / settings_seq ports as soon as the
// tick counter reaches t (the CPU reads them during composer_tick(t)); RESEED pulses cmd_reseed
// at t - 1 like a button.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <vector>
#include <algorithm>
#include "Vcpu_sys.h"
#include "verilated.h"
#include "../../model/setscript.h"

struct Press { uint64_t tick; uint32_t mask; };

static uint32_t btn_mask(const char *s)
{
    static const struct { const char *n; uint32_t m; } T[] = {
        {"UP", 1u << 0}, {"DOWN", 1u << 1}, {"LEFT", 1u << 2}, {"RIGHT", 1u << 3}, {"A", 1u << 4},
        {"B", 1u << 5}, {"X", 1u << 6}, {"Y", 1u << 7}, {"L", 1u << 8}, {"R", 1u << 9},
        {"SELECT", 1u << 14}, {"START", 1u << 15}};
    for (auto &t : T) if (!strcmp(s, t.n)) return t.m;
    fprintf(stderr, "unknown button %s\n", s);
    exit(2);
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    const char *out_path = nullptr;
    uint32_t mode = 0, seed = 1, density = 3, bright = 3, reverb = 1, sleep_sel = 0;
    uint64_t dur_ms = 1000;
    int autostart = 1, fast = 0, rd_lat = 10, wr_lat = 8;
    std::vector<Press> presses;
    static setscript_t sets[4096];
    int nset = 0;
    std::vector<uint64_t> frame_ms;
    const char *frame_prefix = "frame";
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : "";
#define ARG(n) (!strcmp(o, n) && ++i < argc)
        if (ARG("--mode")) {
            const char *M[4] = {"drift", "tides", "hush", "mother"};
            mode = 4;
            for (uint32_t k = 0; k < 4; k++) if (!strcmp(v, M[k])) mode = k;
            if (mode == 4) { fprintf(stderr, "unknown mode %s\n", v); return 2; }
        }
        else if (ARG("--seed")) seed = (uint32_t)strtoul(v, nullptr, 0);
        else if (ARG("--density")) density = (uint32_t)atoi(v);
        else if (ARG("--brightness")) bright = (uint32_t)atoi(v);
        else if (ARG("--reverb")) reverb = (uint32_t)atoi(v);
        else if (ARG("--sleep")) sleep_sel = (uint32_t)atoi(v);
        else if (ARG("--seconds")) dur_ms = (uint64_t)(atof(v) * 1000.0 + 0.5);
        else if (ARG("--out")) out_path = v;
        else if (ARG("--rd")) rd_lat = atoi(v);
        else if (ARG("--wr")) wr_lat = atoi(v);
        else if (ARG("--press")) {
            std::vector<char> s(v, v + strlen(v) + 1);
            for (char *t = strtok(s.data(), ","); t; t = strtok(nullptr, ",")) {
                char *c = strchr(t, ':');
                if (!c) { fprintf(stderr, "bad --press %s\n", t); return 2; }
                *c = 0;
                presses.push_back({(uint64_t)(atof(t) * 1000.0 + 0.5), btn_mask(c + 1)});
            }
        }
        else if (ARG("--set")) nset = setscript_parse(v, sets, nset, 4096);
        else if (ARG("--frames")) {
            std::vector<char> s(v, v + strlen(v) + 1);
            for (char *t = strtok(s.data(), ","); t; t = strtok(nullptr, ",")) frame_ms.push_back((uint64_t)(atof(t) * 1000.0 + 0.5));
        }
        else if (ARG("--frame-prefix")) frame_prefix = v;
        else if (!strcmp(o, "--no-autostart")) autostart = 0;
        else if (!strcmp(o, "--fast")) fast = 1;
        else if (o[0] == '+') ;                                      // verilator plusargs
        else { fprintf(stderr, "unknown/incomplete option %s\n", o); return 2; }
#undef ARG
    }
    if (autostart) presses.push_back({1, btn_mask("A")});
    std::stable_sort(presses.begin(), presses.end(), [](const Press &a, const Press &b) { return a.tick < b.tick; });
    for (auto &p : presses) if (p.tick == 0) { fprintf(stderr, "presses at tick 0 are not supported\n"); return 2; }

    FILE *out = out_path ? fopen(out_path, "wb") : nullptr;
    if (out_path && !out) { perror(out_path); return 2; }

    Vcpu_sys *d = new Vcpu_sys;
    d->cfg_mode = mode; d->cfg_density = density; d->cfg_bright = bright; d->cfg_reverb = reverb;
    d->cfg_visuals = 0; d->cfg_seed_lock = 1; d->cfg_seed = seed; d->cfg_sleep = sleep_sel;
    d->fw_we = 0; d->fw_addr = 0; d->fw_data = 0;
    d->cmd_reseed = 0; d->settings_seq = 0; d->rtc_epoch = 0; d->btn_edge = 0; d->frame_tick = 0;
    d->mem_ack = 0; d->mem_rd = 0;
    d->vclk = 0; d->vsync = 0; d->vx = 0x3FF; d->vy = 0x3FF; d->diag_frame = 0;

    // video timing as core_top: 400 x 512 pixel clocks per frame at 12.288 MHz, visible
    // 320 x 288 from (10, 10); coordinates go in three clocks ahead of the colour
    int xc = 0, yc = 0;
    double vacc = 0.0;
    std::vector<uint8_t> img(320 * 288 * 3);
    size_t fi = 0;
    int capturing = 0;
    auto vclk_edge = [&]() {
        d->vclk = 1;
        d->vsync = xc == 0 && yc == 0;
        int lx = xc + 3 - 10, ly = yc - 10;
        d->vx = (uint32_t)(lx & 0x3FF);
        d->vy = (uint32_t)(ly & 0x3FF);
        d->eval();
        // after this edge, rgb belongs to the pixel whose coordinates went in three edges ago:
        // visible (xc - 10, yc - 10)
        int ox = xc - 10, oy = yc - 10;
        if (capturing && ox >= 0 && ox < 320 && oy >= 0 && oy < 288) {
            uint32_t c = d->rgb;
            uint8_t *q = &img[(oy * 320 + ox) * 3];
            q[0] = (uint8_t)(c >> 16); q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)c;
        }
        if (++xc == 400) {
            xc = 0;
            if (++yc == 512) {
                yc = 0;
                if (capturing) {
                    char path[512];
                    snprintf(path, sizeof path, "%s_%06llu.ppm", frame_prefix, (unsigned long long)frame_ms[fi]);
                    FILE *f = fopen(path, "wb");
                    if (f) { fprintf(f, "P6\n320 288\n255\n"); fwrite(img.data(), 1, img.size(), f); fclose(f); }
                    capturing = 0;
                    fi++;
                }
                if (fi < frame_ms.size() && d->tick >= frame_ms[fi]) capturing = 1;
            }
        }
        d->vclk = 0;
        d->eval();
    };

    std::vector<uint16_t> sram(131072);
    uint64_t rs = 0x9E3779B97F4A7C15ull;
    for (auto &w : sram) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; w = (uint16_t)rs; }
    int ack_cnt = -1;
    uint16_t ack_data = 0;

    size_t pi = 0;
    int si = 0;
    uint32_t ctrl[CR_NUM_REGS] = {0};
    ctrl[CR_MODE] = mode; ctrl[CR_DENSITY] = density; ctrl[CR_BRIGHT] = bright; ctrl[CR_REVERB] = reverb;
    ctrl[CR_SEED_LOCK] = 1; ctrl[CR_SEED] = seed; ctrl[CR_SLEEP] = sleep_sel;
    uint64_t cyc = 0, samples = 0, want = dur_ms * 48;
    double frame_acc = 0.0;
    clock_t t0 = clock();
    d->rst = 1;
    for (int k = 0; k < 8; k++) { d->clk = 0; d->eval(); d->clk = 1; d->eval(); }
    d->rst = 0;
    while (samples < want || fi < frame_ms.size()) {
        if (samples >= want && cyc > (uint64_t)(dur_ms + 200) * 74250ull) break;
        // inputs for this cycle (set before the rising edge)
        uint32_t edge = 0;
        while (pi < presses.size() && presses[pi].tick - 1 <= d->tick) edge |= presses[pi++].mask;
        d->btn_edge = edge;
        d->cmd_reseed = 0;
        while (si < nset && (sets[si].reg == SET_RESEED ? sets[si].tick - 1 : sets[si].tick) <= d->tick) {
            const setscript_t *q = &sets[si++];
            if (q->reg == SET_RESEED) { d->cmd_reseed = 1; continue; }
            if (ctrl[q->reg] != q->val) { ctrl[q->reg] = q->val; ctrl[CR_SETTINGS_SEQ]++; }
            d->cfg_mode = ctrl[CR_MODE]; d->cfg_density = ctrl[CR_DENSITY]; d->cfg_bright = ctrl[CR_BRIGHT];
            d->cfg_reverb = ctrl[CR_REVERB]; d->cfg_visuals = ctrl[CR_VISUALS]; d->cfg_seed_lock = ctrl[CR_SEED_LOCK];
            d->cfg_seed = ctrl[CR_SEED]; d->cfg_sleep = ctrl[CR_SLEEP]; d->settings_seq = ctrl[CR_SETTINGS_SEQ];
        }
        if (fast) d->frame_tick = (cyc & 1u) && d->fifo_level > 16;
        else { frame_acc += 1.0; d->frame_tick = 0; if (frame_acc >= 1546.875) { frame_acc -= 1546.875; d->frame_tick = 1; } }
        d->mem_ack = 0;
        if (ack_cnt == 0) { d->mem_ack = 1; d->mem_rd = ack_data; ack_cnt = -1; }
        else if (ack_cnt > 0) ack_cnt--;

        d->clk = 0; d->eval();
        // sample the request on the edge, like a registered SRAM controller
        int req = d->mem_req, we = d->mem_we;
        uint32_t a = d->mem_addr;
        uint16_t wd = d->mem_wd;
        int sv = d->smp_valid;
        uint16_t sl = d->smp_l, sr = d->smp_r;
        d->clk = 1; d->eval();
        cyc++;
        vacc += 12.288 / 74.25;                        // pixel clocks per system clock
        while (vacc >= 1.0) { vacc -= 1.0; vclk_edge(); }
        if (req) {
            if (ack_cnt >= 0) { fprintf(stderr, "SRAM request while busy at cycle %llu\n", (unsigned long long)cyc); return 1; }
            if (we) sram[a] = wd; else ack_data = sram[a];
            ack_cnt = (we ? wr_lat : rd_lat) - 2;
            if (ack_cnt < 0) ack_cnt = 0;
        }
        if (sv && samples < want) {
            if (out) { int16_t s2[2] = {(int16_t)sl, (int16_t)sr}; fwrite(s2, sizeof s2, 1, out); }
            samples++;
        }
        if (d->trap) { fprintf(stderr, "CPU trap at cycle %llu, tick %llu\n", (unsigned long long)cyc, (unsigned long long)d->tick); return 1; }
        if (d->bus_err) { fprintf(stderr, "bus error at cycle %llu, tick %llu\n", (unsigned long long)cyc, (unsigned long long)d->tick); return 1; }
        if (cyc > 200000000ull && d->tick == 0) { fprintf(stderr, "no tick after 200M cycles\n"); return 1; }
    }
    if (out) fclose(out);
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("sim_sys: %llu samples (%.1f s audio), %llu ticks, %llu cycles in %.0f s (%.2f MHz); "
           "CPU cycles/tick max %u; FIFO min %u, underruns %u%s\n",
           (unsigned long long)samples, samples / 48000.0, (unsigned long long)d->tick, (unsigned long long)cyc,
           secs, cyc / secs / 1e6, d->tick_cycles_max, d->fifo_min, d->underruns, fast ? " (fast pacing)" : "");
    delete d;
    return 0;
}
