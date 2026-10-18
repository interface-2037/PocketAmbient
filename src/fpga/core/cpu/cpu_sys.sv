// cpu_sys.sv - Phase 4: PicoRV32 composer + 64 KB RAM + MMIO + the audio engine, one clock.
//
// Tick sequence (same order as model/render.c, DECISIONS D4 / D48):
//   CPU: composer_init(), then per tick composer_tick() and a write to CR_TICK_DONE.
//   Sequencer: on TICK_DONE, renders the tick's 48 samples into the audio FIFO (waiting
//   whenever the FIFO is full), then advances the tick counter and publishes the button
//   edges collected during the tick. The CPU polls CR_TICK_LO for the change.
// The engine is idle whenever the CPU runs, so CPU register writes go straight into it and
// status reads see the state at the tick boundary: no shadow registers needed.
//
// Audio FIFO: 1024 stereo frames. The I2S side pops one frame per frame tick once the FIFO
// has first filled to PRIME frames (silence before that); an empty FIFO at a pop is an
// underrun (counted, last frame repeated). The firmware's worst tick needs ~90 frames of
// lead (model/rvsim.c), so PRIME = 512 leaves a wide margin.
//
// MMIO (fw/hal/regs.h): voice writes 0x1000_0000, voice STATUS reads (word 31; other voice
// words read 0), FX writes 0x1000_1000 (word 31 reads the fade gain, others 0), control block
// 0x1000_2000, visual events 0x1000_3000 (accepted and dropped), video control 0x1000_4000 and
// the back framebuffer 0x3000_0000 (words) / 0x3100_0000 (pixels) (fb_video, D52). Any other
// address sets bus_err.
`default_nettype none

module cpu_sys #(
    parameter FW_HEX = "core/cpu/composer.hex",
    parameter [10:0] PRIME = 11'd512
) (
    input  wire        clk,
    input  wire        rst,
    // settings (bridge / menu in Phase 5)
    input  wire  [2:0] cfg_mode,
    input  wire  [2:0] cfg_density,
    input  wire  [2:0] cfg_bright,
    input  wire  [1:0] cfg_reverb,
    input  wire  [1:0] cfg_visuals,
    input  wire        cfg_seed_lock,
    input  wire [31:0] cfg_seed,
    input  wire  [1:0] cfg_sleep,
    input  wire        cmd_reseed,         // pulse
    input  wire [31:0] settings_seq,
    input  wire [31:0] rtc_epoch,
    input  wire [15:0] btn_edge,           // pressed-edge pulses (debounced), regs.h BTN_* bits
    input  wire  [1:0] lock_state,         // Pocket lock (D62): bit0 locked, bit1 lock screen showing
    // firmware load (dev builds: APF data slot over the bridge, D50); used only while the CPU
    // is held in reset, otherwise ignored
    input  wire        fw_we,
    input  wire [13:0] fw_addr,            // word address in the 64 KB RAM
    input  wire [31:0] fw_data,            // little-endian CPU word
    // video (fb_video.sv; the CPU draws the screens, fw/ui)
    input  wire        vclk,
    input  wire        vsync,              // vclk pulse at the start of a frame
    input  wire  [9:0] vx,                 // visible x / y three vclks ahead
    input  wire  [9:0] vy,
    output wire [23:0] rgb,
    input  wire [31:0] diag_frame,         // CR_DIAG_FRAME: shortest / longest frame-tick gap
    // audio out
    input  wire        frame_tick,         // one pulse per 48 kHz frame
    output reg  [15:0] pcm_l,
    output reg  [15:0] pcm_r,
    // SRAM port (to sram_ctrl)
    output wire        mem_req,
    output wire        mem_we,
    output wire [16:0] mem_addr,
    output wire [15:0] mem_wd,
    input  wire        mem_ack,
    input  wire [15:0] mem_rd,
    // composer status (CR_ST_*, for video / bridge)
    output wire [31:0] st_state,
    output wire [31:0] st_mode,
    output wire [31:0] st_elapsed,
    output wire [31:0] st_seed,
    output wire [31:0] st_density,
    output wire [31:0] st_bright,
    output wire [31:0] st_visuals,
    output wire [31:0] st_overlay,
    output wire [31:0] st_reverb,
    output wire [31:0] st_sleep,
    output reg         st_valid = 1'b0,    // the composer has published its status (tick > 0)
    // diagnostics
    output reg  [63:0] tick,
    output reg  [31:0] tick_cycles,        // CPU cycles of the last tick (incl. composer_init at 0)
    output reg  [31:0] tick_cycles_max,
    output reg  [15:0] underruns,
    output reg  [10:0] fifo_min,           // lowest FIFO level since it primed
    output reg  [10:0] fifo_level,
    output reg         primed,
    output reg         bus_err,
    output wire        trap,
    // full-system testbench taps
    output wire        smp_valid,          // a sample entered the FIFO
    output wire [15:0] smp_l,
    output wire [15:0] smp_r
);
    // ------------------------------------------------------------- engine
    wire        eng_ready, eng_busy, eng_done;
    wire [31:0] st_status, fx_gain;
    wire signed [15:0] out_l, out_r;
    reg         eng_start;
    reg         ewr_en, ewr_fx;
    reg   [9:0] ewr_addr;
    reg  [31:0] ewr_data;
    reg   [4:0] st_voice;
    audio_engine u_eng (.clk(clk), .rst(rst),
        .wr_en(ewr_en), .wr_fx(ewr_fx), .wr_addr(ewr_addr), .wr_data(ewr_data),
        .start(eng_start), .busy(eng_busy), .done(eng_done), .ready(eng_ready),
        .bus_l(), .bus_r(), .bus_rev(), .bus_echo(), .out_l(out_l), .out_r(out_r),
        .st_voice(st_voice), .st_status(st_status), .fx_gain(fx_gain),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wd(mem_wd),
        .mem_ack(mem_ack), .mem_rd(mem_rd));

    // ------------------------------------------------------------- CPU
    // held in reset until the engine has cleared its RAMs and the SRAM
    reg         cpu_rstn;
    always @(posedge clk) cpu_rstn <= !rst && eng_ready;

    wire        mem_valid, mem_instr;
    wire [31:0] mem_addr_c, mem_wdata;
    wire  [3:0] mem_wstrb;
    reg         mem_ready;
    reg  [31:0] mem_rdata;                  // I/O read data
    reg         ram_rd;                     // this ready cycle returns RAM data
    reg  [31:0] ram_q;
    wire [31:0] cpu_rdata = ram_rd ? ram_q : mem_rdata;
    picorv32 #(
        .ENABLE_COUNTERS(0), .ENABLE_COUNTERS64(0), .ENABLE_REGS_16_31(1), .ENABLE_REGS_DUALPORT(1),
        .LATCHED_MEM_RDATA(0), .TWO_STAGE_SHIFT(1), .BARREL_SHIFTER(1), .COMPRESSED_ISA(1),
        .CATCH_MISALIGN(1), .CATCH_ILLINSN(1), .ENABLE_PCPI(0), .ENABLE_MUL(0), .ENABLE_FAST_MUL(1),
        .ENABLE_DIV(1), .ENABLE_IRQ(0), .ENABLE_TRACE(0), .PROGADDR_RESET(32'h0)
    ) u_cpu (.clk(clk), .resetn(cpu_rstn), .trap(trap),
        .mem_valid(mem_valid), .mem_instr(mem_instr), .mem_ready(mem_ready),
        .mem_addr(mem_addr_c), .mem_wdata(mem_wdata), .mem_wstrb(mem_wstrb), .mem_rdata(cpu_rdata),
        .mem_la_read(), .mem_la_write(), .mem_la_addr(), .mem_la_wdata(), .mem_la_wstrb(),
        .pcpi_valid(), .pcpi_insn(), .pcpi_rs1(), .pcpi_rs2(), .pcpi_wr(1'b0), .pcpi_rd(32'd0),
        .pcpi_wait(1'b0), .pcpi_ready(1'b0), .irq(32'd0), .eoi(), .trace_valid(), .trace_data());

    // ------------------------------------------------------------- 64 KB RAM (byte enables)
    reg  [3:0][7:0] ram [0:16383];
    initial $readmemh(FW_HEX, ram);
    wire        sel_ram = mem_addr_c[31:16] == 16'h0000;
    wire        ram_we_ok = mem_valid && !mem_ready && sel_ram;
    // one port: the firmware loader while the CPU is in reset, else the CPU
    wire        ld = fw_we && !cpu_rstn;
    wire [13:0] ram_a = ld ? fw_addr : mem_addr_c[15:2];
    wire  [3:0] ram_be = ld ? 4'hF : ram_we_ok ? mem_wstrb : 4'h0;
    wire [31:0] ram_wd = ld ? fw_data : mem_wdata;
    always @(posedge clk) begin
        if (ram_be[0]) ram[ram_a][0] <= ram_wd[7:0];
        if (ram_be[1]) ram[ram_a][1] <= ram_wd[15:8];
        if (ram_be[2]) ram[ram_a][2] <= ram_wd[23:16];
        if (ram_be[3]) ram[ram_a][3] <= ram_wd[31:24];
        ram_q <= ram[ram_a];
    end

    // ------------------------------------------------------------- framebuffers + palette
    wire        fbw = mem_valid && !mem_ready && mem_addr_c[31:24] == 8'h30;   // word window
    wire        fbp = mem_valid && !mem_ready && mem_addr_c[31:24] == 8'h31;   // pixel window
    wire        vid = mem_valid && !mem_ready && mem_addr_c[31:8] == 24'h100040;
    wire        fb_ok = fbw ? (mem_addr_c[23:15] == 9'd0 && mem_addr_c[14:2] < 13'd5760 && &mem_wstrb == |mem_wstrb)
                            : (mem_addr_c[23:20] == 4'd0 && mem_addr_c[10:2] < 9'd320 && mem_addr_c[19:11] < 9'd288 && |mem_wstrb);
    reg         fb_wait;
    wire        fb_req = (fbw || fbp) && fb_ok && !fb_wait;
    wire        fb_ready, swap_pending;
    wire [31:0] fb_rdata;
    wire [15:0] vsyncs;
    fb_video u_vid (.clk(clk), .rst(rst),
        .req(fb_req), .we(|mem_wstrb), .pixel(fbp), .word(mem_addr_c[14:2]), .px(mem_addr_c[10:2]),
        .py(mem_addr_c[19:11]), .wdata(mem_wdata), .rdata(fb_rdata), .ready(fb_ready),
        .pal_we(vid && |mem_wstrb && mem_addr_c[7:4] == 4'd0), .pal_idx(mem_addr_c[3:2]), .pal_data(mem_wdata[23:0]),
        .swap(vid && |mem_wstrb && mem_addr_c[7:2] == 6'd4), .swap_pending(swap_pending), .vsyncs(vsyncs),
        .vclk(vclk), .vsync(vsync), .vx(vx), .vy(vy), .rgb(rgb));

    // ------------------------------------------------------------- control block
    reg  [31:0] st_r [16:25];
    reg  [15:0] btn_pend, btn_evt;
    reg         cmd_pend, cmd_evt;
    assign st_state = st_r[16];
    assign st_mode = st_r[17];
    assign st_elapsed = st_r[18];
    assign st_seed = st_r[19];
    assign st_density = st_r[20];
    assign st_bright = st_r[21];
    assign st_visuals = st_r[22];
    assign st_overlay = st_r[23];
    assign st_reverb = st_r[24];
    assign st_sleep = st_r[25];
    // st_valid == (tick != 0), kept as a flag: the 64-bit compare fed the menu's write logic
    // (settings_bridge) in the same cycle and was the slowest CPU-side path in v0.4.0

    function automatic [31:0] ctrl_rd(input [4:0] r);
        case (r)
            5'd0:  ctrl_rd = {29'd0, cfg_mode};
            5'd1:  ctrl_rd = {29'd0, cfg_density};
            5'd2:  ctrl_rd = {29'd0, cfg_bright};
            5'd3:  ctrl_rd = {30'd0, cfg_reverb};
            5'd4:  ctrl_rd = {30'd0, cfg_visuals};
            5'd5:  ctrl_rd = {31'd0, cfg_seed_lock};
            5'd6:  ctrl_rd = cfg_seed;
            5'd7:  ctrl_rd = {30'd0, cfg_sleep};
            5'd8:  ctrl_rd = {31'd0, cmd_evt};
            5'd9:  ctrl_rd = settings_seq;
            5'd10: ctrl_rd = {16'd0, btn_evt};
            5'd11: ctrl_rd = rtc_epoch;
            5'd12: ctrl_rd = tick[31:0];
            5'd13: ctrl_rd = tick[63:32];
            5'd15: ctrl_rd = tick_cycles;
            5'd26: ctrl_rd = tick_cycles_max;
            5'd27: ctrl_rd = {21'd0, fifo_min};
            5'd28: ctrl_rd = {16'd0, underruns};
            5'd29: ctrl_rd = diag_frame;
            5'd30: ctrl_rd = {30'd0, lock_state};
            5'd16, 5'd17, 5'd18, 5'd19, 5'd20, 5'd21, 5'd22, 5'd23, 5'd24, 5'd25: ctrl_rd = st_r[r];
            default: ctrl_rd = 32'd0;
        endcase
    endfunction

    // ------------------------------------------------------------- bus + sequencer
    localparam [1:0] Q_CPU = 2'd0, Q_START = 2'd1, Q_WAIT = 2'd2;
    reg   [1:0] q;
    reg   [5:0] n_left;                     // samples left in this tick
    reg   [1:0] bwait;                      // status-read latency
    reg  [31:0] cyc;
    wire        fifo_full;

    wire        io = mem_valid && !mem_ready && mem_addr_c[31:28] == 4'h1;
    wire  [1:0] io_blk = mem_addr_c[13:12];
    wire        io_wr = |mem_wstrb;
    wire        io_ok = mem_addr_c[27:14] == 14'd0 && (io_blk == 2'd3 ? mem_addr_c[11:0] == 12'd0 :
                        io_blk == 2'd0 ? 1'b1 : mem_addr_c[11:8] == 4'd0);

    always @(posedge clk) begin
        mem_ready <= 1'b0;
        ram_rd <= 1'b0;
        ewr_en <= 1'b0;
        eng_start <= 1'b0;
        if (rst) begin
            q <= Q_CPU; tick <= 64'd0; st_valid <= 1'b0; cyc <= 32'd0; tick_cycles <= 32'd0; tick_cycles_max <= 32'd0;
            btn_pend <= 16'd0; btn_evt <= 16'd0; cmd_pend <= 1'b0; cmd_evt <= 1'b0;
            bwait <= 2'd0; bus_err <= 1'b0; fb_wait <= 1'b0;
        end else begin
            btn_pend <= btn_pend | btn_edge;
            cmd_pend <= cmd_pend | cmd_reseed;
            if (q == Q_CPU && cpu_rstn) cyc <= cyc + 32'd1;

            // RAM: registered read, ready (with ram_q) in the next cycle
            if (mem_valid && !mem_ready && sel_ram) begin mem_ready <= 1'b1; ram_rd <= 1'b1; end

            if (fb_req) fb_wait <= 1'b1;
            if (fb_ready) begin mem_ready <= 1'b1; mem_rdata <= fb_rdata; fb_wait <= 1'b0; end
            if ((fbw || fbp) && !fb_ok) begin bus_err <= 1'b1; mem_rdata <= 32'd0; mem_ready <= 1'b1; end
            if (vid) begin                                     // video control (VC_*)
                mem_ready <= 1'b1;
                mem_rdata <= mem_addr_c[7:2] == 6'd5 ? {vsyncs, 15'd0, swap_pending} : 32'd0;
            end else if (io) begin
                if (!io_ok) begin
                    bus_err <= 1'b1; mem_rdata <= 32'd0; mem_ready <= 1'b1;
                end else if (io_wr) begin
                    case (io_blk)
                        2'd0, 2'd1: if (!eng_busy) begin       // engine idle between ticks
                            ewr_en <= 1'b1; ewr_fx <= io_blk[0];
                            ewr_addr <= io_blk[0] ? {5'd0, mem_addr_c[6:2]} : mem_addr_c[11:2];
                            ewr_data <= mem_wdata;
                            mem_ready <= 1'b1;
                        end
                        2'd2: begin
                            mem_ready <= 1'b1;
                            if (mem_addr_c[6:2] >= 5'd16 && mem_addr_c[6:2] <= 5'd25) st_r[mem_addr_c[6:2]] <= mem_wdata;
                            if (mem_addr_c[6:2] == 5'd14 && q == Q_CPU) begin   // CR_TICK_DONE
                                q <= Q_START; n_left <= 6'd48;
                                tick_cycles <= cyc;
                                if (cyc > tick_cycles_max) tick_cycles_max <= cyc;
                            end
                        end
                        default: mem_ready <= 1'b1;              // visual event: dropped for now
                    endcase
                end else begin
                    case (io_blk)
                        2'd0: begin                              // voice status: 2-cycle RAM read
                            st_voice <= mem_addr_c[11:7];
                            bwait <= bwait + 2'd1;
                            if (bwait == 2'd3) begin
                                bwait <= 2'd0; mem_ready <= 1'b1;
                                mem_rdata <= mem_addr_c[6:2] == 5'd31 ? st_status : 32'd0;
                            end
                        end
                        2'd1: begin
                            mem_ready <= 1'b1; mem_rdata <= mem_addr_c[6:2] == 5'd31 ? fx_gain : 32'd0;
                        end
                        2'd2: begin
                            mem_ready <= 1'b1;
                            mem_rdata <= ctrl_rd(mem_addr_c[6:2]);
                            if (mem_addr_c[6:2] == 5'd10) btn_evt <= 16'd0;   // clear-on-read
                            if (mem_addr_c[6:2] == 5'd8) cmd_evt <= 1'b0;
                        end
                        default: begin mem_ready <= 1'b1; mem_rdata <= 32'd0; end
                    endcase
                end
            end else if (mem_valid && !mem_ready && !sel_ram && !fbw && !fbp) begin
                bus_err <= 1'b1; mem_rdata <= 32'd0; mem_ready <= 1'b1;
            end

            // sequencer: 48 samples per tick
            case (q)
                Q_START: if (!fifo_full && !eng_busy) begin eng_start <= 1'b1; q <= Q_WAIT; end
                Q_WAIT: if (eng_done) begin
                    if (n_left == 6'd1) begin
                        q <= Q_CPU; cyc <= 32'd0;
                        tick <= tick + 64'd1;
                        st_valid <= 1'b1;
                        btn_evt <= btn_evt | btn_pend | btn_edge; btn_pend <= 16'd0;
                        cmd_evt <= cmd_evt | cmd_pend | cmd_reseed; cmd_pend <= 1'b0;
                    end else begin
                        n_left <= n_left - 6'd1; q <= Q_START;
                    end
                end
                default: ;
            endcase
        end
    end

    // ------------------------------------------------------------- audio FIFO
    reg  [31:0] fifo [0:1023];
    reg   [9:0] wp, rp;
    reg  [31:0] fifo_q;
    assign fifo_full = fifo_level >= 11'd1023;      // one slot spare for the sample in flight
    assign smp_valid = eng_done;
    assign smp_l = out_l;
    assign smp_r = out_r;
    wire        push = eng_done;
    wire        pop = frame_tick && primed && fifo_level != 11'd0;
    always @(posedge clk) begin
        if (push) fifo[wp] <= {out_l, out_r};
        fifo_q <= fifo[rp];
    end
    reg pop_d;
    always @(posedge clk) begin
        pop_d <= pop;
        if (rst) begin
            wp <= 10'd0; rp <= 10'd0; fifo_level <= 11'd0; primed <= 1'b0; underruns <= 16'd0;
            fifo_min <= 11'h7FF; pcm_l <= 16'd0; pcm_r <= 16'd0;
        end else begin
            if (push) wp <= wp + 10'd1;
            if (pop) rp <= rp + 10'd1;
            fifo_level <= fifo_level + {10'd0, push} - {10'd0, pop};
            if (fifo_level >= PRIME) primed <= 1'b1;
            if (frame_tick && primed) begin
                if (fifo_level == 11'd0) underruns <= (underruns == 16'hFFFF) ? underruns : underruns + 16'd1;
                if (fifo_level < fifo_min) fifo_min <= fifo_level;
            end
            if (pop_d) begin pcm_l <= fifo_q[31:16]; pcm_r <= fifo_q[15:0]; end
        end
    end
endmodule
`default_nettype wire
