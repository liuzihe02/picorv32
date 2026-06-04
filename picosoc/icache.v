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
// Same port list as the original icache, so picosoc.v needs no changes.

module icache #(
	parameter integer SETS           = 256,  // number of indices (power of 2)
	parameter integer WAYS           = 1,    // associativity (1 = direct-mapped)
	parameter integer WORDS_PER_LINE = 4     // words per line/block (power of 2)
) (
	input         clk,
	input         resetn,

	// CPU side: flash-region fetches from picorv32
	input         cpu_valid,
	input  [23:0] cpu_addr,
	output reg    cpu_ready,
	output reg [31:0] cpu_rdata,

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
		end

		else
		case (state)
			s_idle: begin
				if (cpu_valid && !cpu_ready) begin
					req_idx <= cpu_addr[2+OFF_BITS +: IDX_BITS];
					req_tag <= cpu_addr[2+OFF_BITS+IDX_BITS +: TAG_BITS];
					req_off <= (OFF_BITS > 0) ? cpu_addr[2 +: FCW] : {FCW{1'b0}};
					tag_q   <= tag_mem [cpu_addr[2+OFF_BITS +: IDX_BITS]];
					data_q  <= data_mem[cpu_addr[2+OFF_BITS +: IDX_BITS]];
					state   <= s_check;
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
				// fetch the line word-by-word; sequential addrs ride spimemio's stream
				spi_valid <= 1;
				spi_addr  <= {req_tag, req_idx, fill_cnt, 2'b00};
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
		end
	end
endmodule
