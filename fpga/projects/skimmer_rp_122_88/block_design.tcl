# The skimmer's own bitstream for the STEMlab 122.88-16 (docs/FPGA.md).
#
# N slots (a multiple of 4), each: input select → NCO × mixer → CIC ÷8
# (4 stages, per I and Q); then CICs ÷20 (6 stages) each shared by 8 of
# those channels; all slots through one time-shared FIR ÷2 (384 kHz) and
# three half-bands ÷2 (192, 96, 48 kHz), unity gain; the packer takes each
# slot at its own rate into tagged 64-bit words, the DMA writer puts them
# into a ring in DDR (ACP).
#
# cfg (32-bit words, at 0x40000000):
#   0 control: bit 0 run (0 = every slot held: the sync), bit 1 apply (a
#     rising edge loads the new phase increments), bit 2 DMA on, bit 3
#     the ring is mapped cached (CMA) — else its writes bypass the caches
#   1 input: bit s = slot s on IN2      2 enable: bit s = slot s on
#   3..6 rate masks: bit s = slot s at 384 / 192 / 96 / 48 kHz
#   7 shifts: bits 4:0, 12:8, 20:16, 28:24 = the output shift a rate
#   8 the ring's base address (8 MB)     9 tone phase increment
#   10 tone amplitude (bits 15:0)        11 + s: slot s's phase increment
# sts (at 0x41000000): 0 "SKRF", 1 version (7:0 minor, 15:8 major,
#   23:16 MIX_W, 31:24 N), 2 the writer's position (128-byte bursts),
#   3 flags, 4 peaks (15:0 IN1, 31:16 IN2), 5–6 ticks since the sync.

if {![info exists N]} {
  set N [expr {[info exists ::env(SKR_N)] ? $::env(SKR_N) : 24}]
}
# the mixer's output, the first CIC's input: whole bytes (an AXIS tdata
# narrower than its bytes would leave the padding unconnected)
if {![info exists MIX_W]} {
  set MIX_W [expr {[info exists ::env(SKR_MIX_W)] ? $::env(SKR_MIX_W) : 24}]
}
set VERSION_MAJOR 0
set VERSION_MINOR 3

if {$N % 4 != 0} { error "N must be a multiple of 4 (8 channels a shared CIC)" }
set CH [expr {2 * $N}]
set CH_W [expr {int(ceil(log($CH) / log(2)))}]
set CFG_W [expr {32 * (11 + $N)}]
set STS_W 224
# the second CICs: NG of them, 8 channels each — CIC g takes the channels
# g, g + NG, …, g + 7·NG, so their outputs side by side are channels
# NG·k … NG·k + NG − 1, in order
set NG [expr {$CH / 8}]

