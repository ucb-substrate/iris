package edu.berkeley.cs.iris

import chisel3._
import chisel3.util._
import org.chipsalliance.cde.config.{Config, Parameters}
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.subsystem._
import freechips.rocketchip.amba.axi4._
import freechips.rocketchip.prci._
import freechips.rocketchip.devices.debug._
import freechips.rocketchip.devices.tilelink.BootROMLocated
import freechips.rocketchip.util._
import sifive.blocks.devices.uart._
import testchipip.serdes._
import testchipip.boot._
import testchipip.util.ClockedIO
import shuttle.common._
import saturn.common.VectorParams
import edu.berkeley.cs.chippy.{ChippySystem, ChippySystemModule}
import edu.berkeley.cs.chippy.clocking.HasChippyPRCI

class OPUSystem(implicit p: Parameters)
    extends ChippySystem
    with CanHaveMasterAXI4MemPort
    with CanHavePeripheryTLSerial
    with HasPeripheryUART
    with HasChippyPRCI

class OPUTop(implicit p: Parameters) extends LazyModule with BindingScope {
  val system = LazyModule(new OPUSystem)

  val clockGroupsSourceNode = ClockGroupSourceNode(Seq(ClockGroupSourceParameters()))
  system.chiptopClockGroupsNode := clockGroupsSourceNode

  val debugClockSinkNode = ClockSinkNode(Seq(ClockSinkParameters()))
  debugClockSinkNode := system.locateTLBusWrapper(p(ExportDebug).slaveWhere).fixedClockNode

  val mbusClockSinkNode = ClockSinkNode(Seq(ClockSinkParameters()))
  mbusClockSinkNode := system.locateTLBusWrapper(MBUS).fixedClockNode

  override lazy val module = new OPUTopImpl
  class OPUTopImpl extends LazyRawModuleImp(this) with DontTouch {
    val io = IO(new Bundle {
      val clock = Input(Clock())
      val reset = Input(AsyncReset())
    })

    def debugClockBundle = debugClockSinkNode.in.head._1
    def mbusClockBundle  = mbusClockSinkNode.in.head._1

    clockGroupsSourceNode.out.foreach { case (bundle, _) =>
      bundle.member.data.foreach { b =>
        b.clock := io.clock
        b.reset := io.reset
      }
    }

    val debug_io = system.debug.get
    Debug.connectDebugClockAndReset(Some(debug_io), debugClockBundle.clock)

    system.psd.psd.foreach { _ <> 0.U.asTypeOf(new PSDTestMode) }
    system.resetctrl.map { rcio =>
      rcio.hartIsInReset.map { _ := debugClockBundle.reset.asBool }
    }
    debug_io.extTrigger.foreach { t =>
      t.in.req := false.B; t.out.ack := t.out.req
    }
    debug_io.disableDebug.foreach { d => d := false.B }
    debug_io.systemjtag.map { j =>
      j.reset     := ResetCatchAndSync(j.jtag.TCK, debugClockBundle.reset.asBool)
      j.mfr_id    := p(JtagDTMKey).idcodeManufId.U(11.W)
      j.part_number := p(JtagDTMKey).idcodePartNum.U(16.W)
      j.version   := p(JtagDTMKey).idcodeVersion.U(4.W)
    }

    system.module.interrupts := DontCare

    val jtag = IO(new JTAGChipIO(false))
    debug_io.systemjtag.get.jtag.TCK := jtag.TCK
    debug_io.systemjtag.get.jtag.TMS := jtag.TMS
    debug_io.systemjtag.get.jtag.TDI := jtag.TDI
    jtag.TDO := debug_io.systemjtag.get.jtag.TDO.data

    val mem_axi4 = system.mem_axi4.zipWithIndex.map { case (m, i) =>
      val port = IO(new ClockedIO(chiselTypeOf(m))).suggestName(s"axi4_mem_$i")
      port.bits <> m
      port.clock := mbusClockBundle.clock
      port
    }

    val serial_tl = IO(new DecoupledExternalSyncPhitIO(p(SerialTLKey)(0).phyParams.phitWidth))
    serial_tl <> system.serial_tls(0)

