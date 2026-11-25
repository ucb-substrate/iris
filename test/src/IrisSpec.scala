package edu.berkeley.cs.iris

import chisel3._
import chisel3.util._
import chisel3.experimental.BundleLiterals._

import org.chipsalliance.cde.config.Config
import org.scalatest.funspec.AnyFunSpec
import org.chipsalliance.diplomacy.lazymodule._
import org.chipsalliance.diplomacy._
import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.subsystem._
import freechips.rocketchip.prci._
import _root_.circt.stage.ChiselStage
import testchipip.uart.UARTAdapter
import freechips.rocketchip.jtag.JTAGIO
import freechips.rocketchip.util._
import freechips.rocketchip.devices.debug.SimJTAG
import edu.berkeley.cs.chippy.{SimTSI, TSIIO}
import testchipip.dram._
import testchipip.tsi.SerialRAM
import testchipip.dram.FastRAM
import edu.berkeley.cs.chippy.{SimTSI, TSIIO => ChippyTSIIO}
import testchipip.serdes.{SerialTLKey, CreditedSourceSyncPhitIO}
import testchipip.soc.{ChipletIO, ChipletRoutingKey}
import chisel3.simulator.stimulus.RunUntilSuccess
import chisel3.testing.HasTestingDirectory
import java.nio.file.Paths
import os.RelPath
import os.Path
import chisel3.experimental.dataview._

class ClockSourceIO extends Bundle {
  val power = Input(Bool())
  val gate = Input(Bool())
  val clk = Output(Clock())
}

class ClockSourceAtFreqMHz(val freqMHz: Double)
    extends BlackBox(
      Map(
        "PERIOD" -> DoubleParam(1000 / freqMHz)
      )
    )
    with HasBlackBoxInline {
  val io = IO(new ClockSourceIO)
  val moduleName = this.getClass.getSimpleName

  setInline(
    s"$moduleName.v",
    s"""
      |module $moduleName #(parameter PERIOD="") (
      |    input power,
      |    input gate,
      |    output clk);
      |  timeunit 1ns/1ps;
      |  reg clk_i = 1'b0;
      |  always #(PERIOD/2.0) clk_i = ~clk_i & (power & ~gate);
      |  assign clk = clk_i;
      |endmodule
      |""".stripMargin
  )
}

/** Counts transitions on a UCIe link's observation bumps and reports them at
  * the end of simulation. A test uses this to check that the bumps bring-up
  * probes with a scope are driven at the chip's pins.
  */
class DebugBumpMonitor(name: String)
    extends BlackBox(Map("NAME" -> StringParam(name)))
    with HasBlackBoxInline {
  val io = IO(new Bundle {
    val mux0 = Input(Bool())
    val mux1 = Input(Bool())
    val txData = Input(Bool())
  })
  setInline(
    "DebugBumpMonitor.sv",
    """module DebugBumpMonitor #(parameter NAME = "") (
      |    input mux0, input mux1, input txData);
      |  longint unsigned n_mux0 = 0, n_mux1 = 0, n_txData = 0;
      |  always @(mux0) n_mux0++;
      |  always @(mux1) n_mux1++;
      |  always @(txData) n_txData++;
      |  final $display("Debug bumps %s: mux0 %0d mux1 %0d txData %0d",
      |      NAME, n_mux0, n_mux1, n_txData);
      |endmodule
      |""".stripMargin
  )
}

/** Prints a clock's period whenever it settles on a new value: after three
  * consecutive posedge-to-posedge intervals of the same length, and only if
  * that length differs from the last one printed. Tests grep the log for the
  * `<name> period <ns> ns` lines.
  */
