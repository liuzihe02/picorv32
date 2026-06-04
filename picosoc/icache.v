module icache #(
	// LOOKAHEAD_HIT (Step 1a-ii): 0 = today's 2-cycle hit (byte-identical);
	// 1 = pre-read the EBR off the look-ahead address so a hit acks in 1 cycle
	//     (cpu_ready still registered -- NOT the rejected combinational hit).
	parameter [0:0] LOOKAHEAD_HIT = 0
) (
	input         clk,
	input         resetn,

	// CPU side: flash region requests from picorv32
	input         cpu_valid,
	input  [23:0] cpu_addr,
	output reg    cpu_ready,
	output reg [31:0] cpu_rdata,

	// Look-ahead side (LOOKAHEAD_HIT only): the flash-region fetch one cycle early.
	// cpu_la_valid = mem_la_read && (mem_la_addr in flash region); cpu_la_addr = mem_la_addr[23:0].
	input         cpu_la_valid,
	input  [23:0] cpu_la_addr,

	// SPI side: forwarded to spimemio on a miss
	output reg        spi_valid,
	output reg [23:0] spi_addr,
	input             spi_ready,
	input      [31:0] spi_rdata
);
	// 256-line direct-mapped cache, one word per line.
	// Yosys infers these as SB_RAM40_4K EBR blocks on iCE40:
	//   data_mem: 256x32 = 2 EBR blocks
	//   tag_mem:  256x15 = 1 EBR block  (bit 14 = valid, bits 13:0 = tag)
	/*	addr broken down into
	*	23                 10 9        2 1   0
	*	+--------------------+----------+-----+
	*	|   tag (14 bits)    | index(8) | 00  |
	*	+--------------------+----------+-----+
	*	req_tag               req_idx     byte
	*/
	reg [31:0] data_mem [0:255];
	reg [14:0] tag_mem  [0:255];

	localparam cache_state_idle  = 2'd0;  // accept request, issue EBR tag/data read  (= "serve" when LOOKAHEAD_HIT)
	localparam cache_state_check = 2'd1;  // EBR read-out ready: compare tag -> hit/miss
	localparam cache_state_fill  = 2'd2;  // miss: fetch the word from flash via spimemio

	reg [1:0]  cache_state;
	reg [7:0]  req_idx;   // cpu_addr[9:2]
	reg [13:0] req_tag;   // cpu_addr[23:10]
	reg [14:0] tag_q; // _q: registered EBR read-out of tag_mem  (valid in cache_state_check)
	reg [31:0] data_q; // _q: registered EBR read-out of data_mem
	reg        pre_valid; // LOOKAHEAD_HIT: a look-ahead pre-read is staged in tag_q/data_q/req_*
	reg        rd_en;     // LOOKAHEAD_HIT: scratch -- this serve cycle's single EBR read enable
	reg [7:0]  rd_idx;    // LOOKAHEAD_HIT: scratch -- and its (muxed) index. One read site => one EBR read port.

	// EBR powers up undefined on silicon, so ALL valid bits must be cleared (swept through) explicitly after reset before any lookup.
	// (In sim, uninitialised tag_mem reads X and the hit-compare is accidentally false. On hardware may give corrupted hit compare)
	reg        init_done;  // flag to check
	reg [7:0]  init_idx;   // sweep counter, walks all 256 lines

