// async_sram_model.sv - 128K x 16 asynchronous SRAM with access-time behaviour and
// timing checks (defaults: a 55 ns low-power part, e.g. tAA 55, tOE 25, tHZOE 20,
// tWP 40, tAW 45, tDW 25). Read data is X until both tAA and tOE have elapsed, and the
// chip keeps driving X for tHZOE after OE# rises, so sampling too early or driving the
// bus too soon shows up as X / a contention error.
`timescale 1ns/1ps
module async_sram_model #(
    parameter real TAA = 55.0,
    parameter real TOE = 25.0,
    parameter real THZ = 20.0,
    parameter real TWP = 40.0,
    parameter real TAW = 45.0,
    parameter real TDW = 25.0,
    parameter RANDOM_INIT = 1
) (
    input  wire [16:0] a,
    inout  wire [15:0] dq,
    input  wire        oe_n,
    input  wire        we_n,
    input  wire        ub_n,
    input  wire        lb_n,
    input  wire        host_drive            // the FPGA drives dq (for the contention check)
);
    reg [15:0] mem [0:131071];
    integer i, errors;
    initial begin
        errors = 0;
        for (i = 0; i < 131072; i = i + 1) mem[i] = RANDOM_INIT ? $random : 16'd0;
    end

    // inertial delays: a_ok / oe_ok become true only after the inputs are stable long enough
    wire [16:0] a_d;
    wire        oe_d, oe_late;
    assign #(TAA) a_d = a;
    assign #(0, TOE) oe_d = oe_n;                // falls TOE after OE# falls, rises at once
    assign #(THZ, 0) oe_late = oe_n;             // keep driving THZ after OE# rises
    wire drive = !oe_late && we_n;
    wire valid = (a_d === a) && !oe_n && !oe_d && we_n;
    // too-early reads return garbage (as silicon would), not X, so logic that compares
    // with != (synthesisable) still sees the failure
    reg [15:0] junk;
    always @(a or oe_n) junk = $random;
    assign dq = drive ? (valid ? mem[a] : junk) : 16'hzzzz;

    // write timing
    realtime t_we_fall, t_a_chg, t_dq_chg;
    initial begin t_we_fall = 0; t_a_chg = 0; t_dq_chg = 0; end
    always @(a)  begin
        t_a_chg = $realtime;
        if (!we_n) begin errors = errors + 1; $display("SRAM ERROR %t: address changed while WE# low", $realtime); end
    end
    always @(dq) t_dq_chg = $realtime;
    reg writing = 0;
    reg bad_w;
    always @(negedge we_n) if (we_n === 1'b0) begin t_we_fall = $realtime; writing = 1; end
    always @(posedge we_n) if (writing) begin
        writing = 0;
        bad_w = 0;
        if ($realtime - t_we_fall < TWP) begin bad_w = 1; $display("SRAM ERROR %t: tWP", $realtime); end
        if ($realtime - t_a_chg < TAW)   begin bad_w = 1; $display("SRAM ERROR %t: tAW", $realtime); end
        if ($realtime - t_dq_chg < TDW)  begin bad_w = 1; $display("SRAM ERROR %t: tDW", $realtime); end
        errors = errors + bad_w;
        if (!ub_n || !lb_n) mem[a] = bad_w ? $random : dq;     // a failed write stores garbage
    end
    always @(*) if (drive && host_drive) begin
        errors = errors + 1;
        $display("SRAM ERROR %t: bus contention", $realtime);
    end
endmodule
