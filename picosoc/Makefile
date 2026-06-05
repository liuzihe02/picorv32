
#as in GB3 ipynb, modify this line to where the GNU toolchain is installed
#on eitl PCs this is `CROSS=/usr/local/apps/GB3-RISCV/riscv32imc/bin/riscv32-unknown-elf-`
# for mac, set environment variable `export RISCV_GNU_TOOLCHAIN_INSTALL_PREFIX=/opt/homebrew/opt/riscv-gnu-toolchain/main` and then use `$(RISCV_GNU_TOOLCHAIN_INSTALL_PREFIX)/bin/riscv32-unknown-elf-` in the Makefile as shown below
# for zach's wsl just run the makefile with 
# make icebprog CROSS=/opt/riscv32imc/bin/riscv32-unknown-elf-
CROSS=$(RISCV_GNU_TOOLCHAIN_INSTALL_PREFIX)/bin/riscv32-unknown-elf-

CFLAGS=

# ---- Hacky fix: System clock (single source of truth: benchmarks.h F_CLK_HZ) ----
# sed F_CLK_HZ/BAUD out of benchmarks.h and bridge them to the tools that can't read a C header:
# the sim tb (-D) and nextpnr/icetime (MHz). Fail loudly if benchmarks.h is missing them
F_CLK_HZ := $(shell sed -n 's/^#define F_CLK_HZ[[:space:]]*\([0-9]*\).*/\1/p' benchmarks.h)
BAUD     := $(shell sed -n 's/^#define BAUD[[:space:]]*\([0-9]*\).*/\1/p' benchmarks.h)
ifeq ($(F_CLK_HZ),)
$(error F_CLK_HZ not found in benchmarks.h -- expected a line like '#define F_CLK_HZ 17250000u')
endif
ifeq ($(BAUD),)
$(error BAUD not found in benchmarks.h -- expected a line like '#define BAUD 115200u')
endif
CLK_DEFS := -DF_CLK_HZ=$(F_CLK_HZ) -DBAUD=$(BAUD)
CLK_MHZ  := $(shell awk 'BEGIN{printf "%g", $(F_CLK_HZ)/1000000}')

# ---- iCE40 HX8K Breakout Board ----

hx8ksim: hx8kdemo_tb.vvp hx8kdemo_fw.hex
	vvp -N $< +firmware=hx8kdemo_fw.hex

hx8ksynsim: hx8kdemo_syn_tb.vvp hx8kdemo_fw.hex
	vvp -N $< +firmware=hx8kdemo_fw.hex

hx8kdemo.json: hx8kdemo.v spimemio.v simpleuart.v picosoc.v ../picorv32.v
	yosys -ql hx8kdemo.log -p 'synth_ice40 -top hx8kdemo -json hx8kdemo.json' $^

hx8kdemo_tb.vvp: hx8kdemo_tb.v hx8kdemo.v spimemio.v simpleuart.v picosoc.v ../picorv32.v spiflash.v
	iverilog -DNO_ICE40_DEFAULT_ASSIGNMENTS -s testbench -o $@ $^ `yosys-config --datdir/ice40/cells_sim.v`

hx8kdemo_syn_tb.vvp: hx8kdemo_tb.v hx8kdemo_syn.v spiflash.v
	iverilog -DNO_ICE40_DEFAULT_ASSIGNMENTS -s testbench -o $@ $^ `yosys-config --datdir/ice40/cells_sim.v`

hx8kdemo_syn.v: hx8kdemo.json
	yosys -p 'read_json hx8kdemo.json; write_verilog hx8kdemo_syn.v'

hx8kdemo.asc: hx8kdemo.pcf hx8kdemo.json
	nextpnr-ice40 --hx8k --package ct256 --asc hx8kdemo.asc --json hx8kdemo.json --pcf hx8kdemo.pcf

hx8kdemo.bin: hx8kdemo.asc
	icetime -d hx8k -c 12 -mtr hx8kdemo.rpt hx8kdemo.asc
	icepack hx8kdemo.asc hx8kdemo.bin

hx8kprog: hx8kdemo.bin hx8kdemo_fw.bin
	iceprog hx8kdemo.bin
	iceprog -o 1M hx8kdemo_fw.bin

hx8kprog_fw: hx8kdemo_fw.bin
	iceprog -o 1M hx8kdemo_fw.bin

hx8kdemo_sections.lds: sections.lds
	$(CROSS)cpp -P -DHX8KDEMO -o $@ $^

hx8kdemo_fw.elf: hx8kdemo_sections.lds start.s firmware.c
	$(CROSS)gcc $(CFLAGS) -DHX8KDEMO -mabi=ilp32 -march=rv32imc -Wl,--build-id=none,-Bstatic,-T,hx8kdemo_sections.lds,--strip-debug -ffreestanding -nostdlib -o hx8kdemo_fw.elf start.s firmware.c

hx8kdemo_fw.hex: hx8kdemo_fw.elf
	$(CROSS)objcopy -O verilog hx8kdemo_fw.elf hx8kdemo_fw.hex

