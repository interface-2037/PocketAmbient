// tb_pocket_lock.sv - unit test of pocket_lock.sv (D63) with short HOLD / HINT times.
//   iverilog -g2012 -s tb_pocket_lock -o sim/out/tb_pocket_lock.vvp sim/tb/tb_pocket_lock.sv src/fpga/core/pocket_lock.sv
//   vvp -n sim/out/tb_pocket_lock.vvp
`timescale 1ns/1ps
module tb_pocket_lock;
    localparam integer HOLD = 100, HINT = 50;
    localparam [15:0] A = 16'h0010, B = 16'h0020, UP = 16'h0001, SEL = 16'h4000, START = 16'h8000;
    reg clk = 0;
    always #5 clk = ~clk;
    reg         rst = 1, menu = 0;
    reg  [15:0] keys = 0, kd = 0;
    always @(posedge clk) kd <= keys;
    wire [15:0] edges = keys & ~kd;
    wire [15:0] out;
    wire        locked, hint;
    pocket_lock #(.HOLD(HOLD), .HINT(HINT)) dut (.clk(clk), .rst(rst), .keys(keys), .edges(edges),
        .menu_lock(menu), .btn_out(out), .locked(locked), .hint(hint));

    integer errs = 0;
    reg [15:0] seen;                       // OR of every btn_out since the last clear
    integer    n_out;                      // number of cycles with btn_out != 0
    always @(posedge clk) if (out != 0) begin seen = seen | out; n_out = n_out + 1; end
    task clr; begin seen = 0; n_out = 0; end endtask
    task wait_n(input integer n); repeat (n) @(negedge clk); endtask
    task press(input [15:0] m); begin @(negedge clk) keys = keys | m; end endtask
    task release_(input [15:0] m); begin @(negedge clk) keys = keys & ~m; end endtask
    task tap(input [15:0] m); begin press(m); wait_n(5); release_(m); wait_n(5); end endtask
    task check(input cond, input [8*64-1:0] what);
        begin if (!cond) begin errs = errs + 1; $display("FAIL: %0s (t=%0t)", what, $time); end end
    endtask

    initial begin
        wait_n(4); rst = 0; wait_n(2);

        // ---- unlocked: buttons act on their press, Start on its release
        clr; tap(A);
        check(seen == A && n_out == 1, "A passes once");
        clr; press(START); wait_n(20);
        check(n_out == 0, "Start not sent while held");
        release_(START); wait_n(3);
        check(seen == START && n_out == 1, "Start sent once on release");
        clr; press(SEL); wait_n(3); tap(START); release_(SEL); wait_n(3);
        check(n_out == 0, "Select then Start: no play/stop");
        clr; press(START); wait_n(3); press(SEL); wait_n(3); release_(SEL); wait_n(3); release_(START); wait_n(3);
        check(n_out == 0, "Start then Select: no play/stop");
        clr; press(SEL); wait_n(3); tap(UP); release_(SEL); wait_n(3);
        check(seen == UP && n_out == 1, "other buttons still act with Select down");
        check(!locked && !hint, "still unlocked");

        // ---- a short chord does nothing
        press(SEL | START); wait_n(HOLD - 5); release_(SEL | START); wait_n(3);
        check(!locked, "chord shorter than HOLD does not lock");

        // ---- holding the chord locks once, with the hint first
        clr; press(SEL | START); wait_n(HOLD + 2);
        check(locked && hint, "locked after HOLD, lock screen showing");
        wait_n(3 * HOLD);
        check(locked, "held on: no second toggle");
        check(!hint, "player screen back after the hint");
        release_(SEL | START); wait_n(3);
        check(n_out == 0, "no button reached the composer during the lock chord");

        // ---- locked: presses are swallowed and show the hint
        clr; tap(A); tap(B); tap(UP); tap(START);
        check(n_out == 0, "locked: nothing passes");
        check(hint, "locked: a press shows the lock screen");
        wait_n(HINT + 2);
        check(!hint && locked, "lock screen times out, still locked");
        tap(A);
        check(hint, "lock screen again on the next press");
        wait_n(HINT + 2);

        // ---- unlock: hold the chord again
        clr; press(SEL | START); wait_n(5);
        check(hint, "pressing the chord shows the lock screen");
        wait_n(HOLD);
        check(!locked && !hint, "unlocked after HOLD");
        release_(SEL | START); wait_n(3);
        check(n_out == 0, "unlock: no play/stop on release");
        clr; tap(A);
        check(seen == A && n_out == 1, "buttons work again");

        // ---- the menu action locks; Start held across it is not sent
        press(START); wait_n(3);
        @(negedge clk) menu = 1; @(negedge clk) menu = 0;
        clr; wait_n(3);
        check(locked && hint, "menu lock, lock screen showing");
        release_(START); wait_n(3);
        check(n_out == 0, "Start pressed before the lock is not sent");
        wait_n(HINT + 2);
        check(locked && !hint, "menu lock: player screen after the hint");
        @(negedge clk) menu = 1; @(negedge clk) menu = 0; wait_n(2);
        check(locked && !hint, "menu action while locked changes nothing");

        // ---- a reset unlocks
        @(negedge clk) rst = 1; wait_n(3); @(negedge clk) rst = 0; wait_n(2);
        check(!locked && !hint, "reset unlocks");

        if (errs == 0) $display("PASS pocket_lock"); else $display("FAIL pocket_lock: %0d", errs);
        $finish;
    end
endmodule
