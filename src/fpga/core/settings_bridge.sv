// settings_bridge.sv - Phase 5: the Pocket menu (pkg/interact.json) <-> the composer's control
// block. clk_74a, bridge window 0x5000_0000 (one 32-bit word per setting):
//
//   0x00 Mode 0-3        0x04 Density 1-5     0x08 Brightness 1-5    0x0C Reverb 0-2
//   0x10 Visuals 0-2     0x14 Lock seed 0/1   0x18 Seed (u32)        0x1C Sleep 0-3
//   0x20 Reseed (action, write-only)
//
// Reads return the composer's live value (CR_ST_*), so changes made with the buttons show in
// the menu; before the composer has published its status they return the stored setting.
// The APF reads, updates and writes every element back each frame. A write that only echoes
// the value last read is ignored (otherwise a button press landing between the APF's read and
// write-back would be undone), and so is one equal to the live value; a real change sets the
// stored value and increments SETTINGS_SEQ and the composer re-reads CR_MODE..CR_SLEEP at its next tick. Turning Lock seed on while running latches the current
// session seed, so the piece playing now is the one that comes back after a reload.
// Persisted values are written by the APF just before Reset Exit, so these registers are not
// cleared by the core reset (power-up defaults only).
`default_nettype none

module settings_bridge (
    input  wire        clk,
    input  wire        bridge_wr,
    input  wire        bridge_rd,
    input  wire [31:0] bridge_addr,
    input  wire [31:0] bridge_wr_data,
    output reg  [31:0] bridge_rd_data,
    // live composer status
    input  wire        st_valid,
    input  wire  [2:0] st_mode,
    input  wire  [2:0] st_density,
    input  wire  [2:0] st_bright,
    input  wire  [1:0] st_reverb,
    input  wire  [1:0] st_visuals,
    input  wire  [1:0] st_sleep,
    input  wire [31:0] st_seed,
    // settings for the composer (CR_MODE..CR_SLEEP, CR_SETTINGS_SEQ, CR_CMD)
    output reg   [2:0] cfg_mode = 3'd0,
    output reg   [2:0] cfg_density = 3'd3,
    output reg   [2:0] cfg_bright = 3'd3,
    output reg   [1:0] cfg_reverb = 2'd1,
    output reg   [1:0] cfg_visuals = 2'd0,
    output reg         cfg_seed_lock = 1'b0,
    output reg  [31:0] cfg_seed = 32'd0,
    output reg   [1:0] cfg_sleep = 2'd0,
    output reg  [31:0] settings_seq = 32'd0,
    output reg         cmd_reseed = 1'b0
);
    wire       sel = bridge_addr[31:8] == 24'h500000;
    wire [5:0] w = bridge_addr[7:2];
    wire [31:0] d = bridge_wr_data;

    // what the menu shows for setting w now (live value once the composer runs)
    reg  [31:0] cur;
    always_comb begin
        case (w)
            6'd0: cur = {29'd0, st_valid ? st_mode : cfg_mode};
            6'd1: cur = {29'd0, st_valid ? st_density : cfg_density};
            6'd2: cur = {29'd0, st_valid ? st_bright : cfg_bright};
            6'd3: cur = {30'd0, st_valid ? st_reverb : cfg_reverb};
            6'd4: cur = {30'd0, st_valid ? st_visuals : cfg_visuals};
            6'd5: cur = {31'd0, cfg_seed_lock};
            6'd6: cur = (cfg_seed_lock || !st_valid) ? cfg_seed : st_seed;
            6'd7: cur = {30'd0, st_valid ? st_sleep : cfg_sleep};
            default: cur = 32'd0;
        endcase
    end

    // value last returned for each setting; a write of the same value is the APF's echo
    reg  [31:0] last_rd [0:7];
    reg   [7:0] read_once = 8'd0;
    integer i;
    initial for (i = 0; i < 8; i = i + 1) last_rd[i] = 32'd0;
    wire        echo = w < 6'd8 && read_once[w[2:0]] && d == last_rd[w[2:0]];

    function automatic [2:0] clamp15(input [31:0] v);
        clamp15 = (v < 32'd1) ? 3'd1 : (v > 32'd5) ? 3'd5 : v[2:0];
    endfunction

    always @(posedge clk) begin
        cmd_reseed <= 1'b0;
        if (bridge_rd && sel && w < 6'd8) begin
            last_rd[w[2:0]] <= cur;
            read_once[w[2:0]] <= 1'b1;
        end
        if (bridge_wr && sel && !echo) begin
            case (w)
                6'd0: if ({30'd0, d[1:0]} != cur) begin cfg_mode <= {1'b0, d[1:0]}; settings_seq <= settings_seq + 32'd1; end
                6'd1: if ({29'd0, clamp15(d)} != cur) begin cfg_density <= clamp15(d); settings_seq <= settings_seq + 32'd1; end
                6'd2: if ({29'd0, clamp15(d)} != cur) begin cfg_bright <= clamp15(d); settings_seq <= settings_seq + 32'd1; end
                6'd3: if ({30'd0, d[1:0]} != cur && d[1:0] != 2'd3) begin cfg_reverb <= d[1:0]; settings_seq <= settings_seq + 32'd1; end
                6'd4: if ({30'd0, d[1:0]} != cur && d[1:0] != 2'd3) begin cfg_visuals <= d[1:0]; settings_seq <= settings_seq + 32'd1; end
                6'd5: if ({31'd0, d[0]} != cur) begin
                    cfg_seed_lock <= d[0];
                    if (d[0] && st_valid) cfg_seed <= st_seed;          // keep the piece playing now
                    settings_seq <= settings_seq + 32'd1;
                end
                6'd6: if (d != cur) begin cfg_seed <= d; settings_seq <= settings_seq + 32'd1; end
                6'd7: if ({30'd0, d[1:0]} != cur) begin cfg_sleep <= d[1:0]; settings_seq <= settings_seq + 32'd1; end
                6'd8: cmd_reseed <= 1'b1;
                default: ;
            endcase
        end
        // registered read data (valid the cycle after the address, like core_bridge_cmd)
        bridge_rd_data <= cur;
    end
endmodule
`default_nettype wire
