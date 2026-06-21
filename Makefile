# Self-contained, auto-detecting VGA monitor -- one monitor, three simulators,
# streamed to a standard viewer as raw rgb24 over TCP.
#
# One monitor variant: no clock, no parameters. The same C++ backend
# (backend/vga_monitor.cpp) is reached by three thin shims -- DPI (sv/),
# VHPIDIRECT (vhdl/), VPI (v/). It reconstructs the framebuffer in-process and
# streams each finished frame; no windowing toolkit is linked into the simulator.
#
# Start a viewer first (it listens), then run a stream target (the sim connects
# once geometry locks):
#   ffplay -f rawvideo -pixel_format rgb24 -video_size 640x480 \
#          -i 'tcp://0.0.0.0:5000?listen=1'
#   make stream-dpi       # or stream-vhpi / stream-vpi   (STREAM=host:port)
#   make stream-two       # two moving patterns -> two viewers (ports base, base+1)
#
# Full end-to-end test (ffmpeg grabs a frame off the socket, compares to golden):
#   make stream-test      # build from source;  SIM=dpi|vhpi|nvc|vpi|all
#   make dist && make dist-vpi && make dist-test   # test the PREBUILT artifacts
#   make clean

BACKEND := backend/vga_monitor.cpp
BUILD   := build
MODULE  ?= tb_gradient
STREAM  ?= 127.0.0.1:5000
SIM     ?= all
DIST    ?= $(BUILD)/dist

# DUT shared by every fixture. The Verilog DUT (.sv) also serves the VPI flow.
SV_DUT   := examples/vga_signal_generator.sv examples/gradient_source.sv
VHDL_DUT := examples/vga_signal_generator.vhdl examples/gradient_source.vhdl

.PHONY: stream-dpi stream-two stream-vhpi stream-nvc stream-vpi stream-test \
        stream-dpi-dist stream-vhpi-dist stream-nvc-dist stream-vpi-dist \
        dist dist-vpi dist-test clean

# ---- DPI / Verilator, built WITHOUT --timing (clock from sim_main) ----------
stream-dpi:
	verilator --cc --exe --build -j 0 -Wno-WIDTH -Wno-CASEINCOMPLETE --timescale 1ns/1ps \
		--top-module tb_stream --Mdir $(BUILD)/stream-dpi -o tb_stream \
		examples/tb_stream.sv examples/vga_signal_generator.sv examples/gradient_source.sv \
		sv/vga_monitor.sv $(BACKEND) examples/sim_main_stream.cpp
	VGA_MONITOR_STREAM=$(STREAM) ./$(BUILD)/stream-dpi/tb_stream

# ---- Two monitors, two MOVING patterns -> two streams (ports base, base+1) --
# A richer thing to watch. Start two viewers first (default ports 5000 + 5001):
#   ffplay -f rawvideo -pixel_format rgb24 -video_size 640x480 -i 'tcp://0.0.0.0:5000?listen=1'
#   ffplay -f rawvideo -pixel_format rgb24 -video_size 640x480 -i 'tcp://0.0.0.0:5001?listen=1'
stream-two:
	verilator --cc --exe --build -j 0 -Wno-WIDTH -Wno-CASEINCOMPLETE --timescale 1ns/1ps \
		--top-module tb_two_monitors --Mdir $(BUILD)/stream-two -o tb_two_monitors \
		examples/tb_two_monitors.sv examples/vga_signal_generator.sv \
		examples/scroll_source.sv examples/bars_box_source.sv \
		sv/vga_monitor.sv $(BACKEND) examples/sim_main.cpp
	VGA_MONITOR_STREAM=$(STREAM) ./$(BUILD)/stream-two/tb_two_monitors

# ---- VHPIDIRECT / GHDL -----------------------------------------------------
$(BUILD)/stream-vhpi/backend.o: $(BACKEND)
	mkdir -p $(BUILD)/stream-vhpi
	g++ -O2 -fPIC -c $(BACKEND) -o $@
$(BUILD)/stream-vhpi/vhpi.o: vhdl/vga_monitor_vhpi.cpp
	mkdir -p $(BUILD)/stream-vhpi
	g++ -O2 -fPIC -c $< -o $@
stream-vhpi: $(BUILD)/stream-vhpi/backend.o $(BUILD)/stream-vhpi/vhpi.o
	ghdl -a --std=08 --workdir=$(BUILD)/stream-vhpi \
		vhdl/vga_monitor_pkg.vhdl vhdl/vga_monitor.vhdl $(VHDL_DUT) examples/tb_gradient.vhdl
	ghdl -e --std=08 --workdir=$(BUILD)/stream-vhpi -o $(BUILD)/stream-vhpi/$(MODULE) \
		-Wl,$(BUILD)/stream-vhpi/backend.o -Wl,$(BUILD)/stream-vhpi/vhpi.o -Wl,-lstdc++ $(MODULE)
	VGA_MONITOR_STREAM=$(STREAM) ./$(BUILD)/stream-vhpi/$(MODULE)

