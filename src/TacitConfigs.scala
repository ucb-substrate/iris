package edu.berkeley.cs.iris

import org.chipsalliance.cde.config.{Config, Parameters}
import org.chipsalliance.diplomacy.lazymodule.LazyModule
import freechips.rocketchip.subsystem.{InSubsystem, RocketTileAttachParams, TilesLocated}
import freechips.rocketchip.trace.{TraceCoreParams, TraceEncoderParams}
import shuttle.common.ShuttleTileAttachParams
import tacit.{MPQueueImpl, TacitBPParams, TacitEncoder, TacitParallelEncoder}

class WithTacitEncoder(
    encoderBaseAddr: BigInt = 0x03000000L,
    bufferDepth: Int = 16,
    bpEntries: Int = 1024)
    extends Config((site, here, up) => {
  case TilesLocated(InSubsystem) => up(TilesLocated(InSubsystem), site).map {
    case tp: RocketTileAttachParams =>
      val xlen = tp.tileParams.core.xLen
      tp.copy(tileParams = tp.tileParams.copy(
        traceParams = Some(TraceEncoderParams(
          encoderBaseAddr = encoderBaseAddr + tp.tileParams.tileId * 0x1000,
          buildEncoder = (p: Parameters) => LazyModule(new TacitEncoder(
            new TraceCoreParams(nGroups = 1, xlen = xlen, iaddrWidth = xlen),
            bufferDepth = bufferDepth,
            coreStages = 5,
            bpParams = TacitBPParams(xlen = xlen, n_entries = bpEntries))(p)),
          useArbiterMonitor = false,
          buildSinks = tp.tileParams.traceParams.map(_.buildSinks).getOrElse(Seq.empty)
        )),
        core = tp.tileParams.core.copy(enableTraceCoreIngress = true)
      ))
    case tp: ShuttleTileAttachParams =>
      val xlen = tp.tileParams.core.xLen
      tp.copy(tileParams = tp.tileParams.copy(
        traceParams = Some(TraceEncoderParams(
          encoderBaseAddr = encoderBaseAddr + tp.tileParams.tileId * 0x1000,
          buildEncoder = (p: Parameters) => LazyModule(new TacitEncoder(
            new TraceCoreParams(
              nGroups = tp.tileParams.core.retireWidth,
              xlen = xlen,
              iaddrWidth = xlen),
            bufferDepth = bufferDepth,
            coreStages = 7,
            bpParams = TacitBPParams(xlen = xlen, n_entries = bpEntries))(p)),
          useArbiterMonitor = false,
          buildSinks = tp.tileParams.traceParams.map(_.buildSinks).getOrElse(Seq.empty)
        )),
        core = tp.tileParams.core.copy(enableTraceCoreIngress = true)
      ))
    case other => other
  }
})

class WithTacitParallelEncoder(
    encoderBaseAddr: BigInt = 0x03000000L,
    bufferDepth: Int = 16,
    queueImpl: MPQueueImpl = MPQueueImpl.SRAM)
    extends Config((site, here, up) => {
  case TilesLocated(InSubsystem) => up(TilesLocated(InSubsystem), site).map {
    case tp: RocketTileAttachParams =>
      val xlen = tp.tileParams.core.xLen
      tp.copy(tileParams = tp.tileParams.copy(
        traceParams = Some(TraceEncoderParams(
          encoderBaseAddr = encoderBaseAddr + tp.tileParams.tileId * 0x1000,
          buildEncoder = (p: Parameters) => LazyModule(new TacitParallelEncoder(
            new TraceCoreParams(nGroups = 1, xlen = xlen, iaddrWidth = xlen),
            bufferDepth = bufferDepth,
            coreStages = 5,
            queueImpl = queueImpl)(p)),
          useArbiterMonitor = false,
          buildSinks = tp.tileParams.traceParams.map(_.buildSinks).getOrElse(Seq.empty)
        )),
        core = tp.tileParams.core.copy(enableTraceCoreIngress = true)
      ))
    case tp: ShuttleTileAttachParams =>
      val xlen = tp.tileParams.core.xLen
      tp.copy(tileParams = tp.tileParams.copy(
        traceParams = Some(TraceEncoderParams(
          encoderBaseAddr = encoderBaseAddr + tp.tileParams.tileId * 0x1000,
          buildEncoder = (p: Parameters) => LazyModule(new TacitParallelEncoder(
            new TraceCoreParams(
              nGroups = tp.tileParams.core.retireWidth,
              xlen = xlen,
              iaddrWidth = xlen),
            bufferDepth = bufferDepth,
            coreStages = 7,
            queueImpl = queueImpl)(p)),
          useArbiterMonitor = false,
          buildSinks = tp.tileParams.traceParams.map(_.buildSinks).getOrElse(Seq.empty)
        )),
        core = tp.tileParams.core.copy(enableTraceCoreIngress = true)
      ))
    case other => other
  }
})

class WithTacitTraceArbiterMonitor extends Config((site, here, up) => {
  case TilesLocated(InSubsystem) => up(TilesLocated(InSubsystem), site).map {
    case tp: RocketTileAttachParams => tp.copy(tileParams = tp.tileParams.copy(
      traceParams = Some(tp.tileParams.traceParams.get.copy(useArbiterMonitor = true))))
    case tp: ShuttleTileAttachParams => tp.copy(tileParams = tp.tileParams.copy(
      traceParams = Some(tp.tileParams.traceParams.get.copy(useArbiterMonitor = true))))
    case other => other
  }
})

/*
class TacitIrisAlwaysConfig(sim: Boolean = false)
    extends Config(
      new tacit.WithTraceSinkAlways(0) ++
      new WithTacitTraceArbiterMonitor ++
      new WithTacitEncoder ++
      new IrisConfig(sim))

class TacitTinyIrisAlwaysConfig(sim: Boolean = false)
    extends Config(
      new tacit.WithTraceSinkAlways(0) ++
      new WithTacitTraceArbiterMonitor ++
      new WithTacitEncoder ++
      new TinyIrisConfig(sim))

class TacitTinyIrisAlwaysParallelConfig(sim: Boolean = false)
    extends Config(
      new tacit.WithTraceSinkAlways(0) ++
      new WithTacitTraceArbiterMonitor ++
      new WithTacitParallelEncoder ++
      new TinyIrisConfig(sim))

class TacitIrisDMAConfig(sim: Boolean = false)
    extends Config(
      new tacit.WithTraceSinkDMA(1) ++
      new tacit.WithTraceSinkAlways(0) ++
      new WithTacitTraceArbiterMonitor ++
      new WithTacitEncoder ++
      new IrisConfig(sim))

class TacitTinyIrisDMAConfig(sim: Boolean = false)
    extends Config(
      new tacit.WithTraceSinkDMA(1) ++
      new tacit.WithTraceSinkAlways(0) ++
      new WithTacitTraceArbiterMonitor ++
      new WithTacitEncoder ++
      new TinyIrisConfig(sim))

class TacitTinyIrisDMAParallelConfig(sim: Boolean = false)
    extends Config(
      new tacit.WithTraceSinkDMA(1) ++
      new tacit.WithTraceSinkAlways(0) ++
      new WithTacitTraceArbiterMonitor ++
      new WithTacitParallelEncoder ++
      new TinyIrisConfig(sim))
*/
