// pocket_lock.sv - Pocket lock (D62): ignore the buttons and black out the screen while the
// music plays on, so the Pocket can go in a pocket. clk_74a.
//
// Lock and unlock: hold Select + Start together for HOLD cycles (2 s). The core menu's
// "Pocket Lock" action (settings_bridge, 0x5000_0024) also locks. A core reset unlocks.
//
// While locked:
//   - no button edge reaches the composer;
//   - `blank` is high: ap_top sends black video. The openFPGA API has no backlight or panel
//     power command, so black is as "off" as a core can make the screen;
//   - for HINT cycles (3 s) after locking and after any button press, `blank` drops so the
//     firmware's lock screen ("hold Select + Start to unlock") shows.
//
// Start is the play / stop button and also half of the lock chord, so its press is passed on
// when Start is released, and only if Select was not down at any time while Start was held.
// Select itself is only the lock modifier (the composer has no use for it) and is not passed on.
// Every other button acts on its press as before.
`default_nettype none

module pocket_lock #(
    parameter integer HOLD = 148_500_000,      // 2 s at 74.25 MHz
    parameter integer HINT = 222_750_000       // 3 s
) (
    input  wire        clk,
    input  wire        rst,
    input  wire [15:0] keys,                   // synchronised button levels (regs.h BTN_* bits)
    input  wire [15:0] edges,                  // pressed edges of `keys`
    input  wire        menu_lock,              // pulse: the menu's Pocket Lock action
    output reg  [15:0] btn_out = 16'd0,        // edges for the composer
    output reg         locked = 1'b0,
    output wire        blank
);
    localparam integer B_SELECT = 14, B_START = 15;

    wire       chord = keys[B_SELECT] && keys[B_START];
    reg [27:0] hold_n = 28'd0;
    reg        armed = 1'b1;                   // the chord was released since the last toggle
    reg [27:0] hint_n = 28'd0;
    reg        start_pend = 1'b0;              // Start is down and still counts as play / stop
    reg        start_d = 1'b0;

    assign blank = locked && hint_n == 28'd0;

    always @(posedge clk) begin
        btn_out <= 16'd0;
        start_d <= keys[B_START];
        if (rst) begin
            locked <= 1'b0; hold_n <= 28'd0; armed <= 1'b1; hint_n <= 28'd0; start_pend <= 1'b0;
        end else begin
            if (hint_n != 28'd0) hint_n <= hint_n - 28'd1;

            // Start: pending from its press, cancelled by Select, sent on release
            if (keys[B_SELECT] || (start_d && !keys[B_START])) start_pend <= 1'b0;
            else if (edges[B_START]) start_pend <= 1'b1;
            if (!locked) begin
                btn_out <= edges & ~(16'd1 << B_START) & ~(16'd1 << B_SELECT);
                if (start_d && !keys[B_START] && start_pend) btn_out[B_START] <= 1'b1;
            end else if (|edges) hint_n <= HINT[27:0];

            // the chord, held for HOLD cycles, toggles the lock once per hold
            if (!chord) begin
                hold_n <= 28'd0; armed <= 1'b1;
            end else if (armed) begin
                if (hold_n == HOLD[27:0] - 28'd1) begin
                    locked <= !locked; armed <= 1'b0; hold_n <= 28'd0;
                    hint_n <= locked ? 28'd0 : HINT[27:0];
                end else hold_n <= hold_n + 28'd1;
            end

            if (menu_lock && !locked) begin locked <= 1'b1; hint_n <= HINT[27:0]; end
        end
    end
endmodule
`default_nettype wire
