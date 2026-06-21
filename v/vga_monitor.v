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

`timescale 1ns/1ps

module vga_monitor (
    input  [7:0] r,
    input  [7:0] g,
    input  [7:0] b,
    input        hsync,
    input        vsync
);
    integer handle;

    initial handle = $vga_monitor_open();

    always @(r or g or b or hsync or vsync)
        $vga_monitor_event(handle, $realtime, r, g, b, hsync, vsync);
endmodule
