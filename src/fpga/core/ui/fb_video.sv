// fb_video.sv - Phase 5 video: two 320x288 2-bpp framebuffers, a 4-colour palette per buffer,
// swapped at vsync (DECISIONS D52, fw/hal/regs.h).
//
// CPU side (clk = clk_74a): the CPU always draws into the back buffer.
//   word access : 5760 words, 16 pixels per word, pixel x%16 in bits [2*(x%16)+1 : 2*(x%16)]
//   pixel write : the read-modify-write is done here (3 cycles)
//   palette     : 4 x 0x00RRGGBB for the back buffer
//   swap        : request; the buffers (and palettes) change at the next vsync; `swap_pending`
//                 stays high until the video side has done it
// Video side (vclk = pixel clock): `vx`, `vy` are the visible coordinates of the pixel three
// clocks ahead (as for text_layer); `rgb` is that pixel's colour three clocks later.
// Memory: 2 x 5760 x 32 bits in one true dual-port, dual-clock RAM (~40 M10K).
`default_nettype none

module fb_video (
    input  wire        clk,
    input  wire        rst,
    // CPU port (one request at a time; `ready` pulses when it is done)
    input  wire        req,
    input  wire        we,
    input  wire        pixel,             // pixel write (else word access)
    input  wire [12:0] word,              // word index (word access)
    input  wire  [8:0] px,                // pixel x (pixel write)
    input  wire  [8:0] py,                // pixel y (pixel write)
    input  wire [31:0] wdata,
    output reg  [31:0] rdata,
    output reg         ready,
    input  wire        pal_we,
    input  wire  [1:0] pal_idx,
    input  wire [23:0] pal_data,
    input  wire        swap,              // pulse
    output reg         swap_pending,
    output reg  [15:0] vsyncs,            // vsyncs seen (CPU clock domain)
    // video
    input  wire        vclk,
    input  wire        vsync,             // pulse at the start of a frame (vclk)
    input  wire  [9:0] vx,
    input  wire  [9:0] vy,
    output reg  [23:0] rgb
);
    localparam integer WORDS = 5760;

    reg  [31:0] mem [0:2 * WORDS - 1];
    reg  [23:0] pal [0:7];                   // {buffer, index}

    // ------------------------------------------------------------- CPU side
    // swap handshake: `swap_pending` (a level) asks the video side to flip `front_v` at its next
    // vsync; the CPU sees the flip through a synchroniser and drops the request. The CPU's back
    // buffer is always the one not shown, also after a core reset that the video side missed.
    reg         front_v = 1'b0, vs_v = 1'b0;
    reg         f_s1, f_s2, f_req, vs_s1, vs_s2, vs_seen;
    wire        back = ~f_s2;                // buffer the CPU draws into
    reg  [13:0] a_addr;
    reg  [31:0] a_wd, a_q;
    reg         a_we;
    always @(posedge clk) begin
        if (a_we) mem[a_addr] <= a_wd;
        a_q <= mem[a_addr];
    end

    function automatic [13:0] phys(input b, input [12:0] w);
        phys = b ? {1'b0, w} + 14'd5760 : {1'b0, w};
    endfunction

    // y * 20 + x / 16
    wire [12:0] pword = {py, 4'd0} + {2'd0, py, 2'd0} + {8'd0, px[8:4]};
    reg   [2:0] st;
    reg   [3:0] psh;
    reg   [1:0] pcol;
    localparam [2:0] IDLE = 3'd0, RD1 = 3'd1, RD2 = 3'd2, PX1 = 3'd3, PX2 = 3'd4, PX3 = 3'd5;

    always @(posedge clk) begin
        a_we <= 1'b0;
        ready <= 1'b0;
        f_s1 <= front_v; f_s2 <= f_s1;
        vs_s1 <= vs_v;   vs_s2 <= vs_s1;
        if (rst) begin
            st <= IDLE; swap_pending <= 1'b0; vs_seen <= vs_s2; vsyncs <= 16'd0;
        end else begin
            if (vs_s2 != vs_seen) begin vs_seen <= vs_s2; vsyncs <= vsyncs + 16'd1; end
            if (swap_pending && f_s2 != f_req) swap_pending <= 1'b0;   // the video side flipped
            if (swap && !swap_pending) begin swap_pending <= 1'b1; f_req <= f_s2; end
            if (pal_we) pal[{back, pal_idx}] <= pal_data;
            case (st)
                IDLE: if (req) begin
                    if (pixel) begin
                        a_addr <= phys(back, pword); psh <= px[3:0]; pcol <= wdata[1:0]; st <= PX1;
                    end else if (we) begin
                        a_addr <= phys(back, word); a_wd <= wdata; a_we <= 1'b1; ready <= 1'b1;
                    end else begin
                        a_addr <= phys(back, word); st <= RD1;
                    end
                end
                RD1: st <= RD2;                                // a_q valid next cycle
                RD2: begin rdata <= a_q; ready <= 1'b1; st <= IDLE; end
                PX1: st <= PX2;
                PX2: begin                                     // merge the pixel into the word
                    a_wd <= (a_q & ~(32'd3 << {psh, 1'b0})) | ({30'd0, pcol} << {psh, 1'b0});
                    a_we <= 1'b1;
                    st <= PX3;
                end
                default: begin ready <= 1'b1; st <= IDLE; end  // PX3: write done
            endcase
        end
    end

    // ------------------------------------------------------------- video side
    reg         req_v1, req_v2;
    always @(posedge vclk) begin
        req_v1 <= swap_pending; req_v2 <= req_v1;
        if (vsync) begin
            vs_v <= ~vs_v;
            if (req_v2) front_v <= ~front_v;
        end
    end
    wire        vis = vx < 10'd320 && vy < 10'd288;
    wire [12:0] vword = {vy[8:0], 4'd0} + {2'd0, vy[8:0], 2'd0} + {8'd0, vx[8:4]};
    reg  [31:0] b_q, b_q2;
    reg   [3:0] sh1, sh2;
    reg         vis1, vis2, f1, f2;
    always @(posedge vclk) begin
        b_q <= mem[phys(front_v, vword)];             // RAM read
        b_q2 <= b_q;                                  // RAM output register
        sh1 <= vx[3:0]; vis1 <= vis; f1 <= front_v;
        sh2 <= sh1;     vis2 <= vis1; f2 <= f1;
        rgb <= vis2 ? pal[{f2, b_q2[{sh2, 1'b0} +: 2]}] : 24'd0;
    end
endmodule
`default_nettype wire
