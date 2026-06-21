// scroll_source.sv
//
// Animated pattern: a diagonal gradient that scrolls with the frame counter.
// Every active pixel is non-blank (blue is forced >= 0x20) so the auto-detecting
// monitor locks a stable 640x480; pixels are black only while blanked, which is
// what lets the monitor find the active box.

module scroll_source (
    input  logic        video_on,
    input  logic [11:0] pixel_x,
    input  logic [11:0] pixel_y,
    input  int          frame,
    output logic [7:0]  red,
    output logic [7:0]  green,
    output logic [7:0]  blue
);
    logic [7:0] f;
    always_comb begin
        f = 8'(frame);                       // animation phase
        if (video_on) begin
            red   = pixel_x[7:0] + f;        // ramp scrolls left
            green = pixel_y[7:0] - f;        // ramp scrolls up
            blue  = (pixel_x[7:0] ^ pixel_y[7:0]) | 8'h20;  // never 0 -> non-blank
        end else begin
            red = 8'h00; green = 8'h00; blue = 8'h00;
        end
    end
endmodule
