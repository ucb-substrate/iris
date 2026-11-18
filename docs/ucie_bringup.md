# UCIe bring-up on Iris

Iris has two UCIe links. This page covers what's specific to Iris: where the links sit in the
address map, how software reaches them, which clocks come in on bumps, and how bumps map to links.

How to clock, reset and bring up PhyTest, the IP's link tester, is in the UCIe IP's own guide:
[`ucie/docs/phytest_bringup.md`](../ucie/docs/phytest_bringup.md). Read section 3 of that guide
("The register port, and how not to hang it") before touching a link.

File citations are `file:line` from the repository root. `ucie/...` is the UCIe submodule.
`IrisSpec.scala` is being edited, so its line numbers are for the working tree on 2026-11-10.

---

## 1. The two links

Both links are chiplet-router ports (`src/Iris.scala:553-578`). Their `UcieTLParams` are identical
apart from `address` and `moduleId`:
- `numLanes` = 16
- `managerWhere` = SBUS
- `includeDefaultModels` = true
- `sim` follows the config

`UcieComplexPort` puts both `UcieChipletLink`s inside one module, `UcieComplex` (instance
`d2d_ports`), so physical design can harden them together (`src/UcieComplex.scala:15-67`).
`moduleId` 1 gives the second link its own module names, `UcieTL_1` and `UcieChipletLink_1`
(`ucie/scala/src/tilelink/TileLink.scala:95-106`).

| | Link 0 | Link 1 |
|---|---|---|
| IrisTop IO | `c2c_ucie0` | `c2c_ucie1` |
| Chiplet-router port (and `program_router` `port` argument) | 0 | 1 |
| `UcieTLParams.address` (`base`) | `0x200000` | `0x208000` |
| PhyTest and lane registers (`UcieTLRegs`, PHY test clock) | `0x200000`–`0x203BFF` | `0x208000`–`0x20BBFF` |
| Controller controls and UCIe spec registers (`UcieController`, TX sideband clock) | `0x203C00`–`0x207FFF` | `0x20BC00`–`0x20FFFF` |
| Clock and reset registers (`UcieClkRegs`, chip clock) | `0x210000`–`0x210FFF` | `0x218000`–`0x218FFF` |
| Module (`moduleId`) | `UcieTL` (0) | `UcieTL_1` (1) |

The windows come from `UcieTLRegs.regionSize` = 0x3C00, `UcieController.ctrlBase`/`specBase` =
0x3C00/0x4000 with a 0x4000 spec region, and `UcieClkRegs.offset` = 0x10000 with a 0x1000 region
(`ucie/scala/src/tilelink/TileLink.scala:225,269,993,1695-1704`;
`ucie/scala/src/uciedigital/regs/UcieRegParams.scala:49-50,127-160`). The two links' windows don't
overlap.

Everything outside the windows is unmapped. That includes `0x211000`–`0x217FFF` and `0x219000`
upwards. Iris removed the error device from every bus (`src/Iris.scala:268-280`), so an access to
an unmapped address doesn't return an error. **It hangs** (inference: nothing on the bus can answer
it).

### Registers you'll touch first

Offsets come from `software/ucie.h`, regenerated for IrisConfig in the working tree. Regenerate it
whenever the `ucie` submodule moves; the header is the authority.

| Register | Offset | Link 0 | Link 1 | Safe before `ucieRst` = 0? |
|---|---|---|---|---|
| `UCIE_MAIN_CLK_SEL` | `0x10000` | `0x210000` | `0x218000` | yes |
| `UCIE_PHY_TEST_CLK_BYPASS_EN` | `0x10038` | `0x210038` | `0x218038` | yes |
| `UCIE_CLK_EN` / `UCIE_RX_CLK_EN` | `0x10048` / `0x10050` | `0x210048` / `0x210050` | `0x218048` / `0x218050` | yes |
| `UCIE_CLK_CFG_BUSY` | `0x10088` | `0x210088` | `0x218088` | yes |
| `UCIE_CLK_CFG_UNGATE_REQ` | `0x100A0` | `0x2100A0` | `0x2180A0` | yes |
| `UCIE_PLL_LOCK_OBSERVED` | `0x100A8` | `0x2100A8` | `0x2180A8` | yes |
| `UCIE_UCIE_RST` (reset value 1) | `0x100B0` | `0x2100B0` | `0x2180B0` | yes |
| `UCIE_PHY_TEST_CLK_EN` | `0x100D8` | `0x2100D8` | `0x2180D8` | yes |
| `UCIE_TX_TEST_STATE` | `0x128` | `0x200128` | `0x208128` | **no**: needs `ucieRst` = 0 and a running PHY test clock |
| `UCIE_RX_IDLE_WORDS_OBSERVED` | `0x3C8` | `0x2003C8` | `0x2083C8` | no |
| `UCIE_TX_TEST_FIFO_UNDERFLOW` / `UCIE_RX_TEST_FIFO_OVERFLOW` | `0x3868` / `0x3870` | `0x203868` / `0x203870` | `0x20B868` / `0x20B870` | no |

