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
        wait (tb_settings_boot.done);
        errs = errs + tb_settings_boot.errs;
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
