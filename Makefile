# ITCH order book, software + RTL.
# MSYS2 users: `bash run_all.sh` handles the PATH and Verilator quirks for you
# (docs/BUILDING.md). SIMD is -march=native — don't add -mavx512* by hand, every
# routine has AVX-512/AVX2/scalar paths selected at compile time (sw/util/simd.hpp).

CXX      ?= g++
CXXSTD    = -std=c++17
OPT       = -O3 -march=native -falign-functions=64 -falign-loops=64
WARN      = -Wall -Wextra
CXXFLAGS  = $(CXXSTD) $(OPT) $(WARN) -pthread

BUILD = build

LIB_SRCS = sw/parser/itch_parser.cpp     \
           sw/book/order_ref_table.cpp   \
           sw/book/order_book.cpp        \
           sw/stats/stats_engine.cpp

SW_SRCS  = sw/main.cpp $(LIB_SRCS) sw/receiver/afxdp_receiver.cpp

TEST_SRCS = sw/tests/test_main.cpp            \
            sw/tests/test_util.cpp            \
            sw/tests/test_order_ref_table.cpp \
            sw/tests/test_itch_parser.cpp     \
            sw/tests/test_mold_parser.cpp     \
            sw/tests/test_book_engine.cpp     \
            sw/tests/test_stats.cpp           \
            sw/tests/test_full_pipeline.cpp   \
            sw/tests/test_program_loader.cpp  \
            sw/tests/test_dsl_vm.cpp          \
            sw/tests/test_bar_builder.cpp     \
            sw/tests/test_fill_engine.cpp     \
            sw/tests/test_order_manager.cpp   \
            sw/tests/test_trade_e2e.cpp       \
            sw/trade/program.cpp              \
            sw/trade/fill_engine.cpp          \
            sw/trade/order_manager.cpp

VERILATOR      ?= verilator_bin.exe
VERILATOR_ROOT ?= C:/msys64/ucrt64/share/verilator
export VERILATOR_ROOT

# RTL in compile order — the package (ob_pkg.sv) must precede the modules that
# import its types, so we list files explicitly rather than globbing.
RTL = rtl/ob_pkg.sv rtl/udp_stripper.sv rtl/mold_stripper.sv rtl/itch_framer.sv \
      rtl/itch_decoder.sv rtl/order_ref_table.sv rtl/book_update_engine.sv \
      rtl/best_tracker.sv rtl/stats_engine.sv rtl/perf_counters.sv rtl/orderbook_top.sv

.PHONY: all sw gen bench test sim lint analysis data gui tb_latency clean dirs trade

all: sw gen bench test          ## build SW + tools and run SW tests

dirs:
	@mkdir -p $(BUILD)

sw: dirs                        ## build the software order book
	$(CXX) $(CXXFLAGS) -o $(BUILD)/orderbook_sw $(SW_SRCS)

TRADE_SRCS = sw/trade/trade_main.cpp sw/trade/program.cpp \
             sw/trade/fill_engine.cpp sw/trade/order_manager.cpp

trade: dirs $(BUILD)/dsl_vm.o   ## build the DSL trading engine
	$(CXX) $(CXXFLAGS) -o $(BUILD)/orderbook_trade $(TRADE_SRCS) $(LIB_SRCS) $(BUILD)/dsl_vm.o

gen: dirs                       ## build the synthetic ITCH generator
	$(CXX) $(CXXSTD) -O2 $(WARN) -o $(BUILD)/gen_itch tools/gen_itch.cpp

mold: dirs                      ## build the .itch -> .mold wrapper
	$(CXX) $(CXXSTD) -O2 $(WARN) -o $(BUILD)/mold_wrap tools/mold_wrap.cpp

bench: dirs                     ## build the Phase1-vs-Phase2 benchmark
	$(CXX) $(CXXFLAGS) -o $(BUILD)/bench tools/bench.cpp $(LIB_SRCS)

data: gen                       ## generate a sample ITCH file
	$(BUILD)/gen_itch data/sample.itch 200000 4

# dsl_vm.cpp is compiled alone with -ffp-contract=off: the strategy VM must be
# bit-compatible with the float32 golden model (no FMA contraction).
$(BUILD)/dsl_vm.o: sw/trade/dsl_vm.cpp sw/trade/dsl_vm.hpp sw/trade/program.hpp sw/trade/bar_builder.hpp | dirs
	$(CXX) $(CXXFLAGS) -ffp-contract=off -c -o $@ sw/trade/dsl_vm.cpp

test: dirs $(BUILD)/dsl_vm.o    ## build + run the software test suite
	$(CXX) $(CXXFLAGS) -o $(BUILD)/run_tests $(TEST_SRCS) $(LIB_SRCS) $(BUILD)/dsl_vm.o
	$(BUILD)/run_tests

sim:                            ## build + run the RTL Verilator tests
	bash sim/run_rtl_tests.sh

sim-board:                      ## Verilate + test the board chain (UART bridge + banded book)
	bash sim/run_board_sim.sh

VIVADO ?= C:/Xilinx/Vivado/2022.2/bin/vivado.bat
BOARD  ?= urbana
bitstream: dirs                 ## full P&R + bitstream (BOARD=urbana|arty)
	$(VIVADO) -mode batch -source fpga/board/bitstream.tcl -nojournal \
	    -log build/vivado_bitstream_$(BOARD).log -tclargs $(BOARD)

lint:                           ## Verilator lint of the RTL (package first)
	$(VERILATOR) --lint-only -Wall -Wno-UNUSED -Wno-DECLFILENAME \
	    -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Irtl $(RTL)

analysis:                       ## generate latency/profile/depth plots (needs CSVs)
	python analysis/latency_compare.py
	python analysis/message_profile.py
	python analysis/book_depth.py

gui:                            ## install GUI deps + launch Streamlit
	bash gui/run_gui.sh

tb_latency: sim                 ## build tb_latency perf harness (via sim target)
	@test -x sim/obj_rtl/tb_latency.exe && \
	  echo "tb_latency: sim/obj_rtl/tb_latency.exe ready" || \
	  echo "tb_latency: not built (run 'make sim' first)"

clean:
	rm -rf $(BUILD)/*.exe $(BUILD)/csv obj_dir sim/obj_dir sim/wave.vcd