Other Iris addresses that bring-up programs use (`software/router.h`; `src/Iris.scala:555`):
- the chiplet-router table at `0x4000`, 32 bytes per entry, 4 entries;
- the chip ID register at `0x4080`.

---

## 2. How software reaches the registers

Each link's register port hangs off the **control bus**, through `TLFragmenter(cbus)`, a `TLBuffer`
and a `TLWidthWidget(cbus.beatBytes)` (`src/Iris.scala:102-114`).
- The router can't attach the port itself: it takes a bare register node, and the port is a
  crossbar in front of three blocks (`ucie/scala/src/tilelink/TileLink.scala:2635-2643`).
- The link's chip clock is the `d2d` clock group. That group is combined into `uncore` with `cbus`,
  `sbus`, `pbus` and the other buses, all at 500 MHz (`src/Iris.scala:391-406,453-457`). So the
  port needs no clock crossing.

There are three ways in:

1. **The cores.** C programs include `software/ucie.h`. Every helper takes `base`, `0x200000` or
   `0x208000`, and uses `reg_write64`/`reg_read64` (`software/mmio.h`).
2. **TSI over serial TileLink.**
   - IrisTop's `serial_tl` port is a `DecoupledExternalSyncPhitIO`: 16-bit phits on the tapeout
     config, 32-bit with `sim` (`src/Iris.scala:320-354`). It's clocked by `serial_tl.clock_in`
     from outside.
   - Its client side masters the front bus (`SerialTLClientParams` defaults `masterWhere` to FBUS:
     `testchipip/src/main/scala/serdes/PeripheryTLSerial.scala:47-52`). From there a request goes
     FBUS → SBUS → CBUS → the UCIe register port.
   - Its manager side serves off-chip memory at `0x8000_0000`, 4 GiB, on the memory bus.
   - The host-side `TSIToTileLink` issues a single 8-byte Get or Put only for an 8-byte-aligned,
     two-word request (`testchipip/src/main/scala/tsi/TSIToTileLink.scala:78-92`). Use exactly that
     for UCIe registers.
3. **JTAG.**
   - `WithJtagDTM` and `WithDebugSBA` give a debug module with system-bus access
     (`src/Iris.scala:414-421`). The access width is 64 bits, because the base config uses
     `DefaultDebugModuleParams(64)` (`rocket-chip/src/main/scala/subsystem/Configs.scala:59`).
   - The bus master is on the front bus (`debug[0]` in the NoC map, `src/Iris.scala:497`). Use
     64-bit accesses, such as OpenOCD `write_memory`/`read_memory` with width 64.
   - Abstract commands through a halted hart also work, as long as the control bus isn't wedged.

**Use 64-bit accesses.**
- Writes to fields wider than 32 bits (`txctl_N_tile`, `clkPhaseSel`, seeds, packet counts,
  `txDataChunkIn0/1`, `sbTxPacket`) are ignored silently unless the whole field is in the byte mask
  (`rocket-chip/src/main/scala/regmapper/RegMapper.scala:146`).
- A 32-bit read of a 64-bit counter returns half of it.

**Hang hazards, and recovery on Iris.**
- These hang a request forever:
  - an access to a link's PhyTest or controller window while that domain is in reset (`ucieRst` = 1
    is the power-on state) or unclocked;
  - an access to an unmapped address;
  - an access to a remote address (the off-chip range at `0x8_0000_0000`, `src/Iris.scala:320`)
    while the link isn't carrying TileLink.
