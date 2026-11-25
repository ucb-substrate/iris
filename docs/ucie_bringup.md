# UCIe bring-up on Iris

Iris has two UCIe links. This page covers what's specific to Iris: the links' address map, how
software reaches them, which clocks come in on bumps, and how bumps map to links.

How to clock, reset and bring up PhyTest, the IP's link tester, is in the UCIe IP's own guide:
[`ucie/docs/phytest_bringup.md`](../ucie/docs/phytest_bringup.md). Read its section 3
("The register port, and how not to hang it") before touching a link.

Paths are from the repository root. `ucie/...` is the UCIe submodule.

---

## 1. The two links

Both links are chiplet-router ports with 16 lanes (`src/Iris.scala`). Their
`UcieTLParams` differ only in `address` and `moduleId`.

| | Link 0 | Link 1 |
|---|---|---|
| IrisTop IO | `c2c_ucie0` | `c2c_ucie1` |
| Chiplet-router port (and `program_router` `port` argument) | 0 | 1 |
| `UcieTLParams.address` (`base`) | `0x200000` | `0x208000` |
| PhyTest and lane registers (`UcieTLRegs`, PHY test clock) | `0x200000`–`0x203BFF` | `0x208000`–`0x20BBFF` |
| Controller controls and UCIe spec registers (`UcieController`, TX sideband clock) | `0x203C00`–`0x207FFF` | `0x20BC00`–`0x20FFFF` |
| Clock and reset registers (`UcieClkRegs`, chip clock) | `0x210000`–`0x210FFF` | `0x218000`–`0x218FFF` |
| Module (`moduleId`) | `UcieTL` (0) | `UcieTL_1` (1) |

Everything outside these windows is unmapped, including `0x211000`–`0x217FFF` and `0x219000` up.
Iris removed the error device from every bus, so an unmapped access
gets no error response. **It hangs** (inference: nothing on the bus can answer it).

### Registers you'll touch first

Offsets come from `software/ucie.h`, which is the authority. Regenerate it whenever the `ucie`
submodule moves (section 5).

| Register | Offset | Link 0 | Link 1 | Safe before `ucieRst` = 0? |
|---|---|---|---|---|
| `UCIE_MAIN_CLK_SEL` | `0x10000` | `0x210000` | `0x218000` | yes |
| `UCIE_PHY_TEST_CLK_BYPASS_EN` | `0x10038` | `0x210038` | `0x218038` | yes |
| `UCIE_CLK_EN` / `UCIE_RX_CLK_EN` | `0x10048` / `0x10050` | `0x210048` / `0x210050` | `0x218048` / `0x218050` | yes |
| `UCIE_CLK_CFG_BUSY` | `0x10088` | `0x210088` | `0x218088` | yes |
| `UCIE_CLK_CFG_UNGATE_REQ` | `0x100A0` | `0x2100A0` | `0x2180A0` | yes |
| `UCIE_PLL_LOCK_OBSERVED` | `0x100A8` | `0x2100A8` | `0x2180A8` | yes |
| `UCIE_RST` (reset value 1) | `0x100B0` | `0x2100B0` | `0x2180B0` | yes |
| `UCIE_PHY_TEST_CLK_EN` | `0x100D8` | `0x2100D8` | `0x2180D8` | yes |
| `UCIE_TX_TEST_STATE` | `0x128` | `0x200128` | `0x208128` | **no**: needs `ucieRst` = 0 and a running PHY test clock |
| `UCIE_RX_IDLE_WORDS_OBSERVED` | `0x3C8` | `0x2003C8` | `0x2083C8` | no |
| `UCIE_TX_TEST_FIFO_UNDERFLOW` / `UCIE_RX_TEST_FIFO_OVERFLOW` | `0x3868` / `0x3870` | `0x203868` / `0x203870` | `0x20B868` / `0x20B870` | no |

Other Iris addresses that bring-up programs use (`software/router.h`): the
chiplet-router table at `0x4000` (32 bytes per entry, 4 entries), and the chip ID register at
`0x4080`.

---

## 2. How software reaches the registers

Each link's register port is on the SoC's **control bus** (CBUS), coupled there in
`src/Iris.scala`. It does not go through the chiplet router. `ChipletRoutingKey.controlBusWhere`
only names which bus that is. The link's chip clock is the `uncore` clock, shared with every bus at
500 MHz.

