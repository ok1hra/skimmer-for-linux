`timescale 1 ns / 1 ps

// The packer (docs/FPGA.md, design 6–8).
//
// Four stage outputs come in, each carrying every slot's I and Q
// interleaved (FIR Compiler, channel 2s = slot s's I, 2s+1 its Q, the
// channel number in tuser): 0 = 384 kHz (the FIR), 1 = 192, 2 = 96,
// 3 = 48 kHz (the half-bands); 32 bits, the filter's top bits. Stages
// 0–2 are passed on to the next half-band (its top 24 bits) through a
// small FIFO each (the tees). From each stage the
// slots whose rate it is (mask_r) are scaled (shift_r: an arithmetic
// right shift with rounding, saturated to 24 bits) and written as one
// 64-bit word a sample, in memory byte order:
//   0..2 I (big-endian 24 bits), 3..5 Q, 6 the slot, 7 the tag 0x10 + r.
// After every 48 kHz vector a tick word (tag 0xFF: 0..5 the tick count
// since the sync, big-endian; 6 the flags), every 256 ticks a peak word
// (tag 0xFE: 0..1 IN1's peak |sample|, 2..3 IN2's, 4 clips in the window:
// bit 0 IN1, bit 1 IN2; 6 the flags). Flags: bit 0 overflow (a FIFO was
// full: samples were lost), bit 1 IN1 clipped, bit 2 IN2 clipped — sticky
// until the next sync (aresetn is the chain's run).
// The four word streams merge round-robin into m_axis (the DMA writer);
// nothing waits on it upstream — a full FIFO drops and sets overflow.
module skr_pack #
(
  parameter integer N = 24,
  parameter integer CH_W = 6
)
(
  input  wire            aclk,
  input  wire            aresetn,

  input  wire [N-1:0]    mask_0,
  input  wire [N-1:0]    mask_1,
  input  wire [N-1:0]    mask_2,
  input  wire [N-1:0]    mask_3,
  input  wire [31:0]     shifts,
  input  wire [31:0]     adc_data,

  input  wire [31:0]     s_axis_0_tdata,
  input  wire            s_axis_0_tvalid,
  input  wire [CH_W-1:0] s_axis_0_tuser,
  input  wire [31:0]     s_axis_1_tdata,
  input  wire            s_axis_1_tvalid,
  input  wire [CH_W-1:0] s_axis_1_tuser,
  input  wire [31:0]     s_axis_2_tdata,
  input  wire            s_axis_2_tvalid,
  input  wire [CH_W-1:0] s_axis_2_tuser,
  input  wire [31:0]     s_axis_3_tdata,
  input  wire            s_axis_3_tvalid,
  input  wire [CH_W-1:0] s_axis_3_tuser,

  output wire [23:0]     m_axis_0_tdata,
  output wire            m_axis_0_tvalid,
  input  wire            m_axis_0_tready,
  output wire [23:0]     m_axis_1_tdata,
  output wire            m_axis_1_tvalid,
  input  wire            m_axis_1_tready,
  output wire [23:0]     m_axis_2_tdata,
  output wire            m_axis_2_tvalid,
  input  wire            m_axis_2_tready,

  output wire [63:0]     m_axis_tdata,
  output wire            m_axis_tvalid,
  input  wire            m_axis_tready,

  output wire [31:0]     flags,
  output wire [31:0]     peaks,
  output wire [63:0]     ticks
);

  localparam [CH_W-1:0] LAST = 2 * N - 1;

  wire [31:0]     in_data  [3:0];
  wire            in_valid [3:0];
  wire [CH_W-1:0] in_user  [3:0];
  wire [N-1:0]    in_mask  [3:0];

  assign in_data[0] = s_axis_0_tdata;  assign in_valid[0] = s_axis_0_tvalid;
  assign in_user[0] = s_axis_0_tuser;  assign in_mask[0]  = mask_0;
  assign in_data[1] = s_axis_1_tdata;  assign in_valid[1] = s_axis_1_tvalid;
  assign in_user[1] = s_axis_1_tuser;  assign in_mask[1]  = mask_1;
  assign in_data[2] = s_axis_2_tdata;  assign in_valid[2] = s_axis_2_tvalid;
  assign in_user[2] = s_axis_2_tuser;  assign in_mask[2]  = mask_2;
  assign in_data[3] = s_axis_3_tdata;  assign in_valid[3] = s_axis_3_tvalid;
  assign in_user[3] = s_axis_3_tuser;  assign in_mask[3]  = mask_3;

  wire [3:0] ovf_tee, ovf_tap;
  wire [3:0] fifo_empty;
  wire [63:0] fifo_dout [3:0];
  wire [3:0] fifo_rd;

  reg int_ovf_reg, int_clip1_reg, int_clip2_reg;
  wire [7:0] int_flags_wire = {5'd0, int_clip2_reg, int_clip1_reg, int_ovf_reg};

  // ---- peaks and clips of the two inputs ----

  wire [15:0] adc1 = adc_data[15:0];
  wire [15:0] adc2 = adc_data[31:16];
  wire [15:0] abs1 = adc1[15] ? -adc1 : adc1;
  wire [15:0] abs2 = adc2[15] ? -adc2 : adc2;
  wire clip1 = (adc1 == 16'h7fff) | (adc1 == 16'h8000);
  wire clip2 = (adc2 == 16'h7fff) | (adc2 == 16'h8000);

  reg [15:0] int_max1_reg, int_max2_reg;
  reg [1:0] int_wclip_reg;
  reg [31:0] int_peaks_reg;
  reg [1:0] int_pclip_reg;
  reg [63:0] int_tick_reg;
  reg int_tick_pend, int_peak_pend;

  wire tick = in_valid[3] & (in_user[3] == LAST);
  wire window_end = tick & (int_tick_reg[7:0] == 8'hff);

  always @(posedge aclk)
  begin
    if(~aresetn)
    begin
      int_max1_reg <= 16'd0;
      int_max2_reg <= 16'd0;
      int_wclip_reg <= 2'd0;
      int_peaks_reg <= 32'd0;
      int_pclip_reg <= 2'd0;
      int_tick_reg <= 64'd0;
      int_clip1_reg <= 1'b0;
      int_clip2_reg <= 1'b0;
    end
    else
    begin
      if(window_end)
      begin
        int_peaks_reg <= {int_max2_reg, int_max1_reg};
        int_pclip_reg <= int_wclip_reg;
        int_max1_reg <= abs1;
        int_max2_reg <= abs2;
        int_wclip_reg <= {clip2, clip1};
      end
      else
      begin
        if(abs1 > int_max1_reg) int_max1_reg <= abs1;
        if(abs2 > int_max2_reg) int_max2_reg <= abs2;
        int_wclip_reg <= int_wclip_reg | {clip2, clip1};
      end
      if(tick) int_tick_reg <= int_tick_reg + 1'b1;
      if(clip1) int_clip1_reg <= 1'b1;
      if(clip2) int_clip2_reg <= 1'b1;
    end
  end

  always @(posedge aclk)
  begin
    if(~aresetn) int_ovf_reg <= 1'b0;
    else if(|ovf_tee | |ovf_tap) int_ovf_reg <= 1'b1;
  end

  // ---- the tees: stages 0..2 go on to the next half-band ----

  wire [23:0] tee_dout [2:0];
  wire [2:0] tee_empty, tee_full;
  wire [2:0] tee_rd = {m_axis_2_tready, m_axis_1_tready, m_axis_0_tready} & ~tee_empty;

  genvar r;
  generate
    for(r = 0; r < 3; r = r + 1)
    begin : TEE
      xpm_fifo_sync #(
        .WRITE_DATA_WIDTH(24),
        .FIFO_WRITE_DEPTH(128),
        .READ_DATA_WIDTH(24),
        .READ_MODE("fwft"),
        .FIFO_READ_LATENCY(0),
        .FIFO_MEMORY_TYPE("distributed"),
        .USE_ADV_FEATURES("0000")
      ) fifo_0 (
        .rst(~aresetn),
        .wr_clk(aclk),
        .wr_en(in_valid[r] & ~tee_full[r]),
        .din(in_data[r][31:8]),
        .full(tee_full[r]),
        .rd_en(tee_rd[r]),
        .dout(tee_dout[r]),
        .empty(tee_empty[r])
      );
      assign ovf_tee[r] = in_valid[r] & tee_full[r];
    end
  endgenerate
  assign ovf_tee[3] = 1'b0;

  assign m_axis_0_tdata = tee_dout[0];  assign m_axis_0_tvalid = ~tee_empty[0];
  assign m_axis_1_tdata = tee_dout[1];  assign m_axis_1_tvalid = ~tee_empty[1];
  assign m_axis_2_tdata = tee_dout[2];  assign m_axis_2_tvalid = ~tee_empty[2];

  // ---- the taps: each stage's own slots into tagged words ----

  wire [4:0] shift [3:0];
  assign shift[0] = shifts[4:0];
  assign shift[1] = shifts[12:8];
  assign shift[2] = shifts[20:16];
  assign shift[3] = shifts[28:24];

  generate
    for(r = 0; r < 4; r = r + 1)
    begin : TAP
      localparam [7:0] TAG = 8'h10 + r;
      // stage 1: shift, keep the rounding bit
      reg signed [31:0] s1_data;
      reg s1_round, s1_valid, s1_q;
      reg [CH_W-2:0] s1_slot;
      reg s1_keep;
      // stage 2: round and saturate
      reg [23:0] s2_i;
      reg [63:0] w_data;
      reg w_valid;
      wire signed [31:0] x = in_data[r];
      wire signed [32:0] y = {s1_data[31], s1_data} + s1_round;
      wire [23:0] sat = (y > 33'sd8388607) ? 24'h7fffff :
                        (y < -33'sd8388608) ? 24'h800000 : y[23:0];
      wire tap_full;
      reg [63:0] f_din;
      reg f_wr;

      always @(posedge aclk)
      begin
        if(~aresetn)
        begin
          s1_valid <= 1'b0;
          w_valid <= 1'b0;
          s1_keep <= 1'b0;
        end
        else
        begin
          s1_valid <= in_valid[r];
          s1_q <= in_user[r][0];
          s1_slot <= in_user[r][CH_W-1:1];
          s1_keep <= in_mask[r][in_user[r][CH_W-1:1]];
          s1_data <= x >>> shift[r];
          s1_round <= (shift[r] == 5'd0) ? 1'b0 : x[shift[r] - 1'b1];
          w_valid <= 1'b0;
          if(s1_valid)
          begin
            if(~s1_q)
            begin
              s2_i <= sat;
            end
            else if(s1_keep)
            begin
              w_data <= {TAG, {(9-CH_W){1'b0}}, s1_slot,
                         sat[7:0], sat[15:8], sat[23:16],
                         s2_i[7:0], s2_i[15:8], s2_i[23:16]};
              w_valid <= 1'b1;
            end
          end
        end
      end

      if(r == 3)
      begin : HDR
        // the tick and peak words share this tap's FIFO: they go in a
        // cycle without a sample (the 48 kHz vector ends with its last
        // channel; the next one comes 2560 clocks later)
        always @(posedge aclk)
        begin
          if(~aresetn)
          begin
            int_tick_pend <= 1'b0;
            int_peak_pend <= 1'b0;
          end
          else
          begin
            if(tick) int_tick_pend <= 1'b1;
            if(window_end) int_peak_pend <= 1'b1;
            if(~w_valid & int_tick_pend) int_tick_pend <= 1'b0;
            else if(~w_valid & int_peak_pend) int_peak_pend <= 1'b0;
          end
        end
        always @*
        begin
          f_wr = w_valid | int_tick_pend | int_peak_pend;
          if(w_valid)
            f_din = w_data;
          else if(int_tick_pend)
            f_din = {8'hff, int_flags_wire,
                     int_tick_reg[7:0], int_tick_reg[15:8], int_tick_reg[23:16],
                     int_tick_reg[31:24], int_tick_reg[39:32], int_tick_reg[47:40]};
          else
            f_din = {8'hfe, int_flags_wire, 8'd0, 6'd0, int_pclip_reg,
                     int_peaks_reg[23:16], int_peaks_reg[31:24],
                     int_peaks_reg[7:0], int_peaks_reg[15:8]};
        end
      end
      else
      begin : NOHDR
        always @*
        begin
          f_wr = w_valid;
          f_din = w_data;
        end
      end

      xpm_fifo_sync #(
        .WRITE_DATA_WIDTH(64),
        .FIFO_WRITE_DEPTH(512),
        .READ_DATA_WIDTH(64),
        .READ_MODE("fwft"),
        .FIFO_READ_LATENCY(0),
        .FIFO_MEMORY_TYPE("block"),
        .USE_ADV_FEATURES("0000")
      ) fifo_1 (
        .rst(~aresetn),
        .wr_clk(aclk),
        .wr_en(f_wr & ~tap_full),
        .din(f_din),
        .full(tap_full),
        .rd_en(fifo_rd[r]),
        .dout(fifo_dout[r]),
        .empty(fifo_empty[r])
      );
      assign ovf_tap[r] = f_wr & tap_full;
    end
  endgenerate

  // ---- the merge: round-robin over the four FIFOs ----

  reg [1:0] int_cur_reg;
  reg [63:0] int_out_reg;
  reg int_out_valid;
  reg [1:0] pick;
  reg any;
  integer k;

  always @*
  begin
    any = 1'b0;
    pick = int_cur_reg;
    for(k = 3; k >= 0; k = k - 1)
    begin
      if(~fifo_empty[(int_cur_reg + k) % 4])
      begin
        any = 1'b1;
        pick = (int_cur_reg + k) % 4;
      end
    end
  end

  wire load = ~int_out_valid | m_axis_tready;
  assign fifo_rd = (load & any) ? (4'b0001 << pick) : 4'b0000;

  always @(posedge aclk)
  begin
    if(~aresetn)
    begin
      int_cur_reg <= 2'd0;
      int_out_valid <= 1'b0;
    end
    else if(load)
    begin
      int_out_valid <= any;
      if(any)
      begin
        int_out_reg <= fifo_dout[pick];
        int_cur_reg <= pick + 1'b1;
      end
    end
  end

  assign m_axis_tdata = int_out_reg;
  assign m_axis_tvalid = int_out_valid;

  assign flags = {24'd0, int_flags_wire};
  assign peaks = int_peaks_reg;
  assign ticks = int_tick_reg;

endmodule