- A hung access to a UCIe register sits on the control bus. That's inferred from the bus structure.
  It blocks:
  - both links' registers;
  - the router table and chip ID;
  - the clock divider;
  - CLINT, PLIC and the boot ROM;
  - the debug module's hart-facing ROM;
  - the periphery bus with the UART.
- TSI and JTAG system-bus access can still reach memory.
- IrisTop doesn't consume the debug module's `ndreset` (`src/Iris.scala:130-149`), so JTAG can't
  reset the chip. **Only `reset` recovers.** Plan the bench so you can drive Iris's reset pin from
  the host.
- Rules:
  - Only one agent touches a link at a time. Halt the cores before poking UCIe registers from TSI
    or JTAG.
  - Never access a link's `0x0000`–`0x7FFF` window until that link's `UcieClkRegs` checks pass
    (`phytest_bringup.md`, sections 3 and 4).

---

## 3. Clocks that come from bumps

Each link has its own set of PHY clock bumps, all under `c2c_uciek.phy`. The bump bundle is
`UcieBumpsIO` (`ucie/scala/src/tilelink/TileLink.scala:121-170`); its PHY part is `PhyBumpsIO`
(`ucie/scala/src/phy/Phy.scala:172-194`). The Iris comment at `src/Iris.scala:558-562` notes the
per-link bypass bumps.

| Bump (per link) | What it feeds | When it must be driven | Harness (`test/src/IrisSpec.scala:203-208`) |
|---|---|---|---|
| `phy.bypassClk` | The analog bypass input: ESD, then a clock receiver, then the clocking tile (`Phy.scala:405-411,457-461`). It's the UCIe main clock while `mainClkSel` = 3 (reset). It can also be the source the PHY test and sideband dividers divide (source 2). ≤ 16 GHz. | Always for the reset clocking. It sets the TX line rate: lane rate = 2 × bypass ÷ `txClkDiv`. | 8000 MHz (16 GT/s at the reset ÷1) |
| `phy.digitalBypassClk` | The PHY test clock while `phyTestClkBypassEn` = 1 (reset). That clock runs the PhyTest register block, PhyTest itself and every RX AFE controller. | **Before any access to the link's PhyTest window**, unless software first moves the PHY test clock onto a PLL. It must be faster than every packet clock in use (`phytest_bringup.md` §1.3). | 500 MHz. This **equals** the 16 GT/s packet clock (8 GHz/16), so there's no margin; the UCIe repo's own harness uses 800 MHz. |
| `phy.sidebandBypassClk` | The TX sideband clock while `sbClkBypassEn` = 1 (reset). That clock runs the UCIe controller, its registers, and every sideband serializer. | Before any access to the controller window, or any sideband use. | 800 MHz |
| `phy.refClk` | The 100 MHz reference for PLL8 (×80) and PLL12 (×120). | Only when a PLL is used. | 100 MHz |
| `phy.rxClkP`, `phy.rxClkN`, `phy.sbRxClk` | Forwarded clocks from the partner (or the same link, looped back). | n/a | Looped back or ringed (section 4) |

Chip-level clock pins are separate from the UCIe ones:
- `clock` feeds every SoC clock group through the programmable `TLClockDivider` (`src/Iris.scala:88-94,118-128`). That includes the UCIe chip clock, through the `uncore` group.
- `serial_tl.clock_in` clocks the serial TileLink port.
- `jtag.TCK` clocks JTAG.
- `clock_tap` is an output tapping an SoC clock (`src/CanHaveClockTap.scala`). It isn't a UCIe clock.

**Bench notes for Iris:**
- To run the link at the reset clocking, supply `bypassClk`, `digitalBypassClk` and
  `sidebandBypassClk` on every link you bring up. Moving onto the PLLs needs `refClk` instead, and
  is done through `UcieClkRegs`, which is safe while the link is still in reset.
- Run `digitalBypassClk` faster than the packet clock. With the harness's 8 GHz bypass at ÷1, the
  packet clock is 500 MHz, so 800 MHz suits up to 24 GT/s. Otherwise switch the PHY test clock to
  PLL8 ÷8 (1 GHz).
