`timescale 1 ns / 1 ps

// The control word (cfg word 0): bit 0 run — 0 holds every NCO, CIC, FIR
// and the packer in reset, so a 0 → 1 is the common sync of all slots;
// bit 1 apply — a rising edge loads every slot's new phase increment on
// the same clock; bit 2 the DMA writer on; bit 3 its writes cacheable
// (a ring the ARM maps cached; skr_ram_writer).
module skr_ctrl
(
  input  wire        aclk,
  input  wire        aresetn,
  input  wire [31:0] ctrl,
  output wire        run_n,
  output wire        apply,
  output wire        dma_n,
  output wire        cacheable
);

  reg int_run_reg, int_dma_reg, int_app_reg, int_pulse_reg, int_cache_reg;

  always @(posedge aclk)
  begin
    if(~aresetn)
    begin
      int_run_reg <= 1'b0;
      int_dma_reg <= 1'b0;
      int_app_reg <= 1'b0;
      int_pulse_reg <= 1'b0;
      int_cache_reg <= 1'b0;
    end
    else
    begin
      int_run_reg <= ctrl[0];
      int_dma_reg <= ctrl[2];
      int_app_reg <= ctrl[1];
      int_pulse_reg <= ctrl[1] & ~int_app_reg;
      int_cache_reg <= ctrl[3];
    end
  end

  assign run_n = int_run_reg;
  assign apply = int_pulse_reg;
  assign dma_n = int_dma_reg;
  assign cacheable = int_cache_reg;

endmodule
