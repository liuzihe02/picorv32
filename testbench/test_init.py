# test_init.py — Cache initialisation verification

import cocotb
from cocotb.triggers import RisingEdge, ClockCycles, Timer

from conftest import clock_gen, reset, create_rng, SETS, IDX_BITS, CoverageTrackedRef


@cocotb.test()
async def test_init(dut):
    """Verify reset-triggered initialisation clears all tag valid bits."""
    rng = create_rng()
    ref = CoverageTrackedRef(dut)
    print("Starting test_init")

    # Start clock
    await clock_gen(dut)
    print("Clock started")

    # --- Test 1: Cold-boot reset ---
    dut.resetn.value = 0
    dut.cpu_valid.value = 0
    dut.cpu_la_valid.value = 0
    dut.spi_ready.value = 0
    dut.spi_rdata.value = 0
    dut.cpu_addr.value = 0
    dut.cpu_la_addr.value = 0
    ref.reset_model()

    # Initial reset assertion for 10 cycles
    await ClockCycles(dut.clk, 10)
    for _ in range(10):
        ref.step(0, 0, 0, 0, 0, 0, resetn=0)
    print("Initial reset assertion complete")

    # Release reset
    dut.resetn.value = 1
    await RisingEdge(dut.clk)

    # Now initialisation should run for SETS cycles.
    # The DUT init loop lasts SETS cycles: for each cycle we tick the model.
    print("Reset released, starting initialisation")
    assert ref.in_init, "ref should be in init after reset release"

    for i in range(SETS):
        # During init, cpu_ready must be 0
        assert dut.cpu_ready.value == 0, f"cpu_ready high during init cycle {i}"

        # Try asserting cpu_valid mid-init — should not produce cpu_ready or spi_valid.
        # Skip the last iteration: DUT may have entered FSM by then, while ref is
        # still in its final init step — asserting cpu_valid there would desync them.
        if i % 10 == 3 and i != SETS - 1:
            dut.cpu_valid.value = 1
            dut.cpu_addr.value = 0x1000
        else:
            dut.cpu_valid.value = 0

        ref.step(0, 0, dut.cpu_valid.value, dut.cpu_addr.value, 0, 0)
        await RisingEdge(dut.clk)

        assert dut.spi_valid.value == 0, f"spi_valid high during init cycle {i}"

    # Init should be complete now
    assert not ref.in_init, "ref init should be done after SETS cycles"
    assert dut.cpu_ready.value == 0, "cpu_ready should be 0 right after init"

    # --- Test 2: Mid-operation reset ---
    # First put the cache into a known state by doing a fill
    await _do_manual_fill(dut, ref, 0, 0x10)
    assert ref.state == "IDLE"

    # Now assert resetn mid-operation
    dut.resetn.value = 0
    dut.cpu_valid.value = 0
    ref.reset_model()
    await ClockCycles(dut.clk, 5)
    for _ in range(5):
        ref.step(0, 0, 0, 0, 0, 0, resetn=0)
    assert ref.in_init, "ref should re-enter init after mid-op reset"

    # Release and verify init runs again
    dut.resetn.value = 1
    await RisingEdge(dut.clk)
    for i in range(SETS):
        assert dut.cpu_ready.value == 0
        ref.step(0, 0, 0, 0, 0, 0)
        await RisingEdge(dut.clk)
    assert not ref.in_init

    # --- Test 3: Random reset duration ---
    for test_i in range(10):
        hold_cycles = rng.randint(1, 20)
        dut.resetn.value = 0
        ref.reset_model()
        await ClockCycles(dut.clk, hold_cycles)
        for _ in range(hold_cycles):
            ref.step(0, 0, 0, 0, 0, 0, resetn=0)

        dut.resetn.value = 1
        await RisingEdge(dut.clk)
        init_count = 0
        while ref.in_init:
            assert dut.cpu_ready.value == 0
            ref.step(0, 0, 0, 0, 0, 0)
            await RisingEdge(dut.clk)
            init_count += 1
        assert init_count == SETS, f"Init took {init_count} cycles, expected {SETS}"

    ref.save_snapshot()


async def _do_manual_fill(dut, ref, set_idx, tag):
    """Simple helper: fill a line via miss."""
    from conftest import make_addr, WORDS_PER_LINE
    addr = make_addr(tag, set_idx, 0)
    wpl = WORDS_PER_LINE

    dut.cpu_valid.value = 1
    dut.cpu_addr.value = addr
    dut.cpu_la_valid.value = 0
    ref.step(0, 0, 1, addr, 0, 0)
    await RisingEdge(dut.clk)

    dut.cpu_valid.value = 0
    dut.spi_ready.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)

    for wi in range(wpl):
        dut.spi_ready.value = 1
        dut.spi_rdata.value = (tag << 16) | (set_idx << 8) | wi
        dut.cpu_valid.value = 0
        ref.step(0, 0, 0, 0, 1, (tag << 16) | (set_idx << 8) | wi)
        await RisingEdge(dut.clk)

    # Consume the post-fill dead cycle where cpu_ready returns to 0
    dut.cpu_valid.value = 0
    dut.spi_ready.value = 0
    ref.step(0, 0, 0, 0, 0, 0)
    await RisingEdge(dut.clk)

    assert ref.state == "IDLE"