    val uart = IO(chiselTypeOf(system.uart(0)))
    uart <> system.uart(0)
  }
}

class OPUBaseConfig extends Config(
  // Client-only SerialTL: external host masters FBUS (matches chipyard AbstractConfig)
  new WithSerialTL(Seq(
    SerialTLParams(
      client    = Some(SerialTLClientParams(totalIdBits = 4, masterWhere = FBUS)),
      phyParams = DecoupledExternalSyncSerialPhyParams(phitWidth = 32, flitWidth = 32)
    )
  )) ++
  new freechips.rocketchip.subsystem.WithNMemoryChannels(1) ++
  new freechips.rocketchip.subsystem.WithNoMMIOPort ++
  new freechips.rocketchip.subsystem.WithNoSlavePort ++
  new Config((site, here, up) => { case PeripheryUARTKey =>
    Seq(UARTParams(address = 0x10020000))
  }) ++
  new freechips.rocketchip.subsystem.WithDebugSBA ++
  new Config((site, here, up) => { case DebugModuleKey =>
    up(DebugModuleKey).map(_.copy(nAbstractDataWords = 8))
  }) ++
  new freechips.rocketchip.subsystem.WithJtagDTM ++
  new WithBootAddrReg ++
  new freechips.rocketchip.subsystem.WithNExtTopInterrupts(0) ++
  new freechips.rocketchip.subsystem.WithDTS("ucb-bar,chipyard", Nil) ++
  new Config((site, here, up) => { case BootROMLocated(x) =>
    up(BootROMLocated(x), site).map(_.copy(
      address = 0x10000,
      size    = 0x10000,
      hang    = 0x10000,
      contentFileName = ResourceFileName(s"/testchipip/bootrom/bootrom.rv${site(MaxXLen)}.img")
    ))
  }) ++
  new testchipip.soc.WithMbusScratchpad(base = 0x08000000, size = 64 * 1024) ++
  new freechips.rocketchip.subsystem.WithCoherentBusTopology ++
  new edu.berkeley.cs.chippy.clocking.WithClockGroupsCombinedByName(
    ("uncore", Seq("implicit", "sbus", "mbus", "cbus", "fbus", "pbus"), Nil)
  ) ++
  new edu.berkeley.cs.chippy.clocking.WithPeripheryBusFrequency(500.0) ++
  new edu.berkeley.cs.chippy.clocking.WithMemoryBusFrequency(500.0) ++
  new edu.berkeley.cs.chippy.clocking.WithControlBusFrequency(500.0) ++
  new edu.berkeley.cs.chippy.clocking.WithSystemBusFrequency(500.0) ++
  new edu.berkeley.cs.chippy.clocking.WithFrontBusFrequency(500.0) ++
  new edu.berkeley.cs.chippy.clocking.WithInheritBusFrequencyAssignments ++
  new edu.berkeley.cs.chippy.clocking.WithNoSubsystemClockIO ++
  new freechips.rocketchip.subsystem.WithDontDriveBusClocksFromSBus ++
  new freechips.rocketchip.subsystem.WithClockGateModel ++
  new freechips.rocketchip.system.BaseConfig
)

class OPUV128D64IrisConfig extends Config(
  new saturn.shuttle.WithShuttleVectorUnit(128, 64, VectorParams.opuParams) ++
  new WithShuttleTileBeatBytes(8) ++
  new Config((site, here, up) => { case SystemBusKey =>
    up(SystemBusKey, site).copy(beatBytes = 8)
  }) ++
  new WithNShuttleCores(1) ++
  new OPUBaseConfig
)

class OPUV512D256IrisConfig extends Config(
  new saturn.shuttle.WithShuttleVectorUnit(512, 256, VectorParams.opuParams) ++
  new WithShuttleTileBeatBytes(32) ++
  new Config((site, here, up) => { case SystemBusKey =>
    up(SystemBusKey, site).copy(beatBytes = 32)
  }) ++
  new WithNShuttleCores(1) ++
  new OPUBaseConfig
)
