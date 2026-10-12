// sys_test.sv - Phase 4 hardware test (v0.3.x), all in clk_74a: the real composer firmware on
// the PicoRV32 drives the audio engine (cpu_sys), with the Pocket's buttons wired to the
// composer and a diagnostics screen. Settings are fixed until the menu arrives in Phase 5:
// DRIFT, density 3, brightness 3, reverb Med, seed locked to 123 (so the first piece matches
// `render --mode drift --seed 123`).
//
// Screen: mode / state from the composer, uptime, worst CPU cycles per tick, lowest FIFO
// level, FIFO underruns, shortest / longest gap between I2S frame ticks (clk_74a cycles;
// nominal 1546-1547, this is the Phase 3 overrun diagnostic), and CPU trap / bus error.
`default_nettype none

module sys_test (
    input  wire        clk,
    input  wire        reset_n,
    input  wire [31:0] cont1_key,
    input  wire        tick,               // 48 kHz frame tick (start of a stereo frame)
    output wire [15:0] pcm_l,
    output wire [15:0] pcm_r,
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

    // ------------------------------------------------------------- buttons: pressed edges
    reg [15:0] k0, k1, k2;
    always @(posedge clk) begin k0 <= cont1_key[15:0]; k1 <= k0; k2 <= k1; end
    wire [15:0] btn_edge = rst ? 16'd0 : (k1 & ~k2);

    // ------------------------------------------------------------- system
    wire        m_req, m_we, m_ack;
    wire [16:0] m_addr;
    wire [15:0] m_wd, m_rd;
    sram_ctrl u_sram (.clk(clk), .rst(rst), .rd_wait(4'd6), .wr_pulse(4'd5),
        .req(m_req), .we(m_we), .addr(m_addr), .wd(m_wd), .ack(m_ack), .rd(m_rd),
        .sram_a(sram_a), .sram_dq(sram_dq), .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n),
        .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));

    wire [31:0] st_state, st_mode, tick_cyc, tick_cyc_max;
    wire [63:0] tick_n;
    wire [15:0] underruns;
    wire [10:0] fifo_min, fifo_level;
    wire        primed, bus_err, trap;
    cpu_sys u_sys (.clk(clk), .rst(rst),
        .cfg_mode(3'd0), .cfg_density(3'd3), .cfg_bright(3'd3), .cfg_reverb(2'd1), .cfg_visuals(2'd0),
        .cfg_seed_lock(1'b1), .cfg_seed(32'd123), .cfg_sleep(2'd0), .cmd_reseed(1'b0),
        .settings_seq(32'd0), .rtc_epoch(32'd0), .btn_edge(btn_edge),
        .frame_tick(tick), .pcm_l(pcm_l), .pcm_r(pcm_r),
        .mem_req(m_req), .mem_we(m_we), .mem_addr(m_addr), .mem_wd(m_wd), .mem_ack(m_ack), .mem_rd(m_rd),
        .st_state(st_state), .st_mode(st_mode), .st_elapsed(), .st_seed(),
        .tick(tick_n), .tick_cycles(tick_cyc), .tick_cycles_max(tick_cyc_max), .underruns(underruns),
        .fifo_min(fifo_min), .fifo_level(fifo_level), .primed(primed), .bus_err(bus_err), .trap(trap),
        .smp_valid(), .smp_l(), .smp_r());

    // ------------------------------------------------------------- frame tick spacing
    reg [15:0] gap, gap_min, gap_max;
    reg  [1:0] seen;
    always @(posedge clk) begin
        if (rst) begin
            gap <= 16'd0; gap_min <= 16'hFFFF; gap_max <= 16'd0; seen <= 2'd0;
        end else if (tick) begin
            if (seen == 2'd2) begin
                if (gap < gap_min) gap_min <= gap;
                if (gap > gap_max) gap_max <= gap;
            end else seen <= seen + 2'd1;
            gap <= 16'd1;
        end else if (gap != 16'hFFFF) gap <= gap + 16'd1;
    end

    // ------------------------------------------------------------- uptime mm:ss (1000 ticks = 1 s)
    reg  [9:0] ms;
    reg        t0_d;
    reg  [3:0] s0, s1, m0, m1;
    always @(posedge clk) begin
        t0_d <= tick_n[0];
        if (rst) begin ms <= 10'd0; s0 <= 0; s1 <= 0; m0 <= 0; m1 <= 0; end
        else if (tick_n[0] != t0_d) begin
            if (ms != 10'd999) ms <= ms + 10'd1;
            else begin
                ms <= 10'd0;
                if (s0 != 4'd9) s0 <= s0 + 4'd1;
                else begin
                    s0 <= 4'd0;
                    if (s1 != 4'd5) s1 <= s1 + 4'd1;
                    else begin
                        s1 <= 4'd0;
                        if (m0 != 4'd9) m0 <= m0 + 4'd1; else begin m0 <= 4'd0; m1 <= m1 + 4'd1; end
                    end
                end
            end
        end
    end

    // ------------------------------------------------------------- screen fields
    // character codes are ASCII - 32; colours: 0 dim, 1 amber, 2 green, 3 red
    function automatic [5:0] hexc(input [3:0] d);
        hexc = (d < 4'd10) ? 6'd16 + {2'd0, d} : 6'd33 + {2'd0, d} - 6'd10;
    endfunction
    function automatic [5:0] asc(input [7:0] c); asc = c[5:0] - 6'd32; endfunction
    function automatic [7:0] mode_ch(input [1:0] s, input [2:0] i);
        reg [47:0] n;
        begin
            case (s)
                2'd0: n = "DRIFT ";
                2'd1: n = "TIDES ";
                2'd2: n = "HUSH  ";
                default: n = "MOTHER";
            endcase
            mode_ch = n[8 * (5 - i) +: 8];
        end
    endfunction
    function automatic [7:0] state_ch(input [1:0] s, input [2:0] i);
        reg [47:0] n;
        begin
            case (s)
                2'd0: n = "READY ";
                2'd1: n = "PLAY  ";
                2'd2: n = "FADE  ";
                default: n = "?     ";
            endcase
            state_ch = n[8 * (5 - i) +: 8];
        end
    endfunction
    function automatic [7:0] err_ch(input t, input b, input p, input [2:0] i);
        reg [47:0] n;
        begin
            n = t ? "TRAP  " : b ? "BUS   " : p ? "OK    " : "START ";
            err_ch = n[8 * (5 - i) +: 8];
        end
    endfunction
    function automatic [3:0] nib(input [19:0] v, input [2:0] k);   // k = 0: most significant of 5
        nib = v[4 * (4 - k) +: 4];
    endfunction

    // 6 cells per row slot, value column 11, rows 4..13
    reg [6:0] fi = 7'd0;
    wire [2:0] c = fi[2:0];
    wire [3:0] rw = fi[6:3];
    always @(posedge clk) begin
        fi <= (fi == 7'd79) ? 7'd0 : fi + 7'd1;
        txt_we <= c < 3'd6;
        txt_addr <= {5'd4 + {1'b0, rw}, 5'd11 + {2'd0, c}};
        case (rw)
            4'd0: txt_data <= {2'd2, asc(mode_ch(st_mode[1:0], c))};
            4'd1: txt_data <= {st_state[1:0] == 2'd1 ? 2'd2 : 2'd1, asc(state_ch(st_state[1:0], c))};
            4'd2: case (c)                                                    // uptime mm:ss
                3'd0: txt_data <= {2'd0, hexc(m1)};
                3'd1: txt_data <= {2'd0, hexc(m0)};
                3'd2: txt_data <= {2'd0, asc(":")};
                3'd3: txt_data <= {2'd0, hexc(s1)};
                3'd4: txt_data <= {2'd0, hexc(s0)};
                default: txt_data <= {2'd0, asc(" ")};
            endcase
            4'd4: txt_data <= {2'd1, c < 3'd5 ? hexc(nib(tick_cyc_max[19:0], c)) : asc(" ")};
            4'd5: txt_data <= {2'd1, c < 3'd5 ? hexc(nib({9'd0, fifo_min}, c)) : asc(" ")};
            4'd6: txt_data <= {underruns != 16'd0 ? 2'd3 : 2'd1, c < 3'd5 ? hexc(nib({4'd0, underruns}, c)) : asc(" ")};
            4'd7: txt_data <= {2'd1, c < 3'd5 ? hexc(nib({4'd0, gap_min}, c)) : asc(" ")};
            4'd8: txt_data <= {2'd1, c < 3'd5 ? hexc(nib({4'd0, gap_max}, c)) : asc(" ")};
            4'd9: txt_data <= {(trap || bus_err) ? 2'd3 : 2'd2, asc(err_ch(trap, bus_err, primed, c))};
            default: txt_we <= 1'b0;
        endcase
    end
endmodule
`default_nettype wire
