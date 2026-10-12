// sram_bist.sv - hardware SRAM timing test: for each wait-state pair, write a pattern to
// all 131,072 words, read it back and count mismatches. Pairs (rd_wait, wr_pulse):
// (6,5) (5,4) (4,3) (3,2). About 130 ms. The result tells how much margin the default
// setting (6,5) has on the real part.
`default_nettype none

module sram_bist #(
    parameter [16:0] LAST = 17'h1FFFF     // last address tested (smaller in simulation)
) (
    input  wire        clk,
    input  wire        rst,
    output reg         done,
    output reg   [3:0] pass,               // bit k: pair k had no errors
    output reg  [15:0] err0, err1, err2, err3,   // mismatch counts (saturating)
    // to sram_ctrl
    output wire  [3:0] rd_wait,
    output wire  [3:0] wr_pulse,
    output reg         req,
    output reg         we,
    output reg  [16:0] addr,
    output reg  [15:0] wd,
    input  wire        ack,
    input  wire [15:0] rd
);
    reg  [1:0] k;                          // pair under test
    reg        rphase;                     // 0 = writing, 1 = reading back
    reg        pend;
    reg [15:0] err, e;
    assign rd_wait  = 4'd6 - {2'd0, k};
    assign wr_pulse = 4'd5 - {2'd0, k};
    function automatic [15:0] pat(input [16:0] a, input [1:0] kk);
        pat = a[15:0] ^ {a[16], a[16:2]} ^ 16'h5AA5 ^ ({14'd0, kk} * 16'h1357);
    endfunction

    always @(posedge clk) begin
        req <= 1'b0;
        if (rst) begin
            done <= 1'b0; pass <= 4'd0; k <= 2'd0; rphase <= 1'b0; pend <= 1'b0;
            addr <= 17'd0; err <= 16'd0;
            err0 <= 16'd0; err1 <= 16'd0; err2 <= 16'd0; err3 <= 16'd0;
        end else if (!done) begin
            if (!pend) begin
                req <= 1'b1;
                we <= !rphase;
                wd <= pat(addr, k);
                pend <= 1'b1;
            end else if (ack) begin
                pend <= 1'b0;
                e = err;
                if (rphase && rd != pat(addr, k) && e != 16'hFFFF) e = e + 16'd1;
                err <= e;
                addr <= addr + 17'd1;
                if (addr == LAST) begin
                    if (!rphase) begin rphase <= 1'b1; addr <= 17'd0; end
                    else begin
                        // pair finished
                        case (k)
                            2'd0: err0 <= e;
                            2'd1: err1 <= e;
                            2'd2: err2 <= e;
                            default: err3 <= e;
                        endcase
                        pass[k] <= e == 16'd0;
                        err <= 16'd0;
                        addr <= 17'd0;
                        rphase <= 1'b0;
                        if (k == 2'd3) done <= 1'b1;
                        k <= k + 2'd1;
                    end
                end
            end
        end
    end
endmodule
`default_nettype wire
