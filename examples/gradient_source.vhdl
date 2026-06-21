-- gradient_source.vhdl
--
-- Example "DUT": a combinational VGA pattern generator, the VHDL twin of
-- examples/gradient_source.sv. Same 2D gradient so the test rig can
-- compute the expected pixels and so the reconstructed frame is byte-identical
-- to the SystemVerilog golden:
--   red   = pixel_x(7..0)        (left -> right ramp)
--   green = pixel_y(7..0)        (top  -> bottom ramp)
--   blue  = 255 - pixel_x(7..0)  = not pixel_x(7..0)

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

entity gradient_source is
    port (
        video_on : in  std_logic;
        pixel_x  : in  std_logic_vector(11 downto 0);
        pixel_y  : in  std_logic_vector(11 downto 0);
        red      : out std_logic_vector(7 downto 0);
        green    : out std_logic_vector(7 downto 0);
        blue     : out std_logic_vector(7 downto 0)
    );
end entity;

architecture rtl of gradient_source is
begin
    red   <= pixel_x(7 downto 0)     when video_on = '1' else (others => '0');
    green <= pixel_y(7 downto 0)     when video_on = '1' else (others => '0');
    blue  <= not pixel_x(7 downto 0) when video_on = '1' else (others => '0');
end architecture;