# ---- VPI / Icarus ----------------------------------------------------------
# If vvp bundles an older glibc than the system (e.g. oss-cad-suite), run its
# real binary against the system loader/libs -- a no-op on a normal install.
IVL_INC    := $(shell dirname $(shell command -v iverilog))/../include/iverilog
VVP        ?= vvp
VVP_PATH   := $(shell command -v $(VVP) 2>/dev/null)
OSS_ROOT   := $(patsubst %/bin/vvp,%,$(VVP_PATH))
SYS_LOADER := $(firstword $(wildcard /lib64/ld-linux-x86-64.so.2 /lib/ld-linux-x86-64.so.2))
ifeq ($(wildcard $(OSS_ROOT)/libexec/vvp),)
  VVP_RUN := $(VVP)
else
  VVP_RUN := $(SYS_LOADER) --library-path /usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu:$(OSS_ROOT)/lib $(OSS_ROOT)/libexec/vvp
endif

stream-vpi:
	mkdir -p $(BUILD)/stream-vpi
	g++ -O2 -fPIC -shared -I$(IVL_INC) -Wl,--wrap=__isoc23_strtol \
		v/vga_monitor_vpi.cpp v/glibc_compat.c $(BACKEND) \
		-o $(BUILD)/stream-vpi/vga_monitor.vpi
	iverilog -g2012 -o $(BUILD)/stream-vpi/$(MODULE).vvp -s $(MODULE) \
		examples/$(MODULE).v v/vga_monitor.v $(SV_DUT)
	VGA_MONITOR_STREAM=$(STREAM) \
		$(VVP_RUN) -M$(BUILD)/stream-vpi -m vga_monitor $(BUILD)/stream-vpi/$(MODULE).vvp

# ---- Full end-to-end: ffmpeg grabs a frame off the socket vs golden --------
# stream-test builds each sim from source; dist-test instead drives the PREBUILT
# artifacts in $(DIST). Both skip simulators whose tool isn't installed.
stream-test:
	python3 tests/stream_e2e.py --sim $(SIM)

dist-test:
	python3 tests/stream_e2e.py --sim $(SIM) --dist $(DIST)

# ---- Shareable artifact: portable, self-contained backend libraries --------
# No simulator dependency. A DPI simulator loads libvga_monitor_dpi alongside
# sv/vga_monitor.sv; GHDL/NVC load libvga_monitor_vhpi. libstdc++/libgcc are
# folded in for portability. Output: $(DIST)/
CXX ?= c++
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LIBPRE     := lib
  LIBEXT     := dylib
  DIST_FLAGS := -dynamiclib
  DIST_LIBS  :=
else ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
  LIBPRE     :=
  LIBEXT     := dll
  DIST_FLAGS := -shared -static
  DIST_LIBS  := -lws2_32
else
  LIBPRE     := lib
  LIBEXT     := so
  DIST_FLAGS := -shared -static-libstdc++ -static-libgcc
  DIST_LIBS  :=
endif
DPI_LIB  := $(LIBPRE)vga_monitor_dpi.$(LIBEXT)
VHPI_LIB := $(LIBPRE)vga_monitor_vhpi.$(LIBEXT)
DIST_ABS := $(abspath $(DIST))
RPATH    := -Wl,-rpath,$(DIST_ABS)
ifeq ($(UNAME_S),Darwin)
  # Make the dylibs relocatable so consumers can find them via -rpath.
  INST_DPI  := -Wl,-install_name,@rpath/$(DPI_LIB)
  INST_VHPI := -Wl,-install_name,@rpath/$(VHPI_LIB)
endif

dist:
	mkdir -p $(DIST)
	$(CXX) -O2 -fPIC $(DIST_FLAGS) $(INST_DPI)  $(BACKEND) $(DIST_LIBS) -o $(DIST)/$(DPI_LIB)
	$(CXX) -O2 -fPIC $(DIST_FLAGS) $(INST_VHPI) $(BACKEND) vhdl/vga_monitor_vhpi.cpp $(DIST_LIBS) -o $(DIST)/$(VHPI_LIB)
	cp sv/vga_monitor.sv vhdl/vga_monitor.vhdl vhdl/vga_monitor_pkg.vhdl \
	   v/vga_monitor.v $(DIST)/
	@echo "built $(DIST)/$(DPI_LIB) and $(DIST)/$(VHPI_LIB)"

