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
	reg [31:0] data_mem [0:255];
	reg [14:0] tag_mem  [0:255];

	localparam ST_IDLE  = 2'd0;
	localparam ST_CHECK = 2'd1;  // waiting one cycle for EBR read output
	localparam ST_FILL  = 2'd2;  // waiting for spimemio

	reg [1:0]  state;
	reg [7:0]  req_idx;   // cpu_addr[9:2]
	reg [13:0] req_tag;   // cpu_addr[23:10]
	reg [14:0] rd_tag;
	reg [31:0] rd_data;

	always @(posedge clk) begin
		cpu_ready <= 0;
		spi_valid <= 0;

		case (state)
			ST_IDLE: begin
				if (cpu_valid) begin
					req_idx <= cpu_addr[9:2];
					req_tag <= cpu_addr[23:10];
					rd_tag  <= tag_mem[cpu_addr[9:2]];
					rd_data <= data_mem[cpu_addr[9:2]];
					state   <= ST_CHECK;
				end
			end

			ST_CHECK: begin
				if (rd_tag == {1'b1, req_tag}) begin
					// Hit: return cached word
					cpu_rdata <= rd_data;
					cpu_ready <= 1;
					state     <= ST_IDLE;
				end else begin
					// Miss: request from SPI flash
					spi_valid <= 1;
					spi_addr  <= {req_tag, req_idx, 2'b00};
					state     <= ST_FILL;
				end
			end

			ST_FILL: begin
				spi_valid <= 1;
				spi_addr  <= {req_tag, req_idx, 2'b00};
				if (spi_ready) begin
					data_mem[req_idx] <= spi_rdata;
					tag_mem[req_idx]  <= {1'b1, req_tag};
					cpu_rdata         <= spi_rdata;
					cpu_ready         <= 1;
					spi_valid         <= 0;
					state             <= ST_IDLE;
				end
			end
		endcase

		if (!resetn) begin
			state     <= ST_IDLE;
			cpu_ready <= 0;
			spi_valid <= 0;
		end
	end
endmodule
