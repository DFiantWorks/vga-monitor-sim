module vga_signal_generator (
    input  logic clk,
    input  logic reset,
    input  int width, height,  // Dynamically received width & height
    output logic [11:0] h_count, v_count,
    output logic h_sync, v_sync,
    output logic pixel_enable
);
    
    int HA_END, HS_STA, HS_END, LINE;
    int VA_END, VS_STA, VS_END, SCREEN;
    int Front_Porch_H, Sync_Pulse_H, Back_Porch_H;
    int Front_Porch_V, Sync_Pulse_V, Back_Porch_V;

    always_comb begin
        case (width)
            640: begin
                Front_Porch_H = 16; Sync_Pulse_H = 96; Back_Porch_H = 48;
                HA_END = 639; HS_STA = HA_END + 1 + Front_Porch_H;
                HS_END = HS_STA + Sync_Pulse_H; LINE = 799;
            end
            800: begin
                Front_Porch_H = 40; Sync_Pulse_H = 128; Back_Porch_H = 88;
                HA_END = 799; HS_STA = HA_END + 1 + Front_Porch_H;
                HS_END = HS_STA + Sync_Pulse_H; LINE = 1055;
            end
            1024: begin
                Front_Porch_H = 24; Sync_Pulse_H = 136; Back_Porch_H = 160;
                HA_END = 1023; HS_STA = HA_END + 1 + Front_Porch_H;
                HS_END = HS_STA + Sync_Pulse_H; LINE = 1343;
            end
            
            1280: begin
                Front_Porch_H = 80; Sync_Pulse_H = 136; Back_Porch_H = 216;
                HA_END = 1279; HS_STA = HA_END + 1 + Front_Porch_H;
                HS_END = HS_STA + Sync_Pulse_H; LINE = 1711;
            end
        endcase

        case (height)
            480: begin
                Front_Porch_V = 10; Sync_Pulse_V = 2; Back_Porch_V = 33;
                VA_END = 479; VS_STA = VA_END + 1 + Front_Porch_V;
                VS_END = VS_STA + Sync_Pulse_V; SCREEN = 524;
            end
            600: begin
                Front_Porch_V = 1; Sync_Pulse_V = 4; Back_Porch_V = 23;
                VA_END = 599; VS_STA = VA_END + 1 + Front_Porch_V;
                VS_END = VS_STA + Sync_Pulse_V; SCREEN = 627;
            end
            768: begin
                Front_Porch_V = 3; Sync_Pulse_V = 6; Back_Porch_V = 29;
                VA_END = 767; VS_STA = VA_END + 1 + Front_Porch_V;
                VS_END = VS_STA + Sync_Pulse_V; SCREEN = 805;
            end
            960: begin
                Front_Porch_V = 1; Sync_Pulse_V = 3; Back_Porch_V = 30;
                VA_END = 959; VS_STA = VA_END + 1 + Front_Porch_V;
                VS_END = VS_STA + Sync_Pulse_V; SCREEN = 993;
            end
        endcase

        h_sync = ~(int'(h_count) >= int'(HS_STA) && int'(h_count) < int'(HS_END));
        v_sync = ~(int'(v_count) >= int'(VS_STA) && int'(v_count) < int'(VS_END));
        pixel_enable = (int'(h_count) <= int'(HA_END) && int'(v_count) <= int'(VA_END));
    end

    always_ff @(posedge clk or posedge reset) begin
        if (reset) begin
            h_count <= 0;
            v_count <= 0;
        end else begin
            if (int'(h_count) == int'(LINE)) begin
                h_count <= 0;
                v_count <= (int'(v_count) == int'(SCREEN)) ? 0 : v_count + 1;
            end else begin
                h_count <= h_count + 1;
            end
        end
    end
endmodule
