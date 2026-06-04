/*
 *  Focused testbench for Step 1a-ii (LOOKAHEAD_HIT) -- see README-GB3.md.
 *
 *  Runs the SoC with the cache's 1-cycle-hit path on and measures, for every
 *  cache transaction, the cpu_valid->cpu_ready latency. A transaction that never
 *  asserts spi_valid is a HIT; with LOOKAHEAD_HIT it must ack in 1 cycle (vs 2 for
 *  the baseline cache). The boot+banner running trap-silent is the end-to-end
 *  no-false-hit proof (a wrong word served from the cache would corrupt execution).
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

	// --- cache hit-latency checker -----------------------------------------
	// shorthand for the cache instance
	`define CACHE uut.soc.icache_gen.icache0
	reg     in_req        = 0;   // a cache transaction is in flight
	reg     req_miss      = 0;   // spi_valid seen during it -> it's a miss
	integer req_start     = 0;
	integer hits          = 0;
	integer misses        = 0;
	integer max_hit_lat   = 0;
	integer min_hit_lat   = 999;
	reg     hit_lat_bad   = 0;   // any hit took != 1 cycle

	always @(posedge clk) cycle_cnt <= cycle_cnt + 1;

	always @(posedge clk) begin
		if (uut.soc.resetn) begin
			// accept: first cycle cpu_valid is asserted and not already acking
			if (`CACHE.cpu_valid && !`CACHE.cpu_ready && !in_req) begin
				in_req    <= 1;
				req_miss  <= 0;
				req_start <= cycle_cnt;
			end
			if (in_req && `CACHE.spi_valid) req_miss <= 1;
			if (in_req && `CACHE.cpu_ready) begin
				if (!req_miss) begin            // a HIT
					hits = hits + 1;
					if ((cycle_cnt - req_start) > max_hit_lat) max_hit_lat = cycle_cnt - req_start;
					if ((cycle_cnt - req_start) < min_hit_lat) min_hit_lat = cycle_cnt - req_start;
					if ((cycle_cnt - req_start) != 1) begin
						hit_lat_bad <= 1;
						$display("*** HIT LATENCY %0d (expected 1) at cycle %0d", cycle_cnt - req_start, cycle_cnt);
					end
				end else misses = misses + 1;
				in_req <= 0;
			end
		end
	end

	always @(posedge clk)
		if (`CACHE.resetn === 1'b1 && uut.soc.cpu.trap && !trap_seen) begin
			trap_seen <= 1;
			$display("*** CPU TRAP at pc=%h (cycle %0d) ***", uut.soc.cpu.reg_pc, cycle_cnt);
		end

	initial begin
		$dumpfile("cache_la_tb.vcd");
		$dumpvars(0, testbench);
		repeat (700000) @(posedge clk);
		if (trap_seen)
			$display("*** SIM FAIL: CPU trapped");
		else if (uart_bytes == 0)
			$display("*** SIM FAIL: no UART output -- CPU never ran");
		else if (hits == 0)
			$display("*** SIM FAIL: no cache hits observed -- nothing to verify");
		else if (hit_lat_bad)
			$display("*** SIM FAIL: a cache hit did not ack in 1 cycle (max=%0d)", max_hit_lat);
		else
			$display("*** SIM PASS: %0d serial bytes, %0d hits (lat %0d..%0d), %0d misses, no trap",
				uart_bytes, hits, min_hit_lat, max_hit_lat, misses);
		$finish;
	end

	wire led1, led2, led3, led4, led5, ledr_n, ledg_n;
	reg  ser_rx = 1'b1;
	wire ser_tx;
	wire flash_csb, flash_clk, flash_io0, flash_io1, flash_io2, flash_io3;

	icebreaker #(
		.MEM_WORDS(8192),
		.LOOKAHEAD_HIT(1)           // <-- the path under test
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

	// Boot + a couple menu commands to exercise plenty of cache hits.
	initial begin
		ser_rx = 1;
		repeat (400000) @(posedge clk); $display(">>> stim: ENTER"); uart_send(8'h0D);
		repeat (150000) @(posedge clk); $display(">>> stim: 'S'");   uart_send("S");
	end
endmodule
