library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity vga_signal_generator is
    Port (
        clk          : in  std_logic;
        reset        : in  std_logic;
        width        : in  integer;
        height       : in  integer;
        h_count      : out std_logic_vector(11 downto 0);
        v_count      : out std_logic_vector(11 downto 0);
        h_sync       : out std_logic;
        v_sync       : out std_logic;
        pixel_enable : out std_logic
    );
end vga_signal_generator;

architecture Behavioral of vga_signal_generator is

    signal HA_END, HS_STA, HS_END, LINE     : integer := 0;
    signal VA_END, VS_STA, VS_END, SCREEN   : integer := 0;
    signal Front_Porch_H, Sync_Pulse_H, Back_Porch_H : integer := 0;
    signal Front_Porch_V, Sync_Pulse_V, Back_Porch_V : integer := 0;

    signal h_cnt : integer range 0 to 4095 := 0;
    signal v_cnt : integer range 0 to 4095 := 0;

begin

    process(clk, reset)
    begin
        if reset = '1' then
            -- === Horizontal timing for known resolutions ===
            if width = 1280 then
                HA_END         <= 1279;
                Front_Porch_H  <= 80;
                Sync_Pulse_H   <= 136;
                Back_Porch_H   <= 216;
            elsif width = 1024 then
                HA_END         <= 1023;
                Front_Porch_H  <= 24;
                Sync_Pulse_H   <= 136;
                Back_Porch_H   <= 160;
            elsif width = 800 then
                HA_END         <= 799;
                Front_Porch_H  <= 40;
                Sync_Pulse_H   <= 128;
                Back_Porch_H   <= 88;
            else  -- default to 640
                HA_END         <= 639;
                Front_Porch_H  <= 16;
                Sync_Pulse_H   <= 96;
                Back_Porch_H   <= 48;
            end if;

            HS_STA <= HA_END + 1 + Front_Porch_H;
            HS_END <= HS_STA + Sync_Pulse_H;
            LINE   <= HA_END + Front_Porch_H + Sync_Pulse_H + Back_Porch_H;

            -- === Vertical timing ===
            if height = 960 then
                VA_END         <= 1023;
                Front_Porch_V  <= 1;
                Sync_Pulse_V   <= 3;
                Back_Porch_V   <= 30;
            elsif height = 600 then
                VA_END         <= 599;
                Front_Porch_V  <= 1;
                Sync_Pulse_V   <= 4;
                Back_Porch_V   <= 23;
            elsif height = 768 then
                VA_END         <= 767;
                Front_Porch_V  <= 3;
                Sync_Pulse_V   <= 6;
                Back_Porch_V   <= 29;
            else  -- default to 480
                VA_END         <= 479;
                Front_Porch_V  <= 10;
                Sync_Pulse_V   <= 2;
                Back_Porch_V   <= 33;
            end if;

            VS_STA <= VA_END + 1 + Front_Porch_V;
            VS_END <= VS_STA + Sync_Pulse_V;
            SCREEN <= VA_END + Front_Porch_V + Sync_Pulse_V + Back_Porch_V;

            -- Reset counters
            h_cnt <= 0;
            v_cnt <= 0;

        elsif rising_edge(clk) then
            if h_cnt = LINE then
                h_cnt <= 0;
                if v_cnt = SCREEN then
                    v_cnt <= 0;
                else
                    v_cnt <= v_cnt + 1;
                end if;
            else
                h_cnt <= h_cnt + 1;
            end if;
        end if;
    end process;

    -- Output logic
    h_sync       <= '0' when (h_cnt >= HS_STA and h_cnt < HS_END) else '1';
    v_sync       <= '0' when (v_cnt >= VS_STA and v_cnt < VS_END) else '1';
    pixel_enable <= '1' when (h_cnt <= HA_END and v_cnt <= VA_END) else '0';

    h_count <= std_logic_vector(to_unsigned(h_cnt, 12));
    v_count <= std_logic_vector(to_unsigned(v_cnt, 12));

end Behavioral;
