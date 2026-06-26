-- SPDX-License-Identifier: MIT
-- Copyright (c) 2025 MaysAbdallah
-- Copyright (c) 2026 DFiant Inc.
-- MIT-licensed derivative work; see the LICENSE file for full terms and attribution.
--
-- vga_monitor_pkg.vhdl
--
-- Foreign (VHPIDIRECT) bindings to the shared C++ monitor backend
-- (../backend/vga_monitor.cpp), plus a small std_logic->integer helper.
--
-- GHDL binds each procedure below to the C symbol named in its `foreign`
-- attribute. Handles are passed as plain integers (ids into a registry on the
-- C side) because VHPIDIRECT marshals VHDL integers, not pointers.

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

package vga_monitor_pkg is

    function sl2i(s : std_logic) return integer;

    -- Scale an arbitrary-width color channel to the backend's 8-bit value,
    -- mirroring sv/vga_monitor.sv's to8: keep the high 8 bits when wider,
    -- left-justify into 8 bits when narrower.
    function to8(c : std_logic_vector) return integer;

    -- Open a monitor (no clock, no parameters); returns an integer handle.
    procedure mon_open(handle : out integer);
    attribute foreign of mon_open : procedure is "VHPIDIRECT vhpi_monitor_open";

    -- Forward one transition, tagged with the current simulation time, to the
    -- backend, which recovers everything from the signal.
    procedure mon_event(handle : in integer; t : in real;
                        r, g, b, hsync, vsync : in integer);
    attribute foreign of mon_event : procedure is "VHPIDIRECT vhpi_monitor_event";

end package;

package body vga_monitor_pkg is

    function sl2i(s : std_logic) return integer is
    begin
        if s = '1' then
            return 1;
        else
            return 0;
        end if;
    end function;

    function to8(c : std_logic_vector) return integer is
        constant n : integer := c'length;
        variable u : unsigned(n - 1 downto 0);
    begin
        u := unsigned(c);
        if n >= 8 then
            return to_integer(shift_right(u, n - 8));
        else
            return to_integer(shift_left(resize(u, 8), 8 - n));
        end if;
    end function;

    -- Foreign bodies are never executed (GHDL calls the C symbol instead) but
    -- must be present and legal.
    procedure mon_open(handle : out integer) is
    begin
        handle := 0;
    end procedure;

    procedure mon_event(handle : in integer; t : in real;
                        r, g, b, hsync, vsync : in integer) is
    begin
        null;
    end procedure;

end package body;
