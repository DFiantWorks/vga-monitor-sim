// bars_box_source.sv
//
// Animated pattern: eight scrolling color bars with a white box bouncing across
// them. The bars are always non-blank, so the active region stays a stable
// 640x480 for the auto-detecting monitor (black only while blanked).

module bars_box_source (
    input  logic        video_on,
    input  logic [11:0] pixel_x,
    input  logic [11:0] pixel_y,
    input  int          frame,
    output logic [7:0]  red,
    output logic [7:0]  green,
    output logic [7:0]  blue
);
    localparam int BOX = 80;
    int sx, sy, bx, by, px, py;
    logic [2:0] bar;

    always_comb begin
        px = int'(pixel_x);
        py = int'(pixel_y);

        // Bouncing box position: a triangle wave over the available travel.
        sx = frame % (2*(640-BOX));                       // 0 .. 1119
        bx = (sx < (640-BOX)) ? sx : (2*(640-BOX) - sx);  // 0 .. 559
        sy = frame % (2*(480-BOX));                        // 0 .. 799
        by = (sy < (480-BOX)) ? sy : (2*(480-BOX) - sy);  // 0 .. 399

        // Color bars, 64 px wide, scrolling 2 px/frame.
        bar = 3'((px + frame*2) >> 6);

        if (!video_on) begin
            red = 8'h00; green = 8'h00; blue = 8'h00;
        end else if (px >= bx && px < bx+BOX && py >= by && py < by+BOX) begin
            red = 8'hFF; green = 8'hFF; blue = 8'hFF;      // white box
        end else begin
            case (bar)
                3'd0: begin red=8'hFF; green=8'hFF; blue=8'hFF; end // white
                3'd1: begin red=8'hFF; green=8'hFF; blue=8'h00; end // yellow
                3'd2: begin red=8'h00; green=8'hFF; blue=8'hFF; end // cyan
                3'd3: begin red=8'h00; green=8'hFF; blue=8'h00; end // green
                3'd4: begin red=8'hFF; green=8'h00; blue=8'hFF; end // magenta
                3'd5: begin red=8'hFF; green=8'h00; blue=8'h00; end // red
                3'd6: begin red=8'h00; green=8'h00; blue=8'hFF; end // blue
                3'd7: begin red=8'h80; green=8'h80; blue=8'h80; end // gray
            endcase
        end

        // One bit of per-pixel detail so the monitor recovers the true dot
        // clock (the bars/box alone have no pixel-granular transitions). The
        // LSB toggles every pixel and is visually invisible.
        if (video_on)
            green[0] = px[0];
    end
endmodule
