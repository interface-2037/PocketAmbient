// voice_engine_ref.sv - Phase 2a: behavioural, bit-exact RTL of voice_process() in
// model/synth_model.c. One voice per clock; `start` computes one sample's voice
// bus (dry L/R, reverb send, echo send) over 32 voices, then pulses `done`.
//
// This version mirrors the C model statement by statement (C integer semantics:
// arithmetic >>> on signed values, 64-bit products, the same saturation bounds,
// 32-bit wrap on phases/increments). It is the bit-exact reference for Phase 2b,
// which re-times it for the FPGA (registered RAM/ROM reads, a pipelined
// multiplier) and is checked against the same golden vectors.
`default_nettype none

module voice_engine_ref (
    input  wire               clk,
    input  wire               rst,
    // register writes (between samples): addr = voice*32 + reg
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
    // VR_STATUS read
    input  wire        [4:0]  st_voice,
    output wire        [31:0] st_status
);
    // ------------------------------------------------------------- constants
    localparam integer ONE30 = 1 << 30;
    localparam integer ATK_TARGET = 1342177280;
    localparam integer ENV_FLOOR = 64;
    localparam integer Q23_MAX = (1 << 23) - 1;
    localparam integer SVF_LIM = 1 << 27;
    localparam integer SVF_F_MAX = 52429;
    localparam integer SVF_Q_MIN = 1024, SVF_Q_MAX = 32767;
    localparam [1:0] ENV_IDLE = 2'd0, ENV_ATTACK = 2'd1, ENV_DECAY = 2'd2, ENV_RELEASE = 2'd3;

    // register indices (fw/hal/regs.h)
    localparam VR_GATE = 0, VR_ALGO = 1, VR_PINC0 = 2, VR_PINC1 = 3, VR_FM_INDEX = 4, VR_FM_SUS = 5,
               VR_MOD_DECAY = 6, VR_ATTACK = 7, VR_DECAY = 8, VR_SUSTAIN = 9, VR_RELEASE = 10,
               VR_CUTOFF = 11, VR_RESO = 12, VR_FMODE = 13, VR_AMP = 14, VR_PAN = 15, VR_SEND_REV = 16,
               VR_SEND_ECHO = 17, VR_WAVE = 18, VR_NOISE = 19, VR_DUST_RATE = 20, VR_KS_LEN = 21,
               VR_KS_DAMP = 22, VR_KS_BRIGHT = 23, VR_PENV = 24, VR_VIB_RATE = 25, VR_VIB_DEPTH = 26,
               VR_VIB_DELAY = 27, VR_GLIDE = 28, VR_SLEW = 29, VR_FENV = 30;

    // ---------------------------------------------------------------- tables
    reg signed [15:0] sine_q  [0:4097];
    reg signed [15:0] wt_saw  [0:10239];
    reg signed [15:0] wt_tri  [0:10239];
    initial begin
        $readmemh("core/synth/sine_q.hex", sine_q);
        $readmemh("core/synth/wt_saw.hex", wt_saw);
        $readmemh("core/synth/wt_tri.hex", wt_tri);
    end

    // -------------------------------------------------------- register file
    reg [31:0] R [0:1023];
    reg        trig_pending [0:31];

    // ---------------------------------------------------------- voice state
    reg        prev_gate  [0:31];
    reg  [1:0] env_state  [0:31];
    reg signed [31:0] env_level [0:31], mod_env [0:31];
    reg [31:0] phase0 [0:31], phase1 [0:31];
    reg signed [31:0] svf_low [0:31], svf_band [0:31];
    reg [31:0] noise [0:31];
    reg signed [31:0] pink0 [0:31], pink1 [0:31], pink2 [0:31];
    reg signed [31:0] noise_lp [0:31], noise_lp2 [0:31], dust_env [0:31];
    reg signed [31:0] cutoff_s [0:31], amp_s [0:31], pan_s [0:31], fmi_s [0:31];
    reg [31:0] inc_s0 [0:31], inc_s1 [0:31];
    reg [31:0] vib_ph [0:31];
    reg signed [31:0] vib_d [0:31];
    reg [10:0] ks_w [0:31];
    reg [15:0] ks_excite [0:31];
    reg signed [31:0] ks_prev [0:31], ks_lp [0:31];
    reg signed [15:0] ks_buf [0:4*2048-1];

    // -------------------------------------------------------------- helpers
    function automatic signed [31:0] sat(input signed [63:0] x, input signed [63:0] lim);
        if (x > lim) sat = lim[31:0];
        else if (x < -lim) sat = -lim[31:0];
        else sat = x[31:0];
    endfunction

    function automatic [31:0] rate_coef(input [31:0] code);
        rate_coef = {19'd1, code[11:0]} << code[15:12];
    endfunction

    function automatic [31:0] xs32(input [31:0] x0);
        reg [31:0] x;
        begin
            x = x0;
            x = x ^ (x << 13);
            x = x ^ (x >> 17);
            x = x ^ (x << 5);
            xs32 = x;
        end
    endfunction

    function automatic signed [31:0] sin_q23(input [31:0] ph);
        reg [1:0]  q;
        reg [20:0] p;
        reg [12:0] i;
        reg signed [31:0] a, b, f, v;
        begin
            q = ph[31:30];
            p = {1'b0, ph[29:10]};
            if (q[0]) p = 21'h100000 - p;
            i = p[20:8];
            f = {24'd0, p[7:0]};
            a = sine_q[i];
            b = sine_q[i + 1];
            v = (a <<< 8) + (b - a) * f;
            sin_q23 = q[1] ? -v : v;
        end
    endfunction

    function automatic [3:0] mip_level(input [31:0] inc);
        integer k, msb;
        begin
            if (inc < 32'h00100000) mip_level = 4'd0;
            else begin
                msb = 0;
                for (k = 0; k < 32; k = k + 1) if (inc[k]) msb = k;
                mip_level = (msb - 20 > 9) ? 4'd9 : (msb - 20);
            end
        end
    endfunction

    function automatic signed [31:0] wt_read(input tri_sel, input [3:0] lvl, input [31:0] ph);
        reg [9:0] i, i1;
        reg signed [31:0] a, b, f;
        begin
            i = ph[31:22];
            i1 = i + 10'd1;
            f = {24'd0, ph[21:14]};
            a = tri_sel ? wt_tri[lvl * 1024 + i]  : wt_saw[lvl * 1024 + i];
            b = tri_sel ? wt_tri[lvl * 1024 + i1] : wt_saw[lvl * 1024 + i1];
            wt_read = (a <<< 8) + (b - a) * f;
        end
    endfunction

    function automatic signed [15:0] q23_to_16(input signed [31:0] x);
        reg signed [63:0] t;
        begin
            t = ($signed({{32{x[31]}}, x}) + 64'sd128) >>> 8;
            q23_to_16 = sat(t, 64'sd32767);
        end
    endfunction

    // 64-bit signed helpers
    function automatic signed [63:0] s64(input signed [31:0] x); s64 = {{32{x[31]}}, x}; endfunction
    function automatic signed [63:0] u64(input [31:0] x);        u64 = {32'd0, x};         endfunction

    // -------------------------------------------------------- register write
    integer n;
    initial begin
        for (n = 0; n < 1024; n = n + 1) R[n] = 0;
        for (n = 0; n < 4 * 2048; n = n + 1) ks_buf[n] = 0;
    end

    assign st_status = {(env_level[st_voice][31:14] > 18'hFFFF) ? 16'hFFFF : env_level[st_voice][29:14],
                        14'd0, env_state[st_voice]};

    // -------------------------------------------------------------- process
    reg        busy;
`ifdef VE_DEBUG
    integer dbg_sample = 0, dbg_target = -1;
    initial if (!$value$plusargs("dbg=%d", dbg_target)) dbg_target = -1;
    always @(posedge clk) if (done) dbg_sample <= dbg_sample + 1;
