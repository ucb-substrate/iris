package edu.berkeley.cs.iris

import chisel3._
import org.chipsalliance.cde.config.{Config, Parameters}
import org.scalatest.funspec.AnyFunSpec
import freechips.rocketchip.subsystem._

// ============================================================================
// EXPERIMENTS ONLY. Nothing in this file is part of the tapeout configuration.
//
// The tapeout configuration is whatever IrisConfig (src/Iris.scala) builds; the
// IrisSpec tests elaborate it. Everything below layers sim-only overrides on top
// of IrisConfig(sim = false) to answer a performance question, and every
// experiment builds into a build/Experiment_* directory so its RTL and results
// cannot be mistaken for the tapeout ones.
// ============================================================================

/** Trace DMA sinks get `n` TileLink source IDs (outstanding writes) instead of 16. */
class WithExperimentTraceDMASources(n: Int) extends Config((site, here, up) => {
  case tacit.TraceSinkDMASourcesKey => n
})

/** Shrinks the L2 by `divisor` (fewer sets; ways and banks unchanged) so a trace
  * outgrows it in a short simulation. */
class WithExperimentSmallL2(divisor: Int) extends Config((site, here, up) => {
  case InclusiveCacheKey => up(InclusiveCacheKey).copy(sets = up(InclusiveCacheKey).sets / divisor)
})

/** Sizes the L2's outer MSHRs for an `cycles`-cycle memory round trip instead of
  * the tapeout value. */
class WithExperimentL2OuterLatency(cycles: Int) extends Config((site, here, up) => {
  case InclusiveCacheKey => up(InclusiveCacheKey).copy(memCycles = cycles)
})

/** Sim-only probe of the harness memory (FastRAM's TL->AXI4 port, i.e. below
  * serial TL): counts AXI read/write requests and data beats and prints them at
  * the end of simulation. Uses hierarchical references, so it only works in the
  * single-chip fast harness. */
class MemTrafficMonitor extends chisel3.experimental.ExtModule {
  setInline(
    "MemTrafficMonitor.sv",
    """module MemTrafficMonitor;
      |`ifndef SYNTHESIS
      |  `define MTM_P SimTop.harness.chips_success_ram.tl2axi4
      |  longint unsigned ar = 0, aw = 0, r_beats = 0, w_beats = 0;
      |  always @(posedge `MTM_P.clock) begin
      |    if (`MTM_P.auto_out_ar_valid && `MTM_P.auto_out_ar_ready) ar <= ar + 1;
      |    if (`MTM_P.auto_out_aw_valid && `MTM_P.auto_out_aw_ready) aw <= aw + 1;
      |    if (`MTM_P.auto_out_r_valid  && `MTM_P.auto_out_r_ready)  r_beats <= r_beats + 1;
      |    if (`MTM_P.auto_out_w_valid  && `MTM_P.auto_out_w_ready)  w_beats <= w_beats + 1;
      |  end
      |  final $display("MEMTRAFFIC reads=%0d read_beats=%0d writes=%0d write_beats=%0d", ar, r_beats, aw, w_beats);
      |  `undef MTM_P
      |`endif
      |endmodule
      |""".stripMargin
  )
}

class IrisExperimentsSpec extends AnyFunSpec {
  // Tapeout RTL (16b serial TL, no TL monitors) with the link at 100 MHz, plus the
  // given experimental overrides.
  def tapeoutWith(overrides: Config*): Parameters =
    overrides.foldRight(new IrisConfig(sim = false): Config)((o, acc) => new Config(o ++ acc))

  def run(name: String, p: Parameters): Unit = {
    implicit val params: Parameters = p
    Utils.simulateTopWithBinaries(
      Utils.buildRoot / s"Experiment_$name",
      nChips = 1,
      binaryPaths = Seq(Utils.root / "software/tacit-dma.riscv"),
      fast = true,
      serialTLFreqMHz = Some(100),
      memTrafficMonitor = true
    )
  }

  describe("Experiment (not tapeout)") {
    it("tapeout config, memory traffic monitor only") {
      run("tapeout_memmon", tapeoutWith())
    }
    it("64 trace DMA sources") {
      run("dma_src64_memmon", tapeoutWith(new WithExperimentTraceDMASources(64)))
    }
    it("32KB L2") {
      run("l2_32k_memmon", tapeoutWith(new WithExperimentSmallL2(16)))
    }
    it("32KB L2 and 64 trace DMA sources") {
      run("l2_32k_dma_src64_memmon", tapeoutWith(new WithExperimentSmallL2(16), new WithExperimentTraceDMASources(64)))
    }
    // Informational only: the L2 is frozen for tapeout. Sizes the L2's outer MSHRs
    // for a 64-cycle memory round trip instead of the configured 4 (128 needs more outer source IDs than the 8-bit serial-TL source field allows).
    it("L2 sized for a 64-cycle memory") {
      run("l2_mem64_memmon", tapeoutWith(new WithExperimentL2OuterLatency(64)))
    }
    it("32KB L2 sized for a 64-cycle memory") {
      run("l2_32k_mem64_memmon", tapeoutWith(new WithExperimentSmallL2(16), new WithExperimentL2OuterLatency(64)))
    }
  }
}
