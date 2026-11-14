// tb_mute.sv - the output mute of cpu_sys (D67), through ap_top and the pin-level SRAM model.
//   cd src/fpga && iverilog -g2012 -o ../../sim/out/tb_mute.vvp ../../sim/tb/tb_mute.sv \
//       ../../sim/tb/async_sram_model.sv core/ap_top.sv core/pocket_lock.sv core/settings_bridge.sv \
//       core/slot_size.sv core/cpu/cpu_sys.sv core/cpu/picorv32.v core/ui/fb_video.sv \
//       core/synth/audio_engine.sv core/synth/voice_engine.sv core/synth/fx_engine.sv core/synth/sram_ctrl.sv
//   && vvp -n ../../sim/out/tb_mute.vvp
// 1. Boot and play until the FIFO has primed.
// 2. Stall: the sequencer is kept from starting the engine, so the FIFO drains. While it is empty
//    the last frame is held (a large value is planted there); after MUTE_FRAMES empty frames the
//    output must mute and decay smoothly to 0 (not jump), and stay 0.
// 3. Release: the FIFO must re-prime, the mute clear and the output follow the FIFO again.
// 4. Fault: a bus error mutes at once and stays muted although the FIFO is full.
`timescale 1ns/1ps
module tb_mute;
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
    reg         bridge_wr = 0;
    reg  [31:0] bridge_addr = 0, bridge_wr_data = 0;

    ap_top dut (.clk(clk), .reset_n(reset_n),
        .bridge_wr(bridge_wr), .bridge_rd(1'b0), .bridge_addr(bridge_addr), .bridge_wr_data(bridge_wr_data),
        .settings_rd_data(), .ss_snap(1'b0), .ss_restore(1'b0), .rtc_epoch(32'd1760000000), .cont1_key(keys),
        .vclk(vclk), .vsync(1'b0), .vx(10'd0), .vy(10'd0), .rgb(), .fault(fault), .tick(tick), .pcm_l(pcm_l), .pcm_r(pcm_r),
        .sram_a(sram_a), .sram_dq(sram_dq),
        .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n), .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));
    async_sram_model u_mem (.a(sram_a), .dq(sram_dq), .oe_n(sram_oe_n), .we_n(sram_we_n),
        .ub_n(sram_ub_n), .lb_n(sram_lb_n), .host_drive(dut.u_sram.dq_oe));

    integer acc = 0;
    always @(posedge clk) begin
        tick <= 1'b0;
        acc = acc + 8;
        if (acc >= 12375) begin acc = acc - 12375; tick <= 1'b1; end
    end
    always @(posedge clk) if (dut.tick_n == 64'd0 && dut.u_sys.cpu_rstn) keys[4] <= 1'b1;
                          else if (dut.tick_n >= 64'd2) keys[4] <= 1'b0;

    integer bad = 0, fcount, i, prev_l, prev_r, cur_l, cur_r;
    task frames(input integer k); begin fcount = 0; while (fcount < k) begin @(posedge clk); if (tick) fcount = fcount + 1; end end endtask
    task fail(input [8*48-1:0] what); begin $display("FAIL: %0s", what); bad = bad + 1; end endtask
    task bwr(input [31:0] a, input [31:0] x);
        begin @(negedge clk); bridge_addr = a; bridge_wr_data = x; bridge_wr = 1; @(negedge clk) bridge_wr = 0; end
    endtask

    // watchdog: a hang prints where it was instead of running for hours
    initial begin
        #(64'd400_000_000_000);   // 400 ms of simulated time
        $display("FAIL: watchdog at %0t us, primed %b muted %b FIFO %0d", $time / 1000000,
                 dut.u_sys.primed, dut.u_sys.muted, dut.u_sys.fifo_level);
        $finish;
    end

    initial begin
        repeat (5) @(posedge clk);
        bwr(32'h5000_0014, 1); bwr(32'h5000_0018, 123);           // Lock seed on, seed 123
        reset_n = 1;
        wait (dut.u_sys.primed);
        frames(50);
        if (dut.u_sys.muted) fail("muted while playing");
        $display("primed and playing at %0t us, FIFO %0d", $time / 1000000, dut.u_sys.fifo_level);

        // ---- 2. stall
        $display("phase 2: stall at %0t us", $time / 1000000);
        force dut.u_sys.eng_start = 1'b0;
        wait (dut.u_sys.fifo_level == 0);
        frames(2);
        force dut.u_sys.pcm_l = 16'h7000; force dut.u_sys.pcm_r = 16'h9000;   // a loud held frame
        @(posedge clk); release dut.u_sys.pcm_l; release dut.u_sys.pcm_r;
        frames(400);
        if (dut.u_sys.muted) fail("muted before MUTE_FRAMES");
        if (pcm_l !== 16'h7000 || pcm_r !== 16'h9000) fail("held frame changed before the mute");
        wait (dut.u_sys.muted);
        $display("muted after the stall at %0t us", $time / 1000000);
        if (dut.u_sys.primed) fail("still primed after the stall mute");
        prev_l = $signed(pcm_l); prev_r = $signed(pcm_r);
        for (i = 0; i < 1000; i = i + 1) begin
            frames(1); @(posedge clk);
            cur_l = $signed(pcm_l); cur_r = $signed(pcm_r);
            if (cur_l > prev_l || cur_l < 0 || cur_r < prev_r || cur_r > 0) fail("decay not monotonic toward 0");
            if (prev_l - cur_l > 16'h0100 || cur_r - prev_r > 16'h0100) fail("decay step too large (click)");
            if (i == 50 && cur_l < 16'h4000) fail("decay too fast");
            prev_l = cur_l; prev_r = cur_r;
        end
        if (pcm_l !== 16'd0 || pcm_r !== 16'd0) fail("output not 0 after the decay");

        // ---- 3. release
        $display("phase 3: release at %0t us", $time / 1000000);
        release dut.u_sys.eng_start;
        wait (dut.u_sys.primed);
        frames(2);
        if (dut.u_sys.muted) fail("still muted after re-priming");
        frames(20);
        if (dut.u_sys.fifo_level == 0) fail("FIFO not refilled");
        $display("re-primed and unmuted at %0t us, FIFO %0d", $time / 1000000, dut.u_sys.fifo_level);

        // ---- 4. fault
        $display("phase 4: fault at %0t us", $time / 1000000);
        force dut.u_sys.bus_err = 1'b1; @(posedge clk); release dut.u_sys.bus_err;   // sticky reg
        frames(2);
        if (!dut.u_sys.muted) fail("not muted on a fault");
        if (!fault) fail("fault output low");
        frames(1000);
        if (!dut.u_sys.muted || pcm_l !== 16'd0 || pcm_r !== 16'd0) fail("fault mute did not hold at 0");

        if (bad == 0) $display("PASS mute: stall mutes after MUTE_FRAMES with a smooth decay, re-primes, fault mutes");
        else $display("FAIL mute: %0d errors", bad);
        $finish;
    end
endmodule
