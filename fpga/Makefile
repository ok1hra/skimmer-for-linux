# The Red Pitaya bitstreams (STEMlab 122.88-16, Zynq-7020).
#   make            the skimmer's own bitstream (projects/skimmer_rp_122_88)
#   make NAME=sdr_receiver_hpsdr_122_88   Pavel Demin's, for comparison
# Needs Vivado 2025.2 on PATH (build.sh sources its settings64.sh).
# The build system is red-pitaya-notes' (VENDOR.md): scripts/project.tcl
# builds the block design from projects/$(NAME)/block_design.tcl.

NAME = skimmer_rp_122_88
PART = xc7z020clg400-1

VIVADO = vivado -nolog -nojournal -mode batch

.PRECIOUS: tmp/%.xpr tmp/%.bit

bit: tmp/$(NAME).bit

xpr: tmp/$(NAME).xpr

tmp/%.xpr: projects/%
	mkdir -p $(@D)
	$(VIVADO) -source scripts/project.tcl -tclargs $* $(PART)

tmp/%.bit: tmp/%.xpr
	mkdir -p $(@D)
	$(VIVADO) -source scripts/bitstream.tcl -tclargs $*

clean:
	rm -rf tmp .Xil

.PHONY: bit xpr clean
