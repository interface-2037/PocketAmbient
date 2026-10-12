// tb_engine_test.sv - integration test of the Phase 3 hardware test top (engine_test):
// SRAM BIST over a short range against the pin-level 55 ns SRAM model, then excerpt 0
// (DRIFT) through sram_ctrl, compared with the C model's output.
//   vvp tb_engine_test.vvp +bus=build/excerpts/drift_bus.txt [+max=N]
// Expected: BIST pairs (6,5) and (5,4) pass, (4,3) and (3,2) fail on a 55 ns part.
`timescale 1ns/1ps
module tb_engine_test;
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

    engine_test #(.BIST_LAST(17'd255)) dut (.clk(clk), .reset_n(reset_n), .cont1_key(keys),
        .tick(tick), .pcm_l(pcm_l), .pcm_r(pcm_r), .txt_we(txt_we), .txt_addr(txt_addr),
        .txt_data(txt_data), .sram_a(sram_a), .sram_dq(sram_dq), .sram_oe_n(sram_oe_n),
        .sram_we_n(sram_we_n), .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));
    async_sram_model u_mem (.a(sram_a), .dq(sram_dq), .oe_n(sram_oe_n), .we_n(sram_we_n),
        .ub_n(sram_ub_n), .lb_n(sram_lb_n), .host_drive(dut.u_sram.dq_oe));

    // text RAM mirror (checks the screen writer)
    reg [7:0] screen [0:1023];
    integer si; initial for (si = 0; si < 1024; si = si + 1) screen[si] = 8'h00;   // (the Phase 3 screen file is gone)
    always @(posedge clk) if (txt_we) screen[txt_addr] <= txt_data;

    integer tc = 0;
    always @(posedge clk) begin
        tick <= 1'b0;
        tc <= (tc == 1546) ? 0 : tc + 1;
        if (tc == 1546) tick <= 1'b1;
    end

    integer fb, rcb, n, maxn, bad, i, terr;
    reg [31:0] e_l, e_r, e_rev, e_echo, e_ol, e_or;
    reg [1023:0] bfile;
    reg [8*20-1:0] row5;
    reg [8*32-1:0] row9, row11, row12;
    initial begin
        if (!$value$plusargs("bus=%s", bfile)) bfile = "bus.txt";
        if (!$value$plusargs("max=%d", maxn)) maxn = 400;
        fb = $fopen(bfile, "r");
        bad = 0; n = 0;
        repeat (5) @(posedge clk);
        reset_n = 1;
        wait (dut.bist_done);
        $display("BIST pass mask %b, errors %h %h %h %h", dut.bist_pass, dut.err0, dut.err1, dut.err2, dut.err3);
        terr = u_mem.errors;                    // the tighter BIST pairs violate timing on purpose
        while (n < maxn) begin
            @(posedge dut.eng_done);
            @(negedge clk);
            @(negedge clk);                     // pcm_l / pcm_r are registered from out_l / out_r
            rcb = $fscanf(fb, "%h %h %h %h %h %h", e_l, e_r, e_rev, e_echo, e_ol, e_or);
            if (pcm_l !== e_ol[15:0] || pcm_r !== e_or[15:0]) begin
                if (bad < 5) $display("MISMATCH sample %0d: rtl %h %h model %h %h", n, pcm_l, pcm_r, e_ol[15:0], e_or[15:0]);
                bad = bad + 1;
            end
            n = n + 1;
        end
        for (i = 0; i < 20; i = i + 1) row5[8 * (19 - i) +: 8] = (screen[5 * 32 + i] & 8'h3F) + 8'd32;
        $display("screen row 5: \"%s\"", row5);
        for (i = 0; i < 32; i = i + 1) row9[8 * (31 - i) +: 8] = (screen[9 * 32 + i] & 8'h3F) + 8'd32;
        $display("screen row 9: \"%s\"", row9);
        for (i = 0; i < 32; i = i + 1) row11[8 * (31 - i) +: 8] = (screen[11 * 32 + i] & 8'h3F) + 8'd32;
        $display("screen row 11: \"%s\"", row11);
        for (i = 0; i < 32; i = i + 1) row12[8 * (31 - i) +: 8] = (screen[12 * 32 + i] & 8'h3F) + 8'd32;
        $display("screen row 12: \"%s\"", row12);
        if (bad == 0 && u_mem.errors == terr && dut.bist_pass == 4'b0011 && dut.overruns == 0)
            $display("PASS engine_test: BIST as expected, %0d samples bit-exact through the SRAM pins, max %0d cycles",
                     n, dut.cyc_max);
        else
            $display("FAIL engine_test: %0d mismatches, %0d SRAM timing errors, BIST %b, %0d overruns",
                     bad, u_mem.errors - terr, dut.bist_pass, dut.overruns);
        $finish;
    end
endmodule
