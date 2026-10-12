// fx_engine.sv - Phase 3: tape echo, FDN reverb and master chain.
// Bit-exact with echo_process(), reverb_process() and the master section of
// sm_process() in model/synth_model.c.
//
// A sequential step machine with one registered 33x33 multiplier (result two
// steps after issue) and one external-SRAM port (request / acknowledge; the
// machine waits for every access, so reads and writes happen in program order).
// Delay lines sit in the SRAM exactly as sm_init() lays them out (docs/MEMORY_BUDGET.md).
// After reset the SRAM is cleared (the model starts from zeroed memory) and `ready` rises.
// ~50 multiplies + 34 SRAM accesses per sample: ~300 cycles with 3-cycle SRAM access.
`default_nettype none

module fx_engine (
    input  wire               clk,
    input  wire               rst,
    // FX register writes (between samples)
    input  wire               wr_en,
    input  wire        [4:0]  wr_addr,
    input  wire        [31:0] wr_data,
    // one sample
    input  wire               start,
    input  wire signed [31:0] in_l,
    input  wire signed [31:0] in_r,
    input  wire signed [31:0] in_rev,
    input  wire signed [31:0] in_echo,
    output reg                done,
    output reg                ready,          // SRAM cleared
    output reg  signed [15:0] out_l,
    output reg  signed [15:0] out_r,
    output wire        [31:0] gain,           // FX_STATUS_GAIN
    // SRAM port (131,072 x 16)
    output reg                mem_req,
    output reg                mem_we,
    output reg         [16:0] mem_addr,
    output wire        [15:0] mem_wd,
    input  wire               mem_ack,
    input  wire        [15:0] mem_rd
);
    localparam signed [65:0] Q23_MAX = 66'sd8388607;
    localparam signed [65:0] S32_MAX = 66'sd2147483647;
    localparam signed [65:0] S30_MAX = 66'sd1073741823;
    localparam signed [32:0] AP_G = 33'sd20480;
    localparam signed [32:0] HPF_K = 33'sd14055215;
    localparam [31:0] MOD_INC0 = 32'd27739, MOD_INC1 = 32'd38475;
    localparam [31:0] ECHO_MAX = 32'd36000, PRE_MAX = 32'd4800;
    localparam [31:0] LIM_KNEE = 32'd4194304;
    // delay lines
    localparam [3:0] LN_PRE = 4'd0, LN_AP = 4'd1, LN_REV = 4'd5, LN_ECHO_L = 4'd13, LN_ECHO_R = 4'd14;

    function automatic [16:0] lsize(input [3:0] ln);
        case (ln)
            4'd0: lsize = 17'd4801;
            4'd1: lsize = 17'd142;   4'd2: lsize = 17'd107;   4'd3: lsize = 17'd379;   4'd4: lsize = 17'd277;
            4'd5: lsize = 17'd1619;  4'd6: lsize = 17'd2207;  4'd7: lsize = 17'd2999;  4'd8: lsize = 17'd4111;
            4'd9: lsize = 17'd5639;  4'd10: lsize = 17'd7735; 4'd11: lsize = 17'd10589; 4'd12: lsize = 17'd14519;
            default: lsize = 17'd36001;
        endcase
    endfunction
    function automatic [16:0] lbase(input [3:0] ln);
        case (ln)
            4'd0: lbase = 17'd0;
            4'd1: lbase = 17'd4801;  4'd2: lbase = 17'd4943;  4'd3: lbase = 17'd5050;  4'd4: lbase = 17'd5429;
            4'd5: lbase = 17'd5706;  4'd6: lbase = 17'd7325;  4'd7: lbase = 17'd9532;  4'd8: lbase = 17'd12531;
            4'd9: lbase = 17'd16642; 4'd10: lbase = 17'd22281; 4'd11: lbase = 17'd30016; 4'd12: lbase = 17'd40605;
            4'd13: lbase = 17'd55124;
            default: lbase = 17'd91125;
        endcase
    endfunction
    function automatic [16:0] rev_len(input [2:0] i);   // AP_REV_LEN
        case (i)
            3'd0: rev_len = 17'd1601; 3'd1: rev_len = 17'd2207; 3'd2: rev_len = 17'd2999; 3'd3: rev_len = 17'd4111;
            3'd4: rev_len = 17'd5639; 3'd5: rev_len = 17'd7717; 3'd6: rev_len = 17'd10589; default: rev_len = 17'd14519;
        endcase
    endfunction

    function automatic signed [31:0] sat(input signed [65:0] x, input signed [65:0] lim);
        if (x > lim) sat = lim[31:0];
        else if (x < -lim) sat = -lim[31:0];
        else sat = x[31:0];
    endfunction
    function automatic signed [31:0] lo32(input signed [65:0] x); lo32 = x[31:0]; endfunction
    function automatic signed [65:0] e66(input signed [31:0] x);  e66 = x;         endfunction
    function automatic signed [32:0] sx(input signed [31:0] x);   sx = x;          endfunction
    function automatic signed [32:0] ux(input [31:0] x);          ux = {1'b0, x};  endfunction
    function automatic [31:0] clamp_u(input [31:0] x, input [31:0] lo, input [31:0] hi);
        clamp_u = (x < lo) ? lo : (x > hi) ? hi : x;
    endfunction
    function automatic [31:0] xs32(input [31:0] x0);
        reg [31:0] x;
        begin
            x = x0 ^ (x0 << 13);
            x = x ^ (x >> 17);
            xs32 = x ^ (x << 5);
        end
    endfunction
    function automatic signed [15:0] q23_to_16(input signed [31:0] x);
        reg signed [65:0] t;
        begin
            t = (e66(x) + 66'sd128) >>> 8;
            q23_to_16 = sat(t, 66'sd32767);
        end
    endfunction

    assign mem_wd = q23_to_16(wx);

    // ------------------------------------------------------------- tables
    // echo wobble sine: T[i], T[i+1] of the quarter-wave table in one read
    wire [31:0] wob_ph;
    reg  [20:0] sp;
    reg  [12:0] si;
    always @* begin
        sp = {1'b0, wob_ph[29:10]};
        if (wob_ph[30]) sp = 21'h100000 - sp;
        si = sp[20:8];
    end
    reg  [8:0] sin_ctx;                        // {negate, frac}
    wire signed [15:0] sin_a, sin_b;
    fx_rom2 #(.W(16), .AW(13), .DEPTH(4098), .FILE("core/synth/sine_q.hex")) u_sine (.clk(clk),
        .aa(si), .ab(si + 13'd1), .qa(sin_a), .qb(sin_b));
    // limiter curve
    reg  [10:0] lim_ia, lim_ib;
    wire [23:0] lim_a, lim_b;
    fx_rom2 #(.W(24), .AW(11), .DEPTH(1025), .FILE("core/master/lim.hex")) u_lim (.clk(clk),
        .aa(lim_ia), .ab(lim_ib), .qa(lim_a), .qb(lim_b));

    // ------------------------------------------------------------- state
    reg [31:0] FX [0:31];
    reg [16:0] pos [0:14];
    reg [31:0] echo_ph, mph0, mph1, dither;
    reg signed [31:0] lp_l, lp_r, mg;
    reg signed [31:0] dmp [0:7];
    reg signed [63:0] hp [0:3];                // [stage * 2 + channel], value << 16
    assign gain = mg;
    assign wob_ph = echo_ph + FX[22];

    integer n;
    initial begin
        for (n = 0; n < 32; n = n + 1) FX[n] = 32'd0;
        FX[17] = 32'd32768;                    // FX_DRY
        FX[20] = 32'd16384;                    // FX_TRIM
        FX[8]  = 32'd65535;                    // FX_REV_DAMP
        FX[10] = 32'd1;                        // FX_REV_PREDELAY
        FX[14] = 32'd1; FX[15] = 32'd1;        // FX_ECHO_LEN_L / R
        FX[12] = 32'd65535;                    // FX_ECHO_DAMP
        for (n = 0; n < 15; n = n + 1) pos[n] = 17'd0;
        for (n = 0; n < 8; n = n + 1) dmp[n] = 32'sd0;
        for (n = 0; n < 4; n = n + 1) hp[n] = 64'sd0;
        echo_ph = 0; mph0 = 0; mph1 = 0; dither = 32'h2545F491;
        lp_l = 0; lp_r = 0; mg = 0;
    end

    // multiplier
    reg signed [32:0] ma, mb;
    reg signed [65:0] p;
    always @(posedge clk) p <= ma * mb;

    // ------------------------------------------------------------- steps
    localparam [5:0] IDLE = 6'd0, CLR = 6'd1,
                     E0 = 6'd2, E1 = 6'd3, E2 = 6'd4, E3 = 6'd5, E4 = 6'd6, E5 = 6'd7, E6 = 6'd8,
                     E7 = 6'd9, E8 = 6'd10, E9 = 6'd11, E10 = 6'd12, E11 = 6'd13, E12 = 6'd14,
                     R0 = 6'd15, R1 = 6'd16, R2 = 6'd17,
                     A0 = 6'd18, A1 = 6'd19, A2 = 6'd20, A3 = 6'd21,
                     M0 = 6'd22, L0 = 6'd23, L1 = 6'd24, L2 = 6'd25, L2B = 6'd26, L3 = 6'd27, L4 = 6'd28, L5 = 6'd29,
                     H0 = 6'd30, W0 = 6'd31, O0 = 6'd32,
                     X0 = 6'd33, X1 = 6'd34, X2 = 6'd35, X3 = 6'd36, X4 = 6'd37, X5 = 6'd38, X6 = 6'd39,
                     G1 = 6'd40, P0 = 6'd41, P1 = 6'd42, P2 = 6'd43, P3 = 6'd44, P4 = 6'd45, P5 = 6'd46,
                     P6 = 6'd47, P7 = 6'd48, P8 = 6'd49, FIN = 6'd50,
                     E1B = 6'd51, E1X = 6'd52, W0B = 6'd53, X7 = 6'd54, P3B = 6'd55,
                     E11S = 6'd56, E12S = 6'd57, R1S = 6'd58, A2S = 6'd59, A3S = 6'd60, X3S = 6'd61, X6S = 6'd62;

    reg  [5:0] pc;
    reg        mw, mpend;
    reg [15:0] mrd;
    wire signed [31:0] mrd23 = {{8{mrd[15]}}, mrd, 8'd0};
    reg [16:0] clr_a;
    reg        clr_end;
    // per-sample working registers
    reg signed [31:0] bl, br, brev, becho;
    reg [15:0] depth;
    reg [31:0] dl, dr, dmod;
    reg  [8:0] di;
    reg  [7:0] fr, frm;
    reg [11:0] offm0, offm1;
    reg signed [31:0] sn;
    reg signed [15:0] asv;
    reg signed [16:0] dsub;
    reg       sg;
    reg  [7:0] fsv;
    reg signed [31:0] wq, hq;
    reg signed [65:0] sq;
    reg ap_done;
    reg [31:0] wx;                              // SRAM write data (Q1.23), converted to 16 bits at the pins
    reg signed [31:0] ea, rle, rre, rin, x, bap, wap, ra, rv, h, xin, rlv, rrv, ch0, ch1, gg, hd, xo;
    reg signed [31:0] d0, d1;
    reg signed [31:0] y [0:7];
    reg signed [65:0] rsum, acc;
    reg  [2:0] li;
    reg        c, neg;
    reg  [7:0] fl;

    // blocking temporaries
    reg [31:0] lim, ut, ph, tw0, tw1, ofs, ee, ii;
    reg signed [31:0] t, u, sv, xg, av;
    reg signed [65:0] f66;

    // issue helpers (non-blocking side effects)
    task automatic mul(input signed [32:0] a, input signed [32:0] b);
        begin ma <= a; mb <= b; mw <= 1'b1; end
    endtask
    task automatic rd(input [3:0] ln, input [31:0] d);       // sample written d samples ago
        reg [17:0] idx;
        begin
            idx = {1'b0, pos[ln]} + {1'b0, lsize(ln)} - d[17:0];
            if (idx >= {1'b0, lsize(ln)}) idx = idx - {1'b0, lsize(ln)};
            mem_addr <= lbase(ln) + idx[16:0];
            mem_we <= 1'b0; mem_req <= 1'b1; mpend <= 1'b1;
        end
    endtask
    task automatic wr(input [3:0] ln, input signed [31:0] x23);
        begin
            mem_addr <= lbase(ln) + pos[ln];
            wx <= x23;
            mem_we <= 1'b1; mem_req <= 1'b1; mpend <= 1'b1;
        end
    endtask

    always @(posedge clk) begin
        done <= 1'b0;
        mem_req <= 1'b0;
        sin_ctx <= {wob_ph[31], sp[7:0]};
        if (mem_ack) begin mpend <= 1'b0; mrd <= mem_rd; end
        if (mw) mw <= 1'b0;
        if (rst) begin
            pc <= CLR; clr_a <= 17'd0; clr_end <= 1'b0; ready <= 1'b0; mw <= 1'b0; mpend <= 1'b0;
            for (n = 0; n < 15; n = n + 1) pos[n] <= 17'd0;
            for (n = 0; n < 8; n = n + 1) dmp[n] <= 32'sd0;
            for (n = 0; n < 4; n = n + 1) hp[n] <= 64'sd0;
            echo_ph <= 0; mph0 <= 0; mph1 <= 0; dither <= 32'h2545F491;
            lp_l <= 0; lp_r <= 0; mg <= 0;
        end else if (pc == IDLE) begin
            if (wr_en && wr_addr != 5'd31) FX[wr_addr] <= wr_data;
            if (start) begin
                bl <= in_l; br <= in_r; brev <= in_rev; becho <= in_echo;
                pc <= E0;
            end
        end else if (!mw && !mpend) begin
            case (pc)
            // ------------------------------------------------ clear the SRAM
            CLR: begin
                if (clr_end) begin
                    ready <= 1'b1;
                    pc <= IDLE;
                end else begin
                    mem_addr <= clr_a; wx <= 32'd0; mem_we <= 1'b1; mem_req <= 1'b1; mpend <= 1'b1;
                    clr_a <= clr_a + 17'd1;
                    clr_end <= clr_a == 17'h1FFFF;
                end
            end
            // ------------------------------------------------ tape echo
            E0: begin
                ut = {16'd0, FX[21][15:0]};
                depth <= FX[21][15:0];
                lim = ECHO_MAX - 2 * (ut >> 8) - 2;
                dl <= clamp_u(FX[14], 32'd1, lim);
                dr <= clamp_u(FX[15], 32'd1, lim);
                echo_ph <= wob_ph;                   // sine of the new phase is read this step
                pc <= E1;
            end
            E1: begin                                 // sine table outputs -> interpolation operands
                asv <= sin_a;
                dsub <= sin_b - sin_a;
                sg <= sin_ctx[8];
                fsv <= sin_ctx[7:0];
                pc <= E1X;
            end
            E1X: begin                                // interpolate, sign (own stage)
                t = (asv <<< 8) + dsub * $signed({24'd0, fsv});
                sn <= sg ? -t : t;
                pc <= E1B;
            end
            E1B: begin
                mul(sx(sn), ux({16'd0, depth}));
                pc <= E2;
            end
            E2: begin
                ofs = lo32(p >>> 23) + {16'd0, depth};
                di <= ofs[16:8];
                fr <= ofs[7:0];
                rd(LN_ECHO_L, dl + ofs[16:8]);
                pc <= E3;
            end
            E3: begin ea <= mrd23; rd(LN_ECHO_L, dl + di + 1); pc <= E4; end
            E4: begin mul(sx(mrd23 - ea), ux({24'd0, fr})); pc <= E5; end
            E5: begin rle <= ea + lo32(p >>> 8); rd(LN_ECHO_R, dr + di); pc <= E6; end
            E6: begin ea <= mrd23; rd(LN_ECHO_R, dr + di + 1); pc <= E7; end
            E7: begin mul(sx(mrd23 - ea), ux({24'd0, fr})); pc <= E8; end
            E8: begin
                t = ea + lo32(p >>> 8);
                rre <= t;
                mul(sx(rle - lp_l), ux({16'd0, FX[12][15:0]}));
                pc <= E9;
            end
            E9: begin
                lp_l <= lp_l + lo32(p >>> 16);
                mul(sx(rre - lp_r), ux({16'd0, FX[12][15:0]}));
                pc <= E10;
            end
            E10: begin
                t = lp_r + lo32(p >>> 16);
                lp_r <= t;
                mul(sx(t), ux({16'd0, FX[11][15:0]}));
                pc <= E11;
            end
            E11: begin
                t = sat(e66(becho), Q23_MAX);
                sq <= e66(t) + (p >>> 15);                            // ping-pong feedback sum
                mul(sx(lp_l), ux({16'd0, FX[11][15:0]}));
                pc <= E11S;
            end
            E11S: begin wr(LN_ECHO_L, sat(sq, Q23_MAX)); pc <= E12; end
            E12: begin sq <= p >>> 15; pc <= E12S; end
            E12S: begin wr(LN_ECHO_R, sat(sq, Q23_MAX)); pc <= R0; end
            // ------------------------------------------------ reverb input, pre-delay
            R0: begin
                t = (rle >>> 1) + (rre >>> 1);
                mul(sx(t), ux({16'd0, FX[16][15:0]}));
                pc <= R1;
            end
            R1: begin
                sq <= e66(brev) + (p >>> 15);
                rd(LN_PRE, clamp_u(FX[10], 32'd1, PRE_MAX));
                pc <= R1S;
            end
            R1S: begin rin <= sat(sq, Q23_MAX); pc <= R2; end
            R2: begin x <= mrd23; wr(LN_PRE, rin); li <= 3'd0; pc <= A0; end
            // ------------------------------------------------ 4 allpass diffusers
            A0: begin rd(LN_AP + li, {15'd0, lsize(LN_AP + li)}); pc <= A1; end
            A1: begin bap <= mrd23; mul(AP_G, sx(mrd23)); pc <= A2; end
            A2: begin
                sq <= e66(x) + (p >>> 15);
                pc <= A2S;
            end
            A2S: begin
                t = sat(sq, Q23_MAX);
                wap <= t;
                mul(AP_G, sx(t));
                pc <= A3;
            end
            A3: begin
                sq <= e66(bap) - (p >>> 15);
                wr(LN_AP + li, wap);
                ap_done <= (li == 3'd3);
                li <= li + 3'd1;
                pc <= A3S;
            end
            A3S: begin
                x <= sat(sq, Q23_MAX);
                pc <= ap_done ? M0 : A0;
            end
            // ------------------------------------------------ FDN
            M0: begin
                ph = mph0 + MOD_INC0; mph0 <= ph;
                tw0 = ph[31] ? ~ph : ph;
                ph = mph1 + MOD_INC1; mph1 <= ph;
                tw1 = ph[31] ? ~ph : ph;
                offm0 <= tw0[30:19];
                offm1 <= tw1[30:19];
                li <= 3'd0;
                rsum <= 66'sd0;
                pc <= L0;
            end
            L0: begin
                if (li == 3'd0 || li == 3'd5) begin
                    ut = (li == 3'd5) ? {20'd0, offm1} : {20'd0, offm0};
                    ii = {15'd0, rev_len(li)} + (ut >> 8);
                    dmod <= ii;
                    frm <= ut[7:0];
                    rd(LN_REV + li, ii);
                end else rd(LN_REV + li, {15'd0, rev_len(li)});
                pc <= L1;
            end
            L1: begin
                if (li == 3'd0 || li == 3'd5) begin
                    ra <= mrd23;
                    rd(LN_REV + li, dmod + 1);
                    pc <= L2;
                end else begin
                    rv <= mrd23;
                    pc <= L3;
                end
            end
            L2:  begin mul(sx(mrd23 - ra), ux({24'd0, frm})); pc <= L2B; end
            L2B: begin rv <= ra + lo32(p >>> 8); pc <= L3; end
            L3:  begin mul(sx(rv - dmp[li]), ux({16'd0, FX[8][15:0]})); pc <= L4; end
            L4: begin
                t = dmp[li] + lo32(p >>> 16);
                dmp[li] <= t;
                mul(sx(t), ux({16'd0, FX[li][15:0]}));
                pc <= L5;
            end
            L5: begin
                t = lo32(p >>> 16);
                y[li] <= t;
                rsum <= rsum + e66(t);
                li <= li + 3'd1;
                pc <= (li == 3'd7) ? H0 : L0;
            end
            H0: begin
                h <= sat(rsum >>> 2, S32_MAX);
                xin <= x >>> 1;
                li <= 3'd0;
                pc <= W0;
            end
            W0: begin
                // input signs + + - - per line pair: L and R never anti-phase
                f66 = e66(y[li]) - e66(h) + (li[1] ? -e66(xin) : e66(xin));
                wq <= sat(f66, Q23_MAX);
                pc <= W0B;
            end
            W0B: begin
                wr(LN_REV + li, wq);
                li <= li + 3'd1;
                pc <= (li == 3'd7) ? O0 : W0;
            end
            O0: begin
                rlv <= sat((e66(y[0]) + e66(y[2]) + e66(y[4]) + e66(y[6])) >>> 1, S32_MAX);
                rrv <= sat((e66(y[1]) + e66(y[3]) + e66(y[5]) + e66(y[7])) >>> 1, S32_MAX);
                pc <= X0;
            end
            // ------------------------------------------------ mix
            X0: begin mul(sx(bl), ux({16'd0, FX[17][15:0]})); pc <= X1; end
            X1: begin acc <= p >>> 15; mul(sx(rlv), ux({16'd0, FX[9][15:0]})); pc <= X2; end
            X2: begin acc <= acc + (p >>> 15); mul(sx(rle), ux({16'd0, FX[13][15:0]})); pc <= X3; end
            X3: begin sq <= acc + (p >>> 15); mul(sx(br), ux({16'd0, FX[17][15:0]})); pc <= X3S; end
            X3S: begin ch0 <= sat(sq, S32_MAX); pc <= X4; end
            X4: begin acc <= p >>> 15; mul(sx(rrv), ux({16'd0, FX[9][15:0]})); pc <= X5; end
            X5: begin acc <= acc + (p >>> 15); mul(sx(rre), ux({16'd0, FX[13][15:0]})); pc <= X6; end
            X6: begin
                sq <= acc + (p >>> 15);
                // fade gain: linear ramp toward the target, applied squared
                t = (FX[18] > 32'd8388608) ? 32'sd8388608 : $signed(FX[18]);
                u = $signed({12'd0, FX[19][19:0]});
                sv = mg;
                if (sv < t) begin
                    sv = sv + u;
                    if (sv > t) sv = t;
                end else if (sv > t) begin
                    sv = sv - u;
                    if (sv < t) sv = t;
                end
                mg <= sv;
                ut = xs32(dither);
                dither <= ut;
                d0 <= $signed({24'd0, ut[7:0]}) + $signed({24'd0, ut[15:8]}) - 32'sd255;
                d1 <= $signed({24'd0, ut[23:16]}) + $signed({24'd0, ut[31:24]}) - 32'sd255;
                pc <= X6S;
            end
            X6S: begin ch1 <= sat(sq, S32_MAX); pc <= X7; end
            X7: begin                                  // fade gain squared (own stage)
                mul(sx(mg), sx(mg));
                pc <= G1;
            end
            G1: begin gg <= lo32(p >>> 23); c <= 1'b0; pc <= P0; end
            // ------------------------------------------------ per channel: HPF, trim, fade, limiter
            P0: begin
                t = c ? ch1 : ch0;
                u = sat(e66(t) - e66(lo32(hp[{1'b0, c}] >>> 16)), S30_MAX);
                hd <= u;
                mul(sx(u), HPF_K);
                pc <= P1;
            end
            P1: begin
                hp[{1'b0, c}] <= hp[{1'b0, c}] + (p >>> 16);
                u = sat(e66(hd) - e66(lo32(hp[{1'b1, c}] >>> 16)), S30_MAX);
                hd <= u;
                mul(sx(u), HPF_K);
                pc <= P2;
            end
            P2: begin
                hp[{1'b1, c}] <= hp[{1'b1, c}] + (p >>> 16);
                mul(sx(hd), ux({16'd0, FX[20][15:0]}));
                pc <= P3;
            end
            P3: begin hq <= sat(p >>> 14, S30_MAX); pc <= P3B; end
            P3B: begin mul(sx(hq), sx(gg)); pc <= P4; end
            P4: begin
                xg = lo32(p >>> 23);
                av = (xg < 0) ? -xg : xg;
                neg <= xg < 0;
                if (av <= $signed(LIM_KNEE)) begin
                    xo <= xg;
                    pc <= P8;
                end else begin
                    ee = av - LIM_KNEE;
                    ii = ee >> 15;
                    lim_ia <= (ii >= 32'd1024) ? 11'd1024 : ii[10:0];
                    lim_ib <= (ii >= 32'd1023) ? 11'd1024 : ii[10:0] + 11'd1;
                    fl <= (ii >= 32'd1024) ? 8'd0 : ee[14:7];
                    pc <= P5;
                end
            end
            P5: pc <= P6;                                             // table read
            P6: begin mul($signed({9'd0, lim_b}) - $signed({9'd0, lim_a}), ux({24'd0, fl})); pc <= P7; end
            P7: begin
                t = $signed({8'd0, lim_a}) + lo32(p >>> 8);
                xo <= neg ? -t : t;
                pc <= P8;
            end
            P8: begin
                t = c ? d1 : d0;
                u = (mg == 32'sd0) ? 32'sd0 : sat((e66(xo) + e66(t) + 66'sd128) >>> 8, 66'sd32767);
                if (c) out_r <= u[15:0]; else out_l <= u[15:0];
                c <= 1'b1;
                pc <= c ? FIN : P0;
            end
            FIN: begin
                for (n = 0; n < 15; n = n + 1)
                    pos[n] <= (pos[n] + 17'd1 == lsize(n)) ? 17'd0 : pos[n] + 17'd1;
                done <= 1'b1;
                pc <= IDLE;
            end
            default: pc <= IDLE;
            endcase
        end
    end
endmodule

// dual-port ROM, registered reads
module fx_rom2 #(
    parameter W = 16,
    parameter AW = 12,
    parameter DEPTH = 4096,
    parameter FILE = ""
) (
    input  wire          clk,
    input  wire [AW-1:0] aa,
    input  wire [AW-1:0] ab,
    output reg  [W-1:0]  qa,
    output reg  [W-1:0]  qb
);
    reg [W-1:0] mem [0:DEPTH-1];
    initial $readmemh(FILE, mem);
    always @(posedge clk) begin
        qa <= mem[aa];
        qb <= mem[ab];
    end
endmodule
`default_nettype wire
