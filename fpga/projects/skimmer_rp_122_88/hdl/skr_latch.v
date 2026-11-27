`timescale 1 ns / 1 ps

// A slot's phase increment, double-buffered: while the chain is held
// (run_n low) it follows the register, while running it changes only at
// the common apply pulse — a pair of slots retunes on one clock.
module skr_latch
(
  input  wire        aclk,
  input  wire        run_n,
  input  wire        apply,
  input  wire [31:0] din,
  output wire [31:0] dout
);

  reg [31:0] int_data_reg;

  always @(posedge aclk)
  begin
    if(~run_n | apply)
    begin
      int_data_reg <= din;
    end
  end

  assign dout = int_data_reg;

endmodule
