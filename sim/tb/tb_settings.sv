// tb_settings.sv - unit test of settings_bridge.sv (menu <-> composer control block).
`timescale 1ns/1ps
module tb_settings;
    reg clk = 0;
    always #5 clk = ~clk;
    reg         wr = 0, rd = 0, st_valid = 0;
    reg  [31:0] addr = 0, wd = 0, st_seed = 32'hCAFE0001;
    reg   [2:0] st_mode = 0, st_density = 3, st_bright = 3;
    reg   [1:0] st_reverb = 1, st_visuals = 0, st_sleep = 0;
    wire [31:0] rdd, seq;
    wire  [2:0] mode, dens, bright;
    wire  [1:0] rev, vis, sleep;
    wire        lock, reseed;
    wire [31:0] seed;
    settings_bridge dut (.clk(clk), .bridge_wr(wr), .bridge_rd(rd), .bridge_addr(addr), .bridge_wr_data(wd),
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
    task br(input [5:0] k, output [31:0] v);
        begin @(negedge clk); addr = 32'h5000_0000 + 4 * k; rd = 1; @(negedge clk) rd = 0; v = rdd; end
    endtask
    task check(input c, input [8*48-1:0] what);
        begin if (!c) begin $display("FAIL: %0s", what); errs = errs + 1; end end
    endtask
    reg [31:0] v, s0;
    initial begin
        repeat (3) @(negedge clk);
        // 1. persisted values written before Reset Exit, no reads yet
        bw(0, 2); bw(1, 5); bw(5, 1); bw(6, 32'd777);
        check(mode == 2 && dens == 5 && lock && seed == 777 && seq == 4, "pre-boot writes apply");
        br(1, v); check(v == 5, "read before status = stored value");
        // 2. composer running: reads are live
        st_valid = 1; st_mode = 2; st_density = 5;
        br(0, v); check(v == 2, "live mode read");
        // 3. APF echo: read density (5), buttons change it to 4, APF writes 5 back -> ignored
        br(1, v); st_density = 4; s0 = seq; bw(1, v);
        check(seq == s0 && dens == 5, "echo write ignored");
        // 4. writing the live value is not a change
        // 5. user picks the old stored value (5) in the menu while live is 4 -> applies
        br(1, v); bw(1, 5); check(seq == s0 + 1 && dens == 5, "menu change applies");
        // 6. density clamps
        br(1, v); st_density = 5; bw(1, 9); check(dens == 5, "clamp high (no change)"); bw(1, 0); check(dens == 1, "clamp low");
        // 7. lock seed: off, then on while running latches the session seed
        bw(5, 0); check(!lock, "unlock"); br(6, v); check(v == st_seed, "unlocked seed read = live");
        bw(5, 1); check(lock && seed == 32'hCAFE0001, "lock latches the session seed");
        // 8. reseed action pulses once per write, echo or not
        bw(8, 1); bw(8, 1); repeat (2) @(negedge clk); check(pulses == 2, "reseed pulses");
        // 9. invalid reverb value ignored
        s0 = seq; bw(3, 3); check(rev == 1 && seq == s0, "reverb 3 ignored"); bw(3, 2); check(rev == 2, "reverb 2");
        if (errs == 0) $display("PASS settings_bridge"); else $display("FAIL settings_bridge: %0d", errs);
        $finish;
    end
endmodule
