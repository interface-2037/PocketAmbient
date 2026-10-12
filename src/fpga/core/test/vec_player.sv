// vec_player.sv - Phase 3 hardware test: replays composer register writes recorded by
// the C model (tools/gen_vecrom.py) into the audio engine, one sample per tick, with the
// model's semantics: all writes logged for sample s are applied before sample s runs.
// One write per cycle (the ROM is read one word ahead), so even 300-write bursts fit.
`default_nettype none

module vec_player (
    input  wire        clk,
    input  wire  [1:0] sel,               // excerpt
    input  wire        restart,           // load the selected excerpt (engine already reset)
    input  wire        tick,              // 48 kHz frame tick
    input  wire        eng_ready,
    input  wire        eng_busy,
    output reg         eng_start,
    output reg         wr_en,
    output reg         wr_fx,
    output reg   [9:0] wr_addr,
    output reg  [31:0] wr_data,
    output reg  [31:0] samples,           // samples played in this excerpt
    output reg         at_end,            // pulse: excerpt finished
    output reg   [7:0] overruns           // ticks that found the engine still busy
);
`include "vec_rom.vh"
    reg [63:0] rom [0:VEC_DEPTH-1];
    initial $readmemh("core/test/vec_rom.hex", rom);

    localparam [2:0] IDLE = 3'd0, L0 = 3'd1, L1 = 3'd2, WAIT = 3'd3, APPLY = 3'd4, DONE = 3'd5;
    reg  [2:0] st = IDLE;
    reg [13:0] ptr;
    reg [63:0] cur, q;
    reg [15:0] rem;
    wire       adv = st == APPLY && rem == 16'd0 && !cur[0];
    wire [13:0] ra = (st == L0) ? ptr : adv ? ptr + 14'd2 : ptr + 14'd1;
    always @(posedge clk) q <= rom[ra];

    always @(posedge clk) begin
        eng_start <= 1'b0;
        wr_en <= 1'b0;
        at_end <= 1'b0;
        if (restart) begin
            ptr <= (sel == 2'd0) ? VEC_START_0 : (sel == 2'd1) ? VEC_START_1 :
                   (sel == 2'd2) ? VEC_START_2 : VEC_START_3;
            samples <= 32'd0;
            overruns <= 8'd0;
            st <= L0;
        end else case (st)
            L0: st <= L1;                                       // q <= rom[ptr]
            L1: begin cur <= q; rem <= q[63:48]; st <= WAIT; end
            // hold while the engine is clearing after a reset (not an overrun); count only ticks
            // that arrive while the previous sample is still running
            WAIT: if (tick && eng_ready) begin
                if (eng_busy) overruns <= (overruns == 8'hFF) ? overruns : overruns + 8'd1;
                else st <= APPLY;
            end
            APPLY: begin
                if (adv) begin
                    wr_en <= 1'b1;
                    wr_fx <= cur[47];
                    wr_addr <= cur[46:37];
                    wr_data <= cur[36:5];
                    cur <= q;
                    rem <= q[63:48];
                    ptr <= ptr + 14'd1;
                end else if (rem == 16'd0) begin              // end marker reached
                    at_end <= 1'b1;
                    st <= DONE;
                end else begin
                    eng_start <= 1'b1;
                    rem <= rem - 16'd1;
                    samples <= samples + 32'd1;
                    st <= WAIT;
                end
            end
            default: ;                                          // IDLE / DONE: wait for restart
        endcase
    end
    initial overruns = 8'd0;
endmodule
`default_nettype wire
