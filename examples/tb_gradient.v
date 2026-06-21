// tb_gradient.v -- 640x480 gradient driving the zero-everything Verilog
// monitor (no clock, no parameters; the backend auto-detects the mode).
`timescale 1ns/1ps
module tb_gradient;
    localparam WIDTH = 640, HEIGHT = 480;
    reg clk = 1'b0, reset = 1'b1;
    wire [11:0] h_cnt, v_cnt;
    wire h_sync, v_sync, vid_on;
    wire [7:0] red, green, blue;
    always #5 clk = ~clk;
    initial begin reset = 1'b1; repeat (4) @(posedge clk); reset = 1'b0; end
    vga_signal_generator gen (.clk(clk), .reset(reset), .width(WIDTH), .height(HEIGHT),
        .h_count(h_cnt), .v_count(v_cnt), .h_sync(h_sync), .v_sync(v_sync), .pixel_enable(vid_on));
    gradient_source pat (.video_on(vid_on), .pixel_x(h_cnt), .pixel_y(v_cnt),
        .red(red), .green(green), .blue(blue));
    vga_monitor mon (.r(red), .g(green), .b(blue), .hsync(h_sync), .vsync(v_sync));
endmodule
