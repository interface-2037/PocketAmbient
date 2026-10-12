// tb_ap_top.sv - integration test of ap_top (the core below core_top): boot through
// the pin-level 55 ns SRAM model (engine clears the SRAM), CPU runs composer_init and the
// first ticks with 48 kHz frame ticks, autostart press of A; checks that the first samples equal
// the model's (render --mode drift --seed 123, in ref.raw) and that the menu reads back.
//   vvp tb_ap_top.vvp [+ms=N]
// Compiled with -P tb_ap_top.SLOT=1 '-Ptb_ap_top.FW="../../sim/out/zero.hex"' the CPU RAM
// starts empty and the firmware is
// streamed in over the bridge while reset_n is low, like an APF data slot (big-endian words).
`timescale 1ns/1ps
module tb_ap_top #(parameter SLOT = 0, parameter FW = "core/cpu/composer.hex");
    reg clk = 0, reset_n = 0, vclk = 0;
    wire fault;
    always #6.734 clk = ~clk;
    always #40.69 vclk = ~vclk;
    reg  [31:0] keys = 0;
    reg         tick = 0;
    wire [15:0] pcm_l, pcm_r;
    wire [16:0] sram_a;
    wire [15:0] sram_dq;
    wire        sram_oe_n, sram_we_n, sram_ub_n, sram_lb_n;

    reg         bridge_wr = 0, bridge_rd = 0;
    reg  [31:0] bridge_addr = 0, bridge_wr_data = 0;
    wire [31:0] settings_rd_data;
    reg  [31:0] fw [0:16383];
    initial $readmemh("core/cpu/composer.hex", fw);

    ap_top #(.FW_HEX(FW)) dut (.clk(clk), .reset_n(reset_n),
        .bridge_wr(bridge_wr), .bridge_rd(bridge_rd), .bridge_addr(bridge_addr), .bridge_wr_data(bridge_wr_data),
        .settings_rd_data(settings_rd_data), .rtc_epoch(32'd1760000000), .cont1_key(keys),
        .vclk(vclk), .vsync(1'b0), .vx(10'd0), .vy(10'd0), .rgb(), .fault(fault), .tick(tick), .pcm_l(pcm_l), .pcm_r(pcm_r),
.sram_a(sram_a), .sram_dq(sram_dq),
        .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n), .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));
    async_sram_model u_mem (.a(sram_a), .dq(sram_dq), .oe_n(sram_oe_n), .we_n(sram_we_n),
        .ub_n(sram_ub_n), .lb_n(sram_lb_n), .host_drive(dut.u_sram.dq_oe));

    // 48 kHz frame ticks from a fractional accumulator, like core_top
    integer acc = 0;
    always @(posedge clk) begin
        tick <= 1'b0;
        acc = acc + 8;
        if (acc >= 12375) begin acc = acc - 12375; tick <= 1'b1; end    // 74.25 MHz / 48 kHz = 12375 / 8
    end

    // A pressed during tick 0 (visible to composer_tick(1)), like render's autostart
    always @(posedge clk) if (dut.tick_n == 64'd0 && dut.u_sys.cpu_rstn) keys[4] <= 1'b1;
                          else if (dut.tick_n >= 64'd2) keys[4] <= 1'b0;

    integer fr, n, bad, i, r, ms;
    reg [15:0] el, er;
    initial begin
        if (!$value$plusargs("ms=%d", ms)) ms = 30;
        fr = $fopen("ref.raw", "rb");
        if ($test$plusargs("ref")) begin end
        bad = 0; n = 0;
        repeat (5) @(posedge clk);
        if (SLOT) begin                          // data slot load, one bridge write every 4 clocks
            for (i = 0; i < 16384; i = i + 1) begin
                @(negedge clk);
                bridge_addr = 32'h2000_0000 + 4 * i;
                bridge_wr_data = {fw[i][7:0], fw[i][15:8], fw[i][23:16], fw[i][31:24]};
                bridge_wr = 1;
                @(negedge clk) bridge_wr = 0;
                repeat (2) @(negedge clk);
            end
            $display("firmware loaded over the bridge");
        end
        // persisted menu values, written by the APF before Reset Exit: Lock seed on, seed 123
        @(negedge clk) begin bridge_addr = 32'h5000_0014; bridge_wr_data = 1; bridge_wr = 1; end
        @(negedge clk) bridge_wr = 0;
        @(negedge clk) begin bridge_addr = 32'h5000_0018; bridge_wr_data = 123; bridge_wr = 1; end
        @(negedge clk) bridge_wr = 0;
        reset_n = 1;
        wait (dut.u_sys.cpu_rstn);
        $display("engine ready (SRAM cleared) at %0t ns", $time / 1000);
        while (dut.tick_n < ms) begin
            @(posedge clk);
            if (dut.u_sys.smp_valid) begin
                el = {$fgetc(fr), 8'd0}; el = {$fgetc(fr), el[15:8]};
                er = {$fgetc(fr), 8'd0}; er = {$fgetc(fr), er[15:8]};
                if (dut.u_sys.smp_l !== el || dut.u_sys.smp_r !== er) begin
                    if (bad < 5) $display("MISMATCH sample %0d: rtl %h %h model %h %h", n, dut.u_sys.smp_l, dut.u_sys.smp_r, el, er);
                    bad = bad + 1;
                end
                n = n + 1;
            end
            if (fault) begin $display("FAIL: trap %b bus_err %b", dut.u_sys.trap, dut.u_sys.bus_err); $finish; end
        end
        repeat (200) @(posedge clk);
        // the menu reads the composer's live values
        @(negedge clk) begin bridge_addr = 32'h5000_0018; bridge_rd = 1; end
        @(negedge clk) bridge_rd = 0;
        $display("menu: seed register reads %0d", settings_rd_data);
        if (settings_rd_data != 123) bad = bad + 1;
        if (bad == 0 && n == ms * 48 && u_mem.errors == 0)
            $display("PASS ap_top: %0d samples bit-exact through the SRAM pins, CPU max %0d cycles/tick, frame gap %0d..%0d",
                     n, dut.u_sys.tick_cycles_max, dut.gap_min, dut.gap_max);
        else $display("FAIL ap_top: %0d mismatches in %0d samples, %0d SRAM timing errors", bad, n, u_mem.errors);
        $finish;
    end
endmodule
