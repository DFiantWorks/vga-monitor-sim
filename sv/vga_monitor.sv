// SPDX-License-Identifier: MIT
// Copyright (c) 2025 MaysAbdallah
// Copyright (c) 2026 DFiant Inc.
// MIT-licensed derivative work; see the LICENSE file for full terms and attribution.
//
// vga_monitor.sv
//
// A self-contained VGA monitor with NO clock and NO parameters at all -- not even
// the resolution. Instantiate it with nothing but the VGA signaling:
//
//     vga_monitor m (.r, .g, .b, .hsync, .vsync);
//
// A process sensitive to the signals forwards each transition, tagged with the
// current simulation time, to the C++ backend, which recovers EVERYTHING:
//   * sync polarity   -- the shorter-held (short-pulse) level of each sync wire,
//   * pixel period     -- the smallest gap between video transitions,
//   * active size      -- the bounding box of non-blank video over a frame,
//   * active offset    -- where that box sits relative to the sync pulses.
//
// Two honest limitations of needing zero hints:
//   1. the pixel period is recovered from the finest video transitions, so a
//      perfectly flat image has no detail to lock the dot clock onto;
//   2. the detected area is the extent of *non-blank* video, so a picture that
//      doesn't reach its own active edges is cropped to its content.
//
// COLOR_BITS only sizes the r/g/b ports; it carries no timing information.

`timescale 1ns/1ps

module vga_monitor #(
    parameter string NAME       = "VGA Monitor (auto)",
    parameter int    COLOR_BITS = 8
) (
    input logic [COLOR_BITS-1:0]  r,
    input logic [COLOR_BITS-1:0]  g,
    input logic [COLOR_BITS-1:0]  b,
    input logic                   hsync,
    input logic                   vsync
);

    import "DPI-C" function chandle vga_monitor_open(input string name);
    import "DPI-C" function void vga_monitor_event(
        input chandle       handle,
        input real          t,
        input byte unsigned r,
        input byte unsigned g,
        input byte unsigned b,
        input int           hsync,
        input int           vsync
    );
    import "DPI-C" function void vga_monitor_close(input chandle handle);

    chandle handle;

    function automatic byte unsigned to8(input logic [COLOR_BITS-1:0] c);
        // The shift operand is COLOR_BITS wide while the result is 8 wide; the
        // byte' cast resizes deliberately, so silence Verilator's width warning.
        /* verilator lint_off WIDTHEXPAND */
        if (COLOR_BITS >= 8)
            return byte'(c >> (COLOR_BITS - 8));
        else
            return byte'(c << (8 - COLOR_BITS));
        /* verilator lint_on WIDTHEXPAND */
    endfunction

    initial handle = vga_monitor_open(NAME);

    always @(r or g or b or hsync or vsync) begin
        vga_monitor_event(handle, $realtime, to8(r), to8(g), to8(b),
                               int'(hsync), int'(vsync));
    end

    final vga_monitor_close(handle);

endmodule
