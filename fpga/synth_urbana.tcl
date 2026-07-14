# Out-of-context synthesis of orderbook_top for the RealDigital Urbana board
# (Spartan-7 XC7S50CSGA324-1). Non-project mode; no pin constraints needed.
#
#   vivado -mode batch -source fpga/synth_urbana.tcl
#
# Reports land in fpga/reports/. Vivado predefines the SYNTHESIS macro, which
# selects the FPGA-sized memories in ob_pkg.sv (the full-size direct-indexed
# book is simulation-only and cannot exist in fabric).

# single-threaded: skips the helper-process flow (two extra ~0.5 GB vivado
# children) — this design is small enough that the runtime cost is minutes,
# and the 16 GB laptop is usually short on RAM, not cores
set_param general.maxThreads 1

set here    [file dirname [file normalize [info script]]]
set rtl_dir [file normalize [file join $here .. rtl]]
set rpt_dir [file join $here reports]
file mkdir $rpt_dir

# package first: the modules import its types
read_verilog -sv [list \
  $rtl_dir/ob_pkg.sv \
  $rtl_dir/udp_stripper.sv \
  $rtl_dir/mold_stripper.sv \
  $rtl_dir/itch_framer.sv \
  $rtl_dir/itch_decoder.sv \
  $rtl_dir/order_ref_table.sv \
  $rtl_dir/book_update_engine.sv \
  $rtl_dir/best_tracker.sv \
  $rtl_dir/stats_engine.sv \
  $rtl_dir/perf_counters.sv \
  $rtl_dir/orderbook_top.sv \
]

synth_design -top orderbook_top -part xc7s50csga324-1 -mode out_of_context

# Urbana system clock is 100 MHz; constrain post-synth for the timing report.
create_clock -period 10.000 -name clk [get_ports clk]

report_utilization   -file $rpt_dir/utilization.rpt
report_timing_summary -file $rpt_dir/timing_summary.rpt
report_ram_utilization -file $rpt_dir/ram_utilization.rpt

puts "== WNS: [get_property SLACK [get_timing_paths -max_paths 1]] ns (10 ns target)"
puts "== reports written to $rpt_dir"
