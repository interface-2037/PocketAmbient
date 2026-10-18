// tb_sleep.sv - unit test of sleep_state.sv + the save state window of settings_bridge.sv (D62).
// Sleep: the APF-style request handshake (rising edge, ack, busy, ok held until the next
// request), the blob image of the live state, consistent while the state changes during the
// reads. Wake on fresh registers (a new configuration): the menu file is written in reset, the
// core boots, the APF writes the blob while it runs and requests the load; the settings, Lock seed
// and seed must come from the blob, the menu must read them back, and CR_RESUME must carry
// the session seed, state and elapsed time with sequence 1. Rejected blobs (bad magic / format /
// check word / state 3 / out-of-range settings / reserved bits) change nothing. Reset entry clears
// the sequence. A Memories load while running is applied like a wake.
`timescale 1ns/1ps
module tb_sleep;
    reg clk = 0;
    always #5 clk = ~clk;
    reg         wr = 0, rd = 0, st_valid = 0, rst = 1;
    reg  [31:0] addr = 0, wd = 0, st_seed = 0, st_elapsed = 0;
    reg   [2:0] st_mode = 0, st_density = 3, st_bright = 3;
    reg   [1:0] st_reverb = 1, st_visuals = 0, st_sleep = 0, st_state = 0;
    reg         start = 0, load = 0;
    wire [31:0] rdd, seq, rinfo, rseed, ss_addr, ss_size, ss_max;
    wire  [2:0] mode, dens, bright;
    wire  [1:0] rev, vis, sleep;
    wire        lock, reseed, snap, restore, sup;
    wire        s_ack, s_busy, s_ok, s_err, l_ack, l_busy, l_ok, l_err;
    wire [31:0] seed;
    sleep_state ss (.clk(clk), .supported(sup), .addr(ss_addr), .size(ss_size), .maxloadsize(ss_max),
        .start(start), .start_ack(s_ack), .start_busy(s_busy), .start_ok(s_ok), .start_err(s_err),
        .load(load), .load_ack(l_ack), .load_busy(l_busy), .load_ok(l_ok), .load_err(l_err),
        .snap(snap), .restore(restore));
    settings_bridge dut (.clk(clk), .rst(rst), .bridge_wr(wr), .bridge_rd(rd), .bridge_addr(addr), .bridge_wr_data(wd),
        .bridge_rd_data(rdd), .st_valid(st_valid), .st_mode(st_mode), .st_density(st_density),
        .st_bright(st_bright), .st_reverb(st_reverb), .st_visuals(st_visuals), .st_sleep(st_sleep),
        .st_seed(st_seed), .st_state(st_state), .st_elapsed(st_elapsed), .ss_snap(snap), .ss_restore(restore),
        .resume_info(rinfo), .resume_seed(rseed),
        .cfg_mode(mode), .cfg_density(dens), .cfg_bright(bright), .cfg_reverb(rev),
        .cfg_visuals(vis), .cfg_seed_lock(lock), .cfg_seed(seed), .cfg_sleep(sleep), .settings_seq(seq),
        .cmd_reseed(reseed));

    integer errs = 0, acks = 0;
    always @(posedge clk) if (s_ack || l_ack) acks = acks + 1;
    task check(input c, input [8*60-1:0] what);
        begin if (!c) begin $display("FAIL: %0s", what); errs = errs + 1; end end
    endtask
    task bw(input [31:0] a, input [31:0] v);
        begin @(negedge clk); addr = a; wd = v; wr = 1; @(negedge clk) wr = 0; end
    endtask
    task rd_at(input [31:0] a, output [31:0] v);     // APF read: the word is taken later
        begin
            @(negedge clk); addr = a; rd = 1;
            @(negedge clk) rd = 0; addr = a + 4;
            repeat (3) @(negedge clk);
            v = rdd;
        end
    endtask
    // core_bridge_cmd: hold the request until the ack, then wait for ok
    task request(input is_load);
        integer n;
        begin
            @(negedge clk); if (is_load) load = 1; else start = 1;
            n = 0;
            while (!(is_load ? l_ack : s_ack) && n < 10) begin @(negedge clk); n = n + 1; end
            check(n < 10, "request acked");
            @(negedge clk); load = 0; start = 0;
            n = 0;
            while (!(is_load ? l_ok : s_ok) && n < 10) begin @(negedge clk); n = n + 1; end
            check(n < 10, "request reaches ok");
        end
    endtask
    function [31:0] chk(input [31:0] w0, w1, w2, w3, w4, w5, w6);
        reg [31:0] s;
        begin
            s = 32'h5A17_C3E9 + w0 + {w1[30:0], w1[31]} + {w2[29:0], w2[31:30]} + {w3[28:0], w3[31:29]}
                + {w4[27:0], w4[31:28]} + {w5[26:0], w5[31:27]} + {w6[25:0], w6[31:26]};
            chk = ~s;
        end
    endfunction
    // settings word: mode, density, brightness, reverb, visuals, sleep
    function [31:0] sw(input [2:0] m, input [2:0] dn, input [2:0] br, input [1:0] rv, input [1:0] vs, input [1:0] sl);
        sw = {10'd0, sl, 2'd0, vs, 2'd0, rv, 1'b0, br, 1'b0, dn, 1'b0, m};
    endfunction

    localparam [31:0] A = 32'h7000_0000;
    reg [31:0] v, img [0:7];
    integer j;
    initial begin
        check(sup && ss_addr == A && ss_size == 32'h20 && ss_max == 32'h20, "query: supported, addr, size");
        // ---- session before sleep: running TIDES / 4 / 2 / High / Minimal / sleep 60, unlocked
        bw(32'h5000_0000 + 4 * 6, 32'd555);                  // menu seed (lock off)
        @(negedge clk); rst = 0; repeat (2) @(negedge clk);
        st_valid = 1; st_mode = 1; st_density = 4; st_bright = 2; st_reverb = 2; st_visuals = 1; st_sleep = 2;
        st_state = 1; st_seed = 32'hDEAD_BEEF; st_elapsed = 32'd3725;
        repeat (2) @(negedge clk);
        request(0);
        check(acks == 1, "one ack per request (held request)");
        // the state moves on while the APF reads: the blob must not change
        st_elapsed = 32'd3726; st_seed = 32'h1111_2222; st_density = 5;
        for (j = 0; j < 8; j = j + 1) rd_at(A + 4 * j, img[j]);
        check(img[0] == 32'h414D_5353, "blob magic AMSS");
        check(img[1] == 32'h0100_0001, "blob word 1: format 1, unlocked, playing");
        check(img[2] == 32'd555, "blob word 2: menu seed");
        check(img[3] == 32'hDEAD_BEEF, "blob word 3: session seed at the snap");
        check(img[4] == sw(1, 4, 2, 2, 1, 2), "blob word 4: settings at the snap");
        check(img[5] == 32'd3725, "blob word 5: elapsed at the snap");
        check(img[6] == 0 && img[7] == chk(img[0], img[1], img[2], img[3], img[4], img[5], img[6]), "blob check word");
        check(s_ok, "start ok held");
        request(0);
        rd_at(A + 4 * 5, v);
        check(v == 32'd3726, "a second snap takes the new state");
        $display("tb_sleep: sleep side done");
        if (errs == 0) $display("PASS sleep handshake");
        wait (tb_sleep_wake.done);
        errs = errs + tb_sleep_wake.errs;
        if (errs == 0) $display("PASS sleep_state");
        else $display("FAIL sleep_state (%0d)", errs);
        $finish;
    end
endmodule

// wake on fresh registers (a new configuration)
module tb_sleep_wake;
    reg clk = 0;
    always #5 clk = ~clk;
    reg         wr = 0, rd = 0, st_valid = 0, rst = 1;
    reg  [31:0] addr = 0, wd = 0, st_seed = 32'h0BAD_0BAD, st_elapsed = 0;
    reg   [2:0] st_mode = 0, st_density = 3, st_bright = 3;
    reg   [1:0] st_reverb = 1, st_visuals = 0, st_sleep = 0, st_state = 0;
    reg         start = 0, load = 0;
    wire [31:0] rdd, seq, rinfo, rseed;
    wire  [2:0] mode, dens, bright;
    wire  [1:0] rev, vis, sleep;
    wire        lock, reseed, snap, restore;
    wire        s_ack, s_busy, s_ok, l_ack, l_busy, l_ok;
    wire [31:0] seed;
    sleep_state ss (.clk(clk), .supported(), .addr(), .size(), .maxloadsize(),
        .start(start), .start_ack(s_ack), .start_busy(s_busy), .start_ok(s_ok), .start_err(),
        .load(load), .load_ack(l_ack), .load_busy(l_busy), .load_ok(l_ok), .load_err(),
        .snap(snap), .restore(restore));
    settings_bridge dut (.clk(clk), .rst(rst), .bridge_wr(wr), .bridge_rd(rd), .bridge_addr(addr), .bridge_wr_data(wd),
        .bridge_rd_data(rdd), .st_valid(st_valid), .st_mode(st_mode), .st_density(st_density),
        .st_bright(st_bright), .st_reverb(st_reverb), .st_visuals(st_visuals), .st_sleep(st_sleep),
        .st_seed(st_seed), .st_state(st_state), .st_elapsed(st_elapsed), .ss_snap(snap), .ss_restore(restore),
        .resume_info(rinfo), .resume_seed(rseed),
        .cfg_mode(mode), .cfg_density(dens), .cfg_bright(bright), .cfg_reverb(rev),
        .cfg_visuals(vis), .cfg_seed_lock(lock), .cfg_seed(seed), .cfg_sleep(sleep), .settings_seq(seq),
        .cmd_reseed(reseed));

    integer errs = 0;
    reg done = 0;
    task check(input c, input [8*60-1:0] what);
        begin if (!c) begin $display("FAIL wake: %0s", what); errs = errs + 1; end end
    endtask
    task bw(input [31:0] a, input [31:0] v);
        begin @(negedge clk); addr = a; wd = v; wr = 1; @(negedge clk) wr = 0; end
    endtask
    task rd_at(input [31:0] a, output [31:0] v);
        begin
            @(negedge clk); addr = a; rd = 1;
            @(negedge clk) rd = 0; addr = a + 4;
            repeat (3) @(negedge clk);
            v = rdd;
        end
    endtask
    task request_load;
        integer n;
        begin
            @(negedge clk); load = 1;
            n = 0; while (!l_ack && n < 10) begin @(negedge clk); n = n + 1; end
            @(negedge clk); load = 0;
            n = 0; while (!l_ok && n < 10) begin @(negedge clk); n = n + 1; end
            check(n < 10, "load reaches ok");
            repeat (2) @(negedge clk);
        end
    endtask
    function [31:0] chk(input [31:0] w0, w1, w2, w3, w4, w5, w6);
        reg [31:0] s;
        begin
            s = 32'h5A17_C3E9 + w0 + {w1[30:0], w1[31]} + {w2[29:0], w2[31:30]} + {w3[28:0], w3[31:29]}
                + {w4[27:0], w4[31:28]} + {w5[26:0], w5[31:27]} + {w6[25:0], w6[31:26]};
            chk = ~s;
        end
    endfunction
    function [31:0] sw(input [2:0] m, input [2:0] dn, input [2:0] br, input [1:0] rv, input [1:0] vs, input [1:0] sl);
        sw = {10'd0, sl, 2'd0, vs, 2'd0, rv, 1'b0, br, 1'b0, dn, 1'b0, m};
    endfunction
    localparam [31:0] A = 32'h7000_0000;
    reg [31:0] v, b [0:7];
    integer j;
    task blob(input [31:0] w1, input [31:0] w2, input [31:0] w3, input [31:0] w4, input [31:0] w5, input bad_chk);
        begin
            b[0] = 32'h414D_5353; b[1] = w1; b[2] = w2; b[3] = w3; b[4] = w4; b[5] = w5; b[6] = 0;
            b[7] = chk(b[0], b[1], b[2], b[3], b[4], b[5], b[6]) ^ {31'd0, bad_chk};
            for (j = 0; j < 8; j = j + 1) bw(A + 4 * j, b[j]);
        end
    endtask
    // a blob that must be rejected: nothing changes
    task reject(input [31:0] w0, input [31:0] w1, input [31:0] w4, input bad_chk, input [8*60-1:0] what);
        reg [31:0] i0, s0, q0; reg [2:0] m0, d0; reg l0;
        begin
            blob(w1, 32'd99, 32'd98, w4, 32'd5, bad_chk);
            if (w0 != 32'h414D_5353) bw(A, w0);
            i0 = rinfo; s0 = seed; m0 = mode; d0 = dens; l0 = lock; q0 = rseed;
            request_load;
            check(rinfo == i0 && rseed == q0 && seed == s0 && mode == m0 && dens == d0 && lock == l0, what);
        end
    endtask

    initial begin
        // the APF writes the stale menu file in reset: DRIFT / 5 / 3 / Med, lock off
        bw(32'h5000_0000 + 0, 0); bw(32'h5000_0000 + 4, 5); bw(32'h5000_0000 + 8, 3);
        @(negedge clk); rst = 0; repeat (2) @(negedge clk);
        st_valid = 1; st_mode = 0; st_density = 5; st_bright = 3; st_state = 0;   // booted, at the title
        repeat (2) @(negedge clk);
        check(rinfo == 0, "no resume before the load");
        // the blob from the sleep: TIDES / 4 / 2 / High / Minimal / 60 min, lock on (seed 4242),
        // playing for 3725 s, session seed DEADBEEF
        blob(32'h0100_0005, 32'd4242, 32'hDEAD_BEEF, sw(1, 4, 2, 2, 1, 2), 32'd3725, 0);
        check(rinfo == 0 && mode == 0, "writing the blob alone changes nothing");
        request_load;
        check(rinfo == {8'd1, 2'd1, 22'd3725}, "CR_RESUME: seq 1, playing, 3725 s");
        check(rseed == 32'hDEAD_BEEF, "CR_RESUME_SEED: the session seed");
        check(mode == 1 && dens == 4 && bright == 2 && rev == 2 && vis == 1 && sleep == 2, "settings from the blob");
        check(lock && seed == 32'd4242, "Lock seed and the seed from the blob");
        // the composer resumes and publishes the new status (within a tick on the hardware)
        st_mode = mode; st_density = dens; st_bright = bright; st_reverb = rev; st_visuals = vis; st_sleep = sleep;
        repeat (2) @(negedge clk);
        rd_at(32'h5000_0000 + 4 * 1, v); check(v == 4, "menu reads the blob's density");
        rd_at(32'h5000_0000 + 4 * 0, v); check(v == 1, "menu reads the blob's mode");
        // rejected blobs, each with otherwise valid fields
        reject(32'h414D_5354, 32'h0100_0001, sw(2, 1, 1, 0, 0, 0), 0, "bad magic rejected");
        reject(32'h414D_5353, 32'h0200_0001, sw(2, 1, 1, 0, 0, 0), 0, "format 2 rejected");
        reject(32'h414D_5353, 32'h0100_0001, sw(2, 1, 1, 0, 0, 0), 1, "bad check word rejected");
        reject(32'h414D_5353, 32'h0100_0003, sw(2, 1, 1, 0, 0, 0), 0, "state 3 rejected");
        reject(32'h414D_5353, 32'h0100_0011, sw(2, 1, 1, 0, 0, 0), 0, "reserved bit in word 1 rejected");
        reject(32'h414D_5353, 32'h0100_0001, sw(2, 0, 1, 0, 0, 0), 0, "density 0 rejected");
        reject(32'h414D_5353, 32'h0100_0001, sw(2, 1, 6, 0, 0, 0), 0, "brightness 6 rejected");
        reject(32'h414D_5353, 32'h0100_0001, sw(2, 1, 1, 3, 0, 0), 0, "reverb 3 rejected");
        reject(32'h414D_5353, 32'h0100_0001, sw(2, 1, 1, 0, 0, 0) | 32'h8000_0000, 0, "reserved settings bit rejected");
        // a Memories load while running: HUSH, stopped, lock off, elapsed past 22 bits (clamped)
        st_state = 1;
        blob(32'h0100_0000, 32'd7, 32'h0000_1234, sw(2, 1, 5, 0, 2, 3), 32'h0100_0000, 0);
        request_load;
        check(rinfo == {8'd2, 2'd0, 22'h3F_FFFF}, "second load: seq 2, title, elapsed clamped");
        check(rseed == 32'h1234 && !lock && seed == 7 && mode == 2 && bright == 5 && sleep == 3, "second load applied");
        // reset entry clears the sequence, so a core reset cannot replay it
        @(negedge clk); rst = 1; st_valid = 0; repeat (2) @(negedge clk);
        check(rinfo[31:24] == 0, "reset entry clears the resume sequence");
        @(negedge clk); rst = 0; repeat (2) @(negedge clk);
        check(rinfo[31:24] == 0 && mode == 2, "reset exit: no resume, settings kept");
        done = 1;
    end
endmodule
