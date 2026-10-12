// tb_sram.sv - sram_ctrl against the async SRAM timing model at 74.25 MHz, with board
// delays. Random reads and writes are checked against a shadow copy.
//   vvp tb_sram.vvp [+rd=N] [+wr=N] [+taa=ns] [+n=accesses]
// PASS needs correct data and no timing / contention errors; with too few wait states
// the test must FAIL (the bench checks itself at rd_wait 3 against a 55 ns part).
`timescale 1ns/1ps
module tb_sram;
    reg clk = 0, rst = 1;
    always #6.734 clk = ~clk;                    // 74.25 MHz

    reg  [3:0]  rd_wait, wr_pulse;
    reg         req = 0, we = 0;
    reg  [16:0] addr = 0;
    reg  [15:0] wd = 0;
    wire        ack;
    wire [15:0] rd;
    wire [16:0] sram_a;
    wire [15:0] sram_dq;
    wire        sram_oe_n, sram_we_n, sram_ub_n, sram_lb_n;

    sram_ctrl dut (.clk(clk), .rst(rst), .rd_wait(rd_wait), .wr_pulse(wr_pulse),
        .req(req), .we(we), .addr(addr), .wd(wd), .ack(ack), .rd(rd),
        .sram_a(sram_a), .sram_dq(sram_dq), .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n),
        .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));

    // board + I/O delays: 3 ns out, 2 ns back
    wire [16:0] a_b;
    wire        oe_b, we_b;
    assign #3 a_b = sram_a;
    assign #3 oe_b = sram_oe_n;
    assign #3 we_b = sram_we_n;
    wire [15:0] dq_b;
    tran_delay u_link (.fpga(sram_dq), .chip(dq_b));
    async_sram_model u_mem (.a(a_b), .dq(dq_b), .oe_n(oe_b), .we_n(we_b), .ub_n(sram_ub_n),
        .lb_n(sram_lb_n), .host_drive(dut.dq_oe));

    reg [15:0] shadow [0:131071];
    reg        known [0:131071];
    integer k, n, bad, nacc;
    initial begin
        if (!$value$plusargs("rd=%d", k)) k = 6; rd_wait = k;
        if (!$value$plusargs("wr=%d", k)) k = 5; wr_pulse = k;
        if (!$value$plusargs("n=%d", nacc)) nacc = 20000;
        for (k = 0; k < 131072; k = k + 1) known[k] = 0;
        bad = 0;
        repeat (4) @(posedge clk);
        rst = 0;
        for (n = 0; n < nacc; n = n + 1) begin
            @(negedge clk);
            addr = (n < 2000) ? n : ($random & 32'h1FFFF);
            we = (n < 2000) ? 1'b1 : ($random & 1);
            wd = $random;
            req = 1;
            @(negedge clk) req = 0;
            wait (ack);
            if (we) begin shadow[addr] = wd; known[addr] = 1; end
            else if (known[addr] && rd !== shadow[addr]) begin
                if (bad < 5) $display("read %h: got %h want %h", addr, rd, shadow[addr]);
                bad = bad + 1;
            end
            @(posedge clk);
        end
        if (bad == 0 && u_mem.errors == 0)
            $display("PASS sram_ctrl: %0d accesses, rd_wait %0d, wr_pulse %0d", nacc, rd_wait, wr_pulse);
        else
            $display("FAIL sram_ctrl: %0d bad reads, %0d timing errors (rd_wait %0d, wr_pulse %0d)",
                     bad, u_mem.errors, rd_wait, wr_pulse);
        $finish;
    end
endmodule

// bidirectional link with 2-3 ns of delay each way
module tran_delay (inout wire [15:0] fpga, inout wire [15:0] chip);
    wire fpga_drives = fpga !== 16'hzzzz && tb_sram.dut.dq_oe;
    assign #3 chip = tb_sram.dut.dq_oe ? fpga : 16'hzzzz;
    assign #2 fpga = tb_sram.dut.dq_oe ? 16'hzzzz : chip;
endmodule
