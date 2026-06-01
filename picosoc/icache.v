module icache (
	input         clk,
	input         resetn,

	// CPU side: flash region requests from picorv32
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

	localparam cache_state_idle  = 2'd0;  // accept request, issue EBR tag/data read
	localparam cache_state_check = 2'd1;  // EBR read-out ready: compare tag -> hit/miss
	localparam cache_state_fill  = 2'd2;  // miss: fetch the word from flash via spimemio

	reg [1:0]  cache_state;
	reg [7:0]  req_idx;   // cpu_addr[9:2]
	reg [13:0] req_tag;   // cpu_addr[23:10]
	reg [14:0] tag_q; // _q: registered EBR read-out of tag_mem  (valid in cache_state_check)
	reg [31:0] data_q; // _q: registered EBR read-out of data_mem

	// EBR powers up undefined on silicon, so the valid bits must be cleared explicitly after reset before any lookup.
	// (In sim, uninitialised tag_mem reads X and the hit-compare is accidentally false -- silicon has no luck.)
	reg        init_done;  // invalidation sweep finished?
	reg [7:0]  init_idx;   // sweep counter, walks all 256 lines

	always @(posedge clk) begin
		cpu_ready <= 0;
		spi_valid <= 0;

		if (!init_done) begin
			// invalidate every line (clear valid bit 14) before serving requests
			tag_mem[init_idx] <= 0;
			init_idx <= init_idx + 1;
			if (&init_idx) init_done <= 1;   // last line cleared
		end else
		
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
endmodule