There are three ways in:

1. **The cores.** C programs include `software/ucie.h`. Every helper takes `base` (`0x200000` or
   `0x208000`) and uses `reg_write64`/`reg_read64` (`software/mmio.h`).
2. **TSI over serial TileLink.**
   - IrisTop's `serial_tl` port carries 16-bit phits on the tapeout config, 32-bit with `sim`.
     `serial_tl.clock_in` clocks it from outside.
   - Its requests enter at the front bus and reach the UCIe port through FBUS → SBUS → CBUS. Its
     manager side serves off-chip memory at `0x8000_0000`, 4 GiB.
   - The host-side `TSIToTileLink` issues a single 8-byte Get or Put only for an 8-byte-aligned,
     two-word request (`TSIToTileLink` in testchipip). Use exactly that
     for UCIe registers.
3. **JTAG.**
   - The debug module has system-bus access (`WithJtagDTM`, `WithDebugSBA`), 64 bits wide.
   - Use 64-bit accesses, such as OpenOCD `write_memory`/`read_memory` with width 64.
   - Abstract commands through a halted hart also work, unless the control bus is wedged.

**Use 64-bit accesses.**
- Writes to fields wider than 32 bits (`txctl_N_tile`, `clkPhaseSel`, seeds, packet counts,
  `txDataChunkIn0/1`, `sbTxPacket`) are silently ignored unless the whole field is in the byte mask.
- A 32-bit read of a 64-bit counter returns half of it.

**Hang hazards, and recovery on Iris.**
- These hang a request forever:
  - an access to a link's PhyTest or controller window while that domain is in reset (`ucieRst` = 1
    is the power-on state) or unclocked;
  - an access to an unmapped address;
  - an access to a remote address (the off-chip range at `0x8_0000_0000`)
    while the link isn't carrying TileLink.
- A hung UCIe access blocks the control bus (inferred from the bus structure): both links'
  registers, the router table and chip ID, the clock divider, CLINT, PLIC, the boot ROM, the debug
  module's hart-facing ROM, and the periphery bus with the UART. TSI and JTAG system-bus access can
  still reach memory.
- JTAG can't reset the chip: IrisTop doesn't use the debug module's `ndreset`.
  **Only `reset` recovers.** Set up the bench so the host can drive
  Iris's reset pin.
- Rules:
  - Only one agent touches a link at a time. Halt the cores before poking UCIe registers from TSI
    or JTAG.
  - Never access a link's `0x0000`–`0x7FFF` window until that link's `UcieClkRegs` checks pass
    (`phytest_bringup.md`, sections 3 and 4).

---

## 3. Clocks that come from bumps

Each link has its own PHY clock bumps, under `c2c_uciek.phy`.

| Bump (per link) | What it feeds | When it must be driven | Harness (`IrisSpec`) |
|---|---|---|---|
| `phy.bypassClk` | The UCIe main clock while `mainClkSel` = 3 (reset). Also source 2 for the PHY test and sideband dividers. ≤ 16 GHz. | Always, for the reset clocking. Sets the TX line rate: lane rate = 2 × bypass ÷ `txClkDiv`. | 8000 MHz (16 GT/s at the reset ÷1) |
| `phy.digitalBypassClk` | The PHY test clock while `phyTestClkBypassEn` = 1 (reset). Runs the PhyTest register block, PhyTest and every RX AFE controller. | **Before any access to the link's PhyTest window**, unless software first moves the PHY test clock onto a PLL. Must be faster than every packet clock in use (`phytest_bringup.md` §1.3). | 500 MHz. This **equals** the 16 GT/s packet clock (8 GHz/16): no margin. The UCIe repo's own harness uses 800 MHz. |
| `phy.sidebandBypassClk` | The TX sideband clock while `sbClkBypassEn` = 1 (reset). Runs the UCIe controller, its registers and every sideband serializer. | Before any access to the controller window, or any sideband use. | 800 MHz |
| `phy.refClk` | The 100 MHz reference for PLL8 (×80) and PLL12 (×120). | Only when a PLL is used. | 100 MHz |
| `phy.rxClkP`, `phy.rxClkN`, `phy.sbRxClk` | Forwarded clocks from the partner (or the same link, looped back). | n/a | Looped back or ringed (section 4) |

