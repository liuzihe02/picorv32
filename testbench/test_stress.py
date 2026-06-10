# test_stress.py — Soak test: millions of random cycles

import cocotb
from cocotb.triggers import RisingEdge, ClockCycles

from conftest import (
    clock_gen, reset, create_rng,
    SETS, WAYS, WORDS_PER_LINE, ITERATIONS,
    TAG_BITS, get_idx, get_tag, get_offset, make_addr,
    CoverageTrackedRef,
)
from stim import random_spi_gating


@cocotb.test()
async def test_stress(dut):
    """Soak test: random stimulus over many cycles, compare DUT vs reference model."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)

    await clock_gen(dut)
    await reset(dut)
    ref.reset_model()

    # Complete init
    for _ in range(SETS):
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    p_stall = 0.3
    gating = random_spi_gating(rng, p_stall)

    pending_requests = 0  # track outstanding cpu_valid assertions
    last_cpu_valid = False
    errors = 0

    for cycle in range(ITERATIONS):
        # Randomise all inputs
        cpu_la_valid = 1 if rng.random() < 0.3 else 0
        cpu_la_addr  = rng.randint(0, 0xFFFFFF) & ~0x3 if cpu_la_valid else 0

        cpu_valid = 1 if rng.random() < 0.15 else 0
        cpu_addr  = rng.randint(0, 0xFFFFFF) & ~0x3 if cpu_valid else 0

        spi_ready = next(gating)
        spi_rdata = rng.randint(0, 0xFFFFFFFF)

        # Drive DUT
        dut.cpu_la_valid.value = cpu_la_valid
        dut.cpu_la_addr.value = cpu_la_addr
        dut.cpu_valid.value = cpu_valid
        dut.cpu_addr.value = cpu_addr
        dut.spi_ready.value = spi_ready
        dut.spi_rdata.value = spi_rdata

        # Step reference model
        ref_out = ref.step(cpu_la_valid, cpu_la_addr, cpu_valid, cpu_addr,
                           spi_ready, spi_rdata)

        await RisingEdge(dut.clk)

        # Compare outputs
        if dut.cpu_ready.value != ref_out[0]:
            errors += 1
            dut._log.error(f"Cycle {cycle}: cpu_ready mismatch: DUT={dut.cpu_ready.value}, ref={ref_out[0]}")
        if dut.spi_valid.value != ref_out[2]:
            errors += 1
            dut._log.error(f"Cycle {cycle}: spi_valid mismatch: DUT={dut.spi_valid.value}, ref={ref_out[2]}")
        if dut.spi_addr.value != ref_out[3]:
            errors += 1
            dut._log.error(f"Cycle {cycle}: spi_addr mismatch: DUT=0x{dut.spi_addr.value:06x}, ref=0x{ref_out[3]:06x}")

        # When cpu_ready is asserted, check data correctness
        if dut.cpu_ready.value == 1 and ref_out[0] == 1:
            if dut.cpu_rdata.value != ref_out[1]:
                errors += 1
                dut._log.error(f"Cycle {cycle}: cpu_rdata mismatch: DUT=0x{dut.cpu_rdata.value:08x}, ref=0x{ref_out[1]:08x}")

        # Track liveness
        if cpu_valid and not ref.in_init:
            pending_requests += 1
        if dut.cpu_ready.value == 1:
            pending_requests = max(0, pending_requests - 1)

        if errors > 20:
            dut._log.error(f"Too many errors ({errors}), aborting at cycle {cycle}")
            break

    # Liveness check at end
    assert pending_requests == 0, f"Pending requests at end of test: {pending_requests} (deadlock?)"
    assert errors == 0, f"{errors} mismatches between DUT and reference model"
    dut._log.info(f"Soak test complete: {ITERATIONS} cycles, {errors} errors")

    ref.save_snapshot()
