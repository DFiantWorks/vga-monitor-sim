-- tb_gradient.vhdl
--
-- Same 640x480 gradient driving the zero-generic auto-detecting monitor. The
-- monitor is given the pixel clock and the VGA signaling and NOTHING else -- no
-- resolution, no porches, no polarity -- and recovers all of it. The
-- reconstructed image must be byte-identical to the parameterized monitors.

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;

entity tb_gradient is
end entity;

architecture sim of tb_gradient is
    constant WIDTH  : integer := 640;
    constant HEIGHT : integer := 480;

    signal clk   : std_logic := '0';
    signal reset : std_logic := '1';

    signal h_cnt, v_cnt           : std_logic_vector(11 downto 0);
    signal h_sync, v_sync, vid_on : std_logic;
    signal red, green, blue       : std_logic_vector(7 downto 0);
begin
    clk <= not clk after 5 ns;

    reset_proc : process
    begin
        reset <= '1';
        wait for 23 ns;
        reset <= '0';
        wait;
    end process;

    gen : entity work.vga_signal_generator
        port map (
            clk => clk, reset => reset, width => WIDTH, height => HEIGHT,
            h_count => h_cnt, v_count => v_cnt,
            h_sync => h_sync, v_sync => v_sync, pixel_enable => vid_on
        );

    pat : entity work.gradient_source
        port map (
            video_on => vid_on, pixel_x => h_cnt, pixel_y => v_cnt,
            red => red, green => green, blue => blue
        );

    -- No clock and no generics -- the monitor figures everything out itself.
    mon : entity work.vga_monitor
        port map (
            r => red, g => green, b => blue,
            hsync => h_sync, vsync => v_sync
        );
end architecture;
