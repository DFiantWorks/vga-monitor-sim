// tb_stream.sv
//
// Single auto-detecting monitor driving a 640x480 gradient, for the raw-rgb24
// TCP streaming demo (stream-dpi). Built WITHOUT --timing -- clk/reset come from
// sim_main_stream.cpp. With VGA_MONITOR_STREAM=host:port set, the monitor sends
// each finished frame as raw rgb24 to a standard viewer (e.g. ffplay).

module tb_stream (
    input logic clk,
    input logic reset
);
    localparam int WIDTH  = 640;
    localparam int HEIGHT = 480;

    logic [11:0] h_count, v_count;
    logic        h_sync, v_sync, video_on;
    logic [7:0]  red, green, blue;

    vga_signal_generator gen (
        .clk (clk), .reset (reset), .width (WIDTH), .height (HEIGHT),
        .h_count (h_count), .v_count (v_count),
        .h_sync (h_sync), .v_sync (v_sync), .pixel_enable (video_on)
    );

    gradient_source pattern (
        .video_on (video_on), .pixel_x (h_count), .pixel_y (v_count),
        .red (red), .green (green), .blue (blue)
    );

    vga_monitor #(.NAME("stream")) monitor (
        .r (red), .g (green), .b (blue), .hsync (h_sync), .vsync (v_sync)
    );
endmodule
