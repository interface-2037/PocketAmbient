// tb_player.sv - vec_player + audio_engine replaying an excerpt from vec_rom.hex, compared
// with the C model's output for the same excerpt (build/excerpts/<mode>_bus.txt).
//   vvp tb_player.vvp +sel=N +bus=<file> [+max=N]
`timescale 1ns/1ps
module tb_player;
    reg clk = 0, rst = 1;
    always #6.734 clk = ~clk;

    reg  [1:0]  sel;
    reg         restart = 0, tick = 0;
    wire        ready, busy, done, p_start, p_wr, p_fx, p_end;
    wire [9:0]  p_addr;
    wire [31:0] p_data, p_samples;
    wire [7:0]  overruns;
    wire signed [15:0] out_l, out_r;
    wire        mem_req, mem_we;
    wire [16:0] mem_addr;
    wire [15:0] mem_wd;
    reg         mem_ack = 0;
    reg  [15:0] mem_rd = 0;

    audio_engine u_eng (.clk(clk), .rst(rst), .wr_en(p_wr), .wr_fx(p_fx), .wr_addr(p_addr),
        .wr_data(p_data), .start(p_start), .busy(busy), .done(done), .ready(ready),
        .bus_l(), .bus_r(), .bus_rev(), .bus_echo(), .out_l(out_l), .out_r(out_r),
        .st_voice(5'd0), .st_status(), .fx_gain(),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wd(mem_wd),
        .mem_ack(mem_ack), .mem_rd(mem_rd));
    vec_player u_play (.clk(clk), .sel(sel), .restart(restart), .tick(tick), .eng_ready(ready),
        .eng_busy(busy), .eng_start(p_start), .wr_en(p_wr), .wr_fx(p_fx), .wr_addr(p_addr),
        .wr_data(p_data), .samples(p_samples), .at_end(p_end), .overruns(overruns));

    reg [15:0] sram [0:131071];
    always @(posedge clk) begin
        mem_ack <= mem_req;
        if (mem_req) begin
            if (mem_we) sram[mem_addr] <= mem_wd;
            else mem_rd <= sram[mem_addr];
        end
    end

    // 48 kHz ticks: one every 1547 clocks
    integer tc = 0;
    always @(posedge clk) begin
        tick <= 1'b0;
        tc <= (tc == 1546) ? 0 : tc + 1;
        if (tc == 1546) tick <= 1'b1;
    end

    integer fb, rcb, n, maxn, bad, k;
    reg [31:0] e_l, e_r, e_rev, e_echo, e_ol, e_or;
    reg [1023:0] bfile;
    initial begin
        if (!$value$plusargs("sel=%d", k)) k = 0; sel = k;
        if (!$value$plusargs("bus=%s", bfile)) bfile = "bus.txt";
        if (!$value$plusargs("max=%d", maxn)) maxn = 3000;
        fb = $fopen(bfile, "r");
        bad = 0; n = 0;
        repeat (4) @(posedge clk);
        rst = 0;
        wait (ready);
        @(negedge clk) restart = 1;
        @(negedge clk) restart = 0;
        while (n < maxn) begin
            @(posedge done);
            @(negedge clk);
            rcb = $fscanf(fb, "%h %h %h %h %h %h", e_l, e_r, e_rev, e_echo, e_ol, e_or);
            if (out_l !== e_ol[15:0] || out_r !== e_or[15:0]) begin
                if (bad < 5) $display("MISMATCH sample %0d: rtl %h %h model %h %h", n, out_l, out_r, e_ol[15:0], e_or[15:0]);
                bad = bad + 1;
            end
            n = n + 1;
        end
        if (bad == 0 && overruns == 0) $display("PASS vec_player: excerpt %0d, %0d samples bit-exact", sel, n);
        else $display("FAIL vec_player: %0d mismatches, %0d overruns", bad, overruns);
        $finish;
    end
endmodule