# Icarus VPI module -- needs iverilog's headers, so it's split from `dist`.
dist-vpi:
	mkdir -p $(DIST)
	g++ -O2 -fPIC -shared -I$(IVL_INC) -Wl,--wrap=__isoc23_strtol \
		v/vga_monitor_vpi.cpp v/glibc_compat.c $(BACKEND) \
		-o $(DIST)/vga_monitor.vpi

# ---- Artifact tests: drive the design against the PREBUILT libs in $(DIST) --
# Same fixtures as the stream-* targets, but the backend is NOT recompiled --
# each simulator links/loads the already-built artifact (as a downstream user
# would). The harness (tests/stream_e2e.py --dist) calls these per simulator.
stream-dpi-dist:
	mkdir -p $(BUILD)/stream-dpi-dist
	verilator --cc --exe --build -j 0 -Wno-WIDTH -Wno-CASEINCOMPLETE --timescale 1ns/1ps \
		--top-module tb_stream --Mdir $(BUILD)/stream-dpi-dist -o tb_stream \
		examples/tb_stream.sv examples/vga_signal_generator.sv examples/gradient_source.sv \
		sv/vga_monitor.sv examples/sim_main_stream.cpp \
		-LDFLAGS "-L$(DIST_ABS) -lvga_monitor_dpi $(RPATH)"
	VGA_MONITOR_STREAM=$(STREAM) ./$(BUILD)/stream-dpi-dist/tb_stream

stream-vhpi-dist:
	mkdir -p $(BUILD)/stream-vhpi-dist
	ghdl -a --std=08 --workdir=$(BUILD)/stream-vhpi-dist \
		vhdl/vga_monitor_pkg.vhdl vhdl/vga_monitor.vhdl $(VHDL_DUT) examples/tb_gradient.vhdl
	ghdl -e --std=08 --workdir=$(BUILD)/stream-vhpi-dist -o $(BUILD)/stream-vhpi-dist/$(MODULE) \
		-Wl,-L$(DIST_ABS) -Wl,-lvga_monitor_vhpi -Wl,-Wl,-rpath,$(DIST_ABS) $(MODULE)
	VGA_MONITOR_STREAM=$(STREAM) ./$(BUILD)/stream-vhpi-dist/$(MODULE)

stream-nvc-dist:
	mkdir -p $(BUILD)/stream-nvc-dist
	nvc --std=2008 --work=$(BUILD)/stream-nvc-dist/work -a \
		vhdl/vga_monitor_pkg.vhdl vhdl/vga_monitor.vhdl $(VHDL_DUT) examples/tb_gradient.vhdl
	nvc --std=2008 --work=$(BUILD)/stream-nvc-dist/work -e $(MODULE)
	VGA_MONITOR_STREAM=$(STREAM) \
		nvc --std=2008 --work=$(BUILD)/stream-nvc-dist/work -r $(MODULE) --load $(DIST_ABS)/$(VHPI_LIB)

stream-vpi-dist:
	mkdir -p $(BUILD)/stream-vpi-dist
	iverilog -g2012 -o $(BUILD)/stream-vpi-dist/$(MODULE).vvp -s $(MODULE) \
		examples/$(MODULE).v v/vga_monitor.v $(SV_DUT)
	VGA_MONITOR_STREAM=$(STREAM) \
		$(VVP_RUN) -M$(DIST_ABS) -m vga_monitor $(BUILD)/stream-vpi-dist/$(MODULE).vvp

# ---- VHPIDIRECT / NVC (build-from-source twin of stream-vhpi) ---------------
stream-nvc:
	mkdir -p $(BUILD)/stream-nvc
	$(CXX) -O2 -fPIC -shared $(BACKEND) vhdl/vga_monitor_vhpi.cpp -lstdc++ \
		-o $(BUILD)/stream-nvc/$(VHPI_LIB)
	nvc --std=2008 --work=$(BUILD)/stream-nvc/work -a \
		vhdl/vga_monitor_pkg.vhdl vhdl/vga_monitor.vhdl $(VHDL_DUT) examples/tb_gradient.vhdl
	nvc --std=2008 --work=$(BUILD)/stream-nvc/work -e $(MODULE)
	VGA_MONITOR_STREAM=$(STREAM) \
		nvc --std=2008 --work=$(BUILD)/stream-nvc/work -r $(MODULE) --load $(BUILD)/stream-nvc/$(VHPI_LIB)

clean:
	rm -rf $(BUILD) obj_dir