class ClockPeriodMonitor(name: String)
    extends BlackBox(Map("NAME" -> StringParam(name)))
    with HasBlackBoxInline {
  val io = IO(new Bundle { val clk = Input(Clock()) })
  setInline(
    "ClockPeriodMonitor.sv",
    """module ClockPeriodMonitor #(parameter string NAME = "clock") (input clk);
      |  timeunit 1ns/1ps;
      |  realtime last = 0.0, period = 0.0, reported = 0.0;
      |  integer stable = 0;
      |  always @(posedge clk) begin
      |    if (last > 0.0) begin
      |      if ($realtime - last == period) stable = stable + 1;
      |      else stable = 0;
      |      period = $realtime - last;
      |      if (stable >= 3 && period != reported) begin
      |        $display("[ClockPeriodMonitor] %s period %0.3f ns (at %0.3f ns)", NAME, period, $realtime);
      |        reported = period;
      |      end
      |    end
      |    last = $realtime;
      |  end
      |endmodule
      |""".stripMargin
  )
}

class SimTop(
    nChips: Int,
    binaryPaths: Seq[Path],
    plusArgs: Seq[Seq[String]] = Seq.empty,
    fast: Boolean = false,
    serialTLFreqMHz: Option[Double] = None,
    memTrafficMonitor: Boolean = false, // experiments only, see IrisExperiments.scala
    clockTapMonitor: Boolean = false
)(implicit
    p: Parameters
) extends RawModule {
  val driver = Module(new TestDriver)
  val harness = Module(
    new TestHarness(nChips, binaryPaths, plusArgs, fast, serialTLFreqMHz, clockTapMonitor)
  )
  if (memTrafficMonitor) Module(new MemTrafficMonitor)
  harness.io.reset := driver.reset
  driver.success := harness.io.success
}

class TestDriver extends ExtModule {
  val success = IO(Input(Bool()))
  val reset = IO(Output(Bool()))
  setInline(
    "TestDriver.v",
    """module TestDriver(
      | input success,
      | output reg reset
      |);
      |`ifdef DEBUG
      | reg [2047:0] fsdbfile = 0;
      |`endif
      | initial begin
      |`ifdef DEBUG
      |   if ($value$plusargs("fsdbfile=%s", fsdbfile)) begin
      |`ifdef FSDB
      |     $fsdbDumpfile(fsdbfile);
      |     $fsdbDumpvars(0, SimTop, "+all");
      |`else
      |     $fdisplay(32'h80000002, "Error: +fsdbfile passed but compile did not enable +define+FSDB");
      |     $fatal;
      |`endif
      |   end
      |`endif
      |   $display("Resetting chip for 10 ns");
      |   reset = 1'b1;
      |   #10;
      |   reset = 1'b0;
      |   $display("Running test for 1000 ns");
      |   #1000000000;
      |   $display("Test timed out!");
      |   $fatal;
      | end
      | always @(posedge success) begin
      |   if (!reset) begin
      |     $display("Test completed successfully.");
      |`ifdef DEBUG
      |`ifdef FSDB
      |     $fsdbDumpoff;
      |`endif
      |`endif
      |     $finish;
      |   end
      | end
      |endmodule
    """.stripMargin
  )
}

/** @param serialTLFreqMHz
  *   clock for the serial-TL link and the off-chip memory model behind it. None
  *   keeps the link on the 500 MHz chip clock; on silicon it is an external
  *   clock, typically much slower than the core.
  * @param clockTapMonitor
  *   attach a [[ClockPeriodMonitor]] to each chip's `clock_tap` output, which
  *   is otherwise left unconnected.
  */