generate if (!LOOKAHEAD_HIT) begin : base
	// ===== Baseline 2-cycle hit (unchanged) =================================
	always @(posedge clk) begin
		cpu_ready <= 0;
		spi_valid <= 0;

		if (!init_done) begin
			// invalidate every line (clear valid bit 14) before serving cache requests
			tag_mem[init_idx] <= 0;
			init_idx <= init_idx + 1;
			if (&init_idx) init_done <= 1;   // last cache line cleared, flag init_done
		end

		// else: sweep finished, can start the normal cache FSM
		else

		case (cache_state)
			cache_state_idle: begin
				// !cpu_ready: don't re-accept the request we're acking this cycle
				// cpu_valid lingers high during the ack, which would otherwise trigger a phantom hit that corrupts the next request.
				if (cpu_valid && !cpu_ready) begin
					req_idx <= cpu_addr[9:2];
					req_tag <= cpu_addr[23:10];
					tag_q   <= tag_mem[cpu_addr[9:2]];
					data_q  <= data_mem[cpu_addr[9:2]];
					cache_state <= cache_state_check;
				end
			end

			cache_state_check: begin
				if (tag_q == {1'b1, req_tag}) begin
					// Hit: return cached word
					cpu_rdata   <= data_q;
					cpu_ready   <= 1;
					cache_state <= cache_state_idle;
				end else begin
					// Miss: request from SPI flash
					spi_valid   <= 1;
					spi_addr    <= {req_tag, req_idx, 2'b00};
					cache_state <= cache_state_fill;
				end
			end

			cache_state_fill: begin
				spi_valid <= 1;
				spi_addr  <= {req_tag, req_idx, 2'b00};
				if (spi_ready) begin
					data_mem[req_idx] <= spi_rdata;
					tag_mem[req_idx]  <= {1'b1, req_tag};
					cpu_rdata         <= spi_rdata;
					cpu_ready         <= 1;
					spi_valid         <= 0;
					cache_state       <= cache_state_idle;
				end
			end
		endcase

		if (!resetn) begin
			cache_state <= cache_state_idle;
			cpu_ready   <= 0;
			spi_valid   <= 0;
			init_done   <= 0;
			init_idx    <= 0;
		end
	end

end else begin : lahit
	// ===== 1-cycle hit via look-ahead pre-read (Step 1a-ii) =================
	// idle == "serve": the EBR read for a fetch is issued a cycle EARLY off the
	// look-ahead address, so when cpu_valid arrives the read-out (tag_q/data_q) is
	// already staged -> compare-and-ack in one step. cpu_ready stays registered.
	always @(posedge clk) begin
		cpu_ready <= 0;
		spi_valid <= 0;

		if (!init_done) begin
			tag_mem[init_idx] <= 0;
			init_idx <= init_idx + 1;
			if (&init_idx) init_done <= 1;
			pre_valid <= 0;
		end else

		case (cache_state)
			cache_state_idle: begin   // serve
				// This state's SINGLE EBR read (rd_en/rd_idx) -- exactly one read site for
				// tag_mem/data_mem keeps them mapped to a 1R1W SB_RAM40 block. The address is
				// muxed: the look-ahead index when staging a pre-read, or cpu_addr on the rare
				// fallback. A hit reuses the already-staged tag_q/data_q and issues NO read.
				rd_en  = 1'b0;
				rd_idx = cpu_la_addr[9:2];
				if (cpu_valid && !cpu_ready) begin
					if (pre_valid && req_idx == cpu_addr[9:2] && req_tag == cpu_addr[23:10]) begin
						// pre-read is staged for exactly this address -> decide now (no read)
						if (tag_q == {1'b1, req_tag}) begin
							cpu_rdata <= data_q;   // HIT, cpu_ready next cycle (1-cycle)
							cpu_ready <= 1;
						end else begin
							spi_valid <= 1;        // MISS
							spi_addr  <= {req_tag, req_idx, 2'b00};
							cache_state <= cache_state_fill;
						end
						pre_valid <= 0;
					end else begin
						// no usable pre-read (cold / contract broke) -> slow 2-cycle fallback:
						// read EBR off cpu_addr now, compare in check. (check compares tag_q vs
						// req_tag = the actual cpu_addr tag, so a stale pre-read can only miss.)
						rd_en   = 1'b1;
						rd_idx  = cpu_addr[9:2];
						req_idx <= cpu_addr[9:2];
						req_tag <= cpu_addr[23:10];
						cache_state <= cache_state_check;
						pre_valid <= 0;
					end
				end else if (cpu_la_valid) begin
					// stage the look-ahead pre-read (one cycle early)
					rd_en   = 1'b1;
					rd_idx  = cpu_la_addr[9:2];
					req_idx <= cpu_la_addr[9:2];
					req_tag <= cpu_la_addr[23:10];
					pre_valid <= 1;
				end
				if (rd_en) begin   // the one EBR read site
					tag_q  <= tag_mem[rd_idx];
					data_q <= data_mem[rd_idx];
				end
			end

			cache_state_check: begin   // fallback only
				if (tag_q == {1'b1, req_tag}) begin
					cpu_rdata   <= data_q;
					cpu_ready   <= 1;
					cache_state <= cache_state_idle;
				end else begin
					spi_valid   <= 1;
					spi_addr    <= {req_tag, req_idx, 2'b00};
					cache_state <= cache_state_fill;
				end
			end

			cache_state_fill: begin
				spi_valid <= 1;
				spi_addr  <= {req_tag, req_idx, 2'b00};
				if (spi_ready) begin
					data_mem[req_idx] <= spi_rdata;
					tag_mem[req_idx]  <= {1'b1, req_tag};
					cpu_rdata         <= spi_rdata;
					cpu_ready         <= 1;
					spi_valid         <= 0;
					cache_state       <= cache_state_idle;
				end
			end
		endcase

		if (!resetn) begin
			cache_state <= cache_state_idle;
			cpu_ready   <= 0;
			spi_valid   <= 0;
			init_done   <= 0;
			init_idx    <= 0;
			pre_valid   <= 0;
		end
	end
end endgenerate
endmodule