set dir projects/skimmer_rp_122_88
# our modules: before the block design refers to them (project.tcl adds
# projects/NAME/*.v only after it, as top-level sources)
add_files -norecurse [glob $dir/hdl/*.v]
update_compile_order -fileset sources_1
source $dir/filters/fir.tcl
source $dir/filters/hb.tcl

# Create clk_wiz
cell xilinx.com:ip:clk_wiz pll_0 {
  PRIMITIVE PLL
  PRIM_IN_FREQ.VALUE_SRC USER
  PRIM_IN_FREQ 122.88
  PRIM_SOURCE Differential_clock_capable_pin
  CLKOUT1_USED true
  CLKOUT1_REQUESTED_OUT_FREQ 122.88
  CLKOUT2_USED true
  CLKOUT2_REQUESTED_OUT_FREQ 245.76
  CLKOUT2_REQUESTED_PHASE 157.5
  CLKOUT3_USED true
  CLKOUT3_REQUESTED_OUT_FREQ 245.76
  CLKOUT3_REQUESTED_PHASE 202.5
  USE_RESET false
} {
  clk_in1_p adc_clk_p_i
  clk_in1_n adc_clk_n_i
}

# Create processing_system7
cell xilinx.com:ip:processing_system7 ps_0 {
  PCW_IMPORT_BOARD_PRESET cfg/red_pitaya.xml
  PCW_USE_S_AXI_ACP 1
  PCW_USE_DEFAULT_ACP_USER_VAL 1
} {
  M_AXI_GP0_ACLK pll_0/clk_out1
  S_AXI_ACP_ACLK pll_0/clk_out1
}

# Create all required interconnections
apply_bd_automation -rule xilinx.com:bd_rule:processing_system7 -config {
  make_external {FIXED_IO, DDR}
  Master Disable
  Slave Disable
} [get_bd_cells ps_0]

# Create xlconstant
cell constant const_0

# Create proc_sys_reset
cell xilinx.com:ip:proc_sys_reset rst_0 {} {
  ext_reset_in const_0/dout
  dcm_locked pll_0/locked
  slowest_sync_clk pll_0/clk_out1
}

# ADC

# Create axis_red_pitaya_adc
cell axis_red_pitaya_adc adc_0 {
  ADC_DATA_WIDTH 16
} {
  aclk pll_0/clk_out1
  adc_dat_a adc_dat_a_i
  adc_dat_b adc_dat_b_i
  adc_csn adc_csn_o
}

# HUB

# Create axi_hub
cell axi_hub hub_0 {
  CFG_DATA_WIDTH $CFG_W
  STS_DATA_WIDTH $STS_W
} {
  S_AXI ps_0/M_AXI_GP0
  aclk pll_0/clk_out1
  aresetn rst_0/peripheral_aresetn
}

# cfg word w, bits from..to
proc cfg_slice {name w from to} {
  upvar CFG_W CFG_W
  cell port_slicer $name {
    DIN_WIDTH $CFG_W DIN_FROM [expr {32 * $w + $from}] DIN_TO [expr {32 * $w + $to}]
  } {
    din hub_0/cfg_data
  }
}

cfg_slice slice_ctrl 0 31 0
cfg_slice slice_ins 1 [expr {$N - 1}] 0
cfg_slice slice_ena 2 [expr {$N - 1}] 0
for {set r 0} {$r < 4} {incr r} {
  cfg_slice slice_mask_$r [expr {3 + $r}] [expr {$N - 1}] 0
}
cfg_slice slice_shifts 7 31 0
cfg_slice slice_base 8 31 0
cfg_slice slice_tone 9 31 0
cfg_slice slice_amp 10 15 0

# Create skr_ctrl
cell skr_ctrl ctrl_0 {} {
  aclk pll_0/clk_out1
  aresetn rst_0/peripheral_aresetn
  ctrl slice_ctrl/dout
}

# SLOTS

for {set s 0} {$s < $N} {incr s} {

  # Create port_slicer
  cell port_slicer ins_$s {
    DIN_WIDTH $N DIN_FROM $s DIN_TO $s
  } {
    din slice_ins/dout
  }

  # Create port_slicer
  cell port_slicer ena_$s {
    DIN_WIDTH $N DIN_FROM $s DIN_TO $s
  } {
    din slice_ena/dout
  }

  # Create skr_insel
  cell skr_insel insel_$s {} {
    aclk pll_0/clk_out1
    sel ins_$s/dout
    en ena_$s/dout
    din adc_0/m_axis_tdata
  }

  cfg_slice pinc_$s [expr {11 + $s}] 31 0

  # Create skr_latch
  cell skr_latch latch_$s {} {
    aclk pll_0/clk_out1
    run_n ctrl_0/run_n
    apply ctrl_0/apply
    din pinc_$s/dout
  }

  # Create dds (NEGATIVE_SINE FALSE: the stream RF-inverted, as the stock
  # bitstream's — measured 2026-11-27 — so P1 clients see what they expect)
  cell dds dds_$s {
    NEGATIVE_SINE FALSE
  } {
    pinc latch_$s/dout
    aclk pll_0/clk_out1
    aresetn ctrl_0/run_n
  }

  for {set q 0} {$q < 2} {incr q} {
    set c [expr {2 * $s + $q}]

    # Create port_slicer
    cell port_slicer dds_slice_$c {
      DIN_WIDTH 48 DIN_FROM [expr {24 * $q + 23}] DIN_TO [expr {24 * $q}]
    } {
      din dds_$s/dout
    }

    # Create dsp48
    cell dsp48 mult_$c {
      A_WIDTH 24
      B_WIDTH 16
      P_WIDTH $MIX_W
    } {
      A dds_slice_$c/dout
      B insel_$s/dout
      CLK pll_0/clk_out1
    }

    # Create cic_compiler: ÷8, 122.88 → 15.36 MS/s
    cell xilinx.com:ip:cic_compiler cic_$c {
      INPUT_DATA_WIDTH.VALUE_SRC USER
      FILTER_TYPE Decimation
      NUMBER_OF_STAGES 4
      SAMPLE_RATE_CHANGES Fixed
      FIXED_OR_INITIAL_RATE 8
      INPUT_SAMPLE_FREQUENCY 122.88
      CLOCK_FREQUENCY 122.88
      INPUT_DATA_WIDTH $MIX_W
      QUANTIZATION Truncation
      OUTPUT_DATA_WIDTH 24
      USE_XTREME_DSP_SLICE false
      HAS_ARESETN true
    } {
      s_axis_data_tdata mult_$c/P
      s_axis_data_tvalid const_0/dout
      aclk pll_0/clk_out1
      aresetn ctrl_0/run_n
    }
  }
}

# The second CICs: ÷20, 15.36 MS/s → 768 kS/s, 8 channels each

for {set g 0} {$g < $NG} {incr g} {
  set ports {}
  for {set k 0} {$k < 8} {incr k} {
    lappend ports [format S%02d_AXIS $k] cic_[expr {$g + $NG * $k}]/M_AXIS_DATA
  }
  lappend ports aclk pll_0/clk_out1 aresetn ctrl_0/run_n
  cell xilinx.com:ip:axis_combiner comb_$g {
    TDATA_NUM_BYTES.VALUE_SRC USER
    TDATA_NUM_BYTES 3
    NUM_SI 8
  } $ports

  cell xilinx.com:ip:axis_dwidth_converter conv_$g {
    S_TDATA_NUM_BYTES.VALUE_SRC USER
    S_TDATA_NUM_BYTES 24
    M_TDATA_NUM_BYTES 3
  } {
    S_AXIS comb_$g/M_AXIS
    aclk pll_0/clk_out1
    aresetn ctrl_0/run_n
  }

  cell xilinx.com:ip:cic_compiler cic2_$g {
    INPUT_DATA_WIDTH.VALUE_SRC USER
    FILTER_TYPE Decimation
    NUMBER_OF_STAGES 6
    SAMPLE_RATE_CHANGES Fixed
    FIXED_OR_INITIAL_RATE 20
    NUMBER_OF_CHANNELS 8
    INPUT_SAMPLE_FREQUENCY 15.36
    CLOCK_FREQUENCY 122.88
    INPUT_DATA_WIDTH 24
    QUANTIZATION Truncation
    OUTPUT_DATA_WIDTH 24
    USE_XTREME_DSP_SLICE false
    HAS_ARESETN true
  } {
    S_AXIS_DATA conv_$g/M_AXIS
    aclk pll_0/clk_out1
    aresetn ctrl_0/run_n
  }
}

set ports {}
for {set g 0} {$g < $NG} {incr g} {
  lappend ports [format S%02d_AXIS $g] cic2_$g/M_AXIS_DATA
}
lappend ports aclk pll_0/clk_out1 aresetn ctrl_0/run_n
cell xilinx.com:ip:axis_combiner comb_all {
  TDATA_NUM_BYTES.VALUE_SRC USER
  TDATA_NUM_BYTES 3
  NUM_SI $NG
} $ports

# Create axis_dwidth_converter
cell xilinx.com:ip:axis_dwidth_converter conv_all {
  S_TDATA_NUM_BYTES.VALUE_SRC USER
  S_TDATA_NUM_BYTES [expr {3 * $NG}]
  M_TDATA_NUM_BYTES 3
} {
  S_AXIS comb_all/M_AXIS
  aclk pll_0/clk_out1
  aresetn ctrl_0/run_n
}

# The FIR ÷2 (768 → 384 kHz) and the half-bands (→ 192, 96, 48 kHz):
# all slots time-shared, 24-bit data, 24-bit coefficients with 23
# fractional bits (18 bits left the stopband at -80 dB), the output at full
# precision: the sample at unity gain is bits 46:23 (the packer's window);
# the channel number in tuser for the packer.

proc fir_cell {name coef rate} {
  upvar CH CH
  cell xilinx.com:ip:fir_compiler $name {
    DATA_WIDTH.VALUE_SRC USER
    DATA_WIDTH 24
    COEFFICIENTVECTOR {$coef}
    COEFFICIENT_WIDTH 24
    QUANTIZATION Quantize_Only
    BESTPRECISION false
    COEFFICIENT_FRACTIONAL_BITS 23
    FILTER_TYPE Decimation
    DECIMATION_RATE 2
    NUMBER_CHANNELS $CH
    NUMBER_PATHS 1
    SAMPLE_FREQUENCY $rate
    CLOCK_FREQUENCY 122.88
    OUTPUT_ROUNDING_MODE Full_Precision
    M_DATA_HAS_TUSER Chan_ID_Field
    HAS_ARESETN true
  } {
    aclk pll_0/clk_out1
    aresetn ctrl_0/run_n
  }
}

fir_cell fir_0 $fir_coef 0.768
connect_bd_intf_net [get_bd_intf_pins conv_all/M_AXIS] [get_bd_intf_pins fir_0/S_AXIS_DATA]
fir_cell hb_1 $hb_coef 0.384
fir_cell hb_2 $hb_coef 0.192
fir_cell hb_3 $hb_coef 0.096

# the filters' full-precision widths, as tdata (whole bytes)
foreach {k name} {0 fir_0 1 hb_1 2 hb_2 3 hb_3} {
  set w [get_property CONFIG.OUTPUT_WIDTH [get_bd_cells $name]]
  set DW($k) [expr {8 * (($w + 7) / 8)}]
  puts "skimmer_rp: $name output $w bits"
}

# Create skr_pack
cell skr_pack pack_0 {
  N $N
  CH_W $CH_W
  D_W0 $DW(0)
  D_W1 $DW(1)
  D_W2 $DW(2)
  D_W3 $DW(3)
  LSB 23
} {
  aclk pll_0/clk_out1
  aresetn ctrl_0/run_n
  mask_0 slice_mask_0/dout
  mask_1 slice_mask_1/dout
  mask_2 slice_mask_2/dout
  mask_3 slice_mask_3/dout
  shifts slice_shifts/dout
  adc_data adc_0/m_axis_tdata
  S_AXIS_0 fir_0/M_AXIS_DATA
  S_AXIS_1 hb_1/M_AXIS_DATA
  S_AXIS_2 hb_2/M_AXIS_DATA
  S_AXIS_3 hb_3/M_AXIS_DATA
  M_AXIS_0 hb_1/S_AXIS_DATA
  M_AXIS_1 hb_2/S_AXIS_DATA
  M_AXIS_2 hb_3/S_AXIS_DATA
}

# DMA

# Create xlconstant
cell constant const_ring {
  CONST_WIDTH 16
  CONST_VALUE 65535
}

# Create skr_ram_writer
cell skr_ram_writer writer_0 {
  ADDR_WIDTH 16
  AXI_ID_WIDTH 3
  AXIS_TDATA_WIDTH 64
  FIFO_WRITE_DEPTH 512
} {
  S_AXIS pack_0/M_AXIS
  M_AXI ps_0/S_AXI_ACP
  min_addr slice_base/dout
  cacheable ctrl_0/cacheable
  cfg_data const_ring/dout
  aclk pll_0/clk_out1
  aresetn ctrl_0/dma_n
}

# STATUS

cell constant const_magic {
  CONST_WIDTH 32
  CONST_VALUE 1179798355
}

cell constant const_version {
  CONST_WIDTH 32
  CONST_VALUE [expr {($N << 24) | ($MIX_W << 16) | ($VERSION_MAJOR << 8) | $VERSION_MINOR}]
}

cell constant const_pad {
  CONST_WIDTH 16
  CONST_VALUE 0
}

# Create xlconcat
cell xilinx.com:ip:xlconcat concat_sts {
  NUM_PORTS 7
  IN0_WIDTH 32
  IN1_WIDTH 32
  IN2_WIDTH 16
  IN3_WIDTH 16
  IN4_WIDTH 32
  IN5_WIDTH 32
  IN6_WIDTH 64
} {
  In0 const_magic/dout
  In1 const_version/dout
  In2 writer_0/sts_data
  In3 const_pad/dout
  In4 pack_0/flags
  In5 pack_0/peaks
  In6 pack_0/ticks
  dout hub_0/sts_data
}

# DAC: the OUT1 test tone (docs/PHASING.md, "Calibration")

# Create dds
cell dds dds_tone {
  NEGATIVE_SINE TRUE
} {
  pinc slice_tone/dout
  aclk pll_0/clk_out1
  aresetn rst_0/peripheral_aresetn
}

cell port_slicer tone_slice {
  DIN_WIDTH 48 DIN_FROM 23 DIN_TO 0
} {
  din dds_tone/dout
}

# Create dsp48
cell dsp48 mult_tone {
  A_WIDTH 24
  B_WIDTH 16
  P_WIDTH 14
} {
  A tone_slice/dout
  B slice_amp/dout
  CLK pll_0/clk_out1
}

cell constant const_dac {
  CONST_WIDTH 18
  CONST_VALUE 0
}

cell xilinx.com:ip:xlconcat concat_dac {
  NUM_PORTS 2
  IN0_WIDTH 14
  IN1_WIDTH 18
} {
  In0 mult_tone/P
  In1 const_dac/dout
}

# Create axis_red_pitaya_dac
cell axis_red_pitaya_dac dac_0 {
  DAC_DATA_WIDTH 14
} {
  aclk pll_0/clk_out1
  ddr_clk pll_0/clk_out2
  wrt_clk pll_0/clk_out3
  locked pll_0/locked
  dac_clk dac_clk_o
  dac_rst dac_rst_o
  dac_sel dac_sel_o
  dac_wrt dac_wrt_o
  dac_dat dac_dat_o
  s_axis_tdata concat_dac/dout
  s_axis_tvalid const_0/dout
}
