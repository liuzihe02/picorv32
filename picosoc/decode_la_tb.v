/*
 *  Focused testbench for Step 1a-i (LOOKAHEAD_DECODE) -- see README-GB3.md "Critical Path".
 *
 *  Drives the SoC through the icebreaker top, so it verifies whatever LOOKAHEAD_DECODE
 *  mode is compiled into icebreaker.v (the literal passed to picosoc) -- i.e. the exact
 *  bitstream you flash. With decode on, it continuously checks the load-bearing
 *  assumption that makes the look-ahead pre-decode correct: the region-select
 *  registered one cycle early off mem_la_addr must, in every mem_valid cycle, equal
 *  the TRUE combinational decode of the address the CPU actually presents. A mismatch
 *  means mem_la_* did NOT lead mem_valid by exactly one cycle (or the region
 *  overlap/priority was reproduced wrong) -> the transaction would route to the wrong
 *  responder. Plus the usual trap detector and a +stim menu drive to exercise the
 *  cfgreg / UART / GPIO decode that 1a-i reworks.
 */

`timescale 1 ns / 1 ps

`ifndef F_CLK_HZ
`define F_CLK_HZ 17250000
`endif
`ifndef BAUD
`define BAUD 115200
`endif

module testbench;
	reg clk;
	always #5 clk = (clk === 1'b0);

	localparam integer uart_div        = (`F_CLK_HZ + `BAUD/2) / `BAUD;
	localparam integer uart_bit        = uart_div + 1;
	localparam integer ser_half_period = uart_bit / 2;

	integer uart_bytes = 0;
	reg     trap_seen  = 0;
	integer cycle_cnt  = 0;

	// --- look-ahead decode equivalence checker -----------------------------
	integer la_checks = 0;   // mem_valid cycles examined
	reg     la_fail   = 0;   // any registered select != true decode of mem_addr

	always @(posedge clk) cycle_cnt <= cycle_cnt + 1;

	// run length: long enough to boot, print the banner+menu, and run the +stim
	// commands below. Bounded (not forever) so the checker can render a verdict.
	initial begin
		$dumpfile("decode_la_tb.vcd");
		$dumpvars(0, testbench);
		repeat (950000) @(posedge clk);
		if (trap_seen)
			$display("*** SIM FAIL: CPU trapped");
		else if (la_fail)
			$display("*** SIM FAIL: look-ahead decode mismatch (after %0d checks)", la_checks);
		else if (uart_bytes == 0)
			$display("*** SIM FAIL: no UART output -- CPU never ran");
		else
			$display("*** SIM PASS: %0d serial bytes, %0d look-ahead decode checks all matched, no trap",
				uart_bytes, la_checks);
		$finish;
	end

	// Trap detector
	always @(posedge clk) begin
		if (uut.soc.cpu.trap && !trap_seen) begin
			trap_seen <= 1;
			$display("*** CPU TRAP at pc=%h (cycle %0d) ***", uut.soc.cpu.reg_pc, cycle_cnt);
		end
	end

	// The actual test: every cycle the CPU presents a transaction, the look-ahead
	// region-select (registered off mem_la_addr a cycle earlier) must equal the
	// combinational decode of the address presented NOW.
	always @(posedge clk) if (uut.soc.resetn && uut.soc.mem_valid) begin
		la_checks = la_checks + 1;
		if (uut.soc.iomem_sel !== (uut.soc.mem_addr[31:24] > 8'h01)) begin
			la_fail = 1;
			$display("*** LA MISMATCH iomem c=%0d addr=%h sel=%b exp=%b",
				cycle_cnt, uut.soc.mem_addr, uut.soc.iomem_sel, (uut.soc.mem_addr[31:24] > 8'h01));
		end
		if (uut.soc.spimemio_cfgreg_sel !== (uut.soc.mem_addr == 32'h0200_0000)) begin
			la_fail = 1;
			$display("*** LA MISMATCH cfg   c=%0d addr=%h sel=%b",
				cycle_cnt, uut.soc.mem_addr, uut.soc.spimemio_cfgreg_sel);
		end
		if (uut.soc.simpleuart_reg_div_sel !== (uut.soc.mem_addr == 32'h0200_0004)) begin
			la_fail = 1;
			$display("*** LA MISMATCH div   c=%0d addr=%h sel=%b",
				cycle_cnt, uut.soc.mem_addr, uut.soc.simpleuart_reg_div_sel);
		end
		if (uut.soc.simpleuart_reg_dat_sel !== (uut.soc.mem_addr == 32'h0200_0008)) begin
			la_fail = 1;
			$display("*** LA MISMATCH dat   c=%0d addr=%h sel=%b",
				cycle_cnt, uut.soc.mem_addr, uut.soc.simpleuart_reg_dat_sel);
		end
	end

	wire led1, led2, led3, led4, led5, ledr_n, ledg_n;
	reg  ser_rx = 1'b1;
	wire ser_tx;
	wire flash_csb, flash_clk, flash_io0, flash_io1, flash_io2, flash_io3;

	icebreaker #(
		.MEM_WORDS(8192)        // LOOKAHEAD_DECODE is baked into icebreaker.v (literal -> picosoc)
	) uut (
		.clk_in   (clk      ),
		.led1     (led1     ), .led2 (led2), .led3 (led3), .led4 (led4), .led5 (led5),
		.ledr_n   (ledr_n   ), .ledg_n(ledg_n),
		.ser_rx   (ser_rx   ), .ser_tx(ser_tx),
		.flash_csb(flash_csb), .flash_clk(flash_clk),
		.flash_io0(flash_io0), .flash_io1(flash_io1),
		.flash_io2(flash_io2), .flash_io3(flash_io3)
	);

	spiflash spiflash (
		.csb(flash_csb), .clk(flash_clk),
		.io0(flash_io0), .io1(flash_io1), .io2(flash_io2), .io3(flash_io3)
	);

	// UART TX capture
	reg [7:0] buffer;
	always begin
		@(negedge ser_tx);
		repeat (ser_half_period) @(posedge clk);
		repeat (8) begin
			repeat (ser_half_period) @(posedge clk);
			repeat (ser_half_period) @(posedge clk);
			buffer = {ser_tx, buffer[7:1]};
		end
		repeat (ser_half_period) @(posedge clk);
		repeat (ser_half_period) @(posedge clk);
		uart_bytes = uart_bytes + 1;
		if (buffer < 32 || buffer >= 127) $display("Serial data: %0d", buffer);
		else                              $display("Serial data: '%c'", buffer);
	end

	task uart_send(input [7:0] b);
		integer i;
		begin
			ser_rx = 0; repeat (uart_bit) @(posedge clk);
			for (i = 0; i < 8; i = i + 1) begin
				ser_rx = b[i]; repeat (uart_bit) @(posedge clk);
			end
			ser_rx = 1; repeat (uart_bit) @(posedge clk);
		end
	endtask

	// Drive the menu to exercise cfgreg (S = print SPI state) and the UART RX/echo
	// path (e..! ) -- the peripheral decode 1a-i reworks. (Avoids the long matmul.)
	initial begin
		ser_rx = 1;
		repeat (400000) @(posedge clk); $display(">>> stim: ENTER"); uart_send(8'h0D);
		repeat (150000) @(posedge clk); $display(">>> stim: 'S'");   uart_send("S");
		repeat (150000) @(posedge clk); $display(">>> stim: 'e'");   uart_send("e");
		repeat ( 80000) @(posedge clk); $display(">>> stim: 'x'");   uart_send("x");
		repeat ( 80000) @(posedge clk); $display(">>> stim: '!'");   uart_send("!");
	end
endmodule
