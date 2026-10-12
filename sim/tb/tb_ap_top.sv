// tb_ap_top.sv - integration test of ap_top (the core below core_top): boot through
// the pin-level 55 ns SRAM model (engine clears the SRAM), CPU runs composer_init and the
// first ticks with 48 kHz frame ticks, autostart press of A; checks that the first samples equal
// the model's (render --mode drift --seed 123, in ref.raw) and that the menu reads back.
//   vvp tb_ap_top.vvp [+ms=N]
// Compiled with -P tb_ap_top.SLOT=1 '-Ptb_ap_top.FW="../../sim/out/zero.hex"' the CPU RAM
// starts empty and the firmware is
// streamed in over the bridge while reset_n is low, like an APF data slot (big-endian words).
// With -P tb_ap_top.RESTORE=1 the boot-restore path (D57, D58, D59) is tested instead: the APF's
// menu file from its stale cache (DRIFT, density 5, brightness 3, reverb High, Lock seed on; no
// Seed) and the save slot (TIDES, density 4, brightness 2, reverb High, lock on, seed 4242) are
// written before Reset Exit. The save file's settings win: the menu must read them back and the
// first samples must equal render --mode tides --seed 4242 --density 4 --brightness 2 --reverb 2.
// Then Down lowers the density to 3 and, after reset entry, APF-style reads (word taken after
// the address has moved on) of the menu and the save slot must return the current settings.
`timescale 1ns/1ps
module tb_ap_top #(parameter SLOT = 0, parameter RESTORE = 0, parameter FW = "core/cpu/composer.hex");
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

    integer fr, n, bad, i, r, ms, cmax, gmin, gmax;
    reg [15:0] el, er;
    reg [31:0] v, f [0:7];
    task bwr(input [31:0] a, input [31:0] x);
        begin @(negedge clk); bridge_addr = a; bridge_wr_data = x; bridge_wr = 1; @(negedge clk) bridge_wr = 0; end
    endtask
    task brd(input [31:0] a, output [31:0] x);       // APF read: strobe, address moves on, word later
        begin
            @(negedge clk); bridge_addr = a; bridge_rd = 1;
            @(negedge clk) bridge_rd = 0; bridge_addr = 32'hF800_0000;
            repeat (4) @(negedge clk);
            x = settings_rd_data;
        end
    endtask
    task want_eq(input [31:0] got, input [31:0] want, input [8*24-1:0] what);
        begin
            if (got !== want) begin $display("FAIL: %0s reads %h, expected %h", what, got, want); bad = bad + 1; end
        end
    endtask
    function [31:0] chk(input [31:0] w0, w1, w2, w3, w4, w5, w6);   // save file check word
        chk = ~(32'h5A17_C3E9 + w0 + {w1[30:0], w1[31]} + {w2[29:0], w2[31:30]} + {w3[28:0], w3[31:29]}
                + {w4[27:0], w4[31:28]} + {w5[26:0], w5[31:27]} + {w6[25:0], w6[31:26]});
    endfunction
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
        if (RESTORE) begin
            // the APF's menu file (stale; no Seed in it) and the save slot, before Reset Exit
            bwr(32'h5000_0000, 0); bwr(32'h5000_0004, 5); bwr(32'h5000_0008, 3); bwr(32'h5000_000C, 2);
            bwr(32'h5000_0014, 1);
            f[0] = 32'h414D_4253; f[1] = 32'h0100_0001; f[2] = 32'd4242; f[3] = 32'h0000_2241;
            f[4] = 0; f[5] = 0; f[6] = 0; f[7] = chk(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
            for (i = 0; i < 8; i = i + 1) bwr(32'h6000_0000 + 4 * i, f[i]);
        end else begin
            // persisted menu values, written by the APF before Reset Exit: Lock seed on, seed 123
            bwr(32'h5000_0014, 1);
            bwr(32'h5000_0018, 123);
        end
        reset_n = 1;
        if (RESTORE) begin
            // after Reset Exit the menu reads the save file's settings (D59)
            repeat (4) @(negedge clk);
            brd(32'h5000_0000, v); want_eq(v, 1, "boot: mode");
            brd(32'h5000_0004, v); want_eq(v, 4, "boot: density");
            brd(32'h5000_0008, v); want_eq(v, 2, "boot: brightness");
            brd(32'h5000_000C, v); want_eq(v, 2, "boot: reverb");
        end
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
        cmax = dut.u_sys.tick_cycles_max; gmin = dut.gap_min; gmax = dut.gap_max;   // before any reset
        if (RESTORE) begin
            // Down with the buttons: density 4 -> 3 (composer status), then Quit: reset, then reads
            @(negedge clk) keys[1] = 1'b1;
            r = dut.tick_n; while (dut.tick_n < r + 3) @(posedge clk);
            @(negedge clk) keys[1] = 1'b0;
            r = dut.tick_n; while (dut.tick_n < r + 3) @(posedge clk);
            want_eq(dut.st_density, 3, "status density");
            want_eq(dut.st_seed, 4242, "status seed");
            @(negedge clk) reset_n = 0;
            repeat (100) @(negedge clk);
            brd(32'h5000_0000, v); want_eq(v, 1, "quit: mode");
            brd(32'h5000_0004, v); want_eq(v, 3, "quit: density");
            brd(32'h5000_0008, v); want_eq(v, 2, "quit: brightness");
            brd(32'h5000_000C, v); want_eq(v, 2, "quit: reverb");
            brd(32'h5000_0010, v); want_eq(v, 0, "quit: visuals");
            brd(32'h5000_001C, v); want_eq(v, 0, "quit: sleep");
            brd(32'h5000_0014, v); want_eq(v, 1, "quit: lock seed");
            brd(32'h5000_0018, v); want_eq(v, 4242, "quit: seed");
            f[3] = 32'h0000_2231; f[7] = chk(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
            for (i = 0; i < 8; i = i + 1) begin brd(32'h6000_0000 + 4 * i, v); want_eq(v, f[i], "quit: save word"); end
            $display("quit reads: menu TIDES / 3 / 2 / High / lock / seed 4242, save file %h %h %h %h .. %h",
                     f[0], f[1], f[2], f[3], f[7]);
        end else begin
            // the menu reads the composer's live values
            brd(32'h5000_0018, v);
            $display("menu: seed register reads %0d", v);
            if (v != 123) bad = bad + 1;
        end
        if (bad == 0 && n == ms * 48 && u_mem.errors == 0)
            $display("PASS ap_top: %0d samples bit-exact through the SRAM pins, CPU max %0d cycles/tick, frame gap %0d..%0d",
                     n, cmax, gmin, gmax);
        else $display("FAIL ap_top: %0d mismatches in %0d samples, %0d SRAM timing errors", bad, n, u_mem.errors);
        $finish;
    end
endmodule
