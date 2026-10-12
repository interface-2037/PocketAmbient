// text_layer.sv - 20 x 18 character cells of 16 x 16 pixels (8 x 8 glyphs at 2x) on the
// 320 x 288 screen. Character RAM entries are {colour[1:0], code[5:0]} (code = ASCII - 32),
// written from the system clock, read in the video clock. Output 3 video clocks after the
// coordinates: give it the coordinates of the pixel being drawn 3 clocks later.
`default_nettype none

module text_layer #(
    parameter INIT = "core/ui/screen_test.hex"
) (
    input  wire       vclk,
    input  wire [9:0] vx,              // visible x / y (values >= 320 / 288 are blank)
    input  wire [9:0] vy,
    output reg        pix,             // glyph pixel set
    output reg  [1:0] colour,
    // character RAM write port
    input  wire       wclk,
    input  wire       we,
    input  wire [9:0] waddr,           // {row[4:0], col[4:0]}
    input  wire [7:0] wdata
);
    reg [7:0] cram [0:1023];
    reg [7:0] font [0:511];
    initial begin
        $readmemh(INIT, cram);
        $readmemh("core/ui/font8.hex", font);
    end
    always @(posedge wclk) if (we) cram[waddr] <= wdata;

    reg  [7:0] cq, fq;
    reg  [2:0] gx1, gx2, gy1;
    reg  [1:0] col2;
    reg        in1, in2;
    wire       in_vis = vx < 10'd320 && vy < 10'd288;
    always @(posedge vclk) begin
        // c0: character cell
        cq  <= cram[{vy[8:4], vx[8:4]}];
        gx1 <= vx[3:1];
        gy1 <= vy[3:1];
        in1 <= in_vis;
        // c1: glyph row
        fq   <= font[{cq[5:0], gy1}];
        gx2  <= gx1;
        col2 <= cq[7:6];
        in2  <= in1;
        // c2: pixel
        pix    <= in2 & fq[3'd7 - gx2];
        colour <= col2;
    end
endmodule
`default_nettype wire
