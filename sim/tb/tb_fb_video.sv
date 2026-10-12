// tb_fb_video.sv - unit test of fb_video.sv: CPU word / pixel writes into the back buffer,
// word read-back, palette, swap at vsync, and the scanned-out frame vs a reference model.
`timescale 1ns/1ps
module tb_fb_video;
    reg clk = 0, vclk = 0, rst = 1;
    always #6.734 clk = ~clk;          // 74.25 MHz
    always #40.69 vclk = ~vclk;        // 12.288 MHz
    reg         req = 0, we = 0, pixel = 0, pal_we = 0, swap = 0, vsync = 0;
    reg  [12:0] word = 0;
    reg   [8:0] px = 0, py = 0;
    reg  [31:0] wdata = 0;
    reg   [1:0] pal_idx = 0;
    reg  [23:0] pal_data = 0;
    reg   [9:0] vx = 0, vy = 0;
    wire [31:0] rdata;
    wire        ready, swap_pending;
    wire [15:0] vsyncs;
    wire [23:0] rgb;
    fb_video dut (.clk(clk), .rst(rst), .req(req), .we(we), .pixel(pixel), .word(word), .px(px), .py(py),
        .wdata(wdata), .rdata(rdata), .ready(ready), .pal_we(pal_we), .pal_idx(pal_idx), .pal_data(pal_data),
        .swap(swap), .swap_pending(swap_pending), .vsyncs(vsyncs), .vclk(vclk), .vsync(vsync), .vx(vx), .vy(vy), .rgb(rgb));

    reg  [31:0] model [0:5759];
    reg  [23:0] pal [0:3];
    integer i, x, y, errs = 0;
    reg  [31:0] seed = 32'h12345678;

    task cpu(input w, input p, input [12:0] wi, input [8:0] xx, input [8:0] yy, input [31:0] d);
        begin
            @(negedge clk); req = 1; we = w; pixel = p; word = wi; px = xx; py = yy; wdata = d;
            @(negedge clk); req = 0;
            while (!ready) @(negedge clk);
        end
    endtask
    task frame;                         // one frame of vclk: vsync pulse, then scan 320 x 288
        integer cx, cy, k;
        reg [9:0] qx [0:3], qy [0:3];
        begin
            repeat (8) @(negedge vclk);         // a request crosses into vclk well before vsync
            @(negedge vclk) vsync = 1;
            @(negedge vclk) vsync = 0;
            for (k = 0; k < 4; k = k + 1) begin qx[k] = 10'h3FF; qy[k] = 10'h3FF; end
            for (cy = 0; cy < 288; cy = cy + 1)
                for (cx = 0; cx < 324; cx = cx + 1) begin
                    @(negedge vclk);
                    vx = cx; vy = cy;
                    // rgb now holds the pixel requested three clocks ago
                    if (qx[2] < 320 && qy[2] < 288) begin
                        if (rgb !== pal[(model[qy[2] * 20 + qx[2] / 16] >> (2 * (qx[2] % 16))) & 3]) begin
                            if (errs < 5) $display("pixel %0d,%0d: rgb %h", qx[2], qy[2], rgb);
                            errs = errs + 1;
                        end
                    end
                    qx[2] = qx[1]; qy[2] = qy[1]; qx[1] = qx[0]; qy[1] = qy[0]; qx[0] = cx; qy[0] = cy;
                end
        end
    endtask

    initial begin
        repeat (4) @(negedge clk);
        rst = 0;
        repeat (4) @(negedge clk);
        // fill the back buffer with random words, then draw pixels over it
        for (i = 0; i < 5760; i = i + 1) begin
            seed = seed * 1664525 + 1013904223;
            model[i] = seed;
            cpu(1, 0, i, 0, 0, seed);
        end
        for (i = 0; i < 400; i = i + 1) begin
            seed = seed * 1664525 + 1013904223;
            x = seed[24:16] % 320; y = seed[15:7] % 288;
            cpu(1, 1, 0, x, y, seed[1:0]);
            model[y * 20 + x / 16] = (model[y * 20 + x / 16] & ~(32'd3 << (2 * (x % 16)))) | (seed[1:0] << (2 * (x % 16)));
        end
        for (i = 0; i < 5760; i = i + 97) begin
            cpu(0, 0, i, 0, 0, 0);
            if (rdata !== model[i]) begin if (errs < 5) $display("word %0d: %h vs %h", i, rdata, model[i]); errs = errs + 1; end
        end
        pal[0] = 24'h102030; pal[1] = 24'hA0B0C0; pal[2] = 24'h55AA55; pal[3] = 24'hFF0080;
        for (i = 0; i < 4; i = i + 1) begin
            @(negedge clk); pal_we = 1; pal_idx = i; pal_data = pal[i];
            @(negedge clk); pal_we = 0;
        end
        @(negedge clk) swap = 1;
        @(negedge clk) swap = 0;
        if (!swap_pending) begin $display("swap not pending"); errs = errs + 1; end
        frame;                                  // vsync flips; this frame shows the drawn buffer
        repeat (8) @(negedge clk);
        if (swap_pending) begin $display("swap still pending"); errs = errs + 1; end
        if (vsyncs != 1) begin $display("vsyncs %0d", vsyncs); errs = errs + 1; end
        // the CPU now draws into the other buffer: a write there must not show
        cpu(1, 0, 0, 0, 0, 32'hFFFFFFFF);
        frame;
        if (errs == 0) $display("PASS fb_video"); else $display("FAIL fb_video: %0d errors", errs);
        $finish;
    end
endmodule
