-- SPDX-License-Identifier: MIT
-- Copyright (c) 2025 MaysAbdallah
-- Copyright (c) 2026 DFiant Inc.
-- MIT-licensed derivative work; see the LICENSE file for full terms and attribution.
--
-- vga_monitor.vhdl
--
-- VGA monitor with NO clock and NO generics at all (VHDL twin of
-- sv/vga_monitor.sv). Instantiate it with nothing but the VGA
-- signaling and it locks onto an unknown input: a process sensitive to the
-- signals forwards each transition with the current simulation time (`now`),
-- and the shared backend recovers sync polarity, the pixel period (from the
-- finest video transitions), the active resolution (bounding box of non-blank
-- video) and the active offset. Byte-identical to the parameterized monitors.
--
-- Limitations of needing zero hints: the pixel period is recovered from
-- pixel-granular detail (a flat image has none), and the detected area is the
-- extent of non-blank video.

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;
use work.vga_monitor_pkg.all;

entity vga_monitor is
    port (
        r     : in std_logic_vector(7 downto 0);
        g     : in std_logic_vector(7 downto 0);
        b     : in std_logic_vector(7 downto 0);
        hsync : in std_logic;
        vsync : in std_logic
    );
end entity;

architecture rtl of vga_monitor is
begin
    process(r, g, b, hsync, vsync)
        variable handle : integer := -1;
    begin
        if handle = -1 then
            mon_open(handle);
        end if;
        mon_event(handle, real(now / 1 ns),
                       to_integer(unsigned(r)),
                       to_integer(unsigned(g)),
                       to_integer(unsigned(b)),
                       sl2i(hsync), sl2i(vsync));
    end process;
end architecture;
