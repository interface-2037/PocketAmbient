// engine_test.sv - Phase 3 hardware test (v0.2.x), all in clk_74a.
//  1. SRAM timing test at four wait-state settings (sram_bist), results on screen.
//  2. Plays recorded composer excerpts through the real audio engine (vec_player):
//     A = next excerpt, B = restart. Each excerpt starts from a freshly reset engine, so the
//     output is bit-exact with build/excerpts/<mode>.wav from the C model.
//  3. Shows the excerpt, play time, worst engine cycles per sample and tick overruns.
`default_nettype none

module engine_test #(
    parameter [16:0] BIST_LAST = 17'h1FFFF
) (
    input  wire        clk,
    input  wire        reset_n,
    input  wire [31:0] cont1_key,
    input  wire        tick,               // 48 kHz frame tick (start of a stereo frame)
    output reg  [15:0] pcm_l,
    output reg  [15:0] pcm_r,
    // text layer character RAM
    output reg         txt_we,
    output reg   [9:0] txt_addr,
    output reg   [7:0] txt_data,
    // SRAM pins
    output wire [16:0] sram_a,
    inout  wire [15:0] sram_dq,
    output wire        sram_oe_n,
    output wire        sram_we_n,
    output wire        sram_ub_n,
    output wire        sram_lb_n
);
    // ------------------------------------------------------------- reset
    reg [2:0] rst_s;
    always @(posedge clk) rst_s <= {rst_s[1:0], ~reset_n};
    wire rst = rst_s[2];

    // ------------------------------------------------------------- SRAM
    wire        bist_done;
    wire  [3:0] bist_pass;
    wire [15:0] err0, err1, err2, err3;
    wire  [3:0] b_rd, b_wr;
    wire        b_req, b_we;
    wire [16:0] b_addr;
    wire [15:0] b_wd;
    wire        e_req, e_we;
    wire [16:0] e_addr;
    wire [15:0] e_wd;
    wire        m_ack;
    wire [15:0] m_rd;
    sram_ctrl u_sram (.clk(clk), .rst(rst),
        .rd_wait(bist_done ? 4'd6 : b_rd), .wr_pulse(bist_done ? 4'd5 : b_wr),
        .req(bist_done ? e_req : b_req), .we(bist_done ? e_we : b_we),
        .addr(bist_done ? e_addr : b_addr), .wd(bist_done ? e_wd : b_wd),
        .ack(m_ack), .rd(m_rd),
        .sram_a(sram_a), .sram_dq(sram_dq), .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n),
        .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));
    sram_bist #(.LAST(BIST_LAST)) u_bist (.clk(clk), .rst(rst), .done(bist_done), .pass(bist_pass),
        .err0(err0), .err1(err1), .err2(err2), .err3(err3),
        .rd_wait(b_rd), .wr_pulse(b_wr), .req(b_req), .we(b_we), .addr(b_addr), .wd(b_wd),
        .ack(m_ack && !bist_done), .rd(m_rd));

    // ------------------------------------------------------------- engine + player
    reg         eng_rst;
    wire        eng_ready, eng_busy, eng_done;
    wire        p_start, p_wr, p_fx, p_end;
    wire  [9:0] p_addr;
    wire [31:0] p_data, p_samples;
    wire  [7:0] overruns;
    wire signed [15:0] out_l, out_r;
    audio_engine u_eng (.clk(clk), .rst(eng_rst),
        .wr_en(p_wr), .wr_fx(p_fx), .wr_addr(p_addr), .wr_data(p_data),
        .start(p_start), .busy(eng_busy), .done(eng_done), .ready(eng_ready),
        .bus_l(), .bus_r(), .bus_rev(), .bus_echo(), .out_l(out_l), .out_r(out_r),
        .st_voice(5'd0), .st_status(), .fx_gain(),
        .mem_req(e_req), .mem_we(e_we), .mem_addr(e_addr), .mem_wd(e_wd),
        .mem_ack(m_ack && bist_done), .mem_rd(m_rd));
    reg  [1:0] sel;
    reg        restart;
    vec_player u_play (.clk(clk), .sel(sel), .restart(restart), .tick(tick),
        .eng_ready(eng_ready), .eng_busy(eng_busy), .eng_start(p_start),
        .wr_en(p_wr), .wr_fx(p_fx), .wr_addr(p_addr), .wr_data(p_data),
        .samples(p_samples), .at_end(p_end), .overruns(overruns));

    // control: BIST, then reset engine -> play; A = next excerpt, B = restart
    localparam [1:0] C_BIST = 2'd0, C_RST = 2'd1, C_WAIT = 2'd2, C_PLAY = 2'd3;
    reg  [1:0] cs;
    reg  [3:0] rcnt;
    reg  [2:0] ka, kb;
    always @(posedge clk) begin
        ka <= {ka[1:0], cont1_key[4]};
        kb <= {kb[1:0], cont1_key[5]};
        restart <= 1'b0;
        if (rst) begin
            cs <= C_BIST; sel <= 2'd0; eng_rst <= 1'b1;
        end else case (cs)
            C_BIST: if (bist_done) begin cs <= C_RST; rcnt <= 4'd0; end
            C_RST: begin
                eng_rst <= 1'b1;
                rcnt <= rcnt + 4'd1;
                if (rcnt == 4'd15) begin eng_rst <= 1'b0; cs <= C_WAIT; end
            end
            C_WAIT: if (eng_ready) begin restart <= 1'b1; cs <= C_PLAY; end
            default: begin
                if (ka[2:1] == 2'b01) begin sel <= sel + 2'd1; cs <= C_RST; rcnt <= 4'd0; end
                else if (kb[2:1] == 2'b01 || p_end) begin cs <= C_RST; rcnt <= 4'd0; end
            end
        endcase
    end

    // output samples, worst engine time per sample. Channel check: hold X for a 500 Hz
    // beep in the left channel only, Y for the right channel only.
    reg [10:0] cyc, cyc_max;
    reg  [6:0] beep_cnt;
    reg        beep;
    reg  [2:0] kx, ky;
    always @(posedge clk) begin
        kx <= {kx[1:0], cont1_key[6]};
        ky <= {ky[1:0], cont1_key[7]};
        if (tick) begin
            beep_cnt <= (beep_cnt == 7'd47) ? 7'd0 : beep_cnt + 7'd1;
            if (beep_cnt == 7'd47) beep <= ~beep;
        end
        if (p_start) cyc <= 11'd0;
        else if (eng_busy && cyc != 11'h7FF) cyc <= cyc + 11'd1;
        if (eng_done) begin
            pcm_l <= out_l;
            pcm_r <= out_r;
            if (cyc > cyc_max) cyc_max <= cyc;
        end
        if (kx[2] || ky[2]) begin
            pcm_l <= kx[2] ? (beep ? 16'sd3000 : -16'sd3000) : 16'sd0;
            pcm_r <= ky[2] ? (beep ? 16'sd3000 : -16'sd3000) : 16'sd0;
        end
        if (rst) begin cyc_max <= 11'd0; pcm_l <= 16'd0; pcm_r <= 16'd0; end
        if (cs == C_RST) begin pcm_l <= 16'd0; pcm_r <= 16'd0; end
    end

    // play time mm:ss (BCD)
    reg [15:0] sub;
    reg  [3:0] s0, s1, m0, m1;
    always @(posedge clk) begin
        if (restart || rst) begin sub <= 16'd0; s0 <= 0; s1 <= 0; m0 <= 0; m1 <= 0; end
        else if (p_start) begin
            if (sub == 16'd47999) begin
                sub <= 16'd0;
                if (s0 != 4'd9) s0 <= s0 + 4'd1;
                else begin
                    s0 <= 4'd0;
                    if (s1 != 4'd5) s1 <= s1 + 4'd1;
                    else begin
                        s1 <= 4'd0;
                        if (m0 != 4'd9) m0 <= m0 + 4'd1; else begin m0 <= 4'd0; m1 <= m1 + 4'd1; end
                    end
                end
            end else sub <= sub + 16'd1;
        end
    end

    // ------------------------------------------------------------- screen fields
    // character codes are ASCII - 32; colours: 0 dim, 1 amber, 2 green, 3 red
    function automatic [5:0] hexc(input [3:0] d);
        hexc = (d < 4'd10) ? 6'd16 + {2'd0, d} : 6'd33 + {2'd0, d} - 6'd10;
    endfunction
    function automatic [5:0] asc(input [7:0] c); asc = c - 8'd32; endfunction
    function automatic [7:0] name_ch(input [1:0] s, input [2:0] i);   // excerpt names, 6 chars
        reg [47:0] n;
        begin
            case (s)
                2'd0: n = "DRIFT ";
                2'd1: n = "TIDES ";
                2'd2: n = "MOTHER";
                default: n = "HUSH  ";
            endcase
            name_ch = n[8 * (5 - i) +: 8];
        end
    endfunction
    function automatic [15:0] errv(input [1:0] k);
        errv = (k == 2'd0) ? err0 : (k == 2'd1) ? err1 : (k == 2'd2) ? err2 : err3;
    endfunction

    reg  [5:0] fi = 6'd0;                   // field index, one cell written per clock
    reg  [1:0] fk;
    reg  [3:0] fd;
    always @(posedge clk) begin
        fi <= fi + 6'd1;
        txt_we <= 1'b1;
        if (fi < 6'd8) begin                                  // row 5: "k:OK" / "k:NO" per pair
            fk = fi[2:1];
            txt_addr <= {5'd5, 5'd5 * {3'd0, fk} + 5'd2 + {4'd0, fi[0]}};
            if (!bist_done)        txt_data <= {2'd0, asc("-")};
            else if (bist_pass[fk]) txt_data <= {2'd2, fi[0] ? asc("K") : asc("O")};
            else                   txt_data <= {2'd3, fi[0] ? asc("O") : asc("N")};
        end else if (fi < 6'd24) begin                        // row 6: error counts in hex
            fk = (fi - 6'd8) >> 2;
            fd = errv(fk) >> (4'd4 * (4'd3 - {2'd0, fi[1:0]}));
            txt_addr <= {5'd6, 5'd5 * {3'd0, fk} + {3'd0, fi[1:0]}};
            txt_data <= {bist_done ? 2'd1 : 2'd0, bist_done ? hexc(fd) : asc("-")};
        end else if (fi < 6'd30) begin                        // row 9: excerpt name
            txt_addr <= {5'd9, fi[4:0] - 5'd23};
            txt_data <= {2'd2, asc(name_ch(sel, fi - 6'd24))};
        end else if (fi < 6'd35) begin                        // row 9: mm:ss
            txt_addr <= {5'd9, fi[4:0] - 5'd17};
            case (fi - 6'd30)
                6'd0: txt_data <= {2'd0, hexc(m1)};
                6'd1: txt_data <= {2'd0, hexc(m0)};
                6'd2: txt_data <= {2'd0, asc(":")};
                6'd3: txt_data <= {2'd0, hexc(s1)};
                default: txt_data <= {2'd0, hexc(s0)};
            endcase
        end else if (fi < 6'd38) begin                        // row 11: worst cycles (hex)
            txt_addr <= {5'd11, fi[4:0] - 5'd22};
            case (fi - 6'd35)
                6'd0: txt_data <= {2'd1, hexc({1'b0, cyc_max[10:8]})};
                6'd1: txt_data <= {2'd1, hexc(cyc_max[7:4])};
                default: txt_data <= {2'd1, hexc(cyc_max[3:0])};
            endcase
        end else if (fi < 6'd40) begin                        // row 12: overruns (hex)
            txt_addr <= {5'd12, fi[4:0] - 5'd25};
            txt_data <= {(overruns != 8'd0) ? 2'd3 : 2'd1,
                         hexc((fi == 6'd38) ? overruns[7:4] : overruns[3:0])};
        end else txt_we <= 1'b0;
    end
endmodule
`default_nettype wire