Chip-level clock pins are separate from the UCIe ones:
- `clock` feeds every SoC clock group through the programmable `TLClockDivider`, including the
  UCIe chip clock.
- `serial_tl.clock_in` clocks the serial TileLink port.
- `jtag.TCK` clocks JTAG.
- `clock_tap` is an output tapping an SoC clock, not a UCIe clock.

**Bench notes for Iris:**
- At the reset clocking, supply `bypassClk`, `digitalBypassClk` and `sidebandBypassClk` on every
  link you bring up. The PLLs need `refClk` instead. Software moves onto them through
  `UcieClkRegs`, which is safe while the link is still in reset.
- With an 8 GHz bypass at ÷1 the packet clock is 500 MHz, so an 800 MHz `digitalBypassClk` covers
  up to 24 GT/s. Otherwise switch the PHY test clock to PLL8 ÷8 (1 GHz).
- Bring up first at ÷4 (4 GT/s off an 8 GHz bypass), not on a slow bypass clock
  (`phytest_bringup.md` §1.2 and step 5).
- PhyTest never needs the sideband clock. But an unclocked sideband domain wedges the bus if
  anything touches the controller window (`0x203C00`/`0x20BC00` and up).

---

## 4. Bump and link mapping

- `c2c_ucie0` is `system.d2d_port_ios(0)`, chiplet-router port 0: the `UcieTL` at `0x200000`.
- `c2c_ucie1` is `system.d2d_port_ios(1)`, port 1: the `UcieTL_1` at `0x208000`.

Port order is the order of `ChipletRoutingParams.ports`.

Each `c2c_uciek` has two parts:
- `phy`: `txData[16]`, `txValid`, `txTrack`, `txClkP`, `txClkN`, `rxData[16]`, `rxValid`,
  `rxTrack`, `rxClkP`, `rxClkN`, `sbTxClk`, `sbTxData`, `sbRxClk`, `sbRxData`, plus the four clock
  inputs of section 3.
- `debug`: the observation outputs `txClk`, `rxClk`, `rxData`, `clkMux` and `txData`. Each is off
  until software enables its driver.

Chisel flattens the names with underscores, for example `c2c_ucie0_phy_bypassClk`.

**Lane order on the bumps** is PHY lane order: `txData[i]`/`rxData[i]` are data lanes 0-15. Valid,
track and the forwarded clock have their own bumps.

**Harness connectivity** (`test/src/IrisSpec.scala`, `connectRingLink` and the code around it):
- **One chip:** each link loops onto itself (`UcieBumpsIO.loopback`): TX data, valid, track, the
  forwarded clock and the sideband go to the same link's RX. `run_lfsr(base_k, n)` is
  self-consistent there.
- **N chips:** a ring. Chip `i`'s `c2c_ucie1` connects to chip `i+1`'s `c2c_ucie0`, all signals,
  both ways (`UcieBumpsIO.connect`). With two chips, chip 0 link 1 ↔ chip 1 link 0 and chip 1
  link 1 ↔ chip 0 link 0. Each die's TX goes to the *other* die, so its RX must be armed before the
  other die transmits (`phytest_bringup.md` step 7, topology B).

**Routing.** `program_router(entry, chip_id, port)` sends a chip ID to port 0 or 1
(`software/router.h`). In a two-chip ring both ports reach the other chip, so either works.

---

## 5. Building and running bring-up programs

- **Toolchain.** Install `riscv64-unknown-elf-gcc`, and set `RISCV` for the simulator build
  (`test/src/Utils.scala`).
- **Build every program** (`PROGRAMS` in `software/Makefile`): `make -C software`. They link
  against `htif_nano.specs`, so `printf` and the exit code go over HTIF and need a host serving
  it: SimTSI in simulation, the lab TSI host on silicon.
- **Regenerate `software/ucie.h`:** `make -C software ucie.h` or
  `./mill runMain edu.berkeley.cs.iris.GenUcieHeader software/ucie.h`.
  It uses mill, so don't run it during another mill build in the same checkout. It refuses to emit
  a header if the two links' register layouts differ.
