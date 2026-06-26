// SPDX-License-Identifier: MIT
// Copyright (c) 2025 MaysAbdallah
// Copyright (c) 2026 DFiant Inc.
// MIT-licensed derivative work; see the LICENSE file for full terms and attribution.
//
// vga_monitor.v
//
// Verilog VGA monitor with NO clock and NO parameters. Instantiate it with
// nothing but the VGA signaling; the backend recovers sync polarity, the pixel
// period, the active resolution and the active offset purely from the signal.
//
// COLOR_BITS only sizes the r/g/b ports; it carries no timing information.

`timescale 1ns/1ps

module vga_monitor #(
    parameter COLOR_BITS = 8
) (
    input  [COLOR_BITS-1:0] r,
    input  [COLOR_BITS-1:0] g,
    input  [COLOR_BITS-1:0] b,
    input                   hsync,
    input                   vsync
);
    integer handle;

    function [7:0] to8;
        input [COLOR_BITS-1:0] c;
        // The shift operand is COLOR_BITS wide while the result is 8 wide; the
        // truncation is deliberate, so silence Verilator's width warning.
        /* verilator lint_off WIDTHEXPAND */
        begin
            if (COLOR_BITS >= 8)
                to8 = c >> (COLOR_BITS - 8);
            else
                to8 = c << (8 - COLOR_BITS);
        end
        /* verilator lint_on WIDTHEXPAND */
    endfunction

    initial handle = $vga_monitor_open();

    always @(r or g or b or hsync or vsync)
        $vga_monitor_event(handle, $realtime, to8(r), to8(g), to8(b), hsync, vsync);
endmodule
