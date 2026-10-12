// audio_engine.sv - one 48 kHz sample: voice engine, then FX / master chain.
// Register writes (voice or FX) are accepted only while !busy, i.e. between samples,
// which gives the model's tick-boundary commit semantics.
`default_nettype none

module audio_engine (
    input  wire               clk,
    input  wire               rst,
    input  wire               wr_en,
    input  wire               wr_fx,          // 0: voice register (addr = voice*32 + reg), 1: FX register
    input  wire        [9:0]  wr_addr,
    input  wire        [31:0] wr_data,
    input  wire               start,
    output reg                busy,
    output reg                done,
    output wire               ready,          // engine RAMs and SRAM cleared after reset
    output wire signed [31:0] bus_l,
    output wire signed [31:0] bus_r,
    output wire signed [31:0] bus_rev,
    output wire signed [31:0] bus_echo,
    output wire signed [15:0] out_l,
    output wire signed [15:0] out_r,
    input  wire        [4:0]  st_voice,
    output wire        [31:0] st_status,
    output wire        [31:0] fx_gain,
    output wire               mem_req,
    output wire               mem_we,
    output wire        [16:0] mem_addr,
    output wire        [15:0] mem_wd,
    input  wire               mem_ack,
    input  wire        [15:0] mem_rd
);
    wire ve_done, fx_done, ve_ready, fx_ready;
    assign ready = ve_ready && fx_ready;

    voice_engine u_voice (
        .clk(clk), .rst(rst),
        .wr_en(wr_en && !wr_fx && !busy), .wr_addr(wr_addr), .wr_data(wr_data),
        .start(start && !busy && ready), .done(ve_done),
        .bus_l(bus_l), .bus_r(bus_r), .bus_rev(bus_rev), .bus_echo(bus_echo),
        .st_voice(st_voice), .st_status(st_status), .ready(ve_ready));

    fx_engine u_fx (
        .clk(clk), .rst(rst),
        .wr_en(wr_en && wr_fx && !busy), .wr_addr(wr_addr[4:0]), .wr_data(wr_data),
        .start(ve_done), .in_l(bus_l), .in_r(bus_r), .in_rev(bus_rev), .in_echo(bus_echo),
        .done(fx_done), .ready(fx_ready), .out_l(out_l), .out_r(out_r), .gain(fx_gain),
        .mem_req(mem_req), .mem_we(mem_we), .mem_addr(mem_addr), .mem_wd(mem_wd),
        .mem_ack(mem_ack), .mem_rd(mem_rd));

    always @(posedge clk) begin
        done <= fx_done;
        if (rst) busy <= 1'b0;
        else if (start && !busy && ready) busy <= 1'b1;
        else if (fx_done) busy <= 1'b0;
    end
endmodule
`default_nettype wire