- **Run in simulation (VCS):** `./mill test.testOnly edu.berkeley.cs.iris.IrisSpec -- -z "<test name>"`.
  `Utils.simulateTopWithBinaries` builds the harness, compiles VCS and runs the binaries.
  `debug = true` dumps FSDB.
- **Run on silicon.** The TSI host loads the ELF into off-chip memory over `serial_tl`. The chip
  boots from the boot ROM at `0x10000` and the boot-address register.
  The host services HTIF. The bench-side procedure isn't in this repository.
- **Existing UCIe programs:**
  - `software/ucie-loopback.c` runs `setup_ucie` on both links, puts both mainbands in TileLink
    mode, and does remote reads and writes through the router. It tests the TileLink path, not
    PhyTest, and needs both links working.
  - `software/ucie-digital-loopback.c` runs `setup_ucie_digital` on both links. That aligns each
    RX's words to its own TX's through PhyTest (`align_rx_words`, `ucie/docs/phytest_bringup.md`
    §8.4), prints the RX UI delay it set, then trains the UCIe controller. It spins forever if
    link training never completes. On the harness both links delay their RX lanes 20 UI, train,
    and carry remote reads and writes.
  - Neither is a first-silicon program.
  - The PhyTest programs `software/phytest-loopback.c`, `phytest-clocks.c` and `phytest-two-chip.c`
    (shared helpers in `software/phytest.h`) are in section 7.

---

## 6. Iris-specific bring-up notes

- **One link at a time.** The two links share the chip clock, the chip reset and the control bus.
  Their clocking, resets and `UcieClkRegs` are separate, but a hang on either link's registers
  blocks both (section 2).
- **Per link, the order is:**
  1. Ping `UcieClkRegs` (base + `0x10000`).
  2. Configure clocks if not on the bypass pins.
  3. Check that `UCIE_CLK_METER_PHY_TEST` and `UCIE_CLK_METER_SB` read nonzero, then write
     `ucieRst` = 0.
  4. A single read of `UCIE_TX_TEST_STATE`.
  5. `setup_ucie`, which turns on the TX and RX clocks (`UCIE_CLK_EN`, `UCIE_RX_CLK_EN`), then
     the overrides in `phytest_bringup.md` step 4.
  6. Set the rate and phase, and check `UCIE_CLK_METER_TX_DIV` and `_RX_DIV`.
  7. Run the on-chip loopback lane.
  8. Run the mainband LFSR, after an injected error has shown the counters count.
  9. Check the sideband with the sideband tester (`phytest_bringup.md` step 10).
  10. Run gated-clock bursts at the UCIe controller's postamble (step 11).
  11. Hand the link to the UCIe controller (step 12): `setup_ucie_digital` on a link looped back
      onto itself; by hand between two chips, since `align_rx_words` can't measure across them.
- **Lessons from Kodiak, the previous chip** (`phytest_bringup.md` steps 4-9). The generated
  helpers already apply them: observation pad drivers off during link tests (`setup_ucie` leaves
  them off; `enable_debug_drivers` turns them on for a scope), a distinct LFSR seed per lane
  (`seed_lfsrs`), and `run_lfsr` waiting until the packet count stops climbing. Kodiak's RX
  hand-over and vref settings don't apply: Iris's RX front end is different.
- **Physical design and lab checks this page can't settle:**
  - IrisConfig emits the UCIe behavioral models (`includeDefaultModels = true`). Confirm the PD
    flow replaces the analog ones with the macros.
  - `digitalBypassClk`, `sidebandBypassClk` and `refClk` have no ESD cell in the UCIe RTL.
    Confirm the pad ring provides one.
  - Confirm with analog what termination `zen` = 1 with `zctl` = 0 gives (`phytest_bringup.md`
    step 4).

---

## 7. Simulation tests and bring-up programs

The PhyTest bring-up programs in `software/` are the same in simulation and in the lab. They run
on the core and reach both links at their real MMIO bases. Each check prints what it expected and
what it read instead of stopping. The program exits nonzero if any check failed.