- Bring up first at ÷4 (4 GT/s off an 8 GHz bypass), not on a slow bypass clock
  (`phytest_bringup.md` §1.2 and step 5).
- PhyTest never needs the sideband clock, but an unclocked sideband domain wedges the bus if
  anything touches the controller window (`0x203C00`/`0x20BC00` and up).

---

## 4. Bump and link mapping

- `c2c_ucie0` is `system.d2d_port_ios(0)`, chiplet-router port 0: the `UcieTL` at `0x200000`.
- `c2c_ucie1` is `system.d2d_port_ios(1)`, port 1: the `UcieTL_1` at `0x208000`.

Port order is the order of `ChipletRoutingParams.ports` (`src/Iris.scala:170-175,554-578`;
`testchipip/src/main/scala/soc/OffchipRouter.scala:250-333`).

Each `c2c_uciek` has two parts:
- `phy`: `txData[16]`, `txValid`, `txTrack`, `txClkP`, `txClkN`, `rxData[16]`, `rxValid`,
  `rxTrack`, `rxClkP`, `rxClkN`, `sbTxClk`, `sbTxData`, `sbRxClk`, `sbRxData`, plus the four clock
  inputs of section 3.
- `debug`: the observation outputs `txClk`, `rxClk`, `rxData`, `clkMux` and `txData`
  (`ucie/scala/src/phytest/PhyTest.scala:547-553`). Every one is off until software enables its
  driver.

Chisel flattens the names with underscores, for example `c2c_ucie0_phy_bypassClk`.

**Lane order on the bumps** is PHY lane order: `txData[i]`/`rxData[i]` are data lanes 0-15. Valid,
track and the forwarded clock have their own bumps (`ucie/scala/src/phy/Phy.scala:572-585,633-639`).

**Harness connectivity** (`test/src/IrisSpec.scala`, `connectRingLink` and the code around it):
- **One chip:** each link is looped onto itself by `UcieBumpsIO.loopback`. TX data, valid, track,
  the forwarded clock and the sideband all go to the same link's RX. A test on link `k` exercises
  `k`'s TX and `k`'s RX together, and `run_lfsr(base_k, n)` is self-consistent there.
- **N chips:** a ring. Chip `i`'s `c2c_ucie1` connects to chip `i+1`'s `c2c_ucie0`, through
  `UcieBumpsIO.connect`: data, valid, track, forwarded clock and sideband both ways. With two chips,
  chip 0 link 1 ↔ chip 1 link 0 and chip 1 link 1 ↔ chip 0 link 0. Each die's TX then lands on the
  *other* die, so its RX has to be armed before the other die transmits
  (`phytest_bringup.md` step 7, topology B).

**Routing.** `program_router(entry, chip_id, port)` sends a chip ID to port 0 or 1
(`software/router.h`). In a two-chip ring both ports reach the other chip, so either works.

---

## 5. Building and running bring-up programs

- **Toolchain.** Install `riscv64-unknown-elf-gcc`, and set `RISCV` for the simulator build
  (`test/src/Utils.scala`).
- **Build every program.** `make -C software`, as listed in `PROGRAMS` in `software/Makefile`. They
  link against `htif_nano.specs`, so `printf` and the exit code go over HTIF and need a host
  serving it: SimTSI in simulation, the lab TSI host on silicon.
- **Regenerate `software/ucie.h`.** Either `make -C software ucie.h` or
  `./mill runMain edu.berkeley.cs.iris.GenUcieHeader software/ucie.h` (`src/GenUcieHeader.scala`).
  It uses mill, so don't run it while another mill build is running in the same checkout.
  `GenUcieHeader` refuses to emit a header if the two links' register layouts ever differ
  (`src/GenUcieHeader.scala:28-48`).
- **Run in simulation (VCS).** `./mill test.testOnly edu.berkeley.cs.iris.IrisSpec -- -z "<test name>"`.
  `Utils.simulateTopWithBinaries` builds the harness, compiles VCS and runs the binaries. `fast`
  mode preloads through `FastRAM` and `+loadmem`, and `debug = true` dumps FSDB.
- **Run on silicon.**
  - The TSI host loads the ELF into off-chip memory over `serial_tl`.
  - The chip boots from the boot ROM at `0x10000` and the boot-address register
    (`src/Iris.scala:424-444`).
  - The host services HTIF.
  - The bench-side procedure isn't in this repository.
