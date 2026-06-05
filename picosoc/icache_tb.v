// Unit testbench for icache.v -- verifies the cache in isolation against a golden
// flash model. Core invariant: every word returned for address A must equal
// golden(A), whether it came from a hit, a miss/fill, or a refetch after eviction.
// On top of that it asserts hit-vs-miss behaviour (so a cache that silently never
// hits, or never invalidates, is caught -- data correctness alone would miss both).
//
//   make icache_test          # builds + runs this file against icache.v
//
// The cache is parameterised (SETS/WAYS/WORDS_PER_LINE), and the FILL/handshake
// behaviour differs across geometries -- so the test must cover more than one. All
// of the test logic lives in the parameterised `icache_check` module below; the
// top `icache_tb` instantiates it once per (geometry, latency) point and sums the
// results. The instance list deliberately includes the SoC's exact build geometry,
// plus direct-mapped, the single-word edge, higher associativity, and a big line --
// at varied flash latencies to shake out handshake-alignment races.
`timescale 1ns/1ps

// =====================================================================
// One self-contained check: DUT + streaming flash model + stimulus.
// Reports its result on `errs`/`done`; does not call $finish (the top does).
// =====================================================================
module icache_check #(
	parameter integer SETS       = 64,
	parameter integer WAYS       = 2,
	parameter integer WORDS      = 4,
	parameter integer LATENCY    = 4,    // flash "jump" latency: cold/non-seq cmd+addr+dummy
	parameter integer STREAM_LAT = 1     // flash streaming latency: sequential next word (data only)
) (
	input  wire        clk,
	output reg         done = 0,
	output reg [31:0]  errs = 0
);
	reg         resetn = 0;
	reg         cpu_valid = 0;
	reg  [23:0] cpu_addr  = 0;
	reg         cpu_la_valid = 0;   // look-ahead pre-read strobe (LOOKAHEAD_HIT fast path)
	reg  [23:0] cpu_la_addr  = 0;
	wire        cpu_ready;
	wire [31:0] cpu_rdata;

	wire        spi_valid;
	wire [23:0] spi_addr;
	wire        spi_ready;
	wire [31:0] spi_rdata;

	// golden flash contents: a bijection so every address maps to a unique word
	function [31:0] golden(input [23:0] a);
		golden = (a * 32'h9E3779B1) ^ 32'h5A5A5A5A;
	endfunction

	// ---- streaming flash model (mirrors spimemio's contract) -----------------
	// Critical fidelity: rdata follows the address spimemio CURRENTLY holds (rd_addr),
	// NOT the address the consumer presents, and `ready` is a LEVEL high only while
	// spi_addr == rd_addr. A combinational golden(spi_addr) model hides exactly the
	// fill-streaming bugs this cache had (double-grab / skipped last word).
	//   - cold / non-sequential request (a "jump"): pay LATENCY, then hold word@addr.
	//   - sequential request (addr == rd_addr+4): pay STREAM_LAT, advance one word.
	reg [23:0] rd_addr;
	reg        rd_valid = 0;
	integer    lat = 0;

	wire hold = rd_valid && (spi_addr == rd_addr);
	wire seq  = rd_valid && (spi_addr == rd_addr + 24'd4);

	assign spi_rdata = golden(rd_addr);            // data for the word spimemio holds
	assign spi_ready = spi_valid && hold;          // level: high while held word matches request

	always @(posedge clk) begin
		if (!resetn) begin
			rd_valid <= 1'b0;
			lat      <= 0;
		end else if (!spi_valid) begin
			lat <= 0;                              // idle; retain rd_addr/rd_valid like real spimemio
		end else if (hold) begin
			lat <= 0;                              // consumer acking current word; wait for it to advance
		end else if (seq) begin
			if (lat >= STREAM_LAT) begin
				rd_addr  <= rd_addr + 24'd4;       // stream the next sequential word
				rd_valid <= 1'b1;
				lat      <= 0;
			end else lat <= lat + 1;
		end else begin
			if (lat >= LATENCY) begin              // cold / non-sequential -> full command+addr+dummy
				rd_addr  <= spi_addr;
				rd_valid <= 1'b1;
				lat      <= 0;
			end else lat <= lat + 1;
		end
	end

	// ---- flash-traffic observer ---------------------------------------------
	// One word is delivered per (spi_valid && spi_ready) cycle. flash_words is the
	// running total; the read tasks snapshot it to classify hit (0 new words) vs
	// miss (>=1 new word -- a line fill).
	integer flash_words = 0;
	always @(posedge clk)
		if (resetn && spi_valid && spi_ready)
			flash_words <= flash_words + 1;

	icache #(.SETS(SETS), .WAYS(WAYS), .WORDS_PER_LINE(WORDS)) dut (
		.clk(clk), .resetn(resetn),
		.cpu_valid(cpu_valid), .cpu_addr(cpu_addr),
		.cpu_ready(cpu_ready), .cpu_rdata(cpu_rdata),
		.cpu_la_valid(cpu_la_valid), .cpu_la_addr(cpu_la_addr),
		.spi_valid(spi_valid), .spi_addr(spi_addr),
		.spi_ready(spi_ready), .spi_rdata(spi_rdata)
	);

	integer reads = 0;

	// ---- address helpers -----------------------------------------------------
	// Flash addr layout: { tag | index | offset | byte(2) }.
	// WAY_STRIDE = bytes per (index x line) span; addrs this far apart share an
	// index but differ in tag -> they collide in the cache.
	localparam integer LINE_BYTES = WORDS * 4;
	localparam integer WAY_STRIDE = SETS * WORDS * 4;

	function [23:0] waddr(input [23:0] tag, input [23:0] idx, input [23:0] off);
		waddr = (tag * WAY_STRIDE) + (idx * LINE_BYTES) + (off * 4);
	endfunction

	// ---- timeout-guarded wait for cpu_ready ----------------------------------
	task wait_ready;
		integer g;
		begin
			g = 0;
			while (!cpu_ready) begin
				@(posedge clk);
				g = g + 1;
				if (g > 100000) begin
					$display("  FAIL  hang: cpu_ready never asserted (addr=%06h)", cpu_addr);
					errs = errs + 1;
					done = 1;  // let the top declare failure
				end
			end
		end
	endtask

	// ---- isolated read (cpu_valid drops between reads) -----------------------
	// mode: 0 = expect HIT (no flash words), 1 = expect MISS (>=1), 2 = don't care.
	task do_read(input [23:0] a, input integer mode);
		integer f0;
		begin
			f0 = flash_words;
			cpu_addr  = a;
			cpu_valid = 1;
			@(posedge clk);
			wait_ready;
			reads = reads + 1;
			if (cpu_rdata !== golden(a)) begin
				errs = errs + 1;
				$display("  FAIL  data addr=%06h got=%08h exp=%08h", a, cpu_rdata, golden(a));
			end
			if (mode == 1 && flash_words == f0) begin
				errs = errs + 1;
				$display("  FAIL  expected MISS but no flash fetch (addr=%06h)", a);
			end
			if (mode == 0 && flash_words != f0) begin
				errs = errs + 1;
				$display("  FAIL  expected HIT but fetched %0d words (addr=%06h)", flash_words - f0, a);
			end
			cpu_valid = 0;
			@(posedge clk);
		end
	endtask

	// ---- look-ahead read: stage cpu_la one cycle, then present the matching request ------
	// Drives the LOOKAHEAD_HIT fast path. Returns latency (cpu_valid->cpu_ready) and whether a
	// flash fetch happened (miss); checks data == golden. A staged hit must ack in 1 cycle.
	task do_read_la(input [23:0] a, output integer o_lat, output reg o_missed);
		integer g; reg sawspi;
		begin
			cpu_la_addr = a; cpu_la_valid = 1; cpu_valid = 0;
			@(posedge clk);                       // stage the pre-read
			cpu_la_valid = 0;
			cpu_addr = a; cpu_valid = 1;          // present the request
			o_lat = 0; sawspi = 0; g = 0;
			@(posedge clk); #1;                  // sample after NBAs commit (avoid active-region race)
			while (!cpu_ready && g < 100000) begin
				o_lat = o_lat + 1;
				if (spi_valid) sawspi = 1;
				@(posedge clk); #1;
				g = g + 1;
			end
			o_lat = o_lat + 1;                   // count the cycle cpu_ready asserted
			if (g >= 100000) begin errs = errs + 1; $display("  FAIL la hang addr=%06h", a); end
			reads = reads + 1;
			if (cpu_rdata !== golden(a)) begin
				errs = errs + 1;
				$display("  FAIL la data addr=%06h got=%08h exp=%08h", a, cpu_rdata, golden(a));
			end
			o_missed = sawspi;
			cpu_valid = 0;
			@(posedge clk);
		end
	endtask

	integer i, it, li, off, f0, nhot, idx;
	integer la_lat;
	reg     la_miss;
	reg [23:0] hot [0:63];   // hot-loop working-set addresses
	reg [23:0] a;
	reg [31:0] rng;

	initial begin
		resetn = 0; repeat (4) @(posedge clk);
		resetn = 1; repeat (4) @(posedge clk);

		$display("icache_check: SETS=%0d WAYS=%0d WORDS=%0d LATENCY=%0d STREAM_LAT=%0d",
		         SETS, WAYS, WORDS, LATENCY, STREAM_LAT);

		// 1) post-reset sweep: the very first access MUST miss (valid bits cleared,
		//    nothing cached yet). Catches a broken/absent invalidation sweep.
		do_read(waddr(1, 0, 0), 1);

		// 2) cold sequential walk: first word of each line misses+fills; the other
		//    offsets of that line are then hits. Asserts the multi-word fill landed
		//    every offset correctly (the class of bug that shipped).
		for (i = 0; i < 8; i = i + 1) begin
			do_read(waddr(2, i, 0), 1);                 // line head: miss
			for (off = 1; off < WORDS; off = off + 1)
				do_read(waddr(2, i, off), 0);           // rest of line: hit
		end
		for (i = 0; i < 8; i = i + 1)                   // re-read region: all hits
			for (off = 0; off < WORDS; off = off + 1)
				do_read(waddr(2, i, off), 0);

		// 3) first-touch at a NON-zero offset, then read the other offsets forward
		//    and backward. This is the exact SoC pattern (fetch ...f8 then ...fc):
		//    the line is first entered mid-way and every other offset must hit.
		if (WORDS > 1) begin
			do_read(waddr(3, 5, WORDS-2), 1);           // enter line at second-to-last word
			do_read(waddr(3, 5, WORDS-1), 0);           // forward neighbour (the bug case)
			for (off = WORDS-2; off >= 0; off = off - 1) // walk backward over the rest
				do_read(waddr(3, 5, off), 0);
		end

		// 4) hot loop with cpu_valid HELD high. Drives like picorv32: cpu_addr stays
		//    stable until an ack, then advances in the SAME cycle as the ack -- so
		//    cpu_valid never coincides with a stale address. Exercises the idle-state
		//    `!cpu_ready` lingering-valid guard, which the isolated do_read never hits.
		//    After the warmup pass the set is fully cached, so every later pass must be
		//    100% hit -> ZERO new flash words (guards the steady-state plateau).
		nhot = 0;
		for (li = 0; li < 4 && li < SETS; li = li + 1)
			for (off = 0; off < WORDS; off = off + 1) begin
				hot[nhot] = waddr(7, li, off);
				nhot = nhot + 1;
			end
		idx = 0; it = 0; f0 = flash_words;
		cpu_addr = hot[0];
		cpu_valid = 1;
		while (it < 6) begin
			@(posedge clk);
			if (cpu_ready) begin
				reads = reads + 1;
				if (cpu_rdata !== golden(hot[idx])) begin
					errs = errs + 1;
					$display("  FAIL  data(hot) addr=%06h got=%08h exp=%08h",
					         hot[idx], cpu_rdata, golden(hot[idx]));
				end
				idx = idx + 1;
				if (idx == nhot) begin
					idx = 0; it = it + 1;
					if (it == 1) f0 = flash_words;     // start counting after warmup pass
				end
				cpu_addr = hot[idx];                    // advance on the ack; stable till next ack
			end
		end
		cpu_valid = 0;
		@(posedge clk);
		if (flash_words != f0) begin
			errs = errs + 1;
			$display("  FAIL  hot loop not 100%% hit after warmup: %0d extra fetches",
			         flash_words - f0);
		end

		// 5) conflict / eviction: WAYS+2 distinct tags all mapping to one index.
		//    Order matters (robust for direct-mapped and set-assoc alike): the
		//    LAST-filled tag is still cached (hit), the FIRST-filled was evicted
		//    (miss). Re-reading the first tag would itself evict the last in a
		//    direct-mapped cache, so assert the hit before the miss.
		for (i = 0; i < WAYS + 2; i = i + 1)
			do_read(waddr(8 + i, 9, 0), 2);             // fill/evict (don't-care class)
		do_read(waddr(8 + (WAYS+1), 9, 0), 0);         // most-recently filled -> hit
		do_read(waddr(8 + 0, 9, 0), 1);                // first tag evicted -> miss

		// 6) invalidation across reset: warm a line, pulse reset, re-read -> MUST miss.
		//    Without the post-reset sweep this would falsely hit (stale tag) and, on
		//    real EBR power-up, return garbage (the on-board freeze class).
		do_read(waddr(4, 3, 0), 2);                     // warm
		do_read(waddr(4, 3, 0), 0);                     // confirm cached (hit)
		resetn = 0; repeat (3) @(posedge clk);
		resetn = 1; repeat (4) @(posedge clk);
		do_read(waddr(4, 3, 0), 1);                     // post-reset: must refetch

		// 7) deterministic random stress (xorshift, reproducible): mixes reuse +
		//    eviction across the range. Don't-care on hit/miss; correctness enforced.
		rng = 32'hC0FFEE11;
		for (i = 0; i < 3000; i = i + 1) begin
			rng = rng ^ (rng << 13); rng = rng ^ (rng >> 17); rng = rng ^ (rng << 5);
			a = rng & (WAY_STRIDE*16 - 1) & 24'hFFFFFC;  // word-aligned, bounded -> forces reuse
			do_read(a, 2);
		end

		// 8) LOOKAHEAD fast-path (drive cpu_la one cycle before cpu_valid). Tags >=16 sit
		//    outside the random-stress range (a < WAY_STRIDE*16) and all earlier tests, so
		//    first-touch is a deterministic miss. (Tests 1-7 above run with cpu_la_valid=0,
		//    i.e. they cover the 2-cycle FALLBACK path == the pre-look-ahead baseline.)
		do_read_la(waddr(16, 2, 0), la_lat, la_miss);    // (a) first touch -> staged MISS + fill
		if (!la_miss)
			begin errs = errs + 1; $display("  FAIL la(a) first-touch should miss"); end
		do_read_la(waddr(16, 2, 0), la_lat, la_miss);    // (b) re-read -> 1-cycle HIT, no flash
		if (la_miss || la_lat != 1)
			begin errs = errs + 1; $display("  FAIL la(b) hit lat=%0d miss=%b (want 1,0)", la_lat, la_miss); end

		// (c) no-false-hit: cache X(tag17,idx4); stage la=X; request Y(tag18, SAME index, diff
		//     tag). Must serve golden(Y), never golden(X) -- the compare uses cpu_tag, not req_tag.
		do_read(waddr(17, 4, 0), 2);                     // cache X
		cpu_la_addr = waddr(17, 4, 0); cpu_la_valid = 1; cpu_valid = 0; @(posedge clk);  // stage X
		cpu_la_valid = 0; cpu_addr = waddr(18, 4, 0); cpu_valid = 1;                      // request Y
		la_lat = 0;
		@(posedge clk); #1;
		while (!cpu_ready && la_lat < 100000) begin @(posedge clk); #1; la_lat = la_lat + 1; end
		reads = reads + 1;
		if (cpu_rdata !== golden(waddr(18, 4, 0)))
			begin errs = errs + 1; $display("  FAIL la(c) no-false-hit got=%08h exp=%08h",
				cpu_rdata, golden(waddr(18, 4, 0))); end
		cpu_valid = 0; @(posedge clk);

		// (d) multi-word: warm a line, stage la for word 0, request word 1 of the SAME line ->
		//     1-cycle hit serving the correct (cpu-offset) word.
		if (WORDS > 1) begin
			do_read(waddr(19, 6, 0), 2);                 // warm the whole line
			cpu_la_addr = waddr(19, 6, 0); cpu_la_valid = 1; cpu_valid = 0; @(posedge clk);  // stage word0
			cpu_la_valid = 0; cpu_addr = waddr(19, 6, 1); cpu_valid = 1;                      // request word1
			la_lat = 0; la_miss = 0;
			@(posedge clk); #1;
			while (!cpu_ready && la_lat < 100000) begin
				la_lat = la_lat + 1;
				if (spi_valid) la_miss = 1;
				@(posedge clk); #1;
			end
			la_lat = la_lat + 1;
			reads = reads + 1;
			if (cpu_rdata !== golden(waddr(19, 6, 1)))
				begin errs = errs + 1; $display("  FAIL la(d) multiword got=%08h exp=%08h",
					cpu_rdata, golden(waddr(19, 6, 1))); end
			if (la_miss || la_lat != 1)
				begin errs = errs + 1; $display("  FAIL la(d) not 1-cyc hit lat=%0d miss=%b", la_lat, la_miss); end
			cpu_valid = 0; @(posedge clk);
		end

		$display("icache_check: SETS=%0d WAYS=%0d WORDS=%0d LAT=%0d SLAT=%0d -> %0d reads, %0d errors %s",
		         SETS, WAYS, WORDS, LATENCY, STREAM_LAT, reads, errs, (errs == 0) ? "PASS" : "FAIL");
		done = 1;
	end
endmodule

// =====================================================================
// Top: instantiate the checker across geometries + latencies, sum results.
// Configs cover the SoC's exact build plus direct-mapped, the single-word edge,
// higher associativity, and a big line, at varied flash latencies.
// =====================================================================
module icache_tb;
	reg clk = 0;
	always #5 clk = ~clk;   // 100 MHz

	wire        d0, d1, d2, d3, d4;
	wire [31:0] e0, e1, e2, e3, e4;

	//                         SETS WAYS WORDS LAT SLAT
	icache_check #(256, 2, 4,  8, 1) chk_soc (.clk(clk), .done(d0), .errs(e0)); // SoC build geometry
	icache_check #( 64, 1, 4,  4, 0) chk_dm  (.clk(clk), .done(d1), .errs(e1)); // direct-mapped, tight stream
	icache_check #( 64, 1, 1,  2, 1) chk_w1  (.clk(clk), .done(d2), .errs(e2)); // single-word line (edge)
	icache_check #( 64, 4, 2, 16, 1) chk_sa  (.clk(clk), .done(d3), .errs(e3)); // 4-way set-assoc
	icache_check #( 32, 2, 8,  4, 0) chk_bl  (.clk(clk), .done(d4), .errs(e4)); // big (8-word) line

	integer total;
	initial begin
		$dumpfile("icache_tb.vcd");
		$dumpvars(0, icache_tb);

		wait (d0 && d1 && d2 && d3 && d4);
		total = e0 + e1 + e2 + e3 + e4;
		$display("icache_tb: total %0d errors across 5 configs -> %s",
		         total, (total == 0) ? "PASS" : "FAIL");
		$display("RESULT: %s", (total == 0) ? "PASS" : "FAIL");
		if (total != 0) $fatal(1);
		$finish;
	end

	// watchdog: catch a global hang (e.g. an FSM that never asserts cpu_ready)
	initial begin
		#50000000;
		$display("icache_tb: TIMEOUT (hang)");
		$display("RESULT: FAIL");
		$fatal(1);
	end
endmodule
