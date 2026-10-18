// sleep_state.sv - sleep / wake through APF save states (D62): host commands 0xA0 Savestate
// Start/Query and 0xA4 Savestate Load/Query, core.json "sleep_supported". clk_74a.
// The handshake follows PocketIR's lib/sleep_state.v, verified on hardware (pocket-learnings §4).
// The blob itself is a bridge window in settings_bridge.sv (ADDR, SIZE bytes):
//   - Sleep: the APF raises `start`; we pulse `snap` (settings_bridge latches the image of the
//     live state), ack, report busy for one cycle, then ok. The APF then reads SIZE bytes at ADDR.
//   - Wake: the FPGA is configured again and boots normally; the APF writes the blob to ADDR while
//     the core runs, then raises `load`; we pulse `restore` (settings_bridge checks the blob and
//     hands it to the composer), ack, report busy, then ok. A Memories load works the same way.
// core_bridge_cmd holds a request high until it sees the ack, so we act on the rising edge; ok
// stays up until the next request.
`default_nettype none

module sleep_state #(
    parameter [31:0] ADDR = 32'h7000_0000,
    parameter [31:0] SIZE = 32'h20
) (
    input  wire        clk,
    output wire        supported,
    output wire [31:0] addr,
    output wire [31:0] size,
    output wire [31:0] maxloadsize,
    input  wire        start,
    output reg         start_ack  = 1'b0,
    output reg         start_busy = 1'b0,
    output reg         start_ok   = 1'b0,
    output wire        start_err,
    input  wire        load,
    output reg         load_ack   = 1'b0,
    output reg         load_busy  = 1'b0,
    output reg         load_ok    = 1'b0,
    output wire        load_err,
    output reg         snap       = 1'b0,   // pulse: latch the blob image (sleep / Memories save)
    output reg         restore    = 1'b0    // pulse: the blob at ADDR is written, apply it
);
    assign supported   = 1'b1;
    assign addr        = ADDR;
    assign size        = SIZE;
    assign maxloadsize = SIZE;
    assign start_err   = 1'b0;
    assign load_err    = 1'b0;

    reg start_r = 1'b0, load_r = 1'b0;

    always @(posedge clk) begin
        start_r <= start;
        load_r <= load;
        start_ack <= 1'b0; load_ack <= 1'b0;
        snap <= 1'b0; restore <= 1'b0;

        // busy lasts one cycle after the ack
        if (start_busy) begin start_busy <= 1'b0; start_ok <= 1'b1; end
        if (load_busy)  begin load_busy  <= 1'b0; load_ok  <= 1'b1; end

        if (start && !start_r) begin
            start_ack <= 1'b1; start_busy <= 1'b1; snap <= 1'b1;
            start_ok <= 1'b0; load_ok <= 1'b0;
        end
        if (load && !load_r) begin
            load_ack <= 1'b1; load_busy <= 1'b1; restore <= 1'b1;
            start_ok <= 1'b0; load_ok <= 1'b0;
        end
    end
endmodule
`default_nettype wire
