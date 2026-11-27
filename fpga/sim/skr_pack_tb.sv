`timescale 1 ns / 1 ps

// skr_pack against what docs/FPGA.md says it writes: run with
//   fpga/sim/run.sh skr_pack_tb
// Four stages feed every slot's I and Q (channel 2s, 2s+1) as the FIR
// Compiler does; the values encode (stage, slot, sample, I/Q), so every
// word that comes out can be checked by itself, and the count of each
// slot's words against its rate. Ends with "PASS" or "FAIL: …".
module skr_pack_tb;

  localparam integer N = 6;
  localparam integer CH_W = 4;
  localparam integer CH = 2 * N;

  reg aclk = 0, aresetn = 0;
  always #4.069 aclk = ~aclk;

  // slot s at rate index rate_of[s]: 0 = 384 k … 3 = 48 k
  integer rate_of [N] = '{0, 1, 2, 3, 0, 3};
  reg [N-1:0] mask [4];
  reg [31:0] shifts = {3'd0, 5'd3, 3'd0, 5'd2, 3'd0, 5'd1, 3'd0, 5'd0};
  reg [31:0] adc = 0;

  reg [31:0] sd [4];
  reg sv [4];
  reg [CH_W-1:0] su [4];

  wire [23:0] td [3];
  wire tv [3];
  wire [63:0] od;
  wire ov;
  reg oready = 1;
  wire [31:0] flags, peaks;
  wire [63:0] ticks;

  skr_pack #(.N(N), .CH_W(CH_W)) dut (
    .aclk(aclk), .aresetn(aresetn),
    .mask_0(mask[0]), .mask_1(mask[1]), .mask_2(mask[2]), .mask_3(mask[3]),
    .shifts(shifts), .adc_data(adc),
    .s_axis_0_tdata(sd[0]), .s_axis_0_tvalid(sv[0]), .s_axis_0_tuser(su[0]),
    .s_axis_1_tdata(sd[1]), .s_axis_1_tvalid(sv[1]), .s_axis_1_tuser(su[1]),
    .s_axis_2_tdata(sd[2]), .s_axis_2_tvalid(sv[2]), .s_axis_2_tuser(su[2]),
    .s_axis_3_tdata(sd[3]), .s_axis_3_tvalid(sv[3]), .s_axis_3_tuser(su[3]),
    .m_axis_0_tdata(td[0]), .m_axis_0_tvalid(tv[0]), .m_axis_0_tready(1'b1),
    .m_axis_1_tdata(td[1]), .m_axis_1_tvalid(tv[1]), .m_axis_1_tready(1'b1),
    .m_axis_2_tdata(td[2]), .m_axis_2_tvalid(tv[2]), .m_axis_2_tready(1'b1),
    .m_axis_tdata(od), .m_axis_tvalid(ov), .m_axis_tready(oready),
    .flags(flags), .peaks(peaks), .ticks(ticks)
  );

  // the value of (stage r, slot s, sample n, q): fits 24 bits, negative
  // for odd n so the sign and the rounding of negatives are checked
  function automatic integer val(input integer r, input integer s, input integer n, input integer q);
    integer v;
    v = ((r * 16 + s) * 4096 + (n % 2048)) * 2 + q;
    if(n % 2) v = -v;
    return v;
  endfunction

  // the packer's scaling: an arithmetic shift, rounded by the bit below
  function automatic integer scaled(input integer x, input integer r);
    return (x >>> r) + ((r > 0) ? ((x >>> (r - 1)) & 1) : 0);
  endfunction

  integer errors = 0;
  integer got [N];
  integer nexp [N];
  integer ticks_seen = 0, peaks_seen = 0;
  integer per_stage_vectors [4] = '{64, 32, 16, 8};

  task automatic fail(input string m);
    errors = errors + 1;
    if(errors < 20) $display("FAIL: %s", m);
  endtask

  // check each word as it leaves
  always @(posedge aclk)
  begin
    if(ov & oready)
    begin
      integer tag, slot, i, q, r, n, ei, eq, s;
      tag = od[63:56];
      slot = od[55:48];
      i = {{8{od[7]}}, od[7:0], od[15:8], od[23:16]};
      q = {{8{od[31]}}, od[31:24], od[39:32], od[47:40]};
      if(tag == 8'hff)
        ticks_seen = ticks_seen + 1;
      else if(tag == 8'hfe)
        peaks_seen = peaks_seen + 1;
      else if(tag >= 8'h10 && tag <= 8'h13)
      begin
        r = tag - 8'h10;
        s = slot;
        if(s >= N) fail($sformatf("slot %0d out of range", s));
        else
        begin
          if(rate_of[s] != r) fail($sformatf("slot %0d came at rate %0d, set %0d", s, r, rate_of[s]));
          n = got[s];
          // stage r's shift is r (shifts above)
          ei = scaled(val(r, s, n, 0), r);
          eq = scaled(val(r, s, n, 1), r);
          if(i != ei || q != eq)
            fail($sformatf("slot %0d sample %0d: I %0d Q %0d, expected %0d %0d", s, n, i, q, ei, eq));
          got[s] = got[s] + 1;
        end
      end
      else fail($sformatf("unknown tag %02x", tag));
    end
  end

  // the four stages: stage r sends per_stage_vectors[r] vectors, spread
  // over the same time (a 48 k vector = 8 of 384 k)
  initial
  begin
    integer t, r, c, n;
    for(r = 0; r < 4; r = r + 1) begin sv[r] = 0; sd[r] = 0; su[r] = 0; mask[r] = 0; end
    for(c = 0; c < N; c = c + 1) begin mask[rate_of[c]][c] = 1'b1; got[c] = 0; end
    repeat(10) @(posedge aclk);
    aresetn = 1;
    repeat(40) @(posedge aclk);
    // 8 ticks of 48 k: each tick, stage r sends 8 >> r vectors
    for(t = 0; t < 8; t = t + 1)
    begin
      for(r = 0; r < 4; r = r + 1)
      begin
        for(n = 0; n < (8 >> r); n = n + 1)
        begin
          for(c = 0; c < CH; c = c + 1)
          begin
            @(posedge aclk);
            sv[r] <= 1'b1;
            su[r] <= c;
            sd[r] <= val(r, c / 2, t * (8 >> r) + n, c % 2);
            // the ADC: one clip on IN2 in tick 3
            adc <= (t == 3 && c == 0 && n == 0 && r == 0) ? 32'h8000_0100 : 32'h0000_0100;
          end
          @(posedge aclk);
          sv[r] <= 1'b0;
          // gaps, with the output stalled now and then
          oready <= (t % 3) != 2;
          repeat(3) @(posedge aclk);
        end
      end
    end
    oready <= 1;
    repeat(2000) @(posedge aclk);

    for(c = 0; c < N; c = c + 1)
    begin
      nexp[c] = 64 >> rate_of[c];
      if(got[c] != nexp[c]) fail($sformatf("slot %0d: %0d words, expected %0d", c, got[c], nexp[c]));
    end
    if(ticks_seen != 8) fail($sformatf("%0d tick words, expected 8", ticks_seen));
    if(ticks != 8) fail($sformatf("tick count %0d, expected 8", ticks));
    if(flags[0]) fail("overflow set");
    if(!flags[2]) fail("IN2 clip not set");
    if(flags[1]) fail("IN1 clip set");
    if(errors == 0) $display("PASS: %0d slots, %0d ticks", N, ticks_seen);
    else $display("FAIL: %0d errors", errors);
    $finish;
  end

endmodule
