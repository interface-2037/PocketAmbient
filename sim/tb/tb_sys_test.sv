// tb_sys_test.sv - integration test of the Phase 4 hardware test top (sys_test): boot through
// the pin-level 55 ns SRAM model (engine clears the SRAM), CPU runs composer_init and the
// first ticks with 48 kHz frame ticks, autostart press of A; checks the screen fields and
// that the first samples equal the model's (render --mode drift --seed 123).
//   vvp tb_sys_test.vvp +ref=<render raw> [+ms=N]
`timescale 1ns/1ps
module tb_sys_test;
    reg clk = 0, reset_n = 0;
    always #6.734 clk = ~clk;
    reg  [31:0] keys = 0;
    reg         tick = 0;
    wire [15:0] pcm_l, pcm_r;
    wire        txt_we;
    wire [9:0]  txt_addr;
    wire [7:0]  txt_data;
    wire [16:0] sram_a;
    wire [15:0] sram_dq;
    wire        sram_oe_n, sram_we_n, sram_ub_n, sram_lb_n;

    sys_test dut (.clk(clk), .reset_n(reset_n), .cont1_key(keys), .tick(tick), .pcm_l(pcm_l), .pcm_r(pcm_r),
        .txt_we(txt_we), .txt_addr(txt_addr), .txt_data(txt_data), .sram_a(sram_a), .sram_dq(sram_dq),
        .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n), .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));
    async_sram_model u_mem (.a(sram_a), .dq(sram_dq), .oe_n(sram_oe_n), .we_n(sram_we_n),
        .ub_n(sram_ub_n), .lb_n(sram_lb_n), .host_drive(dut.u_sram.dq_oe));

    reg [7:0] screen [0:1023];
    initial $readmemh("core/ui/screen_test.hex", screen);
    always @(posedge clk) if (txt_we) screen[txt_addr] <= txt_data;

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
    reg [8*20-1:0] row;
    initial begin
        if (!$value$plusargs("ms=%d", ms)) ms = 30;
        fr = $fopen("ref.raw", "rb");
        if ($test$plusargs("ref")) begin end
        bad = 0; n = 0;
        repeat (5) @(posedge clk);
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
            if (dut.u_sys.trap || dut.u_sys.bus_err) begin $display("FAIL: trap %b bus_err %b", dut.u_sys.trap, dut.u_sys.bus_err); $finish; end
        end
        repeat (200) @(posedge clk);
        for (r = 4; r <= 13; r = r + 1) begin
            for (i = 0; i < 20; i = i + 1) row[8 * (19 - i) +: 8] = (screen[r * 32 + i] & 8'h3F) + 8'd32;
            $display("screen row %2d: \"%s\"", r, row);
        end
        if (bad == 0 && n == ms * 48 && u_mem.errors == 0)
            $display("PASS sys_test: %0d samples bit-exact through the SRAM pins, CPU max %0d cycles/tick, frame gap %0d..%0d",
                     n, dut.u_sys.tick_cycles_max, dut.gap_min, dut.gap_max);
        else $display("FAIL sys_test: %0d mismatches in %0d samples, %0d SRAM timing errors", bad, n, u_mem.errors);
        $finish;
    end
endmodule
