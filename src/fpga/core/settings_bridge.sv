// settings_bridge.sv - Phase 5: the Pocket menu (pkg/interact.json) <-> the composer's control
// block, and the save slot (pkg/data*.json, ambient.sav). clk_74a, two bridge windows:
//
//   0x5000_0000, one 32-bit word per setting:
//   0x00 Mode 0-3        0x04 Density 1-5     0x08 Brightness 1-5    0x0C Reverb 0-2
//   0x10 Visuals 0-2     0x14 Lock seed 0/1   0x18 Seed (u32)        0x1C Sleep 0-3
//   0x20 Reseed (action, write-only)
//
//   0x6000_0000, the save slot (8 words = the 32-byte file, word 0 = file bytes 0..3, MSB first):
//   0 magic "AMBS"   1 {format 1, 23'd0, lock}   2 seed   3 settings (see sav_img)   4-6 0
//   7 check word (sav_check)
//
// Reads (D57). The APF latches a read on the bridge_rd strobe and takes the word later, after
// bridge_addr has moved on (core_bridge_cmd.v does the same), so the word is captured on the
// strobe and held until the next one; core_top selects this module by the region of that strobe.
// A read returns the true current setting at any time: the live value (CR_ST_*) is copied into
// live_* while the composer runs, and those registers, like everything here, are not cleared by
// the core reset. On Quit / power-off the APF asserts reset first and then reads, so a read in
// reset returns the last live value, button changes included.
//
// Writes. The APF reads, updates and writes every element back each frame. A write that only
// echoes the value last read is ignored (otherwise a button press landing between the APF's read
// and write-back would be undone), and so is one equal to the current value; a real change sets
// the stored value and increments SETTINGS_SEQ and the composer re-reads CR_MODE..CR_SLEEP at its
// next tick. Turning Lock seed on while running latches the current session seed, so the piece
// playing now is the one that comes back after a reload. Persisted values are written by the APF
// just before Reset Exit, so these registers are not cleared by the core reset (power-up
// defaults only).
//
// Save slot (D58). The APF does not persist the read-only Seed, so the locked seed lives in a
// nonvolatile data slot. On start the APF writes the file into sav_w while reset is held; at
// Reset Exit a valid file (magic + check word) restores Lock seed and the seed. On Quit the APF
// reads the slot back in reset: until the composer has run once (booted) it gets the loaded words
// back unchanged, afterwards the image of the current settings.
`default_nettype none

module settings_bridge (
    input  wire        clk,
    input  wire        rst,                 // core reset (only its falling edge is used)
    input  wire        bridge_wr,
    input  wire        bridge_rd,
    input  wire [31:0] bridge_addr,
    input  wire [31:0] bridge_wr_data,
    output reg  [31:0] bridge_rd_data = 32'd0,
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
    localparam [31:0] SAV_MAGIC = 32'h414D_4253;    // "AMBS"
    localparam  [7:0] SAV_FORMAT = 8'd1;

    wire       sel = bridge_addr[31:8] == 24'h500000;
    wire       sel_sav = bridge_addr[31:5] == 27'h300_0000;      // 0x6000_0000..0x6000_001F
    wire [5:0] w = bridge_addr[7:2];
    wire [2:0] k = bridge_addr[4:2];
    wire [31:0] d = bridge_wr_data;

    // last live value of each setting (CR_ST_*), kept through the core reset
    reg   [2:0] live_mode = 3'd0, live_density = 3'd3, live_bright = 3'd3;
    reg   [1:0] live_reverb = 2'd1, live_visuals = 2'd0, live_sleep = 2'd0;
    reg  [31:0] live_seed = 32'd0;
    reg         booted = 1'b0;                  // the composer has published its status once

    // what the menu shows for setting w now
    reg  [31:0] cur;
    always_comb begin
        case (w)
            6'd0: cur = {29'd0, live_mode};
            6'd1: cur = {29'd0, live_density};
            6'd2: cur = {29'd0, live_bright};
            6'd3: cur = {30'd0, live_reverb};
            6'd4: cur = {30'd0, live_visuals};
            6'd5: cur = {31'd0, cfg_seed_lock};
            6'd6: cur = (cfg_seed_lock || !booted) ? cfg_seed : live_seed;
            6'd7: cur = {30'd0, live_sleep};
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

    // ------------------------------------------------------------- save slot
    // check word over words 0-6: each word rotated left by its index, summed, inverted
    function automatic [31:0] sav_check(input [223:0] ws);
        reg [31:0] s, x;
        integer j;
        begin
            s = 32'h5A17_C3E9;
            for (j = 0; j < 7; j = j + 1) begin
                x = ws[32 * j +: 32];
                s = s + ((x << j) | (x >> (32 - j)));
            end
            sav_check = ~s;
        end
    endfunction

    reg  [31:0] sav_w [0:7];                    // words loaded by the APF (power-up 0)
    initial for (i = 0; i < 8; i = i + 1) sav_w[i] = 32'd0;
    reg         sav_new = 1'b0;                 // loaded since the last Reset Exit
    reg         sav_ok = 1'b0;                  // sav_w holds a valid file
    reg  [31:0] img [0:7];                      // the file for the current settings
    initial for (i = 0; i < 8; i = i + 1) img[i] = 32'd0;
    wire [31:0] img_seed = (cfg_seed_lock || !booted) ? cfg_seed : live_seed;
    wire [31:0] img3 = {10'd0, live_sleep, 2'd0, live_visuals, 2'd0, live_reverb, 1'b0, live_bright,
                        1'b0, live_density, 1'b0, live_mode};
    reg         rst_d = 1'b0;

    always @(posedge clk) begin
        img[0] <= SAV_MAGIC;
        img[1] <= {SAV_FORMAT, 23'd0, cfg_seed_lock};
        img[2] <= img_seed;
        img[3] <= img3;
        img[7] <= sav_check({32'd0, 32'd0, 32'd0, img3, img_seed, {SAV_FORMAT, 23'd0, cfg_seed_lock}, SAV_MAGIC});
        sav_ok <= sav_w[0] == SAV_MAGIC && sav_w[1][31:24] == SAV_FORMAT &&
                  sav_w[7] == sav_check({sav_w[6], sav_w[5], sav_w[4], sav_w[3], sav_w[2], sav_w[1], sav_w[0]});
    end

    always @(posedge clk) begin
        cmd_reseed <= 1'b0;
        rst_d <= rst;
        if (st_valid) begin
            live_mode <= st_mode; live_density <= st_density; live_bright <= st_bright;
            live_reverb <= st_reverb; live_visuals <= st_visuals; live_sleep <= st_sleep;
            live_seed <= st_seed; booted <= 1'b1;
        end
        if (bridge_rd && sel && w < 6'd8) begin
            last_rd[w[2:0]] <= cur;
            read_once[w[2:0]] <= 1'b1;
        end
        // read data: captured on the strobe, held until the next one
        if (bridge_rd && sel) bridge_rd_data <= cur;
        if (bridge_rd && sel_sav) bridge_rd_data <= booted ? img[k] : sav_w[k];
        if (bridge_wr && sel_sav) begin sav_w[k] <= d; sav_new <= 1'b1; end
        if (bridge_wr && sel && !echo) begin
            case (w)
                6'd0: if ({30'd0, d[1:0]} != cur) begin cfg_mode <= {1'b0, d[1:0]}; live_mode <= {1'b0, d[1:0]}; settings_seq <= settings_seq + 32'd1; end
                6'd1: if ({29'd0, clamp15(d)} != cur) begin cfg_density <= clamp15(d); live_density <= clamp15(d); settings_seq <= settings_seq + 32'd1; end
                6'd2: if ({29'd0, clamp15(d)} != cur) begin cfg_bright <= clamp15(d); live_bright <= clamp15(d); settings_seq <= settings_seq + 32'd1; end
                6'd3: if ({30'd0, d[1:0]} != cur && d[1:0] != 2'd3) begin cfg_reverb <= d[1:0]; live_reverb <= d[1:0]; settings_seq <= settings_seq + 32'd1; end
                6'd4: if ({30'd0, d[1:0]} != cur && d[1:0] != 2'd3) begin cfg_visuals <= d[1:0]; live_visuals <= d[1:0]; settings_seq <= settings_seq + 32'd1; end
                6'd5: if ({31'd0, d[0]} != cur) begin
                    cfg_seed_lock <= d[0];
                    if (d[0] && booted) cfg_seed <= st_valid ? st_seed : live_seed;   // keep the piece playing now
                    settings_seq <= settings_seq + 32'd1;
                end
                6'd6: if (d != cur) begin cfg_seed <= d; settings_seq <= settings_seq + 32'd1; end
                6'd7: if ({30'd0, d[1:0]} != cur) begin cfg_sleep <= d[1:0]; live_sleep <= d[1:0]; settings_seq <= settings_seq + 32'd1; end
                6'd8: cmd_reseed <= 1'b1;
                default: ;
            endcase
        end
        // Reset Exit: a save file loaded for this start restores Lock seed and the seed (it is
        // the only copy of the seed). Without one, a Lock seed restored from the menu file has
        // no seed to lock (the APF does not persist Seed), so it is turned off: RTC seed.
        if (rst_d && !rst) begin
            sav_new <= 1'b0;
            if (sav_new && sav_ok) begin
                cfg_seed_lock <= sav_w[1][0];
                if (sav_w[1][0]) cfg_seed <= sav_w[2];
            end else if (!booted && cfg_seed_lock && cfg_seed == 32'd0) cfg_seed_lock <= 1'b0;
        end
    end
endmodule
`default_nettype wire
