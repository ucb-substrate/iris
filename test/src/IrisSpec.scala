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

class SimTop(nChips: Int, binaryPaths: Seq[Path], plusArgs: Seq[Seq[String]] = Seq.empty, fast: Boolean = false)(implicit
    p: Parameters
) extends RawModule {
  val driver = Module(new TestDriver)
  val harness = Module(new TestHarness(nChips, binaryPaths, plusArgs, fast))
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

class TestHarness(nChips: Int, binaryPaths: Seq[Path], plusArgs: Seq[Seq[String]] = Seq.empty, fast: Boolean = false)(implicit
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

  val digitalFreqMHz = 500

  val digitalClock = Wire(Clock())
  val source = Module(new ClockSourceAtFreqMHz(digitalFreqMHz))
  source.io.power := true.B
  source.io.gate := false.B
  digitalClock := source.io.clk

  val ucieBypassFreqMHz = 8000
  val ucieBypassClock = Wire(Clock())
  val ucieBypassClockSource = Module(new ClockSourceAtFreqMHz(ucieBypassFreqMHz))
  ucieBypassClockSource.io.power := true.B
  ucieBypassClockSource.io.gate := false.B
  ucieBypassClock := ucieBypassClockSource.io.clk

  val ucieDigitalBypassFreqMHz = 800
  val ucieDigitalBypassClock = Wire(Clock())
  val ucieDigitalBypassClockSource = Module(new ClockSourceAtFreqMHz(ucieDigitalBypassFreqMHz))
  ucieDigitalBypassClockSource.io.power := true.B
  ucieDigitalBypassClockSource.io.gate := false.B
  ucieDigitalBypassClock := ucieDigitalBypassClockSource.io.clk


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

    chiptop.serial_tl.clock_in := digitalClock

    val success = if (fast) {
      val ram = Module(LazyModule(new FastRAM(chiptop_lazy.system.serdessers(0), p(SerialTLKey)(0), chipId = chipId)(
        chiptop_lazy.system.serdessers(0).p
      )).module)
      ram.io.ser.in <> chiptop.serial_tl.out
      chiptop.serial_tl.in <> ram.io.ser.out
      SimTSI.connect(ram.io.tsi.map(_.viewAs[TSIIO]), digitalClock, io.reset, binaryPath, allPlusArgs)
    } else {
      val ram = Module(LazyModule(new SerialRAM(chiptop_lazy.system.serdessers(0), p(SerialTLKey)(0))(
        chiptop_lazy.system.serdessers(0).p
      )).module)
      ram.io.ser.in <> chiptop.serial_tl.out
      chiptop.serial_tl.in <> ram.io.ser.out
      SimTSI.connect(ram.io.tsi.map(_.viewAs[TSIIO]), digitalClock, io.reset, binaryPath, allPlusArgs)
    }
    val chipSuccessReg = withReset(io.reset.asAsyncReset)(RegInit(false.B))
    when(dtm_success || success) { chipSuccessReg := true.B }
    chipSuccesses(chipId) := chipSuccessReg

    Seq(chiptop.c2c_ucie0, chiptop.c2c_ucie1).foreach { ucie =>
      ucie.phy.refClkP := DontCare
      ucie.phy.refClkN := DontCare
      ucie.phy.bypassClkP := ucieBypassClock
      ucie.phy.bypassClkN := (!ucieBypassClock.asBool).asClock
      ucie.phy.digitalBypassClk := ucieDigitalBypassClock
      ucie.phy.pllRdacVref := 0.U
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
        debug = true,
        fast = true
      )
    }

    it("should run opu- benchmark binaries") {
      implicit val p = new IrisConfig(sim = true)
      val benchmarkDir = Utils.root / "saturn-vectors" / "benchmarks"
      val benchmarkBinaries =
        os.walk(benchmarkDir)
          .filter(os.isFile)
          .filter(path => path.last.startsWith("opu-") && path.last.endsWith(".riscv"))
          .sortBy(_.last)

      require(benchmarkBinaries.nonEmpty, s"No opu-* .riscv binaries found under $benchmarkDir")

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
        fast = true,
        debug = true
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
        fast = true,
        debug = true
      )
    }
  }
}
