`timescale 1 ns / 1 ps

// A slot's input: IN1 (din[15:0]) or IN2 (din[31:16]); a slot that is off
// gets zero, so its mixer and CIC stop toggling.
module skr_insel
(
  input  wire        aclk,
  input  wire        sel,
  input  wire        en,
  input  wire [31:0] din,
  output wire [15:0] dout
);

  reg [15:0] int_data_reg;

  always @(posedge aclk)
  begin
    int_data_reg <= ~en ? 16'd0 : sel ? din[31:16] : din[15:0];
  end

  assign dout = int_data_reg;

endmodule
