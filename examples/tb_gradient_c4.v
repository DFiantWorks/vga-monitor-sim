// tb_gradient_c4.v -- COLOR_BITS=4 end-to-end fixture.
//
// Same 640x480 gradient as tb_gradient.v, but the monitor is instantiated with a
// 4-bit color width and fed the top nibble of each 8-bit channel. The monitor
// left-justifies each 4-bit channel back to 8 bits, so the reconstructed frame
// is the gradient quantized to 16 levels per channel -- a distinct golden
// (gradient_640x480_c4.ppm), exercising the COLOR_BITS path through the whole
// video pass. One simulator (Icarus/VPI) is enough: the backend is
// simulator-agnostic, so the reconstruction is byte-identical across DPI/VHPI/VPI.
`timescale 1ns/1ps
module tb_gradient_c4;
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
    // Drive only the top 4 bits of each channel into a 4-bit-wide monitor.
    vga_monitor #(.COLOR_BITS(4)) mon (.r(red[7:4]), .g(green[7:4]), .b(blue[7:4]),
        .hsync(h_sync), .vsync(v_sync));
endmodule
