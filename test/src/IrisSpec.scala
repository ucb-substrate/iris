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
import svsim.verilator.Backend.CompilationSettings
import svsim.Workspace.getProjectRootOrCwd
import _root_.circt.stage.ChiselStage
import testchipip.uart.UARTAdapter
import freechips.rocketchip.jtag.JTAGIO
import freechips.rocketchip.util._
import freechips.rocketchip.devices.debug.SimJTAG
// import testchipip.tsi._
import testchipip.dram._
import testchipip.tsi.SerialRAM
import testchipip.serdes.{SerialTLKey, CreditedSourceSyncPhitIO}
import testchipip.soc.ChipletRoutingKey
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

class SimTop(chip0BinaryPath: Path, chip1BinaryPath: Path, plusArgs: Seq[String] = Seq.empty)(implicit
    p: Parameters
) extends RawModule {
  val driver = Module(new TestDriver)
  val harness = Module(new TestHarness(chip0BinaryPath, chip1BinaryPath, plusArgs))
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
      | initial begin
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
      |   $display("Test completed successfully.");
      |   $finish;
      | end
      |endmodule
    """.stripMargin
  )
}

class TestHarnessIO extends Bundle {
  val success = Output(Bool())
  val reset = Input(Bool())
}

class TestHarness(chip0BinaryPath: Path, chip1BinaryPath: Path, plusArgs: Seq[String] = Seq.empty)(implicit
    p: Parameters
) extends RawModule {
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

  val c2c0 = Wire(new CreditedSourceSyncPhitIO(p(ChipletRoutingKey).get.ports(0).asInstanceOf[testchipip.serdes.SerialTLParams].phyParams.phitWidth))
  val c2c1 = Wire(new CreditedSourceSyncPhitIO(p(ChipletRoutingKey).get.ports(0).asInstanceOf[testchipip.serdes.SerialTLParams].phyParams.phitWidth))

  withClockAndReset(digitalClock, io.reset) {
    val chipid0 = 0
    val chiptop0_lazy = LazyModule(new IrisTop)
    val chiptop0 = Module(chiptop0_lazy.module)
    chiptop0.io.clock := digitalClock
    chiptop0.io.reset := io.reset.asAsyncReset

    val div =
      (digitalFreqMHz.toDouble * 1000000 / chiptop0.uart.c.initBaudRate.toDouble).toInt
    UARTAdapter.connect(Seq(chiptop0.uart), div, false)

    io.success := false.B

    val dtm_success = WireInit(false.B)
    when(dtm_success) { io.success := true.B }
    val jtag_wire = Wire(new JTAGIO)
    jtag_wire.TDO.data := chiptop0.jtag.TDO
    jtag_wire.TDO.driven := true.B
    chiptop0.jtag.TCK := jtag_wire.TCK
    chiptop0.jtag.TMS := jtag_wire.TMS
    chiptop0.jtag.TDI := jtag_wire.TDI
    val jtag = Module(new SimJTAG(tickDelay = 3))
    jtag.connect(
      jtag_wire,
      digitalClock,
      io.reset,
      ~(io.reset),
      dtm_success
    )

    chiptop0.serial_tl.clock_in := digitalClock
    val ram = Module(
      LazyModule(
        new SerialRAM(chiptop0_lazy.system.serdessers(0), p(SerialTLKey)(0))(
          chiptop0_lazy.system.serdessers(0).p
        )
      ).module
    )
    ram.io.ser.in <> chiptop0.serial_tl.out
    chiptop0.serial_tl.in <> ram.io.ser.out

    c2c0 <> chiptop0.c2c_serial_tl

    implicit def view[A <: Data, B <: Data]
        : DataView[testchipip.tsi.TSIIO, TSIIO] =
      DataView(
        _ => new TSIIO,
        _.in -> _.in,
        _.out -> _.out
      )
    val success =
      CustomSimTSI.connect(
        ram.io.tsi.map(_.viewAs[TSIIO]),
        digitalClock,
        io.reset,
        chipid0,
        chip0BinaryPath,
        plusArgs
      )
    when(success) { io.success := true.B }
  }

  withClockAndReset(digitalClock, io.reset) {
    val chipid1 = 1
    val chiptop1_lazy = LazyModule(new IrisTop)
    val chiptop1 = Module(chiptop1_lazy.module)
    chiptop1.io.clock := digitalClock
    chiptop1.io.reset := io.reset.asAsyncReset

    val div =
      (digitalFreqMHz.toDouble * 1000000 / chiptop1.uart.c.initBaudRate.toDouble).toInt
    UARTAdapter.connect(Seq(chiptop1.uart), div, false)

    io.success := false.B

    val dtm_success = WireInit(false.B)
    when(dtm_success) { io.success := true.B }
    val jtag_wire = Wire(new JTAGIO)
    jtag_wire.TDO.data := chiptop1.jtag.TDO
    jtag_wire.TDO.driven := true.B
    chiptop1.jtag.TCK := jtag_wire.TCK
    chiptop1.jtag.TMS := jtag_wire.TMS
    chiptop1.jtag.TDI := jtag_wire.TDI
    val jtag = Module(new SimJTAG(tickDelay = 3))
    jtag.connect(
      jtag_wire,
      digitalClock,
      io.reset,
      ~(io.reset),
      dtm_success
    )

    chiptop1.serial_tl.clock_in := digitalClock
    val ram = Module(
      LazyModule(
        new SerialRAM(chiptop1_lazy.system.serdessers(0), p(SerialTLKey)(0))(
          chiptop1_lazy.system.serdessers(0).p
        )
      ).module
    )
    ram.io.ser.in <> chiptop1.serial_tl.out
    chiptop1.serial_tl.in <> ram.io.ser.out

    c2c1 <> chiptop1.c2c_serial_tl

    implicit def view[A <: Data, B <: Data]
        : DataView[testchipip.tsi.TSIIO, TSIIO] =
      DataView(
        _ => new TSIIO,
        _.in -> _.in,
        _.out -> _.out
      )
    val success =
      CustomSimTSI.connect(
        ram.io.tsi.map(_.viewAs[TSIIO]),
        digitalClock,
        io.reset,
        chipid1,
        chip1BinaryPath,
        plusArgs
      )
    when(success) { io.success := true.B }
  }

  // Connect the two chips through D2D SerialTL
  c2c0.connect(c2c1)
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

      // TODO: Figure out why this passes even when simulation errors.
      Utils.simulateTopWithBinaries(
        workDir,
        Utils.root / "software/build/hello.riscv",
        Utils.root / "software/build/hello.riscv"
      )
    }

    it("should run router tests") {
      implicit val p = new IrisConfig(sim = true)
      val workDir = Utils.buildRoot / "Iris_should_run_router_test"

      val chipid0 = 1
      val chipid1 = 2
      val chipidReg = p(ChipletRoutingKey).get.routerParams.tableAddress + p(ChipletRoutingKey).get.routerParams.tableEntries * 32
      val plusArgs = Seq(
        f"+chip_id0=0x${chipidReg}%08x:0x${chipid0}%08x",
        f"+chip_id1=0x${chipidReg}%08x:0x${chipid1}%08x"
      )

      // TODO: Figure out why this passes even when simulation errors.
      Utils.simulateTopWithBinaries(
        workDir,
        Utils.root / "software/build/router.riscv",
        Utils.root / "software/build/router.riscv",
        plusArgs
      )
    }
  }
}
