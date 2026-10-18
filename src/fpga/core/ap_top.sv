// ap_top.sv - the Ambient Pocket core below core_top (v0.4.x), all in clk_74a except video:
// the composer firmware on the PicoRV32 drives the audio engine (cpu_sys) and draws the
// screens into fb_video; the Pocket's buttons go to the composer, the core menu to
// settings_bridge (pkg/interact.json, and the save slot ambient.sav), and dev builds load the
// firmware from the SD card. pocket_lock sits between the buttons and the composer and can
// black out the screen (D62).
// Defaults: DRIFT, density 3, brightness 3, reverb Med, seed from the RTC (Lock seed off).
// `fault` (CPU trap or bus error) makes core_top show a red screen.
`default_nettype none

module ap_top #(
    parameter FW_HEX = "core/cpu/composer.hex"
) (
    input  wire        clk,
    input  wire        reset_n,
    // APF bridge (clk_74a): firmware data slot writes to 0x2000_0000..0x2000_FFFF (D50)
    input  wire        bridge_wr,
    input  wire        bridge_rd,
    input  wire [31:0] bridge_addr,
    input  wire [31:0] bridge_wr_data,
    output wire [31:0] settings_rd_data,   // bridge reads of 0x5000_00xx (menu), 0x6000_00xx (save)
    input  wire [31:0] rtc_epoch,
    input  wire [31:0] cont1_key,
    input  wire        tick,               // 48 kHz frame tick (start of a stereo frame)
    output wire [15:0] pcm_l,
    output wire [15:0] pcm_r,
    // video (pixel clock): coordinates three clocks ahead, colour three clocks later
    input  wire        vclk,
    input  wire        vsync,
    input  wire  [9:0] vx,
    input  wire  [9:0] vy,
    output wire [23:0] rgb,
    output wire        fault,
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
    wire [15:0] key_edge = rst ? 16'd0 : (k1 & ~k2);

    // ------------------------------------------------------------- Pocket lock (D62)
    wire [15:0] btn_edge;
    wire        locked, lock_blank, cmd_lock;
    pocket_lock u_lock (.clk(clk), .rst(rst), .keys(k1), .edges(key_edge), .menu_lock(cmd_lock),
        .btn_out(btn_edge), .locked(locked), .blank(lock_blank));
    // black video while locked, outside the hint (2-flop synchroniser into the pixel clock;
    // a frame of mixed value at the change is harmless)
    reg         blank_v1, blank_v2;
    wire [23:0] fb_rgb;
    always @(posedge vclk) begin blank_v1 <= lock_blank; blank_v2 <= blank_v1; end
    assign rgb = blank_v2 ? 24'd0 : fb_rgb;

    // ------------------------------------------------------------- firmware from the SD card
    // The APF loads data slots while the core is held in reset (before Reset Exit). Bridge words
    // arrive big-endian (bridge_endian_little = 0), so file bytes 0..3 are in [31:24]..[7:0];
    // RISC-V words are little-endian. [VERIFY] on hardware: a wrong order shows as CPU TRAP.
    reg         fw_we;
    reg  [13:0] fw_addr;
    reg  [31:0] fw_data;
    always @(posedge clk) begin
        fw_we <= bridge_wr && bridge_addr[31:16] == 16'h2000;
        fw_addr <= bridge_addr[15:2];
        fw_data <= {bridge_wr_data[7:0], bridge_wr_data[15:8], bridge_wr_data[23:16], bridge_wr_data[31:24]};
    end

    // ------------------------------------------------------------- system
    wire        m_req, m_we, m_ack;
    wire [16:0] m_addr;
    wire [15:0] m_wd, m_rd;
    sram_ctrl u_sram (.clk(clk), .rst(rst), .rd_wait(4'd6), .wr_pulse(4'd5),
        .req(m_req), .we(m_we), .addr(m_addr), .wd(m_wd), .ack(m_ack), .rd(m_rd),
        .sram_a(sram_a), .sram_dq(sram_dq), .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n),
        .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));

    wire [31:0] st_state, st_mode, st_seed, st_density, st_bright, st_visuals, st_reverb, st_sleep;
    wire [31:0] tick_cyc, tick_cyc_max;
    wire        st_valid;
    wire  [2:0] cfg_mode, cfg_density, cfg_bright;
    wire  [1:0] cfg_reverb, cfg_visuals, cfg_sleep;
    wire        cfg_seed_lock, cmd_reseed;
    wire [31:0] cfg_seed, settings_seq;
    settings_bridge u_set (.clk(clk), .rst(rst), .bridge_wr(bridge_wr), .bridge_rd(bridge_rd),
        .bridge_addr(bridge_addr), .bridge_wr_data(bridge_wr_data), .bridge_rd_data(settings_rd_data),
        .st_valid(st_valid), .st_mode(st_mode[2:0]), .st_density(st_density[2:0]), .st_bright(st_bright[2:0]),
        .st_reverb(st_reverb[1:0]), .st_visuals(st_visuals[1:0]), .st_sleep(st_sleep[1:0]), .st_seed(st_seed),
        .cfg_mode(cfg_mode), .cfg_density(cfg_density), .cfg_bright(cfg_bright), .cfg_reverb(cfg_reverb),
        .cfg_visuals(cfg_visuals), .cfg_seed_lock(cfg_seed_lock), .cfg_seed(cfg_seed), .cfg_sleep(cfg_sleep),
        .settings_seq(settings_seq), .cmd_reseed(cmd_reseed), .cmd_lock(cmd_lock));
    reg  [15:0] gap, gap_min, gap_max;
    wire [63:0] tick_n;
    wire [15:0] underruns;
    wire [10:0] fifo_min, fifo_level;
    wire        primed, bus_err, trap;
    cpu_sys #(.FW_HEX(FW_HEX)) u_sys (.clk(clk), .rst(rst),
        .cfg_mode(cfg_mode), .cfg_density(cfg_density), .cfg_bright(cfg_bright), .cfg_reverb(cfg_reverb),
        .cfg_visuals(cfg_visuals), .cfg_seed_lock(cfg_seed_lock), .cfg_seed(cfg_seed), .cfg_sleep(cfg_sleep),
        .cmd_reseed(cmd_reseed), .settings_seq(settings_seq), .rtc_epoch(rtc_epoch), .btn_edge(btn_edge),
        .lock_state({lock_blank, locked}),
        .fw_we(fw_we), .fw_addr(fw_addr), .fw_data(fw_data),
        .vclk(vclk), .vsync(vsync), .vx(vx), .vy(vy), .rgb(fb_rgb), .diag_frame({gap_min, gap_max}),
        .frame_tick(tick), .pcm_l(pcm_l), .pcm_r(pcm_r),
        .mem_req(m_req), .mem_we(m_we), .mem_addr(m_addr), .mem_wd(m_wd), .mem_ack(m_ack), .mem_rd(m_rd),
        .st_state(st_state), .st_mode(st_mode), .st_elapsed(), .st_seed(st_seed), .st_density(st_density),
        .st_bright(st_bright), .st_visuals(st_visuals), .st_overlay(), .st_reverb(st_reverb), .st_sleep(st_sleep),
        .st_valid(st_valid),
        .tick(tick_n), .tick_cycles(tick_cyc), .tick_cycles_max(tick_cyc_max), .underruns(underruns),
        .fifo_min(fifo_min), .fifo_level(fifo_level), .primed(primed), .bus_err(bus_err), .trap(trap),
        .smp_valid(), .smp_l(), .smp_r());

    assign fault = trap || bus_err;

    // ------------------------------------------------------------- frame tick spacing
    // shortest / longest gap between I2S frame ticks in clk_74a cycles (nominal 1546-1547),
    // shown in the Y overlay: the Phase 3 overrun diagnostic (TEST_REPORT)
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

endmodule
`default_nettype wire
