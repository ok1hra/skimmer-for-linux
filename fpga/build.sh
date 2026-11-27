#!/bin/bash
# fpga/build.sh [NAME] — build a bitstream with Vivado 2025.2 and print what
# it costs (utilisation, timing, power) into tmp/NAME.summary.
set -e
cd "$(dirname "$0")"
NAME=${1:-skimmer_rp_122_88}
: "${XILINX_DIR:=/opt/Xilinx/2025.2}"
. "$XILINX_DIR/Vivado/settings64.sh"
rm -rf "tmp/$NAME.xpr" "tmp/$NAME.runs" "tmp/$NAME.bit"
make NAME="$NAME" bit
vivado -nolog -nojournal -mode batch -source scripts/report.tcl -tclargs "$NAME"
sha256sum "tmp/$NAME.bit" | tee -a "tmp/$NAME.summary"