class TestHarness(
    nChips: Int,
    binaryPaths: Seq[Path],
    plusArgs: Seq[Seq[String]] = Seq.empty,
    fast: Boolean = false,
    serialTLFreqMHz: Option[Double] = None,
    clockTapMonitor: Boolean = false
)(implicit
    p: Parameters
) extends RawModule {
  require(nChips >= 1, s"nChips must be at least 1, got $nChips")
  if (fast) {
    require(binaryPaths.length == 1,
      s"When fast is enabled only 1 binary is needed (loaded into all chips via FastRAM +loadmem), got ${binaryPaths.length}")
  } else {
    require(binaryPaths.length == nChips,
      s"Number of binaries (${binaryPaths.length}) must match nChips ($nChips)")
  }
  val perChipPlusArgs =
    if (plusArgs.isEmpty) Seq.fill(nChips)(Seq.empty[String]) else plusArgs
  require(perChipPlusArgs.length == nChips,
    s"Number of plusArgs entries (${perChipPlusArgs.length}) must match nChips ($nChips)")

  val io = IO(new Bundle {
    val success = Output(Bool())
    val reset = Input(Bool())
  })

  // A free-running clock from a simulation clock source.
  def freeRunningClock(freqMHz: Double): Clock = {
    val source = Module(new ClockSourceAtFreqMHz(freqMHz))
    source.io.power := true.B
    source.io.gate := false.B
    source.io.clk
  }

  val digitalFreqMHz = 500
  val digitalClock = freeRunningClock(digitalFreqMHz)
  val ucieBypassClock = freeRunningClock(8000)
  val ucieDigitalBypassClock = freeRunningClock(500) // previously was 800
  val ucieSidebandBypassClock = freeRunningClock(800)
  // The clocking tile's PLL reference, as in the UCIe repo's own harness.
  val ucieRefClock = freeRunningClock(100)
  val serialTLClock = serialTLFreqMHz.map(freeRunningClock).getOrElse(digitalClock)

  implicit def view[A <: Data, B <: Data]
      : DataView[testchipip.tsi.TSIIO, TSIIO] =
    DataView(
      _ => new TSIIO,
      _.in -> _.in,
      _.out -> _.out
    )

  val chipSuccesses = WireInit(VecInit(Seq.fill(nChips)(false.B)))

  def connectChip(binaryPath: Path, plusArgs: Seq[String], chipId: Int): Seq[ChipletIO] = {
    val allPlusArgs = if (fast) plusArgs :+ s"+loadmem=${binaryPath.toString}" else plusArgs

    val chiptop_lazy = LazyModule(new IrisTop)
    val chiptop = Module(chiptop_lazy.module)
    chiptop.io.clock := digitalClock
    chiptop.io.reset := io.reset.asAsyncReset

    val div =
      (digitalFreqMHz.toDouble * 1000000 / chiptop.uart.c.initBaudRate.toDouble).toInt
    UARTAdapter.connect(Seq(chiptop.uart), div, false)

    val dtm_success = WireInit(false.B)
    val jtag_wire = Wire(new JTAGIO)
    jtag_wire.TDO.data := chiptop.jtag.TDO
    jtag_wire.TDO.driven := true.B
    chiptop.jtag.TCK := jtag_wire.TCK
    chiptop.jtag.TMS := jtag_wire.TMS
    chiptop.jtag.TDI := jtag_wire.TDI
    val jtag = Module(new SimJTAG(tickDelay = 3))
    jtag.connect(
      jtag_wire,
      digitalClock,
      io.reset,
      ~(io.reset),
      dtm_success
    )

    chiptop.serial_tl.clock_in := serialTLClock

    // The memory model and TSI bridge sit on the far side of the link, so they
    // run on the link clock.
    val success = withClockAndReset(serialTLClock, io.reset) {
      if (fast) {
        val ram = Module(LazyModule(new FastRAM(chiptop_lazy.system.serdessers(0), p(SerialTLKey)(0), chipId = chipId)(
          chiptop_lazy.system.serdessers(0).p
        )).module)
        ram.io.ser.in <> chiptop.serial_tl.out
        chiptop.serial_tl.in <> ram.io.ser.out
        SimTSI.connect(ram.io.tsi.map(_.viewAs[TSIIO]), serialTLClock, io.reset, binaryPath, allPlusArgs)
      } else {
        val ram = Module(LazyModule(new SerialRAM(chiptop_lazy.system.serdessers(0), p(SerialTLKey)(0))(
          chiptop_lazy.system.serdessers(0).p
        )).module)
        ram.io.ser.in <> chiptop.serial_tl.out
        chiptop.serial_tl.in <> ram.io.ser.out
        SimTSI.connect(ram.io.tsi.map(_.viewAs[TSIIO]), serialTLClock, io.reset, binaryPath, allPlusArgs)
      }
    }
    if (clockTapMonitor) {
      Module(new ClockPeriodMonitor(s"chip$chipId clock_tap")).io.clk := chiptop.clock_tap
    }

    val chipSuccessReg = withReset(io.reset.asAsyncReset)(RegInit(false.B))
    when(dtm_success || success) { chipSuccessReg := true.B }
    chipSuccesses(chipId) := chipSuccessReg

    // The PHY bumps carry a single-ended mainband bypass clock; the PLL
    // reference pair and its RDAC bias are no longer bumps.
    Seq(chiptop.c2c_ucie0, chiptop.c2c_ucie1).zipWithIndex.foreach { case (ucie, i) =>
      ucie.phy.bypassClk := ucieBypassClock
      ucie.phy.digitalBypassClk := ucieDigitalBypassClock
      ucie.phy.sidebandBypassClk := ucieSidebandBypassClock
      ucie.phy.refClk := ucieRefClock
      val monitor = Module(new DebugBumpMonitor(s"chip$chipId.ucie$i"))
      monitor.io.mux0 := ucie.debug.mux0
      monitor.io.mux1 := ucie.debug.mux1
      // Absent on a link built without the TX data debug lane.
      monitor.io.txData := ucie.debug.txData.getOrElse(false.B)
    }

    Seq(chiptop.c2c_ucie0, chiptop.c2c_ucie1)
  }

  withClockAndReset(digitalClock, io.reset) {
    val chips = (0 until nChips).map { i =>
      val binaryIdx = if (fast) 0 else i
      connectChip(binaryPaths(binaryIdx), perChipPlusArgs(i), chipId = i)
    }
    // Each chip has 2 ports: [ucie0, ucie1]
    // Ring link from chip i to chip i+1 connects side 1 of chip i to side 0 of chip i+1:
    // ucie1(i) <-> ucie0(i+1)
    def connectRingLink(a: Seq[ChipletIO], b: Seq[ChipletIO]): Unit = {
      a(1).connect(b(0))  // ucie1  <-> ucie0
    }
    if (nChips == 1) {
      chips(0).foreach(_.loopback)
    } else {
      for (i <- 0 until nChips) {
        connectRingLink(chips(i), chips((i + 1) % nChips))
      }
    }
    io.success := chipSuccesses.reduce(_ && _)
  }
}

