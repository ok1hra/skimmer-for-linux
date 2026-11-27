#!/bin/bash
# fpga/sim/run.sh TB — compile and run one testbench with Vivado's xsim
# (the design's own Verilog from projects/skimmer_rp_122_88, XPM included).
set -e
cd "$(dirname "$0")"
TB=${1:-skr_pack_tb}
: "${XILINX_DIR:=/opt/Xilinx/2025.2}"
. "$XILINX_DIR/Vivado/settings64.sh"
# xsim links with its own ld, which does not search the multiarch directory
export LIBRARY_PATH=/usr/lib/x86_64-linux-gnu${LIBRARY_PATH:+:$LIBRARY_PATH}
W=../tmp/sim-$TB
rm -rf "$W"; mkdir -p "$W"; cd "$W"
xvlog -sv -L xpm ../../projects/skimmer_rp_122_88/skr_*.v ../../sim/$TB.sv > xvlog.log
xvlog -sv "$XILINX_VIVADO/data/verilog/src/glbl.v" >> xvlog.log
xelab -L xpm -L unisims_ver -debug off $TB glbl -s $TB > xelab.log
xsim $TB -R | tee xsim.log | grep -E "PASS|FAIL|Error|ERROR"
grep -q "^PASS" xsim.log
