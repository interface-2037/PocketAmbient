// voice_engine.sv - the voice engine for the Cyclone V (Phase 2b, modulo-scheduled).
// Bit-exact with voice_process() in model/synth_model.c and with voice_engine_ref.sv,
// the statement-by-statement reference it was derived from.
//
// Pipeline
//  - A new active voice enters every II = 10 cycles; a voice is in flight for 49 cycles,
//    so up to 5 voices are processed at once. All voices active: ~360 cycles per sample
//    (budget: 1,546 at 74.25 MHz). Frozen voices (IDLE, gate low, no trigger) are skipped.
//  - Offsets t0..t48 of a voice map to stage k = t / 10 and phase t % 10. Every value
//    that lives across a stage boundary is held in a 5-entry array X[k] that shifts at the
//    end of phase 9; ops at phase 9 write X[k+1] directly.
//  - Shared units, each used at most once per phase (modulo reservation, see table below):
//      M0..M3  33x33 multipliers, issue at t -> product at t+3 (ve_mul, 2 DSPs each)
//      SA, SB  sine reads, WT wavetable reads (saw+tri, ports A/B), KS pluck-line read:
//              issue at t -> interpolated value at t+4 (ve_rom2 + ve_interp)
//      register / state RAMs: rows read at t0..t3 (data at t+1); state rows written at
//              t16, t17, t38
//  - Different algorithms share a unit slot only at the same offset (they are exclusive
//    within a voice; voices in flight are 10 cycles apart).
//
//   phase  M0          M1          M2                     M3
//     0    FB1   t10   O2    t40   noise C       t10      noise C      t10
//     1    INC0  t11   INC1  t11   SUB/FM idx/KS damp t11 noise D      t11
//     2    MENV  t2    ENV   t2    F6            t32      -
//     3    CUT   t3    AMP   t3    VIBD          t3       F3           t23
//     4    GL0   t4    GL1   t4    O3 (L)        t44      O4 (R)       t44
//     5    QB1   t5    O5    t45   O6            t45      -
//     6    FENV  t6    PENV  t6    O1            t36      noise E      t16
//     7    F4    t27   F5    t27   FM depth      t7       -
//     8    VIBM  t8    KS brt t8   noise A       t8       noise A      t8
//     9    PAD   t19   PAD   t19   noise B       t9       noise B      t9
//   SA: t4 vibrato, t5 osc 1, t6 osc 0, t17 FM carrier.  SB: t5 / t6 pan gains.
//   WT: t15.  KS: read t5, write t15.
`default_nettype none

module voice_engine (
    input  wire               clk,
    input  wire               rst,
    // register writes (between samples, while !busy): addr = voice*32 + reg
    input  wire               wr_en,
    input  wire        [9:0]  wr_addr,
    input  wire        [31:0] wr_data,
    // one sample
    input  wire               start,
    output reg                done,
    output reg  signed [31:0] bus_l,
    output reg  signed [31:0] bus_r,
    output reg  signed [31:0] bus_rev,
    output reg  signed [31:0] bus_echo,
    // VR_STATUS read (2-cycle latency)
    input  wire        [4:0]  st_voice,
    output wire        [31:0] st_status,
    // RAMs cleared after reset (4,096 cycles); start and writes are ignored until then
    output reg                ready
);
    // ------------------------------------------------------------- constants
    localparam signed [31:0] ONE30 = 32'sd1073741824;
    localparam signed [31:0] ATK_TARGET = 32'sd1342177280;
    localparam signed [31:0] ENV_FLOOR = 32'sd64;
    localparam signed [31:0] SVF_F_MAX = 32'sd52429;
    localparam signed [31:0] SVF_Q_MIN = 32'sd1024;
    localparam signed [31:0] SVF_Q_MAX = 32'sd32767;
    localparam signed [47:0] Q23_MAX = 48'sd8388607;
    localparam signed [47:0] SVF_LIM = 48'sd134217728;
    localparam signed [47:0] S32_MAX = 48'sd2147483647;
    localparam [1:0] ENV_IDLE = 2'd0, ENV_ATTACK = 2'd1, ENV_DECAY = 2'd2, ENV_RELEASE = 2'd3;

    // -------------------------------------------------------------- helpers
    // Filter and bus sums are evaluated at 48 bits: every term is bounded far below 2^46
    // (|f| <= 52429, |q| < 2^15, |band|,|low| <= 2^27, |a| <= 2^24, gains <= 2^23).
    function automatic signed [31:0] sat(input signed [47:0] x, input signed [47:0] lim);
        if (x > lim) sat = lim[31:0];
        else if (x < -lim) sat = -lim[31:0];
        else sat = x[31:0];
    endfunction
    function automatic signed [31:0] lo32(input signed [65:0] x); lo32 = x[31:0]; endfunction
    function automatic signed [47:0] lo48(input signed [65:0] x); lo48 = x[47:0]; endfunction
    function automatic signed [47:0] e48(input signed [31:0] x);  e48 = x;         endfunction
    function automatic signed [32:0] sx(input signed [31:0] x);   sx = x;          endfunction
    function automatic signed [32:0] ux(input [31:0] x);          ux = {1'b0, x};  endfunction
    function automatic [31:0] rate_coef(input [31:0] code);
        rate_coef = {19'd1, code[11:0]} << code[15:12];
    endfunction
    function automatic [31:0] xs32(input [31:0] x0);
        reg [31:0] x;
        begin
            x = x0 ^ (x0 << 13);
            x = x ^ (x >> 17);
            xs32 = x ^ (x << 5);
        end
    endfunction
    function automatic [3:0] mip_level(input [31:0] inc);
        integer k, msb;
        begin
            msb = 0;
            for (k = 0; k < 32; k = k + 1) if (inc[k]) msb = k;
            if (inc < 32'h00100000) mip_level = 4'd0;
            else mip_level = (msb - 20 > 9) ? 4'd9 : (msb - 20);
        end
    endfunction
    function automatic signed [15:0] q23_to_16(input signed [31:0] x);
        reg signed [47:0] t;
        begin
            t = (e48(x) + 48'sd128) >>> 8;
            q23_to_16 = sat(t, 48'sd32767);
        end
    endfunction
    // register index -> {row, bank}: rows are ordered by when the schedule needs them
    function automatic [4:0] rmap(input [4:0] r);
        case (r)
            5'd0:  rmap = {2'd0, 3'd0};   // GATE
            5'd1:  rmap = {2'd0, 3'd1};   // ALGO
            5'd6:  rmap = {2'd0, 3'd2};   // MOD_DECAY
            5'd7:  rmap = {2'd0, 3'd3};   // ATTACK
            5'd8:  rmap = {2'd0, 3'd4};   // DECAY
            5'd9:  rmap = {2'd0, 3'd5};   // SUSTAIN
            5'd10: rmap = {2'd0, 3'd6};   // RELEASE
            5'd29: rmap = {2'd0, 3'd7};   // SLEW
            5'd11: rmap = {2'd1, 3'd0};   // CUTOFF
            5'd14: rmap = {2'd1, 3'd1};   // AMP
            5'd15: rmap = {2'd1, 3'd2};   // PAN
            5'd4:  rmap = {2'd1, 3'd3};   // FM_INDEX
            5'd25: rmap = {2'd1, 3'd4};   // VIB_RATE
            5'd26: rmap = {2'd1, 3'd5};   // VIB_DEPTH
            5'd27: rmap = {2'd1, 3'd6};   // VIB_DELAY
            5'd28: rmap = {2'd1, 3'd7};   // GLIDE
            5'd2:  rmap = {2'd2, 3'd0};   // PINC0
            5'd3:  rmap = {2'd2, 3'd1};   // PINC1
            5'd12: rmap = {2'd2, 3'd2};   // RESO
            5'd30: rmap = {2'd2, 3'd3};   // FENV
            5'd24: rmap = {2'd2, 3'd4};   // PENV
            5'd5:  rmap = {2'd2, 3'd5};   // FM_SUS
            5'd21: rmap = {2'd2, 3'd6};   // KS_LEN
            5'd23: rmap = {2'd2, 3'd7};   // KS_BRIGHT
            5'd22: rmap = {2'd3, 3'd0};   // KS_DAMP
            5'd20: rmap = {2'd3, 3'd1};   // DUST_RATE
            5'd18: rmap = {2'd3, 3'd2};   // WAVE
            5'd19: rmap = {2'd3, 3'd3};   // NOISE
            5'd13: rmap = {2'd3, 3'd4};   // FMODE
            5'd16: rmap = {2'd3, 3'd5};   // SEND_REV
            5'd17: rmap = {2'd3, 3'd6};   // SEND_ECHO
            default: rmap = {2'd3, 3'd7}; // STATUS (unused)
        endcase
    endfunction
    // sm_sin_q23 read: ROM addresses and interpolation context {negate, odd, frac}
    function automatic [33:0] sin_rd(input [31:0] ph);   // {ctx[9:0], even addr[11:0], odd addr[11:0]}
        reg [20:0] p;
        reg [12:0] i, i1;
        begin
            p = {1'b0, ph[29:10]};
            if (ph[30]) p = 21'h100000 - p;
            i = p[20:8];
            i1 = i + 13'd1;
            sin_rd = {ph[31], i[0], p[7:0], i1[12:1], i[12:1]};
        end
    endfunction

    // ------------------------------------------------------------- control
    reg        busy;
    reg  [3:0] ph;                             // phase within the 10-cycle initiation interval
    reg [31:0] mask;                           // active voices not yet entered
    reg  [4:0] vnext;
    reg        have_next;
    reg [31:0] trig_pending, prev_gate, gate_ff;
    reg  [1:0] env_state [0:31];
    reg signed [31:0] acc_l, acc_r, acc_rev, acc_echo;

    // reset clear: registers, voice state (with sm_init's noise seeds), status, pluck lines
    reg        clr;
    reg [11:0] clr_a;
    reg [31:0] seed;
    wire       clr_lo = clr && clr_a[11:7] == 5'd0;      // register / state RAMs: first 128 cycles

    integer n;
    initial begin
        clr = 1'b1; clr_a = 12'd0; ready = 1'b0; seed = 32'h9E3779B9;
        gate_ff = 32'd0; trig_pending = 32'd0; prev_gate = 32'd0;
        for (n = 0; n < 32; n = n + 1) env_state[n] = ENV_IDLE;
    end

    // ------------------------------------------- per-stage (carried) values
    reg  [4:0] v [0:4];
    reg  [4:0] valid;
    reg        trg [0:4], tid [0:4];
    reg  [1:0] est0 [0:4], estn [0:4];
    reg        a_pad [0:4], a_fm [0:4], a_noise [0:4], a_ks [0:4], a_sub [0:4], a_pulse [0:4];
    reg        pink [0:4], dusty [0:4], bypass [0:4], vib_on [0:4];
    reg  [1:0] fsel [0:4];
    reg [15:0] dust16 [0:4], ksdamp16 [0:4], fmsus16 [0:4], srev16 [0:4], secho16 [0:4];
    reg [31:0] wave_c [0:4];
    reg signed [31:0] envn [0:4], menv0 [0:4], menvn [0:4], cutn [0:4], ampn [0:4], nln [0:4], nl2n [0:4];
    reg signed [31:0] pann [0:4], fmin [0:4], vibd0 [0:4], vdn [0:4], dust0 [0:4], dustn [0:4];
    reg [31:0] noisen [0:4], vibphn [0:4], incs0p [0:4], incs1p [0:4], incs0n [0:4], incs1n [0:4];
    reg [31:0] pph0 [0:4], pph1 [0:4], ph0p [0:4], ph1p [0:4], ph0f [0:4], ph1f [0:4];
    reg [31:0] panu [0:4], inc0a [0:4], inc0 [0:4], inc1 [0:4], phB [0:4], fmph [0:4];
    reg signed [31:0] pk0p [0:4], pk1p [0:4], pk2p [0:4], pk0n [0:4], pk1n [0:4], pk2n [0:4];
    reg [10:0] ksw [0:4], kslo [0:4];
    reg  [7:0] ksfr [0:4];
    reg [15:0] ksx0 [0:4], kswd [0:4];
    reg signed [31:0] ksprev0 [0:4], kslp0 [0:4], ksy [0:4], kslpn [0:4], avg [0:4];
    reg signed [31:0] slow0 [0:4], sband0 [0:4], q [0:4], qb1 [0:4], f [0:4];
    reg signed [31:0] sp1 [0:4], sp0 [0:4], idx [0:4], gr [0:4], gl [0:4], dfm [0:4];
    reg signed [31:0] wks [0:4], w1 [0:4], w2 [0:4], dsh [0:4], wsel [0:4];
    reg signed [31:0] x [0:4], s0w [0:4], s1w [0:4], low1 [0:4], band1 [0:4], low2 [0:4], qb2 [0:4];
    reg signed [31:0] high2 [0:4], xs [0:4], slowo [0:4], sbando [0:4], e [0:4], a [0:4];

    // values captured from the register / state rows (used inside stage 0 only)
    reg [31:0] r_algo, r_moddecay, r_attack, r_decay, r_sustain, r_release, r_slew;
    reg [31:0] r_cutoff, r_amp, r_pan, r_fmidx, r_vibrate, r_vibdepth, r_vibdelay, r_glide;
    reg [31:0] r_pinc0, r_pinc1, r_reso, r_fenv, r_penv, r_fmsus, r_kslen, r_ksbright;
    reg signed [31:0] s_env, s_menv, s_cut, s_amp, s_nlp, s_nlp2, s_pan, s_fmi, s_vibd, s_dust;
    reg signed [31:0] s_pk0, s_pk1, s_pk2, s_ksprev, s_kslp, s_low, s_band;
    reg [31:0] s_noise, s_vibph, s_incs0, s_incs1, s_ph0, s_ph1, s_kspack;

    // ------------------------------------------------------------- multipliers
    reg  signed [32:0] ma0, mb0, ma1, mb1, ma2, mb2, ma3, mb3;
    wire signed [65:0] p0, p1, p2, p3;
    ve_mul u_m0 (.clk(clk), .a(ma0), .b(mb0), .p(p0));
    ve_mul u_m1 (.clk(clk), .a(ma1), .b(mb1), .p(p1));
    ve_mul u_m2 (.clk(clk), .a(ma2), .b(mb2), .p(p2));
    ve_mul u_m3 (.clk(clk), .a(ma3), .b(mb3), .p(p3));

    // ------------------------------------------------------------- RAMs
    wire  [1:0] ld_row  = (ph == 4'd1) ? 2'd1 : (ph == 4'd2) ? 2'd2 : (ph == 4'd3) ? 2'd3 : 2'd0;
    wire  [6:0] ld_addr = {v[0], ld_row};
    wire  [4:0] wmap    = rmap(wr_addr[4:0]);
    wire        host_we = !busy && ready && wr_en && wr_addr[4:0] != 5'd31;
    wire [31:0] reg_wd  = (wr_addr[4:0] == 5'd0) ? {31'd0, wr_data[0]} : wr_data;
    // state rows: 0 written at t16 (stage 1, phase 6), 1 at t17 (stage 1, phase 7),
    //             2 at t38 (stage 3, phase 8)
    wire        sv_we   = busy && ((ph == 4'd6 && valid[1]) || (ph == 4'd7 && valid[1]) ||
                                   (ph == 4'd8 && valid[3]));
    wire  [6:0] sv_wa   = (ph == 4'd6) ? {v[1], 2'd0} : (ph == 4'd7) ? {v[1], 2'd1} : {v[3], 2'd2};
    reg [255:0] sv_wd;                                           // bank 7 .. bank 0
    always @* begin
        case (ph)
            4'd6: sv_wd = {pann[1], nl2n[1], nln[1], noisen[1], ampn[1], cutn[1], menvn[1], envn[1]};
            4'd7: sv_wd = {dustn[1], ph1f[1], ph0f[1], incs1n[1], incs0n[1], vibphn[1], vdn[1], fmin[1]};
            default: sv_wd = {sbando[3], slowo[3], kslpn[3], ksy[3], ksx0[3], 5'd0, ksw[3],
                              pk2n[3], pk1n[3], pk0n[3]};
        endcase
    end
    wire [255:0] reg_q, sv_q;
    genvar gb;
    generate for (gb = 0; gb < 8; gb = gb + 1) begin : bank
        ve_sdp #(.W(32), .AW(7)) u_reg (.clk(clk), .we(clr_lo || (host_we && wmap[2:0] == gb)),
            .wa(clr ? clr_a[6:0] : {wr_addr[9:5], wmap[4:3]}), .wd(clr ? 32'd0 : reg_wd),
            .ra(ld_addr), .rd(reg_q[gb * 32 +: 32]));
        ve_sdp #(.W(32), .AW(7), .SEEDS(gb == 4)) u_state (.clk(clk), .we(clr_lo || sv_we),
            .wa(clr ? clr_a[6:0] : sv_wa),
            .wd(clr ? ((gb == 4 && clr_a[1:0] == 2'd0) ? (seed | 32'd1) : 32'd0) : sv_wd[gb * 32 +: 32]),
            .ra(ld_addr), .rd(sv_q[gb * 32 +: 32]));
    end endgenerate
    wire [31:0] status_wd = {(envn[0][31:14] > 18'h0FFFF) ? 16'hFFFF : envn[0][29:14], 14'd0, estn[0]};
    ve_sdp #(.W(32), .AW(5), .OREG(1)) u_status (.clk(clk), .we(clr || (busy && ph == 4'd6 && valid[0])),
        .wa(clr ? clr_a[4:0] : v[0]), .wd(clr ? 32'd0 : status_wd), .ra(st_voice), .rd(st_status));

    // ------------------------------------------------------------- table reads
    // sine A: t4 vibrato LFO, t5 oscillator 1, t6 oscillator 0, t17 FM carrier
    wire [31:0] sa_ph = (ph == 4'd4) ? vibphn[0] : (ph == 4'd5) ? ph1p[0] : (ph == 4'd6) ? ph0p[0] : fmph[1];
    // sine B: t5 right gain sin(pan), t6 left gain sin(32768 - pan)
    wire [31:0] sb_ph = (ph == 4'd5) ? (panu[0] << 15) : ((32'd32768 - panu[0]) << 15);
    wire [33:0] sa_r = sin_rd(sa_ph), sb_r = sin_rd(sb_ph);
    wire [15:0] sinA_e, sinA_o, sinB_e, sinB_o;
    wire signed [31:0] sinA, sinB;
    ve_rom2 #(.AW(12), .DEPTH(2049), .FILE("core/synth/sine_e.hex")) u_sin_e (.clk(clk),
        .aa(sa_r[23:12]), .ab(sb_r[23:12]), .qa(sinA_e), .qb(sinB_e));
    ve_rom2 #(.AW(12), .DEPTH(2049), .FILE("core/synth/sine_o.hex")) u_sin_o (.clk(clk),
        .aa(sa_r[11:0]), .ab(sb_r[11:0]), .qa(sinA_o), .qb(sinB_o));
    ve_interp u_isa (.clk(clk), .ctx(sa_r[33:24]), .qe(sinA_e), .qo(sinA_o), .val(sinA));
    ve_interp u_isb (.clk(clk), .ctx(sb_r[33:24]), .qe(sinB_e), .qo(sinB_o), .val(sinB));
    // wavetables at t15 (stage 1): port A = phase 0 at mip level l0; port B = phase 1 (PAD)
    // or phase 0 + pulse width (PULSE)
    wire  [3:0] l0 = mip_level(inc0[1]), l1 = mip_level(inc1[1]);
    wire  [3:0] lB = a_pad[1] ? l1 : l0;
    wire  [9:0] iA = ph0p[1][31:22], iA1 = iA + 10'd1;
    wire  [9:0] iB = phB[1][31:22],  iB1 = iB + 10'd1;
    wire  [9:0] wA_ctx = {1'b0, iA[0], ph0p[1][21:14]}, wB_ctx = {1'b0, iB[0], phB[1][21:14]};
    wire [15:0] sawA_e, sawA_o, sawB_e, sawB_o, triA_e, triA_o, triB_e, triB_o;
    wire signed [31:0] sawA, sawB, triA, triB;
    ve_rom2 #(.AW(13), .DEPTH(5120), .FILE("core/synth/wt_saw_e.hex")) u_saw_e (.clk(clk),
        .aa({l0, iA1[9:1]}), .ab({lB, iB1[9:1]}), .qa(sawA_e), .qb(sawB_e));
    ve_rom2 #(.AW(13), .DEPTH(5120), .FILE("core/synth/wt_saw_o.hex")) u_saw_o (.clk(clk),
        .aa({l0, iA[9:1]}), .ab({lB, iB[9:1]}), .qa(sawA_o), .qb(sawB_o));
    ve_rom2 #(.AW(13), .DEPTH(5120), .FILE("core/synth/wt_tri_e.hex")) u_tri_e (.clk(clk),
        .aa({l0, iA1[9:1]}), .ab({lB, iB1[9:1]}), .qa(triA_e), .qb(triB_e));
    ve_rom2 #(.AW(13), .DEPTH(5120), .FILE("core/synth/wt_tri_o.hex")) u_tri_o (.clk(clk),
        .aa({l0, iA[9:1]}), .ab({lB, iB[9:1]}), .qa(triA_o), .qb(triB_o));
    ve_interp u_isawA (.clk(clk), .ctx(wA_ctx), .qe(sawA_e), .qo(sawA_o), .val(sawA));
    ve_interp u_isawB (.clk(clk), .ctx(wB_ctx), .qe(sawB_e), .qo(sawB_o), .val(sawB));
    ve_interp u_itriA (.clk(clk), .ctx(wA_ctx), .qe(triA_e), .qo(triA_o), .val(triA));
    ve_interp u_itriB (.clk(clk), .ctx(wB_ctx), .qe(triB_e), .qo(triB_o), .val(triB));
    // pluck delay lines (4 voices x 2048, even/odd banks): read T[lo+1], T[lo] at t5,
    // write at t15. y = T[lo+1]*256 + (T[lo] - T[lo+1])*fr, so "odd" selects ~lo[0].
    wire [10:0] kslo1 = kslo[0] + 11'd1;
    wire        ks_we = busy && ph == 4'd5 && valid[1] && a_ks[1];
    wire [15:0] ksq_e, ksq_o;
    wire signed [31:0] ksval;
    ve_sdp #(.W(16), .AW(12), .OREG(1)) u_ks_e (.clk(clk), .we(clr || (ks_we && !ksw[1][0])),
        .wa(clr ? clr_a : {v[1][1:0], ksw[1][10:1]}), .wd(clr ? 16'd0 : kswd[1]),
        .ra({v[0][1:0], kslo1[10:1]}), .rd(ksq_e));
    ve_sdp #(.W(16), .AW(12), .OREG(1)) u_ks_o (.clk(clk), .we(clr || (ks_we && ksw[1][0])),
        .wa(clr ? clr_a : {v[1][1:0], ksw[1][10:1]}), .wd(clr ? 16'd0 : kswd[1]),
        .ra({v[0][1:0], kslo[0][10:1]}), .rd(ksq_o));
    ve_interp u_iks (.clk(clk), .ctx({1'b0, ~kslo[0][0], ksfr[0]}), .qe(ksq_e), .qo(ksq_o), .val(ksval));

    // ------------------------------------------------------------- sequencer
    reg signed [31:0] t, u, w0, nl0, nl, nl20, nl2, en, vd, mm;
    reg signed [47:0] t48;
    reg [31:0] n1, nz, ut;
    reg  [1:0] es;
    reg        trig_c;
    reg  [1:0] est_c;

    `define SH(X) X[4] <= X[3]; X[3] <= X[2]; X[2] <= X[1]; X[1] <= X[0];

    always @(posedge clk) begin
        done <= 1'b0;
        if (rst) begin
            busy <= 1'b0;
            valid <= 5'd0;
            trig_pending <= 32'd0;
            prev_gate <= 32'd0;
            gate_ff <= 32'd0;
            for (n = 0; n < 32; n = n + 1) env_state[n] <= ENV_IDLE;
            clr <= 1'b1;
            clr_a <= 12'd0;
            seed <= 32'h9E3779B9;
            ready <= 1'b0;
        end else if (clr) begin
            clr_a <= clr_a + 12'd1;
            if (clr_a[1:0] == 2'd3) seed <= seed + 32'h9E3779B9;    // next voice's seed
            if (clr_a == 12'hFFF) begin clr <= 1'b0; ready <= 1'b1; end
        end else if (!busy) begin
            if (host_we && wr_addr[4:0] == 5'd0) begin
                gate_ff[wr_addr[9:5]] <= wr_data[0];
                if (wr_data[1]) trig_pending[wr_addr[9:5]] <= 1'b1;
            end
            if (start) begin
                busy <= 1'b1;
                ph <= 4'd8;
                valid <= 5'd0;
                acc_l <= 0; acc_r <= 0; acc_rev <= 0; acc_echo <= 0;
                for (n = 0; n < 32; n = n + 1)
                    if (env_state[n] == ENV_IDLE && !gate_ff[n] && !trig_pending[n]) begin
                        mask[n] <= 1'b0;
                        prev_gate[n] <= 1'b0;          // frozen this sample
                    end else mask[n] <= 1'b1;
            end
        end else begin
            ph <= (ph == 4'd9) ? 4'd0 : ph + 4'd1;
            case (ph)
            // =========================================================== phase 0
            4'd0: begin
                // t10: f * band (filter pass 1); noise pair C
                ma0 <= sx(f[1]); mb0 <= sx(sband0[1]);
                ma2 <= sx(pk2p[1]); mb2 <= 33'sd37356;
                ma3 <= sx(w2[1]);   mb3 <= 33'sd68989;
                idx[1] <= $signed({16'd0, fmsus16[1]}) + lo32(p2 >>> 30);      // FM index
                sp0[1] <= sinA;
                gl[1] <= sinB;
                if (!(a_pad[1] || a_fm[1] || a_noise[1] || a_ks[1] || a_sub[1] || a_pulse[1])) x[1] <= 32'sd0;
                // t30: low, pass 2; q * band
                low2[3] <= sat(e48(low1[3]) + lo48(p0 >>> 16), SVF_LIM);
                qb2[3] <= lo32(p1 >>> 14);
                // t40: e * amp
                ma1 <= sx(e[4]); mb1 <= sx(ampn[4] >>> 8);
            end
            // =========================================================== phase 1
            4'd1: begin
                // t1: register row 0 / state row 0
                r_algo <= reg_q[63:32];      r_moddecay <= reg_q[95:64];  r_attack <= reg_q[127:96];
                r_decay <= reg_q[159:128];   r_sustain <= reg_q[191:160]; r_release <= reg_q[223:192];
                r_slew <= reg_q[255:224];
                s_env <= sv_q[31:0];    s_menv <= sv_q[63:32];  s_cut <= sv_q[95:64];   s_amp <= sv_q[127:96];
                s_noise <= sv_q[159:128]; s_nlp <= sv_q[191:160]; s_nlp2 <= sv_q[223:192]; s_pan <= sv_q[255:224];
                // t11: increments * vibrato multiplier; source multiply; noise D
                mm = lo32(p0 >>> 23);
                ma0 <= ux(inc0a[1]);  mb0 <= sx(mm);
                ma1 <= ux(incs1n[1]); mb1 <= sx(mm);
                if (a_sub[1])      begin ma2 <= sx(sp1[1]); mb2 <= sx(wave_c[1]); end
                else if (a_fm[1])  begin ma2 <= sx(sp1[1]); mb2 <= sx(idx[1]); end
                else               begin ma2 <= sx(avg[1]); mb2 <= ux({16'd0, ksdamp16[1]}); end
                ma3 <= sx(w2[1]); mb3 <= 33'sd12111;
                pk0n[1] <= (a_noise[1] && pink[1]) ? lo32((p2 + p3) >>> 16) : pk0p[1];
                kslpn[1] <= (a_ks[1] && ksx0[1] != 16'd0) ? kslp0[1] + lo32(p1 >>> 16) : kslp0[1];
                // t21: FM carrier
                if (a_fm[2]) x[2] <= sinA;
                // t31: high, pass 2
                high2[3] <= sat(e48(x[3]) - e48(low2[3]) - e48(qb2[3]), SVF_LIM);
            end
            // =========================================================== phase 2
            4'd2: begin
                // t2: register row 1 / state row 1
                r_cutoff <= reg_q[31:0];    r_amp <= reg_q[63:32];       r_pan <= reg_q[95:64];
                r_fmidx <= reg_q[127:96];   r_vibrate <= reg_q[159:128]; r_vibdepth <= reg_q[191:160];
                r_vibdelay <= reg_q[223:192]; r_glide <= reg_q[255:224];
                s_fmi <= sv_q[31:0];    s_vibd <= sv_q[63:32];   s_vibph <= sv_q[95:64]; s_incs0 <= sv_q[127:96];
                s_incs1 <= sv_q[159:128]; s_ph0 <= sv_q[191:160]; s_ph1 <= sv_q[223:192]; s_dust <= sv_q[255:224];
                // algorithm flags
                a_pad[0]   <= r_algo == 32'd0;
                a_fm[0]    <= r_algo == 32'd1;
                a_noise[0] <= r_algo == 32'd2;
                a_ks[0]    <= r_algo == 32'd3 && v[0] >= 5'd28;
                a_sub[0]   <= r_algo == 32'd4;
                a_pulse[0] <= r_algo == 32'd5;
                // mod envelope and amplitude envelope
                t = trg[0] ? ONE30 : s_menv;
                menv0[0] <= t;
                ma0 <= sx(-t); mb0 <= ux(rate_coef(r_moddecay));
                case (est0[0])
                    ENV_ATTACK:  begin ma1 <= sx(ATK_TARGET - s_env); mb1 <= ux(rate_coef(r_attack)); end
                    ENV_DECAY:   begin
                        u = ((r_sustain > 32'd32768) ? 32'd32768 : r_sustain) << 15;
                        ma1 <= sx(u - s_env); mb1 <= ux(rate_coef(r_decay));
                    end
                    ENV_RELEASE: begin ma1 <= sx(-s_env); mb1 <= ux(rate_coef(r_release)); end
                    default:     begin ma1 <= 33'sd0; mb1 <= 33'sd0; end
                endcase
                // noise: trigger phases (PAD), white noise + first high-pass (NOISE), pluck noise
                n1 = s_noise;
                nl0 = tid[0] ? 32'sd0 : s_nlp;
                nln[0] <= nl0;
                if (tid[0] && r_algo == 32'd0) begin
                    n1 = xs32(n1); pph0[0] <= n1;
                    n1 = xs32(n1); pph1[0] <= n1;
                    noisen[0] <= n1;
                end else if (r_algo == 32'd2) begin
                    nz = xs32(n1);
                    noisen[0] <= nz;
                    w0 = $signed(nz) >>> 8;
                    nl = nl0 + ((w0 - nl0) >>> 6);
                    nln[0] <= nl;
                    w1[0] <= w0 - nl;
                end else if (r_algo == 32'd3 && v[0] >= 5'd28) begin
                    nz = xs32(n1);
                    noisen[0] <= nz;
                    wks[0] <= $signed(nz) >>> 8;
                end else noisen[0] <= n1;
                // t32: f * high, pass 2
                ma2 <= sx(f[3]); mb2 <= sx(high2[3]);
                // t12: noise pair B
                pk1n[1] <= (a_noise[1] && pink[1]) ? lo32((p2 + p3) >>> 16) : pk1p[1];
                // t22: PAD morph sum
                if (a_pad[2]) begin
                    t = s0w[2] + lo32(p0 >>> 15);
                    u = s1w[2] + lo32(p1 >>> 15);
                    x[2] <= (t + u) >>> 1;
                end
            end
            // =========================================================== phase 3
            4'd3: begin
                // t3: register row 2 / state row 2
                r_pinc0 <= reg_q[31:0];   r_pinc1 <= reg_q[63:32];  r_reso <= reg_q[95:64];
                r_fenv <= reg_q[127:96];  r_penv <= reg_q[159:128]; r_fmsus <= reg_q[191:160];
                r_kslen <= reg_q[223:192]; r_ksbright <= reg_q[255:224];
                s_pk0 <= sv_q[31:0];   s_pk1 <= sv_q[63:32];  s_pk2 <= sv_q[95:64];   s_kspack <= sv_q[127:96];
                s_ksprev <= sv_q[159:128]; s_kslp <= sv_q[191:160]; s_low <= sv_q[223:192]; s_band <= sv_q[255:224];
                // cutoff / amplitude slews, vibrato depth
                t = {8'd0, r_cutoff[15:0], 8'd0};
                u = tid[0] ? t : s_cut;
                ma0 <= sx(t - u); mb0 <= ux(rate_coef(r_slew));
                cutn[0] <= (r_slew == 32'd0) ? u + ((t - u) >>> 10) : u;
                t = {8'd0, r_amp[15:0], 8'd0};
                u = tid[0] ? t : s_amp;
                ma1 <= sx(t - u); mb1 <= ux(rate_coef(r_slew));
                ampn[0] <= (r_slew == 32'd0) ? u + ((t - u) >>> 10) : u;
                t = {2'd0, r_vibdepth[15:0], 14'd0};
                u = trg[0] ? 32'sd0 : s_vibd;
                vibd0[0] <= u;
                ma2 <= sx(t - u); mb2 <= ux(rate_coef(r_vibdelay));
                // pan, FM index slews; vibrato LFO phase
                t = {8'd0, r_pan[15:0], 8'd0};
                u = tid[0] ? t : s_pan;
                pann[0] <= u + ((t - u) >>> 10);
                t = {8'd0, r_fmidx[15:0], 8'd0};
                u = tid[0] ? t : s_fmi;
                fmin[0] <= u + ((t - u) >>> 10);
                vibphn[0] <= (tid[0] ? 32'd0 : s_vibph) + r_vibrate;
                // oscillator phases after the trigger
                ph0p[0] <= tid[0] ? (a_pad[0] ? pph0[0] : 32'd0) : s_ph0;
                ph1p[0] <= tid[0] ? (a_pad[0] ? pph1[0] : 32'd0) : s_ph1;
                dust0[0] <= tid[0] ? 32'sd0 : s_dust;
                // noise: second high-pass
                nl20 = tid[0] ? 32'sd0 : s_nlp2;
                if (a_noise[0]) begin
                    nl2 = nl20 + ((w1[0] - nl20) >>> 6);
                    nl2n[0] <= nl2;
                    w2[0] <= w1[0] - nl2;
                end else nl2n[0] <= nl20;
                // t23: f * high, pass 1
                ma3 <= sx(f[2]); mb3 <= sx(sat(e48(x[2]) - e48(low1[2]) - e48(qb1[2]), SVF_LIM));
                // t13: low, pass 1; noise pair C
                low1[1] <= sat(e48(slow0[1]) + lo48(p0 >>> 16), SVF_LIM);
                pk2n[1] <= (a_noise[1] && pink[1]) ? lo32((p2 + p3) >>> 16) : pk2p[1];
                // t43: a = e * amp
                a[4] <= lo32(p1 >>> 15);
            end
            // =========================================================== phase 4
            4'd4: begin
                // t4: register row 3
                ksdamp16[0] <= reg_q[15:0];
                dust16[0] <= reg_q[47:32];
                wave_c[0] <= (reg_q[95:64] > 32'd32768) ? 32'd32768 : reg_q[95:64];
                pink[0] <= reg_q[127:96] == 32'd1 || reg_q[127:96] == 32'd3;
                dusty[0] <= reg_q[127:96] == 32'd2 || reg_q[127:96] == 32'd3;
                bypass[0] <= reg_q[159:128] == 32'd3;
                fsel[0] <= (reg_q[159:128] == 32'd1) ? 2'd1 : (reg_q[159:128] == 32'd2) ? 2'd2 : 2'd0;
                srev16[0] <= reg_q[175:160];
                secho16[0] <= reg_q[207:192];
                fmsus16[0] <= r_fmsus[15:0];
                // glide
                ut = tid[0] ? r_pinc0 : s_incs0;
                incs0p[0] <= ut;
                ma0 <= ux(r_pinc0) - ux(ut); mb0 <= ux(rate_coef(r_glide));
                ut = tid[0] ? r_pinc1 : s_incs1;
                incs1p[0] <= ut;
                ma1 <= ux(r_pinc1) - ux(ut); mb1 <= ux(rate_coef(r_glide));
                // filter q, pan gain index, FM depth, post-trigger pink/pluck/filter state
                t = {16'd0, r_reso[15:0]};
                if (t < SVF_Q_MIN) t = SVF_Q_MIN;
                if (t > SVF_Q_MAX) t = SVF_Q_MAX;
                q[0] <= t;
                ut = pann[0] >>> 8;
                panu[0] <= (ut > 32'd32768) ? 32'd32768 : ut;
                t = fmin[0] >>> 8;
                t = t - $signed({16'd0, r_fmsus[15:0]});
                dfm[0] <= (t < 32'sd0) ? 32'sd0 : t;
                pk0p[0] <= tid[0] ? 32'sd0 : s_pk0;
                pk1p[0] <= tid[0] ? 32'sd0 : s_pk1;
                pk2p[0] <= tid[0] ? 32'sd0 : s_pk2;
                ksprev0[0] <= tid[0] ? 32'sd0 : s_ksprev;
                kslp0[0] <= tid[0] ? 32'sd0 : s_kslp;
                slow0[0] <= tid[0] ? 32'sd0 : s_low;
                sband0[0] <= tid[0] ? 32'sd0 : s_band;
                // pluck: excitation length on trigger, delay-line read position
                ksw[0] <= s_kspack[10:0];
                ksx0[0] <= (trg[0] && r_algo == 32'd3) ? ((r_kslen[31:16] > 16'd2048) ? 16'd2048 : r_kslen[31:16])
                                                     : s_kspack[31:16];
                ut = (r_kslen < (32'd2 << 16)) ? (32'd2 << 16) : (r_kslen > (32'd2040 << 16)) ? (32'd2040 << 16) : r_kslen;
                kslo[0] <= s_kspack[10:0] - ut[26:16] - 11'd1;
                ksfr[0] <= ut[15:8];
                // t14: increments; source results; noise D
                inc0[1] <= vib_on[1] ? inc0a[1] + lo32(p0 >>> 16) : inc0a[1];
                inc1[1] <= vib_on[1] ? incs1n[1] + lo32(p1 >>> 16) : incs1n[1];
                if (a_sub[1]) begin
                    t = lo32(p2 >>> 15);
                    x[1] <= (sp0[1] + t) >>> 1;
                end
                fmph[1] <= ph0p[1] + lo32(p2 >>> 3);
                kswd[1] <= q23_to_16((ksx0[1] != 16'd0) ? kslpn[1] : lo32(p2 >>> 15));
                dsh[1] <= lo32(p3 >>> 16);
                phB[1] <= a_pad[1] ? ph1p[1] : ph0p[1] + (wave_c[1] << 17);
                // t44: a * pan gains
                ma2 <= sx(a[4]); mb2 <= sx(gl[4]);
                ma3 <= sx(a[4]); mb3 <= sx(gr[4]);
            end
            // =========================================================== phase 5
            4'd5: begin
                // t5: filter q * band; envelopes done
                ma0 <= sx(q[0]); mb0 <= sx(sband0[0]);
                menvn[0] <= menv0[0] + lo32(p0 >>> 32);
                en = s_env;
                es = est0[0];
                case (est0[0])
                    ENV_ATTACK: begin
                        en = s_env + lo32(p1 >>> 32) + 32'sd1;
                        if (en >= ONE30) begin en = ONE30; es = ENV_DECAY; end
                    end
                    ENV_DECAY: en = s_env + lo32(p1 >>> 32);
                    ENV_RELEASE: begin
                        en = s_env + lo32(p1 >>> 32);
                        if (en < ENV_FLOOR) begin en = 32'sd0; es = ENV_IDLE; end
                    end
                    default: ;
                endcase
                envn[0] <= en;
                estn[0] <= es;
                if (valid[0]) env_state[v[0]] <= es;
                // t15: phases advance; pluck write (ks_we); noise sum and dust
                ph0f[1] <= ph0p[1] + inc0[1];
                ph1f[1] <= ph1p[1] + inc1[1];
                if (a_ks[1]) ksw[1] <= ksw[1] + 11'd1;
                if (a_ks[1] && ksx0[1] != 16'd0) ksx0[1] <= ksx0[1] - 16'd1;
                t48 = (e48(pk0n[1]) + e48(pk1n[1]) + e48(pk2n[1]) + e48(dsh[1])) >>> 2;
                t = pink[1] ? sat(t48, Q23_MAX) : w2[1];
                wsel[1] <= t;
                if (noisen[1][15:0] < dust16[1]) u = 32'sd8388608;
                else u = dust0[1] - (dust0[1] >>> 7);
                dustn[1] <= (a_noise[1] && dusty[1]) ? u : dust0[1];
                if (a_noise[1] && !dusty[1]) x[1] <= t;
                // t35: band, pass 2; mode select
                t = sat(e48(band1[3]) + lo48(p2 >>> 16), SVF_LIM);
                if (bypass[3]) u = x[3];
                else if (fsel[3] == 2'd1) u = t;
                else if (fsel[3] == 2'd2) u = high2[3];
                else u = low2[3];
                xs[3] <= sat(e48(u), Q23_MAX);
                slowo[3] <= bypass[3] ? slow0[3] : low2[3];
                sbando[3] <= bypass[3] ? sband0[3] : t;
                // t45: sends
                ma1 <= sx(a[4]); mb1 <= ux({16'd0, srev16[4]});
                ma2 <= sx(a[4]); mb2 <= ux({16'd0, secho16[4]});
            end
            // =========================================================== phase 6
            4'd6: begin
                // t6: filter envelope, pitch envelope; slews done (status write: u_status)
                ma0 <= ux({16'd0, r_fenv[15:0]}); mb0 <= sx(envn[0]);
                ma1 <= ux(r_penv); mb1 <= ux(menvn[0]);
                if (r_slew != 32'd0) begin
                    cutn[0] <= cutn[0] + lo32(p0 >>> 32);
                    ampn[0] <= ampn[0] + lo32(p1 >>> 32);
                end
                if (r_vibdelay == 32'd0) vd = {2'd0, r_vibdepth[15:0], 14'd0};
                else vd = vibd0[0] + lo32(p2 >>> 32);
                vdn[0] <= vd;
                vib_on[0] <= vd != 32'sd0;
                // t16: noise E (dust); state row 0 written (sv_we)
                ma3 <= sx(wsel[1]); mb3 <= sx(dustn[1]);
                // t26: band, pass 1
                band1[2] <= sat(e48(sband0[2]) + lo48(p3 >>> 16), SVF_LIM);
                // t36: x * envelope
                ma2 <= sx(xs[3]); mb2 <= sx(envn[3]);
            end
            // =========================================================== phase 7
            4'd7: begin
                // t7: glide done; FM depth * mod envelope
                incs0n[0] <= (r_glide == 32'd0) ? r_pinc0 : incs0p[0] + lo32(p0 >>> 32);
                incs1n[0] <= (r_glide == 32'd0) ? r_pinc1 : incs1p[0] + lo32(p1 >>> 32);
                ma2 <= sx(dfm[0]); mb2 <= sx(menvn[0]);
                // t17: FM carrier read (sa_ph); state row 1 written (sv_we)
                // t27: f * band, q * band (pass 2)
                ma0 <= sx(f[2]); mb0 <= sx(band1[2]);
                ma1 <= sx(q[2]); mb1 <= sx(band1[2]);
                // t47: dry bus
                if (valid[4]) begin
                    acc_l <= sat(e48(acc_l) + lo48(p2 >>> 23), S32_MAX);
                    acc_r <= sat(e48(acc_r) + lo48(p3 >>> 23), S32_MAX);
                end
            end
            // =========================================================== phase 8
            4'd8: begin
                // t8: vibrato amount; pluck brightness; noise pair A
                ma0 <= sx(sinA); mb0 <= sx(vdn[0] >>> 14);
                ma1 <= sx(wks[0] - kslp0[0]); mb1 <= ux({16'd0, r_ksbright[15:0]});
                ma2 <= sx(pk0p[0]); mb2 <= 33'sd64848;
                ma3 <= sx(w2[0]);   mb3 <= 33'sd6491;
                qb1[0] <= lo32(p0 >>> 14);
                // t38: state row 2 written (sv_we)
                // t48: sends
                if (valid[4]) begin
                    acc_rev <= sat(e48(acc_rev) + lo48(p1 >>> 15), S32_MAX);
                    acc_echo <= sat(e48(acc_echo) + lo48(p2 >>> 15), S32_MAX);
                end
                // next voice to enter
                have_next <= mask != 32'd0;
                vnext <= 5'd0;
                for (n = 31; n >= 0; n = n - 1) if (mask[n]) vnext <= n[4:0];
            end
            // =========================================================== phase 9
            default: begin
                // stage shift; phase-9 ops below write stage k+1 directly
                `SH(v) `SH(trg) `SH(tid) `SH(est0) `SH(estn)
                `SH(a_pad) `SH(a_fm) `SH(a_noise) `SH(a_ks) `SH(a_sub) `SH(a_pulse)
                `SH(pink) `SH(dusty) `SH(bypass) `SH(vib_on) `SH(fsel)
                `SH(dust16) `SH(ksdamp16) `SH(fmsus16) `SH(srev16) `SH(secho16) `SH(wave_c)
                `SH(envn) `SH(menv0) `SH(menvn) `SH(cutn) `SH(ampn) `SH(nln) `SH(nl2n)
                `SH(pann) `SH(fmin) `SH(vibd0) `SH(vdn) `SH(dust0) `SH(dustn)
                `SH(noisen) `SH(vibphn) `SH(incs0p) `SH(incs1p) `SH(incs0n) `SH(incs1n)
                `SH(pph0) `SH(pph1) `SH(ph0p) `SH(ph1p) `SH(ph0f) `SH(ph1f)
                `SH(panu) `SH(inc0a) `SH(inc0) `SH(inc1) `SH(phB) `SH(fmph)
                `SH(pk0p) `SH(pk1p) `SH(pk2p) `SH(pk0n) `SH(pk1n) `SH(pk2n)
                `SH(ksw) `SH(kslo) `SH(ksfr) `SH(ksx0) `SH(kswd)
                `SH(ksprev0) `SH(kslp0) `SH(ksy) `SH(kslpn) `SH(avg)
                `SH(slow0) `SH(sband0) `SH(q) `SH(qb1) `SH(f)
                `SH(sp1) `SH(sp0) `SH(idx) `SH(gr) `SH(gl) `SH(dfm)
                `SH(wks) `SH(w1) `SH(w2) `SH(dsh) `SH(wsel)
                `SH(x) `SH(s0w) `SH(s1w) `SH(low1) `SH(band1) `SH(low2) `SH(qb2)
                `SH(high2) `SH(xs) `SH(slowo) `SH(sbando) `SH(e) `SH(a)
                valid <= {valid[3:0], have_next};
                // t9 -> stage 1: cutoff + filter env, pitch env, oscillator 1, right gain, pluck
                t = (cutn[0] >>> 8) + lo32(p0 >>> 30);
                f[1] <= (t > SVF_F_MAX) ? SVF_F_MAX : t;
                inc0a[1] <= incs0n[0] + lo32(p1 >>> 30);
                sp1[1] <= sinA;
                gr[1] <= sinB;
                ksy[1] <= a_ks[0] ? ksval : ksprev0[0];
                avg[1] <= (ksval + ksprev0[0]) >>> 1;
                if (a_ks[0]) x[1] <= ksval;
                ma2 <= sx(pk1p[0]); mb2 <= 33'sd63111;                      // noise pair B
                ma3 <= sx(w2[0]);   mb3 <= 33'sd19433;
                // t19 -> stage 2: wavetables; PAD morph; PULSE; dust
                ma0 <= sx(triA - sawA); mb0 <= sx(wave_c[1]);
                ma1 <= sx(triB - sawB); mb1 <= sx(wave_c[1]);
                s0w[2] <= sawA;
                s1w[2] <= sawB;
                if (a_pulse[1]) x[2] <= (sawA - sawB) >>> 1;
                if (a_noise[1] && dusty[1]) x[2] <= lo32(p3 >>> 23);
                // t39 -> stage 4: e = x * envelope
                e[4] <= lo32(p2 >>> 30);
                // entry of the next voice into stage 0
                if (have_next) begin
                    v[0] <= vnext;
                    mask[vnext] <= 1'b0;
                    est_c = env_state[vnext];
                    trig_c = trig_pending[vnext] | (gate_ff[vnext] & ~prev_gate[vnext]);
                    es = trig_c ? ENV_ATTACK : est_c;
                    if (!gate_ff[vnext] && (es == ENV_ATTACK || es == ENV_DECAY)) es = ENV_RELEASE;
                    trg[0] <= trig_c;
                    tid[0] <= trig_c && est_c == ENV_IDLE;
                    est0[0] <= es;
                    prev_gate[vnext] <= gate_ff[vnext];
                    if (trig_c) trig_pending[vnext] <= 1'b0;
                end else if (valid[3:0] == 4'd0) begin
                    // drained: the last voice finished its t48 step this interval
                    busy <= 1'b0;
                    done <= 1'b1;
                    bus_l <= acc_l; bus_r <= acc_r; bus_rev <= acc_rev; bus_echo <= acc_echo;
                end
            end
            endcase
        end
    end
    `undef SH
endmodule

// 33x33 signed multiplier, product 2 cycles after the operand registers (a, b):
// four 17-bit partial products (two DSP blocks), registered, then summed.
module ve_mul (
    input  wire               clk,
    input  wire signed [32:0] a,
    input  wire signed [32:0] b,
    output reg  signed [65:0] p
);
    wire signed [15:0] ah = a[32:17], bh = b[32:17];
    wire signed [17:0] al = {1'b0, a[16:0]}, bl = {1'b0, b[16:0]};
    reg  signed [35:0] hh, hl, lh, ll;
    always @(posedge clk) begin
        hh <= ah * bh;
        hl <= ah * bl;
        lh <= al * bh;
        ll <= al * bl;
        p  <= (hh <<< 34) + ((hl + lh) <<< 17) + ll;
    end
endmodule

// interpolating table read: ctx = {negate, odd, frac} is given with the ROM address;
// qe / qo (even / odd bank) arrive 2 cycles later; the value 2 cycles after that.
//   a = T[i] (odd bank when i is odd), b = T[i+1]: v = a*256 + (b - a)*frac
module ve_interp (
    input  wire               clk,
    input  wire        [9:0]  ctx,
    input  wire        [15:0] qe,
    input  wire        [15:0] qo,
    output reg  signed [31:0] val
);
    reg [9:0] c1, c2;
    reg signed [23:0] ash;
    reg signed [16:0] d;
    reg  [7:0] fr;
    reg        ng;
    wire signed [15:0] ta = c2[8] ? qo : qe, tb = c2[8] ? qe : qo;
    wire signed [31:0] mv = ash + d * $signed({1'b0, fr});
    always @(posedge clk) begin
        c1 <= ctx;
        c2 <= c1;
        ash <= ta <<< 8;
        d <= tb - ta;
        fr <= c2[7:0];
        ng <= c2[9];
        val <= ng ? -mv : mv;
    end
endmodule

// simple dual-port RAM (M10K / MLAB); OREG adds an output register (2-cycle read)
module ve_sdp #(
    parameter W = 32,
    parameter AW = 7,
    parameter SEEDS = 0,         // per-voice noise seeds in state row 0, bank 4
    parameter OREG = 0
) (
    input  wire          clk,
    input  wire          we,
    input  wire [AW-1:0] wa,
    input  wire [W-1:0]  wd,
    input  wire [AW-1:0] ra,
    output wire [W-1:0]  rd
);
    reg [W-1:0] mem [0:(1 << AW) - 1];
    reg [W-1:0] r0, r1;
    integer i;
    initial begin
        for (i = 0; i < (1 << AW); i = i + 1) mem[i] = 0;
        if (SEEDS) for (i = 0; i < 32; i = i + 1) mem[i * 4] = (32'h9E3779B9 * (i + 1)) | 32'd1;
    end
    always @(posedge clk) begin
        if (we) mem[wa] <= wd;
        r0 <= mem[ra];
        r1 <= r0;
    end
    assign rd = OREG ? r1 : r0;
endmodule

// dual-port ROM, registered address and output (2-cycle read)
module ve_rom2 #(
    parameter AW = 12,
    parameter DEPTH = 2049,
    parameter FILE = ""
) (
    input  wire          clk,
    input  wire [AW-1:0] aa,
    input  wire [AW-1:0] ab,
    output reg  [15:0]   qa,
    output reg  [15:0]   qb
);
    reg [15:0] mem [0:DEPTH-1];
    reg [15:0] ra, rb;
    initial $readmemh(FILE, mem);
    always @(posedge clk) begin
        ra <= mem[aa];
        rb <= mem[ab];
        qa <= ra;
        qb <= rb;
    end
endmodule
`default_nettype wire
