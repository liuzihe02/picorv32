// Parameterised instruction cache: N-way set-associative, M-word lines.
//
//   WAYS = 1  -> direct-mapped (plain multi-word cache)
//   WAYS > 1  -> set-associative (round-robin replacement)
//
// Address split (24-bit flash address):
//   [ tag | index | offset | byte(2) ]
//   index  = SETS rows        (IDX_BITS = clog2(SETS))
//   offset = word in a line   (OFF_BITS = clog2(WORDS_PER_LINE))
//   tag    = the rest
//
// EBR-friendly: every data/tag memory write is FULL-WIDTH (whole set, all ways).
// On a fill we replace only the victim way's slice, copying the other ways from
// the just-read line -- so yosys never sees a dynamic partial-memory write.
//
// 1-cycle hit via look-ahead pre-read: when cpu_la_valid pulses (mem_la_read, gated to the
// flash region in picosoc), the EBR read is issued a cycle EARLY off cpu_la_addr, so when
// cpu_valid arrives the set is already staged and we compare-and-ack in one cycle. Tie
// cpu_la_valid=0 (picosoc LOOKAHEAD_HIT=0) and the fast path constant-folds away, leaving the
// 2-cycle fallback == the pre-look-ahead baseline. cpu_ready stays registered either way.


//TODO: change cache params here
module icache #(
	parameter integer SETS           = 1024,  // number of indices (power of 2)
	parameter integer WAYS           = 2,    // associativity (1 = direct-mapped)
	parameter integer WORDS_PER_LINE = 1     // words per line/block (power of 2)
) (
	input         clk,
	input         resetn,

	// CPU side: flash-region fetches from picorv32
	input         cpu_valid,
	input  [23:0] cpu_addr,
	output reg    cpu_ready,
	output reg [31:0] cpu_rdata,

	// Look-ahead side: the flash-region fetch one cycle early (mem_la_read/mem_la_addr,
	// region-gated in picosoc). cpu_la_valid=0 disables (always 2-cycle fallback).
	input         cpu_la_valid,
	input  [23:0] cpu_la_addr,

	// SPI side: forwarded to spimemio on a miss
	output reg        spi_valid,
	output reg [23:0] spi_addr,
	input             spi_ready,
	input      [31:0] spi_rdata
);
	// ---- derived sizes -------------------------------------------------------
	localparam integer OFF_BITS = (WORDS_PER_LINE > 1) ? $clog2(WORDS_PER_LINE) : 0;
	localparam integer IDX_BITS = $clog2(SETS);
	localparam integer TAG_BITS = 24 - 2 - OFF_BITS - IDX_BITS;
	localparam integer TAGW     = TAG_BITS + 1;            // {valid, tag}
	localparam integer LINE     = WORDS_PER_LINE * 32;     // bits per line
	localparam integer DATAW    = WAYS * LINE;             // all ways of a set
	localparam integer TAGMW    = WAYS * TAGW;             // all way-tags of a set
	localparam integer VW       = (WAYS > 1) ? $clog2(WAYS) : 1;  // victim ctr width
	localparam integer FCW      = (OFF_BITS > 0) ? OFF_BITS : 1;  // fill/offset ctr width

	// ---- storage (yosys infers SB_RAM40_4K) ----------------------------------
	reg [DATAW-1:0] data_mem [0:SETS-1];   // per set: {way[W-1] line, ..., way[0] line}
	reg [TAGMW-1:0] tag_mem  [0:SETS-1];   // per set: {way[W-1] tag,  ..., way[0] tag }

	localparam s_idle  = 2'd0;
	localparam s_check = 2'd1;
	localparam s_fill  = 2'd2;

	reg [1:0]         state;
	reg [IDX_BITS-1:0] req_idx;
	reg [FCW-1:0]      req_off;
	reg [TAG_BITS-1:0] req_tag;
	reg [TAGMW-1:0]    tag_q;
	reg [DATAW-1:0]    data_q;

	reg [FCW-1:0]   fill_cnt;
	reg [LINE-1:0]  fill_buf;
	reg [VW-1:0]    victim_ctr;            // round-robin replacement pointer

	reg            init_done;
	reg [IDX_BITS-1:0] init_idx;

	// Look-ahead pre-read state. pre_valid: a look-ahead read is staged in tag_q/data_q for
	// set req_idx. rd_en/rd_idx: scratch -- this cycle's SINGLE muxed EBR read (one read site
	// keeps tag_mem/data_mem 1R1W -> SB_RAM40). cpu_*/la_idx: combinational address slices.
	reg            pre_valid;
	reg            rd_en;
	reg [IDX_BITS-1:0] rd_idx, la_idx, cpu_idx;
	reg [TAG_BITS-1:0] cpu_tag;
	reg [FCW-1:0]      cpu_off;

	// Byte address of word 0 of the requested line: {tag, index, 0...0}. Word k is
	// line_base + (k<<2). Built by replication (not a fill_cnt concat) so it is also
	// correct when OFF_BITS==0 (WORDS_PER_LINE==1) -- a raw {..,fill_cnt,2'b00} concat
	// would inject a spurious low bit there and corrupt the index.
	wire [23:0] line_base = {req_tag, req_idx, {(OFF_BITS+2){1'b0}}};

	// loop / combinational temporaries
	integer         w, k;
	reg             hit;
	reg [VW-1:0]    hit_way;
	reg [VW-1:0]    victim;
	reg [LINE-1:0]  sel_line, new_line;
	reg [DATAW-1:0] new_data;
	reg [TAGMW-1:0] new_tag;

	always @(posedge clk) begin
		cpu_ready <= 0;
		spi_valid <= 0;

		if (!init_done) begin
			tag_mem[init_idx] <= 0;                 // clear all way valid-bits in this set
			init_idx <= init_idx + 1;
			if (init_idx == SETS-1) init_done <= 1;
			pre_valid <= 0;
		end

		else
		case (state)
			s_idle: begin   // "serve": a look-ahead pre-read may already be staged in tag_q/data_q
				// combinational decode of this cycle's addresses (blocking; used below)
				cpu_idx = cpu_addr[2+OFF_BITS +: IDX_BITS];
				cpu_tag = cpu_addr[2+OFF_BITS+IDX_BITS +: TAG_BITS];
				cpu_off = (OFF_BITS > 0) ? cpu_addr[2 +: FCW] : {FCW{1'b0}};
				la_idx  = cpu_la_addr[2+OFF_BITS +: IDX_BITS];
				rd_en   = 1'b0;
				rd_idx  = la_idx;

				if (cpu_valid && !cpu_ready) begin
					if (pre_valid && req_idx == cpu_idx) begin
						// staged read is the RIGHT SET -> compare its ways against the CPU's
						// ACTUAL tag (not req_tag) and decide now, no EBR read. A stale/wrong
						// pre-read can only miss here, never false-hit.
						hit = 1'b0;
						hit_way = {VW{1'b0}};
						for (w = 0; w < WAYS; w = w + 1)
							if (tag_q[w*TAGW +: TAGW] == {1'b1, cpu_tag}) begin
								hit = 1'b1;
								hit_way = w[VW-1:0];
							end
						if (hit) begin
							sel_line  = data_q[hit_way*LINE +: LINE];
							cpu_rdata <= sel_line[cpu_off*32 +: 32];
							cpu_ready <= 1;                            // 1-cycle hit
						end else begin
							req_idx <= cpu_idx; req_tag <= cpu_tag; req_off <= cpu_off;
							fill_cnt <= {FCW{1'b0}};
							state    <= s_fill;                        // fast miss (spi driven in s_fill)
						end
						pre_valid <= 0;
					end else begin
						// no usable pre-read -> 2-cycle fallback: read cpu's set now, compare in s_check
						rd_en   = 1'b1;
						rd_idx  = cpu_idx;
						req_idx <= cpu_idx;
						req_tag <= cpu_tag;
						req_off <= cpu_off;
						state   <= s_check;
						pre_valid <= 0;
					end
				end else if (cpu_la_valid) begin
					// stage the look-ahead pre-read (one cycle early)
					rd_en   = 1'b1;
					rd_idx  = la_idx;
					req_idx <= la_idx;
					pre_valid <= 1;
				end

				if (rd_en) begin   // the SINGLE EBR read site
					tag_q  <= tag_mem [rd_idx];
					data_q <= data_mem[rd_idx];
				end
			end

			s_check: begin
				// parallel tag compare across all ways
				hit = 1'b0;
				hit_way = {VW{1'b0}};
				for (w = 0; w < WAYS; w = w + 1)
					if (tag_q[w*TAGW +: TAGW] == {1'b1, req_tag}) begin
						hit = 1'b1;
						hit_way = w[VW-1:0];
					end

				if (hit) begin
					sel_line = data_q[hit_way*LINE +: LINE];     // pick the way
					cpu_rdata <= sel_line[req_off*32 +: 32];     // pick the word
					cpu_ready <= 1;
					state     <= s_idle;
				end else begin
					fill_cnt <= {FCW{1'b0}};
					state    <= s_fill;
				end
			end

			s_fill: begin
				// Fetch the line word-by-word; sequential addrs ride spimemio's stream.
				// spimemio holds `ready` HIGH (level) while the requested word is available
				// and presents rdata for the address it currently acks (spi_addr == rd_addr).
				// Capture on the level, but advance spi_addr in LOCKSTEP on the capture cycle
				// (the assignment below the if() overrides this default) so we never re-grab the
				// same word into the next slot. Edge-detecting ready instead is racy: when
				// spimemio advances rd_addr the same cycle the cache advances spi_addr, ready
				// never falls and the rising edge -- hence the word -- is missed.
				spi_valid <= 1;
				spi_addr  <= line_base + (fill_cnt << 2);
				if (spi_ready) begin
					for (k = 0; k < WORDS_PER_LINE; k = k + 1)
						if (k[FCW-1:0] == fill_cnt)
							fill_buf[k*32 +: 32] <= spi_rdata;

					if (fill_cnt == WORDS_PER_LINE-1) begin
						// assemble the completed line (last word = spi_rdata)
						for (k = 0; k < WORDS_PER_LINE; k = k + 1)
							new_line[k*32 +: 32] =
								(k[FCW-1:0] == fill_cnt) ? spi_rdata : fill_buf[k*32 +: 32];

						// choose victim way (round-robin; way 0 when direct-mapped)
						victim = (WAYS > 1) ? victim_ctr : {VW{1'b0}};

						// rebuild the whole set: victim way replaced, others preserved
						for (w = 0; w < WAYS; w = w + 1) begin
							new_data[w*LINE +: LINE] =
								(w[VW-1:0] == victim) ? new_line : data_q[w*LINE +: LINE];
							new_tag[w*TAGW +: TAGW] =
								(w[VW-1:0] == victim) ? {1'b1, req_tag} : tag_q[w*TAGW +: TAGW];
						end

						data_mem[req_idx] <= new_data;
						tag_mem[req_idx]  <= new_tag;
						cpu_rdata   <= new_line[req_off*32 +: 32];
						cpu_ready   <= 1;
						spi_valid   <= 0;
						victim_ctr  <= (WAYS > 1) ? victim_ctr + 1'b1 : victim_ctr;
						state       <= s_idle;
					end else begin
						fill_cnt <= fill_cnt + 1'b1;
						spi_addr <= line_base + ((fill_cnt + 1'b1) << 2);  // advance in lockstep
					end
				end
			end
		endcase

		if (!resetn) begin
			state      <= s_idle;
			cpu_ready  <= 0;
			spi_valid  <= 0;
			fill_cnt   <= {FCW{1'b0}};
			victim_ctr <= {VW{1'b0}};
			init_done  <= 0;
			init_idx   <= 0;
			pre_valid  <= 0;
		end
	end
endmodule
