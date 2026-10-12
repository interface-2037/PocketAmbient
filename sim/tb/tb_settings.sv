// tb_settings.sv - unit test of settings_bridge.sv (menu <-> composer control block, save slot)
// and slot_size.sv (save slot size in the APF data table).
// Reads are APF-style (D57): the word is taken a few cycles after the bridge_rd strobe, when
// bridge_addr has already moved on to the next address.
`timescale 1ns/1ps
module tb_settings;
    reg clk = 0;
    always #5 clk = ~clk;
    reg         wr = 0, rd = 0, st_valid = 0, rst = 1;
    reg  [31:0] addr = 0, wd = 0, st_seed = 32'hCAFE0001;
    reg   [2:0] st_mode = 0, st_density = 3, st_bright = 3;
    reg   [1:0] st_reverb = 1, st_visuals = 0, st_sleep = 0;
    wire [31:0] rdd, seq;
    wire  [2:0] mode, dens, bright;
    wire  [1:0] rev, vis, sleep;
    wire        lock, reseed;
    wire [31:0] seed;
    settings_bridge dut (.clk(clk), .rst(rst), .bridge_wr(wr), .bridge_rd(rd), .bridge_addr(addr), .bridge_wr_data(wd),
        .bridge_rd_data(rdd), .st_valid(st_valid), .st_mode(st_mode), .st_density(st_density),
        .st_bright(st_bright), .st_reverb(st_reverb), .st_visuals(st_visuals), .st_sleep(st_sleep),
        .st_seed(st_seed), .cfg_mode(mode), .cfg_density(dens), .cfg_bright(bright), .cfg_reverb(rev),
        .cfg_visuals(vis), .cfg_seed_lock(lock), .cfg_seed(seed), .cfg_sleep(sleep), .settings_seq(seq),
        .cmd_reseed(reseed));

    integer errs = 0, pulses = 0;
    always @(posedge clk) if (reseed) pulses = pulses + 1;
    task bw(input [5:0] k, input [31:0] v);
        begin @(negedge clk); addr = 32'h5000_0000 + 4 * k; wd = v; wr = 1; @(negedge clk) wr = 0; end
    endtask
    // APF read: strobe, the address moves on, the word is taken later
    task rd_at(input [31:0] a, output [31:0] v);
        begin
            @(negedge clk); addr = a; rd = 1;
            @(negedge clk) rd = 0; addr = a + 4;
            repeat (3) @(negedge clk);
            v = rdd;
        end
    endtask
    task br(input [5:0] k, output [31:0] v);
        rd_at(32'h5000_0000 + 4 * k, v);
    endtask
    task sw(input [2:0] k, input [31:0] v);      // APF loads a save slot word
        begin @(negedge clk); addr = 32'h6000_0000 + 4 * k; wd = v; wr = 1; @(negedge clk) wr = 0; end
    endtask
    task sr(input [2:0] k, output [31:0] v);
        rd_at(32'h6000_0000 + 4 * k, v);
    endtask
    task check(input c, input [8*56-1:0] what);
        begin if (!c) begin $display("FAIL: %0s", what); errs = errs + 1; end end
    endtask
    // the save file's check word, computed independently of the RTL
    function [31:0] chk(input [31:0] w0, w1, w2, w3, w4, w5, w6);
        reg [31:0] s;
        begin
            s = 32'h5A17_C3E9 + w0 + {w1[30:0], w1[31]} + {w2[29:0], w2[31:30]} + {w3[28:0], w3[31:29]}
                + {w4[27:0], w4[31:28]} + {w5[26:0], w5[31:27]} + {w6[25:0], w6[31:26]};
            chk = ~s;
        end
    endfunction
    task reset_enter; begin @(negedge clk); rst = 1; st_valid = 0; end endtask
    task reset_exit; begin @(negedge clk); rst = 0; repeat (2) @(negedge clk); end endtask

    reg [31:0] v, s0, f [0:7];
    integer j;
    initial begin
        repeat (3) @(negedge clk);
        // 1. persisted values written before Reset Exit, no reads yet
        bw(0, 2); bw(1, 5); bw(5, 1); bw(6, 32'd777);
        check(mode == 2 && dens == 5 && lock && seed == 777 && seq == 4, "pre-boot writes apply");
        br(1, v); check(v == 5, "read before status = stored value");
        br(6, v); check(v == 777, "seed read before status = stored seed");
        reset_exit;
        check(lock && seed == 777, "no save file: restored lock + seed kept");
        // 2. composer running: reads are live
        st_valid = 1; st_mode = 2; st_density = 5;
        br(0, v); check(v == 2, "live mode read");
        // 3. APF echo: read density (5), buttons change it to 4, APF writes 5 back -> ignored
        br(1, v); st_density = 4; s0 = seq; bw(1, v);
        check(seq == s0 && dens == 5, "echo write ignored");
        // 4. user picks the old stored value (5) in the menu while live is 4 -> applies
        br(1, v); bw(1, 5); check(seq == s0 + 1 && dens == 5, "menu change applies");
        // 5. density clamps
        br(1, v); st_density = 5; bw(1, 9); check(dens == 5, "clamp high (no change)"); bw(1, 0); check(dens == 1, "clamp low");
        // 6. lock seed: off, then on while running latches the session seed
        bw(5, 0); check(!lock, "unlock"); br(6, v); check(v == st_seed, "unlocked seed read = live");
        bw(5, 1); check(lock && seed == 32'hCAFE0001, "lock latches the session seed");
        // 7. reseed action pulses once per write, echo or not
        bw(8, 1); bw(8, 1); repeat (2) @(negedge clk); check(pulses == 2, "reseed pulses");
        // 8. invalid reverb value ignored
        s0 = seq; bw(3, 3); check(rev == 1 && seq == s0, "reverb 3 ignored"); bw(3, 2); check(rev == 2, "reverb 2"); st_reverb = 2;

        // 9. the v0.4.1 hardware case: menu Density 5 and Brightness 5, mode TIDES with the
        // buttons, Density 4 with the buttons, then Quit / power-off: reset first, then APF reads
        bw(1, 5); st_density = 5; bw(2, 5); st_bright = 5; bw(0, 1); st_mode = 1;
        repeat (2) @(negedge clk);
        st_density = 4; st_seed = 32'hA3DA_CBC8;                       // buttons (composer status)
        bw(5, 0); bw(5, 1); check(seed == 32'hA3DA_CBC8, "lock latches the new session seed");
        repeat (2) @(negedge clk);
        reset_enter;
        st_mode = 0; st_density = 0; st_bright = 0; st_reverb = 0; st_sleep = 0; st_seed = 0;   // stale status
        br(0, v); check(v == 1, "reset read: mode = live TIDES");
        br(1, v); check(v == 4, "reset read: density = live 4 (buttons)");
        br(2, v); check(v == 5, "reset read: brightness = 5");
        br(3, v); check(v == 2, "reset read: reverb = 2");
        br(4, v); check(v == 0, "reset read: visuals = 0");
        br(7, v); check(v == 0, "reset read: sleep = 0");
        br(5, v); check(v == 1, "reset read: lock = 1");
        br(6, v); check(v == 32'hA3DA_CBC8, "reset read: seed = locked seed");
        // the read word is held after the address moves on, until the next strobe
        @(negedge clk); addr = 32'h5000_0004; rd = 1; @(negedge clk) rd = 0;
        addr = 32'h5000_0014; repeat (5) @(negedge clk); check(rdd == 4, "word held until the next strobe");
        addr = 32'hF800_0000; repeat (5) @(negedge clk); check(rdd == 4, "word held outside the window");

        // 10. Quit reads of the save slot in reset: the image of the current settings
        for (j = 0; j < 8; j = j + 1) begin sr(j, v); f[j] = v; end
        check(f[0] == 32'h414D_4253, "save: magic");
        check(f[1] == 32'h0100_0001, "save: format 1, lock on");
        check(f[2] == 32'hA3DA_CBC8, "save: locked seed");
        check(f[3] == {10'd0, 2'd0, 2'd0, 2'd0, 2'd0, 2'd2, 1'b0, 3'd5, 1'b0, 3'd4, 1'b0, 3'd1}, "save: settings");
        check(f[4] == 0 && f[5] == 0 && f[6] == 0, "save: reserved words 0");
        check(f[7] == chk(f[0], f[1], f[2], f[3], f[4], f[5], f[6]), "save: check word");

        // 11. the APF loads the file it just read back, then Reset Exit (power-on restore from
        // fresh registers: tb_settings_boot); the seed comes only from the file
        bw(5, 0); bw(6, 0); check(!lock && seed == 0, "seed state cleared");
        bw(5, 1);
        for (j = 0; j < 8; j = j + 1) sw(j, f[j]);
        check(seed == 0, "seed not applied before Reset Exit");
        reset_exit;
        check(lock && seed == 32'hA3DA_CBC8, "Reset Exit: save file restores the locked seed");
        // a later Reset Exit without a new load does not apply the file again
        bw(5, 0); reset_enter; reset_exit; check(!lock, "no re-apply without a new load");

        // 12. a damaged file is ignored; Lock seed without a seed is turned off
        br(5, v); bw(5, 1); bw(6, 0);
        sw(7, f[7] ^ 32'h1); reset_enter; reset_exit;
        check(lock && seed == 0, "bad check word: file ignored");
        // 13. lock off in the file: Lock seed off, seed untouched
        bw(6, 32'd55); sw(1, 32'h0100_0000); sw(7, chk(f[0], 32'h0100_0000, f[2], f[3], 0, 0, 0));
        reset_enter; reset_exit; check(!lock && seed == 55, "file with lock off");
        wait (tb_settings_boot.done && tb_settings_cold.done);
        errs = errs + tb_settings_boot.errs + tb_settings_cold.errs;
        if (errs == 0) $display("PASS settings_bridge"); else $display("FAIL settings_bridge: %0d", errs);
        $finish;
    end
endmodule

// power-on paths, from fresh registers: before the composer has run, the loaded save words are
// given back unchanged; a Lock seed restored from the menu file without a seed is turned off;
// the menu file + save file written before Reset Exit are applied at boot, and Quit reads after
// a run return them. Also slot_size.
module tb_settings_boot;
    reg clk = 0;
    always #5 clk = ~clk;
    reg         wr = 0, rd = 0, rst = 1, st_valid = 0;
    reg  [31:0] addr = 0, wd = 0, st_seed = 0;
    reg   [2:0] st_mode = 0, st_density = 0, st_bright = 0;
    reg   [1:0] st_reverb = 0;
    wire [31:0] rdd, seed;
    wire  [2:0] mode, dens, bright;
    wire  [1:0] rev;
    wire        lock;
    settings_bridge dut (.clk(clk), .rst(rst), .bridge_wr(wr), .bridge_rd(rd), .bridge_addr(addr), .bridge_wr_data(wd),
        .bridge_rd_data(rdd), .st_valid(st_valid), .st_mode(st_mode), .st_density(st_density), .st_bright(st_bright),
        .st_reverb(st_reverb), .st_visuals(2'd0), .st_sleep(2'd0), .st_seed(st_seed), .cfg_mode(mode), .cfg_density(dens),
        .cfg_bright(bright), .cfg_reverb(rev), .cfg_visuals(), .cfg_seed_lock(lock), .cfg_seed(seed), .cfg_sleep(),
        .settings_seq(), .cmd_reseed());
    // the data table (port a of core_bridge_cmd's mf_datatable: 2 cycles of read latency)
    reg  [31:0] dt [0:255];
    reg  [31:0] q1, q2;
    reg   [1:0] start = 0;
    wire  [9:0] dt_addr;
    wire        dt_wren;
    wire [31:0] dt_data;
    always @(posedge clk) begin q1 <= dt[dt_addr[7:0]]; q2 <= q1; if (dt_wren) dt[dt_addr[7:0]] <= dt_data; end
    slot_size #(.ID(32'd1), .SIZE(32'h20)) u_sz (.clk(clk), .start(start), .dt_addr(dt_addr), .dt_wren(dt_wren),
        .dt_data(dt_data), .dt_q(q2));

    integer errs = 0, j;
    reg     done = 0;
    reg [31:0] v, f [0:7];
    task check(input c, input [8*56-1:0] what);
        begin if (!c) begin $display("FAIL: %0s", what); errs = errs + 1; end end
    endtask
    task wr_at(input [31:0] a, input [31:0] x);
        begin @(negedge clk); addr = a; wd = x; wr = 1; @(negedge clk) wr = 0; end
    endtask
    task rd_at(input [31:0] a, output [31:0] x);
        begin @(negedge clk); addr = a; rd = 1; @(negedge clk) rd = 0; addr = 32'hF800_0000; repeat (3) @(negedge clk); x = rdd; end
    endtask
    initial begin
        repeat (3) @(negedge clk);
        // a damaged / foreign file: given back unchanged before the first run
        for (j = 0; j < 8; j = j + 1) wr_at(32'h6000_0000 + 4 * j, 32'h1000_0000 * j + 32'h0BAD_0000 + j);
        for (j = 0; j < 8; j = j + 1) begin
            rd_at(32'h6000_0000 + 4 * j, v);
            check(v == 32'h1000_0000 * j + 32'h0BAD_0000 + j, "unbooted: loaded words returned unchanged");
        end
        wr_at(32'h5000_0014, 1);
        check(lock, "menu file restores Lock seed");
        @(negedge clk) rst = 0; repeat (2) @(negedge clk);
        check(!lock, "no seed: Lock seed off at Reset Exit");
        // power-on restore: menu file (TIDES, density 4, brightness 5, reverb High, Lock seed on)
        // and a valid save file (lock on, seed A3DACBC8), all before Reset Exit
        @(negedge clk) rst = 1;
        wr_at(32'h5000_0000, 1); wr_at(32'h5000_0004, 4); wr_at(32'h5000_0008, 5); wr_at(32'h5000_000C, 2);
        wr_at(32'h5000_0014, 1);
        f[0] = 32'h414D_4253; f[1] = 32'h0100_0001; f[2] = 32'hA3DA_CBC8; f[3] = 32'h0000_2541; f[4] = 0; f[5] = 0; f[6] = 0;
        f[7] = tb_settings.chk(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
        for (j = 0; j < 8; j = j + 1) wr_at(32'h6000_0000 + 4 * j, f[j]);
        @(negedge clk) rst = 0; repeat (2) @(negedge clk);
        check(mode == 1 && dens == 4 && bright == 5 && rev == 2, "boot: menu values applied");
        check(lock && seed == 32'hA3DA_CBC8, "boot: locked seed restored from the save file");
        // the composer runs with them, then Quit: reset, then the APF reads
        @(negedge clk); st_mode = 1; st_density = 4; st_bright = 5; st_reverb = 2; st_seed = 32'hA3DA_CBC8; st_valid = 1;
        repeat (4) @(negedge clk);
        @(negedge clk); rst = 1; st_valid = 0; st_mode = 0; st_density = 0; st_bright = 0; st_reverb = 0; st_seed = 0;
        rd_at(32'h5000_0000, v); check(v == 1, "quit: mode");
        rd_at(32'h5000_0004, v); check(v == 4, "quit: density");
        rd_at(32'h5000_0008, v); check(v == 5, "quit: brightness");
        rd_at(32'h5000_000C, v); check(v == 2, "quit: reverb");
        rd_at(32'h5000_0014, v); check(v == 1, "quit: lock");
        rd_at(32'h5000_0018, v); check(v == 32'hA3DA_CBC8, "quit: seed");
        for (j = 0; j < 8; j = j + 1) begin
            rd_at(32'h6000_0000 + 4 * j, v); check(v == f[j], "quit: save file = the loaded one");
        end
        // slot_size: dev layout (composer id 0 at index 0, save id 1 at index 1)
        for (j = 0; j < 256; j = j + 1) dt[j] = 32'hFFFF_FFFF;
        dt[0] = 0; dt[1] = 32'd47452; dt[2] = 1; dt[3] = 0;
        @(negedge clk) start = 2'b10; repeat (40) @(negedge clk);
        check(dt[3] == 32'h20 && dt[1] == 32'd47452, "dev table: save slot size set");
        // release layout (save id 1 at index 0), started by Reset Exit
        dt[0] = 1; dt[1] = 0; dt[2] = 32'hFFFF_FFFF; dt[3] = 32'hFFFF_FFFF;
        @(negedge clk) start = 2'b11; repeat (40) @(negedge clk);
        check(dt[1] == 32'h20 && dt[3] == 32'hFFFF_FFFF, "release table: save slot size set");
        // no save slot in the table: nothing written
        dt[1] = 32'd9; dt[0] = 32'd7;
        @(negedge clk) start = 0; @(negedge clk) start = 2'b01; repeat (60) @(negedge clk);
        check(dt[1] == 9 && dt[3] == 32'hFFFF_FFFF, "no save slot: table unchanged");
        done = 1;
    end
endmodule

// v0.4.4 (D59): the settings in ambient.sav win over the menu file at a cold start. Each
// tb_settings_pwr instance is one power-up (fresh registers). Session 0 starts with no save
// file, changes Density / Brightness / Mode with the buttons and Reverb in the menu, then Quit
// reads the menu and the save slot. Session 1 is the next power-on: the APF writes the menu
// values from its stale cache (only menu changes reach it) and the save file read at that Quit.
module tb_settings_cold;
    integer errs = 0;
    reg     done = 0;
    tb_settings_pwr #(.CASE(0)) s0 ();
    tb_settings_pwr #(.CASE(1)) s1 ();      // menu file first, then the save slot
    tb_settings_pwr #(.CASE(2)) s2 ();      // save slot first, then the menu file
    tb_settings_pwr #(.CASE(3)) s3 ();      // invalid / out-of-range / other-format files
    tb_settings_pwr #(.CASE(4)) s4 ();      // the v0.4.3 file from the hardware test
    initial begin
        wait (s0.done && s1.done && s2.done && s3.done && s4.done);
        errs = s0.errs + s1.errs + s2.errs + s3.errs + s4.errs;
        done = 1;
    end
endmodule

module tb_settings_pwr #(parameter CASE = 0);
    reg clk = 0;
    always #5 clk = ~clk;
    reg         wr = 0, rd = 0, rst = 1, st_valid = 0;
    reg  [31:0] addr = 0, wd = 0, st_seed = 32'h0000_1234;
    reg   [2:0] st_mode = 0, st_density = 0, st_bright = 0;
    reg   [1:0] st_reverb = 0, st_visuals = 0, st_sleep = 0;
    wire [31:0] rdd, seed, seq;
    wire  [2:0] mode, dens, bright;
    wire  [1:0] rev, vis, sleep;
    wire        lock;
    settings_bridge dut (.clk(clk), .rst(rst), .bridge_wr(wr), .bridge_rd(rd), .bridge_addr(addr), .bridge_wr_data(wd),
        .bridge_rd_data(rdd), .st_valid(st_valid), .st_mode(st_mode), .st_density(st_density), .st_bright(st_bright),
        .st_reverb(st_reverb), .st_visuals(st_visuals), .st_sleep(st_sleep), .st_seed(st_seed), .cfg_mode(mode),
        .cfg_density(dens), .cfg_bright(bright), .cfg_reverb(rev), .cfg_visuals(vis), .cfg_seed_lock(lock),
        .cfg_seed(seed), .cfg_sleep(sleep), .settings_seq(seq), .cmd_reseed());

    integer errs = 0, j, n;
    reg     done = 0;
    reg [31:0] v, s0, f [0:7], g [0:7], bad3 [0:8];
    reg [31:0] cache [0:7];             // the APF's menu cache: the values last set in the menu
    task check(input c, input [8*80-1:0] what);
        begin if (!c) begin $display("FAIL: power-up %0d: %0s", CASE, what); errs = errs + 1; end end
    endtask
    task wr_at(input [31:0] a, input [31:0] x);
        begin @(negedge clk); addr = a; wd = x; wr = 1; @(negedge clk) wr = 0; end
    endtask
    task rd_at(input [31:0] a, output [31:0] x);
        begin @(negedge clk); addr = a; rd = 1; @(negedge clk) rd = 0; addr = a + 4; repeat (3) @(negedge clk); x = rdd; end
    endtask
    task menu(input [2:0] k, input [31:0] x);       // a menu change: the APF caches and writes it
        begin cache[k] = x; wr_at(32'h5000_0000 + 4 * k, x); end
    endtask
    task restore_menu;                               // power-on: the menu file, from the cache
        for (j = 0; j < 8; j = j + 1) if (j != 6) wr_at(32'h5000_0000 + 4 * j, cache[j]);
    endtask
    task load_sav(input [31:0] w0, w1, w2, w3, w4, w5, w6, w7);
        begin
            wr_at(32'h6000_0000, w0); wr_at(32'h6000_0004, w1); wr_at(32'h6000_0008, w2); wr_at(32'h6000_000C, w3);
            wr_at(32'h6000_0010, w4); wr_at(32'h6000_0014, w5); wr_at(32'h6000_0018, w6); wr_at(32'h6000_001C, w7);
        end
    endtask
    task reset_exit; begin @(negedge clk) rst = 0; repeat (3) @(negedge clk); end endtask
    task reset_enter; begin @(negedge clk) rst = 1; st_valid = 0; repeat (3) @(negedge clk); end endtask
    task run;                                        // the composer starts with cfg_*
        begin
            @(negedge clk); st_mode = mode; st_density = dens; st_bright = bright; st_reverb = rev;
            st_visuals = vis; st_sleep = sleep; st_valid = 1; repeat (3) @(negedge clk);
        end
    endtask
    // live values and APF-style menu reads equal the expected settings
    task expect_set(input [2:0] m, input [2:0] dn, input [2:0] b, input [1:0] r, input [1:0] vi, input [1:0] sl,
                    input [8*64-1:0] what);
        begin
            check(mode == m && dens == dn && bright == b && rev == r && vis == vi && sleep == sl, {what, ": cfg"});
            rd_at(32'h5000_0000, v); check(v == m, {what, ": menu mode"});
            rd_at(32'h5000_0004, v); check(v == dn, {what, ": menu density"});
            rd_at(32'h5000_0008, v); check(v == b, {what, ": menu brightness"});
            rd_at(32'h5000_000C, v); check(v == r, {what, ": menu reverb"});
            rd_at(32'h5000_0010, v); check(v == vi, {what, ": menu visuals"});
            rd_at(32'h5000_001C, v); check(v == sl, {what, ": menu sleep"});
        end
    endtask
    function [31:0] set3(input [2:0] m, input [2:0] dn, input [2:0] b, input [1:0] r, input [1:0] vi, input [1:0] sl);
        set3 = {10'd0, sl, 2'd0, vi, 2'd0, r, 1'b0, b, 1'b0, dn, 1'b0, m};
    endfunction
    function [31:0] ck(input [31:0] w1, w2, w3);
        ck = tb_settings.chk(32'h414D_4253, w1, w2, w3, 0, 0, 0);
    endfunction

    initial begin
        // the APF's cache before session 0: defaults, then Density 5 set in the menu (and kept)
        cache[0] = 0; cache[1] = 3; cache[2] = 3; cache[3] = 1; cache[4] = 0; cache[5] = 0; cache[6] = 0; cache[7] = 0;
        repeat (3) @(negedge clk);
        if (CASE == 0) begin
            // first boot: no save file, the menu file is used
            restore_menu;
            reset_exit;
            expect_set(0, 3, 3, 1, 0, 0, "no save file: menu file kept");
            run;
            menu(1, 5); st_density = 5; check(dens == 5, "menu Density 5");
            // buttons: Down (density 4), L (brightness 2), Right (TIDES); menu Reverb High
            @(negedge clk) st_density = 4; st_bright = 2; st_mode = 1;
            repeat (3) @(negedge clk);
            s0 = seq; menu(3, 2); st_reverb = 2; check(rev == 2 && seq == s0 + 1, "menu Reverb High in the session");
            repeat (3) @(negedge clk);
            // Quit / power-off: reset, then the APF reads the menu (live values) and the save slot
            reset_enter;
            st_mode = 0; st_density = 0; st_bright = 0; st_reverb = 0;
            rd_at(32'h5000_0004, v); check(v == 4, "quit: menu density = live 4");
            for (j = 0; j < 8; j = j + 1) rd_at(32'h6000_0000 + 4 * j, f[j]);
            check(f[0] == 32'h414D_4253 && f[1] == 32'h0100_0000, "quit: save magic / format 1 / lock off");
            check(f[3] == set3(1, 4, 2, 2, 0, 0), "quit: save settings = live (button + menu changes)");
            check(f[7] == tb_settings.chk(f[0], f[1], f[2], f[3], f[4], f[5], f[6]), "quit: save check word");
        end else if (CASE == 1 || CASE == 2) begin
            // the next power-on: the menu file from the APF's stale cache (Density 5, Brightness 3,
            // DRIFT, Reverb High) and the save file read at session 0's Quit
            wait (tb_settings_cold.s0.done);
            cache[1] = 5; cache[3] = 2;
            for (j = 0; j < 8; j = j + 1) g[j] = tb_settings_cold.s0.f[j];
            if (CASE == 1) restore_menu;
            load_sav(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
            if (CASE == 2) restore_menu;
            check(dens == 5 && bright == 3 && mode == 0, "before Reset Exit: the menu file's values");
            reset_exit;
            expect_set(1, 4, 2, 2, 0, 0, "Reset Exit: the save file's settings");
            check(!lock, "lock off from the file");
            run;
            check(st_density == 4 && st_mode == 1, "the composer starts with them");
            // a menu change in this session applies, and a later Reset Exit without a load
            // does not bring the file's values back
            s0 = seq; menu(1, 2); st_density = 2; check(dens == 2 && seq == s0 + 1, "menu change after Reset Exit");
            reset_enter; reset_exit;
            expect_set(1, 2, 2, 2, 0, 0, "second Reset Exit, no load: no re-apply");
            // the APF loads the (now older) file again after the composer has run: not applied
            run;
            load_sav(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
            reset_enter; reset_exit;
            expect_set(1, 2, 2, 2, 0, 0, "new load after the composer ran: not applied");
            run;
            // Quit: the save file carries the menu change
            reset_enter;
            for (j = 0; j < 8; j = j + 1) rd_at(32'h6000_0000 + 4 * j, f[j]);
            check(f[3] == set3(1, 2, 2, 2, 0, 0), "quit: save settings carry the menu change");
        end else if (CASE == 3) begin
            // files that must leave the menu file's values in place (fresh registers: never booted)
            cache[0] = 2; cache[1] = 5; cache[2] = 1; cache[3] = 0; cache[4] = 1; cache[7] = 2;
            bad3[0] = set3(1, 0, 2, 2, 0, 0);       // density 0
            bad3[1] = set3(1, 6, 2, 2, 0, 0);       // density 6
            bad3[2] = set3(1, 4, 0, 2, 0, 0);       // brightness 0
            bad3[3] = set3(1, 4, 7, 2, 0, 0);       // brightness 7
            bad3[4] = set3(4, 4, 2, 2, 0, 0);       // mode 4
            bad3[5] = set3(1, 4, 2, 3, 0, 0);       // reverb 3
            bad3[6] = set3(1, 4, 2, 2, 3, 0);       // visuals 3
            bad3[7] = set3(1, 4, 2, 2, 0, 0) | 32'h0040_0000;   // a reserved bit
            bad3[8] = set3(1, 4, 2, 2, 0, 0) | 32'h0000_0008;   // mode bit 3
            for (n = 0; n < 9; n = n + 1) begin
                restore_menu;
                load_sav(32'h414D_4253, 32'h0100_0001, 32'd99 + n, bad3[n], 0, 0, 0, ck(32'h0100_0001, 32'd99 + n, bad3[n]));
                reset_exit;
                expect_set(2, 5, 1, 0, 1, 2, "out-of-range settings word: menu file kept");
                check(lock && seed == 32'd99 + n, "out-of-range settings word: seed still restored");
                reset_enter;
            end
            // no save file at all
            restore_menu; reset_exit;
            expect_set(2, 5, 1, 0, 1, 2, "no save file: menu file kept");
            reset_enter;
            // a damaged file (check word), a foreign magic, another format: all ignored
            restore_menu;
            load_sav(32'h414D_4253, 32'h0100_0000, 0, set3(1, 4, 2, 2, 0, 0), 0, 0, 0, ck(32'h0100_0000, 0, set3(1, 4, 2, 2, 0, 0)) ^ 32'h1);
            reset_exit; expect_set(2, 5, 1, 0, 1, 2, "bad check word: menu file kept"); reset_enter;
            restore_menu;
            load_sav(32'h414D_4254, 32'h0100_0000, 0, set3(1, 4, 2, 2, 0, 0), 0, 0, 0,
                     tb_settings.chk(32'h414D_4254, 32'h0100_0000, 0, set3(1, 4, 2, 2, 0, 0), 0, 0, 0));
            reset_exit; expect_set(2, 5, 1, 0, 1, 2, "foreign magic: menu file kept"); reset_enter;
            restore_menu;
            load_sav(32'h414D_4253, 32'h0200_0000, 0, set3(1, 4, 2, 2, 0, 0), 0, 0, 0, ck(32'h0200_0000, 0, set3(1, 4, 2, 2, 0, 0)));
            reset_exit; expect_set(2, 5, 1, 0, 1, 2, "format 2 file: menu file kept"); reset_enter;
            // finally a valid one, still before the composer has run: applied
            restore_menu;
            load_sav(32'h414D_4253, 32'h0100_0000, 0, set3(3, 1, 5, 1, 2, 3), 0, 0, 0, ck(32'h0100_0000, 0, set3(3, 1, 5, 1, 2, 3)));
            reset_exit; expect_set(3, 1, 5, 1, 2, 3, "valid file, every field at its limit");
            // a menu change, then Reset Exit without a new load (composer not run yet): kept
            menu(1, 3); reset_enter; reset_exit;
            expect_set(3, 3, 5, 1, 2, 3, "unbooted Reset Exit, no load: no re-apply");
        end else begin
            // the hardware evidence (2026-10-12, 11:57): the v0.4.3 file, with the menu file
            // restoring Density 5 / Brightness 3 / TIDES / Reverb High from the stale cache.
            // Word 1 is 01000000 (format 1, lock off): the check word CBD1BC75 matches only that
            cache[0] = 1; cache[1] = 5; cache[2] = 3; cache[3] = 2;
            restore_menu;
            load_sav(32'h414D_4253, 32'h0100_0000, 32'hA5B2_0AD1, 32'h0000_2241, 0, 0, 0, 32'hCBD1_BC75);
            reset_exit;
            expect_set(1, 4, 2, 2, 0, 0, "v0.4.3 hardware file: density 4, brightness 2");
            check(!lock, "v0.4.3 hardware file: lock off");
        end
        done = 1;
    end
endmodule
