/*
 *  PicoSoC - A simple example SoC using PicoRV32
 *
 *  Copyright (C) 2017  Claire Xenia Wolf <claire@yosyshq.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

`timescale 1 ns / 1 ps

module testbench;
	reg clk;
	always #5 clk = (clk === 1'b0);

	localparam ser_half_period = 53;
	event ser_sample;

	initial begin
		$dumpfile("testbench.vcd");
		$dumpvars(0, testbench);

		repeat ($test$plusargs("stim") ? 120 : 6) begin
			repeat (50000) @(posedge clk);
			$display("+50000 cycles");
		end
		$finish;
	end

	integer cycle_cnt = 0;

	always @(posedge clk) begin
		cycle_cnt <= cycle_cnt + 1;
	end

	// Trap detector + bus trace. A CPU trap (illegal instr / misalign) is the
	// firmware dying; without this it just looks like a silent UART freeze.
	// The trap line is always on; the per-transfer trace is opt-in: vvp ... +trace
	reg trace_en;
	reg trap_seen = 0;
	initial trace_en = $test$plusargs("trace");
	always @(posedge clk) begin
		if (trace_en && uut.soc.mem_valid && uut.soc.mem_ready)
			$display("XFER c=%0d addr=%h instr=%b rdata=%h", cycle_cnt,
				uut.soc.mem_addr, uut.soc.mem_instr, uut.soc.mem_rdata);
		if (uut.soc.cpu.trap && !trap_seen) begin
			trap_seen <= 1;
			$display("*** CPU TRAP at pc=%h (cycle %0d) ***", uut.soc.cpu.reg_pc, cycle_cnt);
		end
	end

	wire led1, led2, led3, led4, led5;
	wire ledr_n, ledg_n;

	wire [6:0] leds = {!ledg_n, !ledr_n, led5, led4, led3, led2, led1};

	reg  ser_rx = 1'b1;   // driven by the +stim uart_send task below
	wire ser_tx;

	wire flash_csb;
	wire flash_clk;
	wire flash_io0;
	wire flash_io1;
	wire flash_io2;
	wire flash_io3;

	always @(leds) begin
		#1 $display("%b", leds);
	end

	icebreaker #(
		// 8192 words = 32 KB: fits the firmware's ~10 KB .bss (benchmark arrays)
		// + stack. The old 256 (1 KB) only covered the boot path -- any workload
		// touching the big arrays mapped outside SPRAM into the flash/cache range.
		.MEM_WORDS(8192)
	) uut (
		.clk      (clk      ),
		.led1     (led1     ),
		.led2     (led2     ),
		.led3     (led3     ),
		.led4     (led4     ),
		.led5     (led5     ),
		.ledr_n   (ledr_n   ),
		.ledg_n   (ledg_n   ),
		.ser_rx   (ser_rx   ),
		.ser_tx   (ser_tx   ),
		.flash_csb(flash_csb),
		.flash_clk(flash_clk),
		.flash_io0(flash_io0),
		.flash_io1(flash_io1),
		.flash_io2(flash_io2),
		.flash_io3(flash_io3)
	);

	spiflash spiflash (
		.csb(flash_csb),
		.clk(flash_clk),
		.io0(flash_io0),
		.io1(flash_io1),
		.io2(flash_io2),
		.io3(flash_io3)
	);

	reg [7:0] buffer;

	always begin
		@(negedge ser_tx);

		repeat (ser_half_period) @(posedge clk);
		-> ser_sample; // start bit

		repeat (8) begin
			repeat (ser_half_period) @(posedge clk);
			repeat (ser_half_period) @(posedge clk);
			buffer = {ser_tx, buffer[7:1]};
			-> ser_sample; // data bit
		end

		repeat (ser_half_period) @(posedge clk);
		repeat (ser_half_period) @(posedge clk);
		-> ser_sample; // stop bit

		if (buffer < 32 || buffer >= 127)
			$display("Serial data: %d", buffer);
		else
			$display("Serial data: '%c'", buffer);
	end

	// RX stimulus (opt-in: vvp ... +stim). Drives ser_rx to exercise the
	// post-"Press ENTER" path -- banner, menu, commands -- which has no sim
	// coverage otherwise (the firmware blocks forever on getchar()).
	task uart_send(input [7:0] b);
		integer i;
		begin
			ser_rx = 0; repeat (105) @(posedge clk);          // start bit
			for (i = 0; i < 8; i = i + 1) begin
				ser_rx = b[i]; repeat (105) @(posedge clk);    // data bits, LSB first
			end
			ser_rx = 1; repeat (105) @(posedge clk);          // stop bit
		end
	endtask

	initial begin
		ser_rx = 1;
		if ($test$plusargs("stim")) begin
			repeat (400000) @(posedge clk);
			$display(">>> stim: ENTER");  uart_send(8'h0D);
			repeat (300000) @(posedge clk);
			$display(">>> stim: 'B' (run scope -> matmul workload)");
			uart_send("B");
			// run_scope loops forever; the trap detector catches any death
		end
	end
endmodule
