// tb_audio.sv - replays golden vectors (render --vectors / fuzz) into audio_engine
// (voice engine + FX/master) and compares the voice bus and the 16-bit output.
//   vvp tb_audio.vvp +writes=<file> +bus=<file> [+max=N] [+lat=N]
// The SRAM model answers each request after `lat` cycles and powers up with random
// contents, so the engine's clear-on-reset is exercised too. Compiled with -DSRAM_PINS,
// the engine goes through sram_ctrl to the pin-level 55 ns async SRAM model instead.
`timescale 1ns/1ps
module tb_audio;
    reg clk = 0, rst = 1;
    always #6.734 clk = ~clk;                    // 74.25 MHz

    reg         wr_en = 0, wr_fx = 0;
    reg  [9:0]  wr_addr = 0;
    reg  [31:0] wr_data = 0;
    reg         start = 0;
    wire        busy, done, ready;
    wire signed [31:0] bus_l, bus_r, bus_rev, bus_echo;
    wire signed [15:0] out_l, out_r;
    wire [31:0] st_status, fx_gain;
    wire        mem_req, mem_we;
    wire [16:0] mem_addr;
    wire [15:0] mem_wd;
    reg         mem_ack = 0;
    reg  [15:0] mem_rd = 0;

    audio_engine dut (.clk(clk), .rst(rst), .wr_en(wr_en), .wr_fx(wr_fx), .wr_addr(wr_addr),
        .wr_data(wr_data), .start(start), .busy(busy), .done(done), .ready(ready),
        .bus_l(bus_l), .bus_r(bus_r), .bus_rev(bus_rev), .bus_echo(bus_echo),
        .out_l(out_l), .out_r(out_r), .st_voice(5'd0), .st_status(st_status), .fx_gain(fx_gain),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wd(mem_wd),
        .mem_ack(mem_ack), .mem_rd(mem_rd));

    // ---------------------------------------------------------- SRAM
`ifdef SRAM_PINS
    wire        ack_w;
    wire [15:0] rd_w;
    wire [16:0] sram_a;
    wire [15:0] sram_dq;
    wire        sram_oe_n, sram_we_n, sram_ub_n, sram_lb_n;
    always @* begin mem_ack = ack_w; mem_rd = rd_w; end
    sram_ctrl u_sc (.clk(clk), .rst(rst), .rd_wait(4'd6), .wr_pulse(4'd5),
        .req(mem_req), .we(mem_we), .addr(mem_addr), .wd(mem_wd), .ack(ack_w), .rd(rd_w),
        .sram_a(sram_a), .sram_dq(sram_dq), .sram_oe_n(sram_oe_n), .sram_we_n(sram_we_n),
        .sram_ub_n(sram_ub_n), .sram_lb_n(sram_lb_n));
    async_sram_model u_mem (.a(sram_a), .dq(sram_dq), .oe_n(sram_oe_n), .we_n(sram_we_n),
        .ub_n(sram_ub_n), .lb_n(sram_lb_n), .host_drive(u_sc.dq_oe));
    integer lat;
    initial lat = 0;
`else
    reg [15:0] sram [0:131071];
    integer lat, cnt = 0, k;
    reg [16:0] a_l;
    reg        we_l;
    reg [15:0] wd_l;
    initial begin
        if (!$value$plusargs("lat=%d", lat)) lat = 3;
        for (k = 0; k < 131072; k = k + 1) sram[k] = $random;
    end
    always @(posedge clk) begin
        mem_ack <= 1'b0;
        if (cnt != 0) begin
            cnt <= cnt - 1;
            if (cnt == 1) begin
                mem_ack <= 1'b1;
                if (we_l) sram[a_l] <= wd_l;
                else mem_rd <= sram[a_l];
            end
        end else if (mem_req) begin
            a_l <= mem_addr; we_l <= mem_we; wd_l <= mem_wd;
            cnt <= lat;
        end
    end
`endif

    // ------------------------------------------------------------ vectors
    integer fw, fb, rc, rcb, nsamp, max_samp, mismatches, nwrites, cyc, cyc_max;
    reg [63:0] w_sample;
    integer w_fx;
    reg [31:0] w_idx, w_val;
    reg [31:0] e_l, e_r, e_rev, e_echo, e_ol, e_or;
    reg have_w;
    reg [1023:0] wfile, bfile;

    task read_write;
        begin
            rc = $fscanf(fw, "%h %d %h %h", w_sample, w_fx, w_idx, w_val);
            have_w = (rc == 4);
        end
    endtask
    task read_bus;
        rcb = $fscanf(fb, "%h %h %h %h %h %h", e_l, e_r, e_rev, e_echo, e_ol, e_or);
    endtask

    always @(posedge clk) if (busy) cyc = cyc + 1;

    initial begin
        if (!$value$plusargs("writes=%s", wfile)) wfile = "writes.txt";
        if (!$value$plusargs("bus=%s", bfile)) bfile = "bus.txt";
        if (!$value$plusargs("max=%d", max_samp)) max_samp = 2000000000;
        fw = $fopen(wfile, "r");
        fb = $fopen(bfile, "r");
        if (fw == 0 || fb == 0) begin
            $display("FAIL: cannot open vectors");
            $finish;
        end
        mismatches = 0; nwrites = 0; nsamp = 0; cyc_max = 0;
        repeat (3) @(posedge clk);
        rst = 0;
        wait (ready);
        @(posedge clk);
        read_write;
        read_bus;
        while (nsamp < max_samp && rcb == 6) begin
            while (have_w && w_sample == nsamp) begin
                @(negedge clk); wr_en = 1; wr_fx = (w_fx != 0); wr_addr = w_idx[9:0]; wr_data = w_val;
                @(negedge clk); wr_en = 0;
                nwrites = nwrites + 1;
                read_write;
            end
            @(negedge clk); start = 1; cyc = 0;
            @(negedge clk); start = 0;
            wait (done);
            @(negedge clk);
            if (cyc > cyc_max) cyc_max = cyc;
            if (bus_l !== e_l || bus_r !== e_r || bus_rev !== e_rev || bus_echo !== e_echo ||
                out_l !== e_ol[15:0] || out_r !== e_or[15:0]) begin
                if (mismatches < 10)
                    $display("MISMATCH sample %0d: rtl %h %h %h %h | %h %h  model %h %h %h %h | %h %h", nsamp,
                             bus_l, bus_r, bus_rev, bus_echo, out_l, out_r,
                             e_l, e_r, e_rev, e_echo, e_ol[15:0], e_or[15:0]);
                mismatches = mismatches + 1;
            end
            nsamp = nsamp + 1;
            read_bus;
        end
`ifdef SRAM_PINS
        if (u_mem.errors != 0) mismatches = mismatches + 1;
`endif
        if (mismatches == 0 && nsamp > 0)
            $display("PASS audio_engine: %0d samples, %0d register writes, bit-exact (max %0d cycles/sample)",
                     nsamp, nwrites, cyc_max);
        else
            $display("FAIL audio_engine: %0d mismatches in %0d samples", mismatches, nsamp);
        $finish;
    end
endmodule