- **Existing UCIe programs, and what they exercise.**
  - `software/ucie-loopback.c` runs `setup_ucie` on both links, puts both mainbands in TileLink
    mode, and does remote reads and writes through the router. It tests the TileLink path, not
    PhyTest, and needs both links working.
  - `software/ucie-digital-loopback.c` runs `setup_ucie_digital` on both links, which first lines
    each RX's words up with its own TX's through PhyTest (`align_rx_words`,
    `ucie/docs/phytest_bringup.md` §8.4) and prints the RX UI delay that set, then trains the UCIe
    controller.
    It spins forever if link training never completes. On the harness both links hold their RX
    lanes back 20 UI, train, and carry remote reads and writes.
  - Neither is a first-silicon program.
  - The PhyTest programs `software/phytest-loopback.c`, `phytest-clocks.c` and `phytest-two-chip.c`
    (shared helpers in `software/phytest.h`) are described in section 7.

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
  5. `setup_ucie`, then the overrides in `phytest_bringup.md` step 4.
  6. Set the rate and phase, and check `UCIE_CLK_METER_TX_DIV` and `_RX_DIV`.
  7. Run the on-chip loopback lane.
  8. Run the mainband LFSR, after an injected error has shown the counters count.
  9. Check the sideband with the sideband tester (`phytest_bringup.md` step 10).
  10. Run gated-clock bursts at the UCIe controller's postamble (step 11).
  11. Hand the link to the UCIe controller (step 12): `setup_ucie_digital` on a link looped back
      onto itself; by hand between two chips, since `align_rx_words` can't measure across them.
- **Lessons from Kodiak, the previous chip** (`phytest_bringup.md` steps 4-9), as the generated
  helpers now apply them:
  - Link tests run with the observation pad drivers off: `setup_ucie` leaves them off, and
    `enable_debug_drivers` turns them on for a scope.
  - Every lane has its own LFSR seed, out of reset and from `seed_lfsrs`.
  - `run_lfsr` waits until the packet count stops climbing, not for a fixed number of reads, and
    says when it comes back short.
  - Iris's RX front end is not Kodiak's, so Kodiak's hand-over and vref settings don't carry over;
    those defaults will change with the AFE's own controller.
- **Physical design and lab checks this page can't settle:**
  - IrisConfig emits the UCIe behavioral models (`includeDefaultModels = true`,
    `src/Iris.scala:567,574`). Confirm the PD flow replaces the analog ones with the macros.
  - `digitalBypassClk`, `sidebandBypassClk` and `refClk` have no ESD cell in the UCIe RTL
    (`ucie/scala/src/phy/Phy.scala:405-411,432-433,459`). Confirm the pad ring provides one.
  - Confirm with analog what termination `zen` = 1 with `zctl` = 0 gives (`phytest_bringup.md`
    step 4).

---

## 7. Simulation tests and bring-up programs

The PhyTest bring-up programs in `software/` are the same programs in
simulation and in the lab. They run on the core, reach both UCIe links over
the SoC bus at their real MMIO bases, and go through the real register map,
clock-domain crossings, clocking tile and lane models. Each check prints
what it expected and what it read rather than stopping, and the program exits
nonzero if any check failed.

