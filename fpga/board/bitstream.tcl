# Full bitstream build for the order-book board top. Board-parameterized:
#
#   vivado -mode batch -source fpga/board/bitstream.tcl -tclargs urbana
#   vivado -mode batch -source fpga/board/bitstream.tcl -tclargs arty [divisor]
#
#   urbana : RealDigital Urbana, xc7s50csga324-1, urbana.xdc,
#            active-HIGH reset button (RESET_ACTIVE_HIGH)
#   arty   : Digilent Arty A7-100, xc7a100tcsg324-1, arty_a7_100.xdc
#
# Optional second tclarg overrides UART_DIVISOR (core clk 100 MHz / baud):
# 868 = 115200 (default, bring-up), 100 = 1 Mbaud (demo).
# Output: build/fpga_top_<board>.bit + timing/utilization/DRC reports.
# Flow mirrors nmc_project/fpga/board/bitstream.tcl (synth -> opt -> place
# ExtraTimingOpt -> phys_opt -> route -> phys_opt).

set_param general.maxThreads 1   ;# 16 GB laptop: RAM, not cores, is the limit

set board "urbana"
if {$argc >= 1} { set board [lindex $argv 0] }
set divisor 868
if {$argc >= 2} { set divisor [lindex $argv 1] }

switch $board {
  urbana {
    set part xc7s50csga324-1
    set xdc  fpga/board/urbana.xdc
    set defs [list RESET_ACTIVE_HIGH=1 UART_DIVISOR_OVR=$divisor]
  }
  arty {
    set part xc7a100tcsg324-1
    set xdc  fpga/board/arty_a7_100.xdc
    set defs [list UART_DIVISOR_OVR=$divisor]
  }
  default { error "unknown board '$board' (urbana|arty)" }
}

file mkdir build

read_verilog -sv {
  rtl/ob_pkg.sv
  rtl/udp_stripper.sv
  rtl/mold_stripper.sv
  rtl/itch_framer.sv
  rtl/itch_decoder.sv
  rtl/order_ref_table.sv
  rtl/book_update_engine.sv
  rtl/best_tracker.sv
  rtl/stats_engine.sv
  rtl/perf_counters.sv
  rtl/orderbook_top.sv
  fpga/board/uart_itch_bridge.sv
  fpga/board/fpga_top.sv
}

read_xdc $xdc

# -generic would be cleaner for the divisor but is unreliable across 2022.x
# for SV parameters; the define route is proven in the nmc flow.
synth_design -top fpga_top -part $part -verilog_define $defs

opt_design
place_design -directive ExtraTimingOpt
phys_opt_design -directive AggressiveExplore
route_design -directive Explore
phys_opt_design -directive Explore

report_timing_summary -file build/timing_route_$board.rpt
report_utilization    -file build/util_route_$board.rpt
report_drc            -file build/drc_route_$board.rpt

set wns [get_property SLACK [get_timing_paths -max_paths 1 -nworst 1 -setup]]
puts "== fpga_top routed on $part ($board): setup WNS = $wns ns =="

write_bitstream -force build/fpga_top_$board.bit
puts "== bitstream: build/fpga_top_$board.bit =="
