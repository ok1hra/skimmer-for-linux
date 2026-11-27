# What a routed design costs: utilisation (whole and by block), timing, power.
set project_name [lindex $argv 0]
set dcp tmp/$project_name.runs/impl_1/system_wrapper_routed.dcp
open_checkpoint $dcp
report_utilization -file tmp/$project_name.util
report_utilization -hierarchical -hierarchical_depth 4 -file tmp/$project_name.hier
report_timing_summary -file tmp/$project_name.timing
report_power -file tmp/$project_name.power
set out [open tmp/$project_name.summary w]
set u [report_utilization -return_string]
foreach line [split $u "\n"] {
  if {[regexp {^\| (Slice LUTs|Slice Registers|Slice|Block RAM Tile|DSPs) +\|} $line]} {
    puts $out $line
  }
}
set p [report_power -return_string]
foreach line [split $p "\n"] {
  if {[regexp {Total On-Chip Power|Junction Temperature} $line]} { puts $out $line }
}
puts $out "WNS [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]] ns"
close $out
close_design
