// Unit testbench for icache.v -- verifies the cache in isolation against a
// golden flash model. Invariant: every word returned for address A must equal
// golden(A), no matter whether it was a hit, miss, or eviction.
//
// Override params at compile time, e.g.:
//   iverilog -Picache_tb.WAYS=2 -Picache_tb.WORDS=4 -o tb icache_tb.v icache.v
`timescale 1ns/1ps

module icache_tb;
	parameter integer SETS    = 256;    // small for fast sim (still exercises eviction)
	parameter integer WAYS    = 2;
	parameter integer WORDS   = 4;
	parameter integer LATENCY = 4;     // flash response latency (cycles)

	reg clk = 0, resetn = 0;
	always #5 clk = ~clk;              // 100 MHz

	reg         cpu_valid = 0;
	reg  [23:0] cpu_addr  = 0;
	wire        cpu_ready;
	wire [31:0] cpu_rdata;

	wire        spi_valid;
	wire [23:0] spi_addr;
	reg         spi_ready = 0;
	wire [31:0] spi_rdata;

	// golden flash contents: a bijection so every address has a unique word
	function [31:0] golden(input [23:0] a);
		golden = (a * 32'h9E3779B1) ^ 32'h5A5A5A5A;
	endfunction

	// flash returns golden(addr) combinationally; ready is latency-gated
	assign spi_rdata = golden(spi_addr);

	integer lat = 0;
	always @(posedge clk) begin
		spi_ready <= 0;
		if (!resetn) begin
			lat <= 0;
		end else if (spi_valid) begin
			if (lat == LATENCY) begin
				spi_ready <= 1;
				lat <= 0;
			end else begin
				lat <= lat + 1;
			end
		end else begin
			lat <= 0;
		end
	end

	icache #(.SETS(SETS), .WAYS(WAYS), .WORDS_PER_LINE(WORDS)) dut (
		.clk(clk), .resetn(resetn),
		.cpu_valid(cpu_valid), .cpu_addr(cpu_addr),
		.cpu_ready(cpu_ready), .cpu_rdata(cpu_rdata),
		.spi_valid(spi_valid), .spi_addr(spi_addr),
		.spi_ready(spi_ready), .spi_rdata(spi_rdata)
	);

	integer errors = 0, reads = 0;

	// one CPU fetch: drive addr, wait for ready, check against golden
	task do_read(input [23:0] a);
		begin
			cpu_addr  = a;
			cpu_valid = 1;
			@(posedge clk);
			while (!cpu_ready) @(posedge clk);
			reads = reads + 1;
			if (cpu_rdata !== golden(a)) begin
				errors = errors + 1;
				$display("  FAIL  addr=%06h  got=%08h  exp=%08h", a, cpu_rdata, golden(a));
			end
			cpu_valid = 0;
			@(posedge clk);
		end
	endtask

	// address helpers
	localparam integer LINE_WORDS = WORDS;
	localparam integer WAY_STRIDE = SETS * WORDS * 4;   // addrs this far apart share an index

	integer i, j;
	reg [23:0] a;

	initial begin
		$dumpfile("icache_tb.vcd");
		$dumpvars(0, icache_tb);

		// reset
		resetn = 0;
		repeat (4) @(posedge clk);
		resetn = 1;
		repeat (4) @(posedge clk);

		$display("icache_tb: SETS=%0d WAYS=%0d WORDS=%0d", SETS, WAYS, WORDS);

		// 1) cold sequential walk, then re-read (hits)
		for (i = 0; i < 8*LINE_WORDS; i = i + 1) do_read(i*4);
		for (i = 0; i < 8*LINE_WORDS; i = i + 1) do_read(i*4);

		// 2) multi-word: miss word0 of a line, then the rest should be hits & correct
		a = 24'h001000;
		for (j = 0; j < LINE_WORDS; j = j + 1) do_read(a + j*4);

		// 3) conflict/eviction: many distinct tags that map to the SAME index
		for (i = 0; i < WAYS + 4; i = i + 1)
			do_read(24'h002000 + i*WAY_STRIDE);
		// re-read them; data must still be correct whether hit or refetched
		for (i = 0; i < WAYS + 4; i = i + 1)
			do_read(24'h002000 + i*WAY_STRIDE);

		// 4) random stress within a bounded range (forces reuse + eviction)
		for (i = 0; i < 4000; i = i + 1) begin
			a = ($random) & 24'h003FFC;     // word-aligned, within 16 KB
			do_read(a);
		end

		$display("icache_tb: %0d reads, %0d errors -> %s",
		         reads, errors, (errors == 0) ? "PASS" : "FAIL");
		$finish;
	end

	// watchdog: catch a hang (e.g. a stuck FSM that never asserts cpu_ready)
	initial begin
		#5000000;
		$display("icache_tb: TIMEOUT (hang) -> FAIL");
		$finish;
	end
endmodule
