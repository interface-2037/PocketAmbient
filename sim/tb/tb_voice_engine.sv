// tb_voice_engine.sv - replays golden vectors from the C model (render --vectors)
// into voice_engine and compares the voice bus sample by sample.
//   vvp tb_voice_engine.vvp +writes=<file> +bus=<file> [+max=N]
`timescale 1ns/1ps
module tb_voice_engine;
    reg clk = 0, rst = 1;
    always #5 clk = ~clk;

    reg         wr_en = 0;
    reg  [9:0]  wr_addr = 0;
    reg  [31:0] wr_data = 0;
    reg         start = 0;
    wire        done;
    wire signed [31:0] bus_l, bus_r, bus_rev, bus_echo;
    wire [31:0] st_status;
    wire        ready;

    voice_engine dut (.clk(clk), .rst(rst), .wr_en(wr_en), .wr_addr(wr_addr), .wr_data(wr_data),
                      .start(start), .done(done), .bus_l(bus_l), .bus_r(bus_r), .bus_rev(bus_rev),
                      .bus_echo(bus_echo), .st_voice(5'd0), .st_status(st_status), .ready(ready));

    integer fw, fb, rc, rcb, nsamp, max_samp, mismatches, nwrites;
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
        mismatches = 0; nwrites = 0; nsamp = 0;
        repeat (3) @(posedge clk);
        rst = 0;
        wait (ready);
        @(posedge clk);
        read_write;
        read_bus;
        while (nsamp < max_samp && rcb == 6) begin
            // register writes that take effect at this sample (FX writes are Phase 3)
            while (have_w && w_sample == nsamp) begin
                if (w_fx == 0) begin
                    @(negedge clk); wr_en = 1; wr_addr = w_idx[9:0]; wr_data = w_val;
                    @(negedge clk); wr_en = 0;
                    nwrites = nwrites + 1;
                end
                read_write;
            end
            @(negedge clk); start = 1;
            @(negedge clk); start = 0;
            wait (done);
            @(negedge clk);
            if (bus_l !== e_l || bus_r !== e_r || bus_rev !== e_rev || bus_echo !== e_echo) begin
                if (mismatches < 10)
                    $display("MISMATCH sample %0d: rtl %h %h %h %h  model %h %h %h %h", nsamp,
                             bus_l, bus_r, bus_rev, bus_echo, e_l, e_r, e_rev, e_echo);
                mismatches = mismatches + 1;
            end
            nsamp = nsamp + 1;
            read_bus;
        end
        if (mismatches == 0 && nsamp > 0)
            $display("PASS voice_engine: %0d samples, %0d register writes, bit-exact", nsamp, nwrites);
        else
            $display("FAIL voice_engine: %0d mismatches in %0d samples", mismatches, nsamp);
        $finish;
    end
endmodule
