// sram_ctrl.sv - request/acknowledge port to the Pocket's 128K x 16 asynchronous SRAM.
// All pin outputs and the data input are registered. Wait states are inputs so a
// diagnostic build can sweep them on hardware ([VERIFY] the part's access time).
//
//  read : A + OE# out at e0; data sampled at e(rd_wait); OE# released; 2 cycles of bus
//         turnaround (tHZOE) before ack. rd_wait + 4 cycles.
//  write: A + data out at e0 (address setup); WE# low for wr_pulse cycles; data held one
//         cycle after WE# rises, then ack. wr_pulse + 3 cycles.
// Defaults (rd_wait 6, wr_pulse 5 at 74.25 MHz: 81 ns / 67 ns) cover a 55 ns low-power part
// with board and I/O delays. Both byte lanes are always enabled.
`default_nettype none

module sram_ctrl (
    input  wire        clk,
    input  wire        rst,
    input  wire  [3:0] rd_wait,               // >= 2
    input  wire  [3:0] wr_pulse,              // >= 1
    // port (one request at a time; next request only after ack)
    input  wire        req,
    input  wire        we,
    input  wire [16:0] addr,
    input  wire [15:0] wd,
    output reg         ack,
    output reg  [15:0] rd,
    // pins
    output reg  [16:0] sram_a,
    inout  wire [15:0] sram_dq,
    output reg         sram_oe_n,
    output reg         sram_we_n,
    output wire        sram_ub_n,
    output wire        sram_lb_n
);
    reg [15:0] dq_out, dq_in;
    reg        dq_oe;
    assign sram_dq = dq_oe ? dq_out : 16'hzzzz;
    assign sram_ub_n = 1'b0;
    assign sram_lb_n = 1'b0;

    localparam [2:0] IDLE = 3'd0, RD = 3'd1, RD_END = 3'd2, TURN = 3'd3, WR_SET = 3'd4, WR = 3'd5,
                     WR_END = 3'd6;
    reg [2:0] st;
    reg [3:0] cnt;

    always @(posedge clk) begin
        dq_in <= sram_dq;                       // input register (fast input register on the pins)
        ack <= 1'b0;
        if (rst) begin
            st <= IDLE;
            sram_oe_n <= 1'b1;
            sram_we_n <= 1'b1;
            dq_oe <= 1'b0;
        end else case (st)
            IDLE: if (req) begin
                sram_a <= addr;
                if (we) begin
                    dq_out <= wd;
                    dq_oe <= 1'b1;
                    st <= WR_SET;
                end else begin
                    sram_oe_n <= 1'b0;
                    cnt <= rd_wait - 4'd1;
                    st <= RD;
                end
            end
            RD: begin                           // dq_in samples the bus at the edge ending cnt == 0
                if (cnt == 4'd0) st <= RD_END;
                else cnt <= cnt - 4'd1;
            end
            RD_END: begin
                rd <= dq_in;
                sram_oe_n <= 1'b1;
                cnt <= 4'd1;
                st <= TURN;
            end
            TURN: begin
                if (cnt == 4'd0) begin ack <= 1'b1; st <= IDLE; end
                else cnt <= cnt - 4'd1;
            end
            WR_SET: begin
                sram_we_n <= 1'b0;
                cnt <= wr_pulse - 4'd1;
                st <= WR;
            end
            WR: begin
                if (cnt == 4'd0) begin sram_we_n <= 1'b1; st <= WR_END; end
                else cnt <= cnt - 4'd1;
            end
            default: begin                      // WR_END: data held one cycle past WE# rising
                dq_oe <= 1'b0;
                ack <= 1'b1;
                st <= IDLE;
            end
        endcase
    end
endmodule
`default_nettype wire