| Program | Topology | What it covers |
|---|---|---|
| `phytest-loopback.c` | each link's bumps looped back onto itself (the single-chip harness, or a loopback board) | on both links: the clock meters against the board clocks; LFSR run with distinct per-lane seeds; injected errors counted exactly once under `rxPauseCounters`; `rxSelfSync` with every RX seed wrong; valid moved onto track at both ends; gated-clock bursts the UCIe way, at the reset values (no preamble, a packet of postamble, the clock parked high and low in turn, then always low), with the forwarded clock centred (`PT_CENTRED_DELAY_TAPS` in `phytest.h`, 29 on the harness; the trained code on silicon) and put back after, after realigning the RX dividers behind the parked clock and one 40 UI training gap, with `rxBurstMode` (and the RX seeing no words while the clock is parked); manual pattern into the capture SRAM, and `rxCaptureStart` past the first lap; error-triggered capture with twin lanes; `rxStartMode` immediate; track counters, ones/zeros/even ones; the loopback lane and its two FIFO flags; per-lane fixed and inverted sources, clean when the RX expects them and every bit wrong, half on each edge, when it does not; one injected error found by lane, UI and class; the observation bumps (on for that test alone) with the TX debug lane; the sideband tester |
| `phytest-clocks.c` | UCIe0 looped back | the TX divider at /1, /2, /4 off the bypass pin; the PHY test clock onto PLL8 /8; the main clock onto PLL8 and PLL12; a PHY test clock too slow for the link, which has to set both the TX FIFO underflow flag and the RX overflow flag (below), with the two packet clocks metering the same, and the recovery; the TX queue capped at 8 words, below its reset value of 16, with the PHY test clock at twice the packet clock (at equal rates 8 runs the queue dry and 10 is the least that holds); the sideband clock onto PLL8 /10; every clock meter after every move, and no PLL lock lost |
| `phytest-two-chip.c` | two chips, UCIe1 of each wired to UCIe0 of the other (the two-chip ring, or two chips on a package) | an LFSR run each way; that each RX hears only the TX it is wired to; `rxManualCheck` scoring the partner's manual pattern against this chip's TX pattern SRAM, with one reference word deliberately wrong |

**A slow PHY test clock starves this die's TX and overflows its RX.** Where the
next data word of a run isn't queued in time, `TxClkGater` at the PHY end sends
an idle word instead (`ucie/scala/src/tilelink/TileLink.scala:2716-2779`), so
the receiving RX gets an idle word in the middle of the run, or with the clock
gated, the burst split in two. With the clock running, the RX still takes a
word every packet clock, more than a slow PHY test clock drains. On a
looped-back link that means `UCIE_TX_TEST_FIFO_UNDERFLOW` and
`UCIE_RX_TEST_FIFO_OVERFLOW` both set. Between two dies, the RX overflow shows
on the die with the slower PHY test clock, whichever one is sending.

`setup_ucie` leaves the observation drivers off, so a program that wants a scope on them calls
`enable_debug_drivers(base, 1)` first; `phytest-loopback.c` does it for one test.

`phytest.h` holds the helpers they share. Every wait is bounded in core
cycles (`PT_TIMEOUT`), because a bound counted in MMIO reads is far too short
on silicon next to a slow divided clock.

### How the two-chip program keeps in step

Each chip arms the RX it is about to be sent to, says so to its partner as a
sideband packet on that link, and only starts its own TX once the partner has
said the same of the RX that TX feeds. Both chips run the same binary and
need nothing else in common: not a host, not a shared clock, not matching
code timing. Run it on both chips at once in the lab.

### Running them in simulation

```bash
export RISCV=<a Chipyard RISC-V toolchain with libfesvr, libriscv, libdramsim>
export PATH=$RISCV/bin:$PATH
make -C software phytest-loopback.riscv phytest-clocks.riscv phytest-two-chip.riscv
./mill test.testOnly edu.berkeley.cs.iris.IrisSpec -- -z "should run phytest"
```

VCS compiles its own C with the host `gcc`; GCC 14 rejects it
(`rmapats.c: implicit declaration of function`), so put a GCC 13 or older
first on `PATH` for the sim.

The tests:

| Test | Config | Notes |
|---|---|---|
| `should run phytest loopback test` | `IrisConfig(sim = true)` | also checks that every observation bump of both links toggled, from `DebugBumpMonitor` in the harness, while the program had them on |
| `should run phytest loopback test on the tapeout config` | `IrisConfig(sim = false)`, serial TL at 100 MHz | the netlist's configuration |
| `should run phytest clocks test` | `IrisConfig(sim = true)` | |
| `should run phytest two-chip test` | two chips, `IrisConfig(sim = true)` | |

### What simulation does not show

- The lanes are the behavioral models (`includeDefaultModels`): a shift
  register per lane, so nothing about a code, a termination or a delay setting
  makes a lane fail. The AMS models in `ucie/verilog` are the tier for that.
- The harness clocks are fixed: an 8 GHz main bypass clock, a 500 MHz PHY test
  bypass clock, an 800 MHz sideband bypass clock and a 100 MHz PLL reference.
- The observation bumps are only checked for toggling, not for what they carry.