class IrisSpec extends AnyFunSpec {
  describe("Iris") {
    it("should generate valid System Verilog") {
      val targetDir = Utils.buildRoot / "Iris_should_generate_valid_System_Verilog"
      implicit val p = new IrisConfig
      ChiselStage.emitSystemVerilogFile(
        LazyModule(new IrisTop).module,
        args = Array(
          "--target-dir",
          targetDir.toString()
        )
      )

      freechips.rocketchip.util.ElaborationArtefacts.files.foreach { case (extension, contents) =>
        os.write.over(targetDir / s"Iris.${extension}", contents ())
      }
    }

    it("should run hello.riscv") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_hello_riscv"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 2,
        binaryPaths = Seq(
          Utils.root / "software/hello0.riscv",
          Utils.root / "software/hello1.riscv",
        )
      )
    }

    it("should run router tests") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_router_test"

      val chipid0 = 1
      val chipid1 = 2
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val chip0PlusArgs = Seq(
        f"+init_write=0x${chipidReg}%08x:0x${chipid0}%08x",
      )
      val chip1PlusArgs = Seq(
        f"+init_write=0x${chipidReg}%08x:0x${chipid1}%08x"
      )

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 2,
        binaryPaths = Seq(
          Utils.root / "software/router.riscv"
        ),
        plusArgs = Seq(chip0PlusArgs, chip1PlusArgs),
        fast = true
      )
    }

    it("should run vme- benchmark binaries") {
      implicit val p = new IrisConfig(sim = true)
      val benchmarkDir = Utils.root / "saturn-vectors" / "benchmarks"
      val benchmarkBinaries =
        os.walk(benchmarkDir)
          .filter(os.isFile)
          .filter(path => path.last.startsWith("vme-") && path.last.endsWith(".riscv"))
          .sortBy(_.last)

      require(benchmarkBinaries.nonEmpty, s"No vme-* .riscv binaries found under $benchmarkDir")

      benchmarkBinaries.foreach { binaryPath =>
        val workDir = Utils.buildRoot / s"Iris_should_run_${binaryPath.last.stripSuffix(".riscv")}"
        Utils.simulateTopWithBinaries(
          workDir,
          nChips = 1,
          binaryPaths = Seq(binaryPath),
          fast = true
        )
      }
    }

    it("should run fast saturn vector test") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_fast_saturn_vector_test"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "saturn-vectors" / "benchmarks" / "vec-sgemm.riscv"),
        fast = true
      )
    }

    it("should run mmt4d pytorch kernel test") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_mmt4d_pytorch"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(os.Path("/scratch/milesrusch/iree/kernels/test_mmt4d_pytorch.riscv")),
        fast = true
      )
    }
    
    it("should run hello.riscv with FastRAM") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_hello_riscv_fast"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 2,
        binaryPaths = Seq(Utils.root / "software/hello0.riscv"),
        fast = true
      )
    }

    it("should run four chip test") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_four_chip_test"

      val nChips = 4
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val plusArgs = (1 to nChips).map { chipId =>
        Seq(f"+init_write=0x${chipidReg}%08x:0x${chipId}%08x")
      }

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = nChips,
        binaryPaths = Seq(Utils.root / "software/hello.riscv"),
        plusArgs = plusArgs,
        fast = true
      )
    }

    it("should run ring test") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_ring_test"

      val nChips = 4
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val plusArgs = (1 to nChips).map { chipId =>
        Seq(f"+init_write=0x${chipidReg}%08x:0x${chipId}%08x")
      }

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = nChips,
        binaryPaths = Seq(Utils.root / "software/ring-hello.riscv"),
        plusArgs = plusArgs,
        fast = true
      )
    }

    it("should run ucie vec memcpy test") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_ucie_vec_memcpy_test"

      val chipid0 = 1
      val chipid1 = 2
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val chip0PlusArgs = Seq(
        f"+init_write=0x${chipidReg}%08x:0x${chipid0}%08x",
      )
      val chip1PlusArgs = Seq(
        f"+init_write=0x${chipidReg}%08x:0x${chipid1}%08x"
      )

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 2,
        binaryPaths = Seq(
          Utils.root / "software/ucie-vec-memcpy.riscv"
        ),
        plusArgs = Seq(chip0PlusArgs, chip1PlusArgs),
        fast = true
      )
    }

    it("should run ucie loopback test") {
            implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_ucie_loopback_test"

      val chipid0 = 1
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val chip0PlusArgs = Seq(
        f"+init_write=0x${chipidReg}%08x:0x${chipid0}%08x",
      )

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/ucie-loopback.riscv"),
        plusArgs = Seq(chip0PlusArgs),
        fast = true
      )
    }

    it("should run ucie digital loopback test") {
            implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_ucie_digital_loopback_test"

      val chipid0 = 1
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val chip0PlusArgs = Seq(
        f"+init_write=0x${chipidReg}%08x:0x${chipid0}%08x",
      )

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/ucie-digital-loopback.riscv"),
        plusArgs = Seq(chip0PlusArgs),
        fast = true,
        debug = true
      )
    }

    // The PhyTest programs bring-up runs on the chip (docs/ucie_bringup.md),
    // run by the core through the SoC bus and UCIe register map, over the
    // behavioral lanes and clocking tile.
    // The loopback program turns every observation driver on for one LFSR run,
    // with `mux0` on the TX divided clock and `mux1` on an RX data lane, and
    // drives the TX debug lane. So every debug bump of both links must toggle,
    // which shows the scope points reach the pins.
    def checkDebugBumps(simLog: Path): Unit =
      for (link <- Seq("chip0.ucie0", "chip0.ucie1")) {
        val counts = Utils.debugBumpCounts(simLog, link)
        assert(counts.keySet == Set("mux0", "mux1", "txData"))
        for ((bump, n) <- counts) {
          assert(n > 64, s"$link debug bump $bump only moved $n times")
        }
      }

    it("should run phytest loopback test") {
      implicit val p = new IrisConfig(sim = true)
      checkDebugBumps(
        Utils.simulateTopWithBinaries(
          Utils.buildRoot / "Iris_should_run_phytest_loopback_test",
          nChips = 1,
          binaryPaths = Seq(Utils.root / "software/phytest-loopback.riscv"),
          fast = true
        )
      )
    }

    it("should run phytest loopback test on the tapeout config") {
      // The configuration that is taped out (16b serial TL, no TL monitors),
      // with the serial TL on a 100 MHz external clock.
      implicit val p = new IrisConfig(sim = false)
      checkDebugBumps(
        Utils.simulateTopWithBinaries(
          Utils.buildRoot / "Iris_should_run_phytest_loopback_test_on_the_tapeout_config",
          nChips = 1,
          binaryPaths = Seq(Utils.root / "software/phytest-loopback.riscv"),
          fast = true,
          serialTLFreqMHz = Some(100)
        )
      )
    }

    it("should run phytest clocks test") {
      implicit val p = new IrisConfig(sim = true)
      Utils.simulateTopWithBinaries(
        Utils.buildRoot / "Iris_should_run_phytest_clocks_test",
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/phytest-clocks.riscv"),
        fast = true
      )
    }

    it("should run phytest two-chip test") {
      implicit val p = new IrisConfig(sim = true)
      // Both chips run the one binary; the ring wires UCIe1 of each to UCIe0
      // of the other.
      Utils.simulateTopWithBinaries(
        Utils.buildRoot / "Iris_should_run_phytest_two_chip_test",
        nChips = 2,
        binaryPaths = Seq(Utils.root / "software/phytest-two-chip.riscv"),
        fast = true
      )
    }

    it("should run tacit always trace workload") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_tacit_always_trace"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/tacit-always.riscv"),
        fast = true
      )
    }

    it("should run tacit always trace workload with parallel encoder") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_tacit_always_parallel_trace"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/tacit-always.riscv"),
        fast = true
      )
    }

    it("should run tacit dma trace workload") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_tacit_dma_trace"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/tacit-dma.riscv"),
        fast = true
      )
    }

    it("should run tacit dma trace workload with parallel encoder") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_tacit_dma_parallel_trace"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/tacit-dma.riscv"),
        fast = true
      )
    }

    it("should run tacit dma trace workload on the tapeout serial TL") {
      // Tapeout config (16b serial TL, no TL monitors) with the link at 100 MHz.
      implicit val p = new IrisConfig(sim = false)
      val workDir = Utils.buildRoot / "Iris_should_run_tacit_dma_trace_tapeout_serial_tl"

      Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/tacit-dma.riscv"),
        fast = true,
        serialTLFreqMHz = Some(100)
      )
    }

    it("should boot hello.riscv over TSI on the tapeout serial TL") {
      // The tethered boot path as built: tapeout config (16b serial TL, no TL
      // monitors), link at 100 MHz, and no +loadmem -- SimTSI writes the
      // program into memory over the link, then raises hart 0's MSIP.
      implicit val p = new IrisConfig(sim = false)
      val workDir = Utils.buildRoot / "Iris_should_boot_hello_riscv_over_tsi_tapeout_serial_tl"

      val simLog = Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/hello.riscv"),
        serialTLFreqMHz = Some(100)
      )
      assert(!os.read(workDir / "sim" / "simulate.sh").contains("+loadmem"))
      Utils.assertLogContains(simLog, "Hello world chip")
    }

    it("should run a vector kernel on every hart and contend with AMOs and LR/SC") {
      // Harts 0 and 1 are the ShuttleTiles, hart 2 is Rocket (the program checks
      // marchid). Each runs a self-checking RVV kernel, then all three hammer
      // shared DRAM lines with AMOs and LR/SC; hart 0 checks the final values.
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_multihart_vec_amo"

      val simLog = Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/multihart-vec-amo.riscv"),
        fast = true
      )
      Utils.assertLogContains(simLog, "multihart-vec-amo: all checks passed")
    }

    it("should keep running with the clock dividers programmed") {
      // clock-div.riscv steps the uncore and per-tile dividers through /2, /3
      // and /7, one at a time and mixed; each hart measures its clock against
      // mtime and runs a memory self-check after every change. It also sets
      // the clock_tap divider, whose output period the harness measures.
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_clock_div"

      val simLog = Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/clock-div.riscv"),
        fast = true,
        clockTapMonitor = true
      )
      Utils.assertLogContains(
        simLog,
        "clock-div: all checks passed",
        // 500 MHz chip clock: undivided, then /2, /3, /7.
        "chip0 clock_tap period 2.000 ns",
        "chip0 clock_tap period 4.000 ns",
        "chip0 clock_tap period 6.000 ns",
        "chip0 clock_tap period 14.000 ns"
      )
    }

    it("should run MX and fp8 vector benchmarks") {
      // saturn-vectors' MX vector benchmarks as software/Makefile builds them,
      // with a fixed copy of vec-mx-matmul, whose check cannot fail upstream
      // (see software/saturn-benchmarks/). The fp8 GEMM, vme-fp8-gemm, checks
      // itself and runs under "should run vme- benchmark binaries".
      // Runs every benchmark, then fails with the list of those that failed.
      //
      // vec-fp8 is left out: at LMUL=8 its `vfmul.vv v3, v2, v1` and
      // `vmsne.vv v24, v3, v2` name register groups that are not multiples of
      // 8 (reserved encodings), it never uses b (v8) or c (v16), and it reads
      // its verdict from v24 rather than the vredor result, so its
      // "Correct"/"Incorrect" says nothing about the hardware (on Iris it
      // prints "Incorrect" and exits 1). Add it back once it is fixed.
      implicit val p = new IrisConfig(sim = true)
      val benchmarkDir = Utils.root / "software"
      val failures =
        Seq("vec-mx-binary", "vec-mx-unary", "vec-mx-matmul").flatMap {
          name =>
            val workDir = Utils.buildRoot / s"Iris_should_run_$name"
            scala.util
              .Try(
                Utils.simulateTopWithBinaries(
                  workDir,
                  nChips = 1,
                  binaryPaths = Seq(benchmarkDir / s"$name.riscv"),
                  fast = true
                )
              )
              .failed
              .toOption
              .map(e => s"$name: ${e.getMessage}")
        }
      assert(failures.isEmpty, failures.mkString("\n"))
    }

    it("should run hello.riscv with X-propagation") {
      // VCS -xprop=tmerge. The RANDOMIZE_* defines still apply, so registers
      // and memories start from random values rather than X; what xprop adds is
      // that an X reaching an if/case condition (e.g. from an undriven or
      // invalid signal) propagates instead of silently taking one branch.
      // Without RANDOMIZE_REG_INIT/MEM_INIT it stops at 21 ns: the chip's UART
      // txd is X while the chip is still in reset, and testchipip's
      // UARTAdapter, out of reset sooner, asserts on the X it samples.
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_hello_riscv_xprop"

      val simLog = Utils.simulateTopWithBinaries(
        workDir,
        nChips = 1,
        binaryPaths = Seq(Utils.root / "software/hello.riscv"),
        fast = true,
        xprop = true
      )
      Utils.assertLogContains(simLog, "Hello world chip")
    }
  }

  describe("IrisDryRun") {
    it("should generate valid System Verilog") {
      val targetDir = Utils.buildRoot / "IrisDryRun_should_generate_valid_System_Verilog"
      implicit val p = new IrisDryRunConfig
      ChiselStage.emitSystemVerilogFile(
        LazyModule(new IrisDryRunTop).module,
        args = Array(
          "--target-dir",
          targetDir.toString()
        )
      )

      freechips.rocketchip.util.ElaborationArtefacts.files.foreach { case (extension, contents) =>
        os.write.over(targetDir / s"Iris.${extension}", contents ())
      }
    }
  }

  describe("TinyIris") {
    it("should generate valid System Verilog for TinyIris") {
      val targetDir = Utils.buildRoot / "TinyIris_should_generate_valid_System_Verilog"
      implicit val p = new TinyIrisConfig
      ChiselStage.emitSystemVerilogFile(
        LazyModule(new TinyIrisTop).module,
        args = Array(
          "--target-dir",
          targetDir.toString()
        )
      )

      freechips.rocketchip.util.ElaborationArtefacts.files.foreach { case (extension, contents) =>
        os.write.over(targetDir / s"Iris.${extension}", contents ())
      }
    }
  }
}