| Program | Topology | What it covers |
|---|---|---|
| `phytest-loopback.c` | each link looped back onto itself (the single-chip harness, or a loopback board) | On both links: clock meters against the board clocks; LFSR run with distinct per-lane seeds; injected errors counted exactly once; `rxSelfSync` with every RX seed wrong; valid moved onto track at both ends; gated-clock bursts the UCIe way after the training in `phytest_bringup.md` §8.3, parking alternately and always low, with the forwarded clock centred (`PT_CENTRED_DELAY_TAPS` in `phytest.h`: 29 on the harness, the trained code on silicon); no RX words while parked; manual pattern into the capture SRAM; error-triggered capture; `rxStartMode` immediate; track counters; the loopback lane and its FIFO flags; per-lane fixed and inverted sources; one injected error found by lane, UI and class; the observation bumps (on for that test only) and the TX debug lane; the sideband tester |
| `phytest-clocks.c` | UCIe0 looped back | TX divider at /1, /2, /4 off the bypass pin; PHY test clock onto PLL8 /8, stopping the program (instead of hanging the bus) if a moved clock doesn't run; main clock onto PLL8 and PLL12; a PHY test clock too slow for the link, which must set both FIFO flags (below), and the recovery; TX queue capped at 8 words with the PHY test clock at twice the packet clock (at equal rates 8 runs dry and 10 holds); sideband clock onto PLL8 /10; every clock meter after every move, and no PLL lock lost |
| `phytest-two-chip.c` | two chips, each one's UCIe1 wired to the other's UCIe0 (the two-chip ring, or two chips on a package) | an LFSR run each way; each RX hears only the TX wired to it; `rxManualCheck` scoring the partner's manual pattern against this chip's TX pattern SRAM, with one reference word deliberately wrong |

**A slow PHY test clock starves this die's TX and overflows its RX.** When a run's next data word
isn't queued in time, `TxClkGater` sends an idle word instead. The receiving RX then gets an idle
word mid-run, or with the clock gated, the burst split in two. With the clock running, the RX still
takes a word every packet clock, more than a slow PHY test clock drains. On a looped-back link,
`UCIE_TX_TEST_FIFO_UNDERFLOW` and `UCIE_RX_TEST_FIFO_OVERFLOW` both set. Between two dies, the RX
overflow shows on the die with the slower PHY test clock, whichever die sends.

Every wait in `phytest.h` is bounded in core cycles (`PT_TIMEOUT`). A bound counted in MMIO reads
is far too short on silicon next to a slow divided clock.

### How the two-chip program keeps in step

Each chip arms the RX it is about to be sent to and tells its partner with a sideband packet on
that link. It starts its own TX only after the partner has said the same of the RX that TX feeds.
The chips need no host or shared clock, but they do need to start close together:
- Each message is sent once. One sent before the partner has set up that link (still in `ucieRst`,
  or before `pt_sb_reset`) is lost, and nothing resends it.
- A chip waits `PT_TIMEOUT` (a few ms) for each message, then carries on alone.

So start both chips within a few milliseconds of each other. On the bench, release both resets
together after both binaries are loaded.

### Running them in simulation

```bash
export RISCV=<a Chipyard RISC-V toolchain with libfesvr, libriscv, libdramsim>
export PATH=$RISCV/bin:$PATH
make -C software phytest-loopback.riscv phytest-clocks.riscv phytest-two-chip.riscv
./mill test.testOnly edu.berkeley.cs.iris.IrisSpec -- -z "should run phytest"
```

VCS compiles its own C with the host `gcc`. GCC 14 rejects it
(`rmapats.c: implicit declaration of function`), so put GCC 13 or older first on `PATH` for the
sim.

The tests run on `IrisConfig(sim = true)` unless noted:
- `should run phytest loopback test`. It also checks that every observation bump of both links
  toggled while the program had them on.
- `should run phytest loopback test on the tapeout config`: `IrisConfig(sim = false)`, serial TL at
  100 MHz, the netlist's configuration.
- `should run phytest clocks test`.
- `should run phytest two-chip test`, on two chips.

### What simulation does not show

- The lanes are the behavioral models (`includeDefaultModels`): a shift register per lane. No
  code, termination or delay setting makes a lane fail. The AMS models in `ucie/verilog` cover
  that.
- The harness clocks are fixed (section 3): 8 GHz main bypass, 500 MHz PHY test bypass, 800 MHz
  sideband bypass, 100 MHz PLL reference.
- The observation bumps are only checked for toggling, not for what they carry.
