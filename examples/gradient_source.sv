// gradient_source.sv
//
// Example "DUT": a trivial VGA pattern generator. Given the active-pixel
// coordinates and the video-on (data-enable) signal, it emits an RGB color.
// The pattern is a 2D gradient so the expected output is easy to compute in the
// test rig and is sensitive to both horizontal and vertical alignment:
//   red   = pixel_x[7:0]        (ramps left->right)
//   green = pixel_y[7:0]        (ramps top->bottom)
//   blue  = 255 - pixel_x[7:0]  (inverse horizontal ramp)
//
// Combinational: r/g/b are a pure function of the pixel coordinates, kept
// phase-aligned with the sync signals from vga_signal_generator.

module gradient_source (
    input  logic        video_on,
    input  logic [11:0] pixel_x,
    input  logic [11:0] pixel_y,
    output logic [7:0]  red,
    output logic [7:0]  green,
    output logic [7:0]  blue
);
    always_comb begin
        if (video_on) begin
            red   = pixel_x[7:0];
            green = pixel_y[7:0];
            blue  = 8'hFF - pixel_x[7:0];
        end else begin
            red   = 8'h00;
            green = 8'h00;
            blue  = 8'h00;
        end
    end
endmodule