hx8kdemo_fw.bin: hx8kdemo_fw.elf
	$(CROSS)objcopy -O binary hx8kdemo_fw.elf hx8kdemo_fw.bin

# ---- iCE40 IceBreaker Board ----

icebsim: icebreaker_tb.vvp icebreaker_fw.hex
	vvp -N $< +firmware=icebreaker_fw.hex

icebsynsim: icebreaker_syn_tb.vvp icebreaker_fw.hex
	vvp -N $< +firmware=icebreaker_fw.hex

icebreaker.json: icebreaker.v ice40up5k_spram.v spimemio.v simpleuart.v picosoc.v icache.v ../picorv32.v
	yosys -ql icebreaker.log -p 'synth_ice40 -dsp -top icebreaker -json icebreaker.json' $^

icebreaker_tb.vvp: icebreaker_tb.v icebreaker.v ice40up5k_spram.v spimemio.v simpleuart.v picosoc.v icache.v ../picorv32.v spiflash.v
	iverilog -DNO_ICE40_DEFAULT_ASSIGNMENTS -DSIM $(CLK_DEFS) -s testbench -o $@ $^ `yosys-config --datdir/ice40/cells_sim.v`

icebreaker_syn_tb.vvp: icebreaker_tb.v icebreaker_syn.v spiflash.v
	iverilog -DNO_ICE40_DEFAULT_ASSIGNMENTS -s testbench -o $@ $^ `yosys-config --datdir/ice40/cells_sim.v`

icebreaker_syn.v: icebreaker.json
	yosys -p 'read_json icebreaker.json; write_verilog icebreaker_syn.v'

icebreaker.asc: icebreaker.pcf icebreaker.json
	nextpnr-ice40 --freq $(CLK_MHZ) --up5k --package sg48 --asc icebreaker.asc --pcf icebreaker.pcf --json icebreaker.json

icebreaker.bin: icebreaker.asc
	icetime -d up5k -c $(CLK_MHZ) -mtr icebreaker.rpt icebreaker.asc
	icepack icebreaker.asc icebreaker.bin

icebprog: icebreaker.bin icebreaker_fw.bin
	iceprog icebreaker.bin
	iceprog -o 1M icebreaker_fw.bin

icebprog_fw: icebreaker_fw.bin
	iceprog -o 1M icebreaker_fw.bin

icebreaker_sections.lds: sections.lds
	$(CROSS)cpp -P -DICEBREAKER -o $@ $^

#compile firmware with rv32im (not rv32ic) as required
icebreaker_fw.elf: icebreaker_sections.lds start.s firmware.c benchmarks.c
	$(CROSS)gcc $(CFLAGS) -DICEBREAKER -mabi=ilp32 -march=rv32im -Wl,-Bstatic,-T,icebreaker_sections.lds,--strip-debug -ffreestanding -nostdlib -o icebreaker_fw.elf start.s firmware.c benchmarks.c

icebreaker_fw.hex: icebreaker_fw.elf
	$(CROSS)objcopy -O verilog icebreaker_fw.elf icebreaker_fw.hex

icebreaker_fw.bin: icebreaker_fw.elf
	$(CROSS)objcopy -O binary icebreaker_fw.elf icebreaker_fw.bin

# ---- Instruction-cache unit test ----

icache_tb: icache_tb.vvp
	vvp -N icache_tb.vvp

icache_tb.vvp: icache_tb.v icache.v
	iverilog -g2012 -s icache_tb -o $@ $^

# ---- Testbench for SPI Flash Model ----

spiflash_tb: spiflash_tb.vvp icebreaker_fw.hex
	vvp -N $< +firmware=icebreaker_fw.hex

spiflash_tb.vvp: spiflash.v spiflash_tb.v
	iverilog -s testbench -o $@ $^

# ---- ASIC Synthesis Tests ----

cmos.log: spimemio.v simpleuart.v picosoc.v ../picorv32.v
	yosys -l cmos.log -p 'synth -top picosoc; abc -g cmos2; opt -fast; stat' $^

# ---- Clean ----

clean:
	rm -f testbench.vvp testbench.vcd spiflash_tb.vvp spiflash_tb.vcd
	rm -f hx8kdemo_fw.elf hx8kdemo_fw.hex hx8kdemo_fw.bin cmos.log
	rm -f icebreaker_fw.elf icebreaker_fw.hex icebreaker_fw.bin
	rm -f hx8kdemo.json hx8kdemo.log hx8kdemo.asc hx8kdemo.rpt hx8kdemo.bin
	rm -f hx8kdemo_syn.v hx8kdemo_syn_tb.vvp hx8kdemo_tb.vvp
	rm -f icebreaker.json icebreaker.log icebreaker.asc icebreaker.rpt icebreaker.bin
	rm -f icebreaker_syn.v icebreaker_syn_tb.vvp icebreaker_tb.vvp
	rm -f icache_tb.vvp icache_tb.vcd

.PHONY: spiflash_tb clean icache_tb
.PHONY: hx8kprog hx8kprog_fw hx8ksim hx8ksynsim
.PHONY: icebprog icebprog_fw icebsim icebsynsim
