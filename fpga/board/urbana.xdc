# RealDigital Urbana pin + timing constraints for the order-book fpga_top
# (Spartan-7 xc7s50csga324-1).
#
# Pins taken from RealDigital's official Urbana master constraints file
# (https://www.realdigital.org/downloads/1f07b1146ec59e165634a5b6c782a17d.txt,
# fetched 2026-07-13): CLK_100MHZ=N15, UART_TXD=B16 (FPGA->PC), UART_RXD=A16
# (PC->FPGA), LED[0..3]=C13/C14/D14/D15, BTN[0]=J2 (LVCMOS25 bank).
# Urbana pushbuttons are ACTIVE-HIGH (reference manual: "output a '1' only
# while actively pressed"), so the bitstream flow builds fpga_top with
# RESET_ACTIVE_HIGH defined and BTN0 drives ck_rstn.

# 100 MHz board oscillator
set_property -dict {PACKAGE_PIN N15 IOSTANDARD LVCMOS33} [get_ports clk100]
create_clock -period 10.000 -name sys_clk [get_ports clk100]

# reset = BTN0 (active HIGH on this board; inverted inside fpga_top)
set_property -dict {PACKAGE_PIN J2 IOSTANDARD LVCMOS25} [get_ports ck_rstn]
set_false_path -from [get_ports ck_rstn]

# USB-UART: UART_RXD carries PC->FPGA data, UART_TXD carries FPGA->PC
set_property -dict {PACKAGE_PIN A16 IOSTANDARD LVCMOS33} [get_ports uart_rx]
set_property -dict {PACKAGE_PIN B16 IOSTANDARD LVCMOS33} [get_ports uart_tx]
set_false_path -from [get_ports uart_rx]
set_false_path -to   [get_ports uart_tx]

# LEDs LED0..LED3
set_property -dict {PACKAGE_PIN C13 IOSTANDARD LVCMOS33} [get_ports {led[0]}]
set_property -dict {PACKAGE_PIN C14 IOSTANDARD LVCMOS33} [get_ports {led[1]}]
set_property -dict {PACKAGE_PIN D14 IOSTANDARD LVCMOS33} [get_ports {led[2]}]
set_property -dict {PACKAGE_PIN D15 IOSTANDARD LVCMOS33} [get_ports {led[3]}]
set_false_path -to [get_ports {led[*]}]

set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
