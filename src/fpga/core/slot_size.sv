// slot_size.sv - sets the size of the save slot in the APF data table (D58), so the APF writes the
// whole file on Quit / power-off even when no file existed yet (it then grows it to SIZE bytes).
// The data table holds, for the slot at index i in data.json, its id at word 2i and its size at
// word 2i+1. The save slot is at index 0 in the release data.json and index 1 in data_dev.json,
// so the first SLOTS entries are searched for its id. Runs when the APF reports all slots loaded
// and again at Reset Exit. Port a of the table (core_bridge_cmd.v) has 2 cycles of read latency.
`default_nettype none

module slot_size #(parameter [31:0] ID = 32'd1, parameter [31:0] SIZE = 32'h20, parameter SLOTS = 4) (
    input  wire        clk,
    input  wire  [1:0] start,               // a rising edge of either bit starts a search
    output reg   [9:0] dt_addr = 10'd0,
    output reg         dt_wren = 1'b0,
    output reg  [31:0] dt_data = 32'd0,
    input  wire [31:0] dt_q
);
    reg  [1:0] start_d = 2'd0;
    reg        busy = 1'b0;
    reg  [2:0] wait_n = 3'd0;
    reg  [3:0] idx = 4'd0;
    always @(posedge clk) begin
        start_d <= start;
        dt_wren <= 1'b0;
        if (|(start & ~start_d)) begin
            busy <= 1'b1; idx <= 4'd0; wait_n <= 3'd0; dt_addr <= 10'd0;
        end else if (busy) begin
            wait_n <= wait_n + 3'd1;
            if (wait_n == 3'd3) begin                       // id of entry idx is on dt_q
                wait_n <= 3'd0;
                if (dt_q == ID) begin
                    dt_addr <= {5'd0, idx, 1'b1}; dt_data <= SIZE; dt_wren <= 1'b1; busy <= 1'b0;
                end else if (idx == SLOTS - 1) busy <= 1'b0;
                else begin idx <= idx + 4'd1; dt_addr <= {5'd0, idx + 4'd1, 1'b0}; end
            end
        end
    end
endmodule
`default_nettype wire
