// tb_two_monitors.sv
//
// Two independent auto-detecting VGA monitors in ONE simulation, each showing a
// different *moving* pattern. Both run at 640x480 off a shared sync generator;
// only the RGB differs. Each vga_monitor instance opens its own backend handle
// and streams to its own viewer ("scroll" -> base port, "bars + box" -> base+1),
// recovering geometry independently and animating every frame.
//
// Built WITHOUT Verilator --timing: there is no #-delay clock here. The clock
// and reset are driven from examples/sim_main.cpp (see the stream-two target),
// which advances simulation time itself -- the fast, pixel-exact path.

`timescale 1ns/1ps

module tb_two_monitors (
    input logic clk,
    input logic reset
);
    localparam int WIDTH  = 640;
    localparam int HEIGHT = 480;

    logic [11:0] h_count, v_count;
    logic        h_sync, v_sync, video_on;

    // Shared VGA timing for both monitors (same resolution, different content).
    vga_signal_generator gen (
        .clk (clk), .reset (reset), .width (WIDTH), .height (HEIGHT),
        .h_count (h_count), .v_count (v_count),
        .h_sync (h_sync), .v_sync (v_sync), .pixel_enable (video_on)
    );

    // Frame counter: advance once per finished frame (vsync leading edge, which
    // is a falling edge here since v_sync is active-low). Drives the animation.
    int   frame   = 0;
    logic v_sync_d = 1'b1;
    always_ff @(posedge clk) begin
        v_sync_d <= v_sync;
        if (v_sync_d && !v_sync)
            frame <= frame + 1;
    end

    // Pattern A: scrolling diagonal gradient.
    logic [7:0] ra, ga, ba;
    scroll_source pat_a (
        .video_on (video_on), .pixel_x (h_count), .pixel_y (v_count),
        .frame (frame), .red (ra), .green (ga), .blue (ba)
    );

    // Pattern B: scrolling color bars with a bouncing box.
    logic [7:0] rb, gb, bb;
    bars_box_source pat_b (
        .video_on (video_on), .pixel_x (h_count), .pixel_y (v_count),
        .frame (frame), .red (rb), .green (gb), .blue (bb)
    );

    vga_monitor #(.NAME("scroll")) mon_a (
        .r (ra), .g (ga), .b (ba), .hsync (h_sync), .vsync (v_sync)
    );
    vga_monitor #(.NAME("bars + box")) mon_b (
        .r (rb), .g (gb), .b (bb), .hsync (h_sync), .vsync (v_sync)
    );
endmodule