`endif
    reg  [5:0] vi;
    reg signed [31:0] acc_l, acc_r, acc_rev, acc_echo;

    // per-voice temporaries (blocking, inside the clocked block)
    reg [31:0] gate, algo, inc0, inc1, n32, len, di, fr, off, pan, mode;
    reg signed [31:0] x, w, tgt, m, sus, pk, d, idx, f, q, high, e, a, gl, gr, y, avg, fb, ka, kb;
    reg signed [31:0] s0, t0, s1, t1, x0, x1, wave;
    reg [3:0]  l0, l1;
    reg signed [63:0] t64;
    integer kk;

    always @(posedge clk) begin
        done <= 1'b0;
        if (rst) begin
            busy <= 1'b0;
            for (n = 0; n < 32; n = n + 1) begin
                trig_pending[n] <= 0; prev_gate[n] <= 0; env_state[n] <= ENV_IDLE;
                env_level[n] <= 0; mod_env[n] <= 0; phase0[n] <= 0; phase1[n] <= 0;
                svf_low[n] <= 0; svf_band[n] <= 0; noise[n] <= (32'h9E3779B9 * (n + 1)) | 32'd1;
                pink0[n] <= 0; pink1[n] <= 0; pink2[n] <= 0; noise_lp[n] <= 0; noise_lp2[n] <= 0;
                dust_env[n] <= 0; cutoff_s[n] <= 0; amp_s[n] <= 0; pan_s[n] <= 0; fmi_s[n] <= 0;
                inc_s0[n] <= 0; inc_s1[n] <= 0; vib_ph[n] <= 0; vib_d[n] <= 0;
                ks_w[n] <= 0; ks_excite[n] <= 0; ks_prev[n] <= 0; ks_lp[n] <= 0;
            end
        end else if (!busy) begin
            if (wr_en && wr_addr[4:0] != 5'd31) begin
                if (wr_addr[4:0] == VR_GATE) begin
                    if (wr_data[1]) trig_pending[wr_addr[9:5]] <= 1'b1;
                    R[wr_addr] <= {31'd0, wr_data[0]};
                end else begin
                    R[wr_addr] <= wr_data;
                end
            end
            if (start) begin
                busy <= 1'b1;
                vi <= 0;
                acc_l = 0; acc_r = 0; acc_rev = 0; acc_echo = 0;
            end
        end else if (vi == 6'd32) begin
            busy <= 1'b0;
            done <= 1'b1;
            bus_l <= acc_l; bus_r <= acc_r; bus_rev <= acc_rev; bus_echo <= acc_echo;
        end else begin : voice
            // ===================================================== one voice
            gate = R[vi * 32 + VR_GATE] & 32'd1;
            algo = R[vi * 32 + VR_ALGO];
            if (env_state[vi] == ENV_IDLE && gate == 0 && !trig_pending[vi]) begin
                prev_gate[vi] = 1'b0;                                  // frozen
            end else begin
                // ---- gate / trigger
                if (trig_pending[vi] || (gate != 0 && !prev_gate[vi])) begin
                    if (env_state[vi] == ENV_IDLE) begin
                        svf_low[vi] = 0; svf_band[vi] = 0;
                        pink0[vi] = 0; pink1[vi] = 0; pink2[vi] = 0;
                        noise_lp[vi] = 0; noise_lp2[vi] = 0; dust_env[vi] = 0;
                        if (algo == 0) begin
                            n32 = xs32(noise[vi]); phase0[vi] = n32;
                            n32 = xs32(n32);       phase1[vi] = n32;
                            noise[vi] = n32;
                        end else begin
                            phase0[vi] = 0; phase1[vi] = 0;
                        end
                        ks_prev[vi] = 0; ks_lp[vi] = 0;
                        cutoff_s[vi] = {8'd0, R[vi * 32 + VR_CUTOFF][15:0], 8'd0};
                        amp_s[vi]    = {8'd0, R[vi * 32 + VR_AMP][15:0], 8'd0};
                        pan_s[vi]    = {8'd0, R[vi * 32 + VR_PAN][15:0], 8'd0};
                        fmi_s[vi]    = {8'd0, R[vi * 32 + VR_FM_INDEX][15:0], 8'd0};
                        inc_s0[vi] = R[vi * 32 + VR_PINC0];
                        inc_s1[vi] = R[vi * 32 + VR_PINC1];
                        vib_ph[vi] = 0;
                    end
                    vib_d[vi] = 0;
                    env_state[vi] = ENV_ATTACK;
                    mod_env[vi] = ONE30;
                    if (algo == 3) begin
                        len = R[vi * 32 + VR_KS_LEN] >> 16;
                        ks_excite[vi] = (len > 2048) ? 16'd2048 : len[15:0];
                    end
                    trig_pending[vi] = 1'b0;
                end
                prev_gate[vi] = gate[0];
                if (gate == 0 && (env_state[vi] == ENV_ATTACK || env_state[vi] == ENV_DECAY))
                    env_state[vi] = ENV_RELEASE;

                // ---- slews
                tgt = {8'd0, R[vi * 32 + VR_CUTOFF][15:0], 8'd0};
                if (R[vi * 32 + VR_SLEW] != 0) begin
                    t64 = (s64(tgt - cutoff_s[vi]) * u64(rate_coef(R[vi * 32 + VR_SLEW]))) >>> 32;
                    cutoff_s[vi] = cutoff_s[vi] + t64[31:0];
                end else cutoff_s[vi] = cutoff_s[vi] + ((tgt - cutoff_s[vi]) >>> 10);
                if (R[vi * 32 + VR_GLIDE] == 0) begin
                    inc_s0[vi] = R[vi * 32 + VR_PINC0];
                    inc_s1[vi] = R[vi * 32 + VR_PINC1];
                end else begin
                    t64 = ((u64(R[vi * 32 + VR_PINC0]) - u64(inc_s0[vi])) * u64(rate_coef(R[vi * 32 + VR_GLIDE]))) >>> 32;
                    inc_s0[vi] = inc_s0[vi] + t64[31:0];
                    t64 = ((u64(R[vi * 32 + VR_PINC1]) - u64(inc_s1[vi])) * u64(rate_coef(R[vi * 32 + VR_GLIDE]))) >>> 32;
                    inc_s1[vi] = inc_s1[vi] + t64[31:0];
                end
                tgt = {2'd0, R[vi * 32 + VR_VIB_DEPTH][15:0], 14'd0};
                if (R[vi * 32 + VR_VIB_DELAY] == 0) vib_d[vi] = tgt;
                else begin
                    t64 = (s64(tgt - vib_d[vi]) * u64(rate_coef(R[vi * 32 + VR_VIB_DELAY]))) >>> 32;
                    vib_d[vi] = vib_d[vi] + t64[31:0];
                end
                vib_ph[vi] = vib_ph[vi] + R[vi * 32 + VR_VIB_RATE];
                tgt = {8'd0, R[vi * 32 + VR_AMP][15:0], 8'd0};
                if (R[vi * 32 + VR_SLEW] != 0) begin
                    t64 = (s64(tgt - amp_s[vi]) * u64(rate_coef(R[vi * 32 + VR_SLEW]))) >>> 32;
                    amp_s[vi] = amp_s[vi] + t64[31:0];
                end else amp_s[vi] = amp_s[vi] + ((tgt - amp_s[vi]) >>> 10);
                tgt = {8'd0, R[vi * 32 + VR_PAN][15:0], 8'd0};
                pan_s[vi] = pan_s[vi] + ((tgt - pan_s[vi]) >>> 10);
                tgt = {8'd0, R[vi * 32 + VR_FM_INDEX][15:0], 8'd0};
                fmi_s[vi] = fmi_s[vi] + ((tgt - fmi_s[vi]) >>> 10);

                // ---- envelopes
                case (env_state[vi])
                    ENV_ATTACK: begin
                        t64 = (s64(ATK_TARGET - env_level[vi]) * u64(rate_coef(R[vi * 32 + VR_ATTACK]))) >>> 32;
                        env_level[vi] = env_level[vi] + t64[31:0] + 1;
                        if (env_level[vi] >= ONE30) begin env_level[vi] = ONE30; env_state[vi] = ENV_DECAY; end
                    end
                    ENV_DECAY: begin
                        tgt = ((R[vi * 32 + VR_SUSTAIN] > 32768) ? 32'd32768 : R[vi * 32 + VR_SUSTAIN]) << 15;
                        t64 = (s64(tgt - env_level[vi]) * u64(rate_coef(R[vi * 32 + VR_DECAY]))) >>> 32;
                        env_level[vi] = env_level[vi] + t64[31:0];
                    end
                    ENV_RELEASE: begin
                        t64 = (s64(-env_level[vi]) * u64(rate_coef(R[vi * 32 + VR_RELEASE]))) >>> 32;
                        env_level[vi] = env_level[vi] + t64[31:0];
                        if (env_level[vi] < ENV_FLOOR) begin env_level[vi] = 0; env_state[vi] = ENV_IDLE; end
                    end
                    default: ;
                endcase
                t64 = (s64(-mod_env[vi]) * u64(rate_coef(R[vi * 32 + VR_MOD_DECAY]))) >>> 32;
                mod_env[vi] = mod_env[vi] + t64[31:0];

                // ---- increments: pitch envelope, vibrato
                t64 = (u64(R[vi * 32 + VR_PENV]) * u64(mod_env[vi])) >> 30;
                inc0 = inc_s0[vi] + t64[31:0];
                inc1 = inc_s1[vi];
                if (vib_d[vi] != 0) begin
                    t64 = (s64(sin_q23(vib_ph[vi])) * s64(vib_d[vi] >>> 14)) >>> 23;
                    m = t64[31:0];
                    t64 = (u64(inc0) * s64(m)) >>> 16; inc0 = inc0 + t64[31:0];
                    t64 = (u64(inc1) * s64(m)) >>> 16; inc1 = inc1 + t64[31:0];
                end

                // ---- source
                x = 0;
                wave = (R[vi * 32 + VR_WAVE] > 32768) ? 32'sd32768 : R[vi * 32 + VR_WAVE];
                case (algo)
                    0: begin                                            // PAD
                        l0 = mip_level(inc0); l1 = mip_level(inc1);
                        s0 = wt_read(1'b0, l0, phase0[vi]); t0 = wt_read(1'b1, l0, phase0[vi]);
                        s1 = wt_read(1'b0, l1, phase1[vi]); t1 = wt_read(1'b1, l1, phase1[vi]);
                        t64 = (s64(t0 - s0) * s64(wave)) >>> 15; x0 = s0 + t64[31:0];
                        t64 = (s64(t1 - s1) * s64(wave)) >>> 15; x1 = s1 + t64[31:0];
                        x = (x0 + x1) >>> 1;
                    end
                    1: begin                                            // FM
                        m = sin_q23(phase1[vi]);
                        sus = {16'd0, R[vi * 32 + VR_FM_SUS][15:0]};
                        pk = fmi_s[vi] >>> 8;
                        d = pk - sus; if (d < 0) d = 0;
                        t64 = (s64(d) * s64(mod_env[vi])) >>> 30;
                        idx = sus + t64[31:0];
                        t64 = (s64(m) * s64(idx)) >>> 3;
                        off = t64[31:0];
                        x = sin_q23(phase0[vi] + off);
                    end
                    2: begin                                            // NOISE
                        n32 = xs32(noise[vi]); noise[vi] = n32;
                        w = $signed(n32) >>> 8;
                        mode = R[vi * 32 + VR_NOISE];
                        noise_lp[vi] = noise_lp[vi] + ((w - noise_lp[vi]) >>> 6);
                        w = w - noise_lp[vi];
                        noise_lp2[vi] = noise_lp2[vi] + ((w - noise_lp2[vi]) >>> 6);
                        w = w - noise_lp2[vi];
                        if (mode == 1 || mode == 3) begin
                            t64 = (s64(pink0[vi]) * 64'sd64848 + s64(w) * 64'sd6491) >>> 16;  pink0[vi] = t64[31:0];
                            t64 = (s64(pink1[vi]) * 64'sd63111 + s64(w) * 64'sd19433) >>> 16; pink1[vi] = t64[31:0];
                            t64 = (s64(pink2[vi]) * 64'sd37356 + s64(w) * 64'sd68989) >>> 16; pink2[vi] = t64[31:0];
                            t64 = (s64(pink0[vi]) + s64(pink1[vi]) + s64(pink2[vi]) + ((s64(w) * 64'sd12111) >>> 16)) >>> 2;
                            w = sat(t64, Q23_MAX);
                        end
                        if (mode == 2 || mode == 3) begin
                            if (n32[15:0] < R[vi * 32 + VR_DUST_RATE][15:0]) dust_env[vi] = 1 << 23;
                            else dust_env[vi] = dust_env[vi] - (dust_env[vi] >>> 7);
                            t64 = (s64(w) * s64(dust_env[vi])) >>> 23;
                            x = t64[31:0];
                        end else x = w;
                    end
                    3: if (vi >= 28) begin                              // KS
                        len = R[vi * 32 + VR_KS_LEN];
                        if (len < (32'd2 << 16)) len = 32'd2 << 16;
                        if (len > (32'd2040 << 16)) len = 32'd2040 << 16;
                        di = len >> 16; fr = (len >> 8) & 32'hFF;
                        ka = ks_buf[(vi - 28) * 2048 + ((ks_w[vi] - di) & 32'h7FF)];
                        kb = ks_buf[(vi - 28) * 2048 + ((ks_w[vi] - di - 1) & 32'h7FF)];
                        y = (ka <<< 8) + (kb - ka) * $signed(fr);
                        avg = (y + ks_prev[vi]) >>> 1;
                        ks_prev[vi] = y;
                        t64 = (s64(avg) * u64({16'd0, R[vi * 32 + VR_KS_DAMP][15:0]})) >>> 15;
                        fb = t64[31:0];
                        n32 = xs32(noise[vi]); noise[vi] = n32;
                        if (ks_excite[vi] != 0) begin
                            ks_excite[vi] = ks_excite[vi] - 1;
                            w = $signed(n32) >>> 8;
                            t64 = (s64(w - ks_lp[vi]) * u64({16'd0, R[vi * 32 + VR_KS_BRIGHT][15:0]})) >>> 16;
                            ks_lp[vi] = ks_lp[vi] + t64[31:0];
                            fb = ks_lp[vi];
                        end
                        ks_buf[(vi - 28) * 2048 + ks_w[vi]] = q23_to_16(fb);
                        ks_w[vi] = ks_w[vi] + 1;
                        x = y;
                    end
                    5: begin                                            // PULSE
                        off = wave << 17;
                        l0 = mip_level(inc0);
                        x = (wt_read(1'b0, l0, phase0[vi]) - wt_read(1'b0, l0, phase0[vi] + off)) >>> 1;
                    end
                    4: begin                                            // SUB
                        s0 = sin_q23(phase0[vi]);
                        s1 = sin_q23(phase1[vi]);
                        t64 = (s64(s1) * s64(wave)) >>> 15;
                        x1 = t64[31:0];                 // part-selects are unsigned: go via a signed reg
                        x = (s0 + x1) >>> 1;
                    end
                    default: ;
                endcase
                phase0[vi] = phase0[vi] + inc0;
                phase1[vi] = phase1[vi] + inc1;

                // ---- state-variable filter, 2x oversampled
                if (R[vi * 32 + VR_FMODE] != 3) begin
                    t64 = (u64({16'd0, R[vi * 32 + VR_FENV][15:0]}) * s64(env_level[vi])) >>> 30;
                    f = (cutoff_s[vi] >>> 8) + t64[31:0];
                    if (f > SVF_F_MAX) f = SVF_F_MAX;
                    q = {16'd0, R[vi * 32 + VR_RESO][15:0]};
                    if (q < SVF_Q_MIN) q = SVF_Q_MIN;
                    if (q > SVF_Q_MAX) q = SVF_Q_MAX;
                    high = 0;
                    for (kk = 0; kk < 2; kk = kk + 1) begin
                        svf_low[vi] = sat(s64(svf_low[vi]) + ((s64(f) * s64(svf_band[vi])) >>> 16), SVF_LIM);
                        high = sat(s64(x) - s64(svf_low[vi]) - ((s64(q) * s64(svf_band[vi])) >>> 14), SVF_LIM);
                        svf_band[vi] = sat(s64(svf_band[vi]) + ((s64(f) * s64(high)) >>> 16), SVF_LIM);
                    end
                    case (R[vi * 32 + VR_FMODE])
                        1: x = svf_band[vi];
                        2: x = high;
                        default: x = svf_low[vi];
                    endcase
                end
                x = sat(s64(x), Q23_MAX);

                // ---- amplitude, pan, sends
                t64 = (s64(x) * s64(env_level[vi])) >>> 30;      e = t64[31:0];
                t64 = (s64(e) * s64(amp_s[vi] >>> 8)) >>> 15;     a = t64[31:0];
`ifdef VE_DEBUG
                if (dbg_sample == dbg_target)
                    $display("R v%02d algo %0d x %0d env %0d amp_s %0d cut_s %0d a %0d ph0 %08h inc_s0 %08h", vi,
                             algo, x, env_level[vi], amp_s[vi], cutoff_s[vi], a, phase0[vi], inc_s0[vi]);
`endif
                pan = pan_s[vi] >>> 8;
                if (pan > 32768) pan = 32768;
                gr = sin_q23(pan << 15);
                gl = sin_q23((32'd32768 - pan) << 15);
                acc_l    = sat(s64(acc_l)    + ((s64(a) * s64(gl)) >>> 23), 64'sh7FFFFFFF);
                acc_r    = sat(s64(acc_r)    + ((s64(a) * s64(gr)) >>> 23), 64'sh7FFFFFFF);
                acc_rev  = sat(s64(acc_rev)  + ((s64(a) * u64({16'd0, R[vi * 32 + VR_SEND_REV][15:0]})) >>> 15), 64'sh7FFFFFFF);
                acc_echo = sat(s64(acc_echo) + ((s64(a) * u64({16'd0, R[vi * 32 + VR_SEND_ECHO][15:0]})) >>> 15), 64'sh7FFFFFFF);
            end
            vi <= vi + 1;
        end
    end
endmodule
`default_nettype wire
