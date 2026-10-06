package edu.berkeley.cs.iris

import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.prci._
import freechips.rocketchip.subsystem.{BaseSubsystem, TLBusWrapperLocation}
import freechips.rocketchip.tilelink._
import edu.berkeley.cs.uciedigital.tilelink.{UcieBumpsIO, UcieTL, UcieTLParams}
import testchipip.soc.{
  CanHaveChipletRouting,
  ChipletLinkParams,
  ChipletLinkWrapper,
  ChipletLinkWrapperInstantiationLike,
  ChipletRoutingKey,
  OffchipSubsystemParams
}

/** The module both UCIe links live in.
  *
  * The chiplet router instantiates its D2D ports as siblings of the router, its
  * address translators and its source shrinkers, all directly inside the
  * router's clock domain. Physical design hardens the two UCIe links as one
  * block, which needs them under a module of their own: this is that module,
  * and it holds nothing but the links and the wires their diplomatic ports
  * punch out to the level above.
  */
class UcieComplex(implicit p: Parameters)
    extends SimpleLazyModule
    with LazyScope {
  override lazy val desiredName = "UcieComplex"
}

/** A UCIe link as a chiplet-router port, minus its register blocks.
  *
  * The router attaches one control node per port to its control bus. A UCIe
  * link has two register blocks: the main one, on the clock its PHY makes, and
  * the clock register block, which sets that clock up and so runs on the
  * chip's. Rather than send one through the router and the other around it,
  * this hands the router neither, and [[CanHaveUcieRegisters]] attaches both
  * straight to the bus.
  *
  * Every port this link puts on [[UcieComplex]] is synchronous to the one
  * digital clock the router gives it, the register ports included. The main
  * register block crosses into the PHY's clock in here, rather than at the
  * bus, so the crossing stays inside the block physical design hardens and
  * its boundary has no clock the block's constraints don't already know.
  *
  * Otherwise this is the UCIe repo's `UcieChipletLink`, module name included,
  * so the hierarchy physical design sees does not move.
  */
class UcieComplexLink(
    val params: UcieTLParams,
    sysParams: OffchipSubsystemParams
)(implicit p: Parameters)
    extends ChipletLinkWrapper {
  override lazy val desiredName = s"UcieChipletLink${params.moduleSuffix}"

  val ucie = LazyModule(
    new UcieTL(
      params,
      sysParams.managerRegion,
      sysParams.managerBeatBytes,
      sysParams.managerBlockBytes
    )
  )

  // The link's digital clock. Named for the UcieTL node it used to feed
  // directly, so that the port it makes on UcieComplex is still
  // `auto_d2d<N>_port_ucie_digital_clock_in`, which the block's SDC names.
  val ucieDigitalClockNode = ClockSinkNode(Seq(ClockSinkParameters()))
  // It is also the chip clock the clock register block runs on: always
  // running, which is all that block asks of it, and the same clock the bus
  // reaches it on, so its register port needs no crossing.
  private val ucieClocks = ClockSourceNode(Seq.fill(2)(ClockSourceParameters()))
  ucie.digitalClockNode := ucieClocks
  ucie.chipDigitalClockNode := ucieClocks

  val client_node = ucie.clientNode
  val manager_node = ucie.managerNode
  val control_manager_node: Option[TLRegisterNode] = None
  val clock_node = Some(ucieDigitalClockNode)
  val top_IO = BundleBridgeSource(() => new UcieBumpsIO(params.numLanes))

  /** The main register block's port. The block runs on the clock the PHY
    * makes, which is unrelated to the digital clock, so the port crosses into
    * it: the source half here, the sink inside the block. One entry, like the
    * debug module's register crossing, since register accesses need no more.
    * The name node keeps the port it makes on UcieComplex called
    * `auto_d2d<N>_port_ucie_regs_in`, as it was before the crossing.
    */
  val regNode: TLInwardNode =
    ucie.regs.crossIn(ucie.regNode).apply(AsynchronousCrossing(depth = 1)) :=*
      TLNameNode("ucie_regs")

  /** The clock register block's port, already on the digital clock. */
  val clkRegNode: TLInwardNode = ucie.clkRegNode

  override lazy val module = new LazyRawModuleImp(this) {
    val digitalClock = ucieDigitalClockNode.in.head._1
    childClock := digitalClock.clock
    childReset := digitalClock.reset
    override def provideImplicitClockToLazyChildren = true

    ucieClocks.out.foreach { case (out, _) =>
      out.clock := digitalClock.clock
      out.reset := digitalClock.reset
    }
    ucie.module.io <> top_IO.out(0)._1
  }
}

/** A chiplet-router port whose UCIe link goes inside the shared [[UcieComplex]].
  *
  * The router builds each port by calling `instantiate` on the port parameters,
  * inside the scope of its own clock domain -- so this is the hook that decides
  * what the link's parent module is, and it needs nothing from testchipip
  * beyond what [[ChipletLinkWrapperInstantiationLike]] already asks for.
  *
  * The wrapper is created by whichever port is instantiated first and found
  * again by the rest. Looking it up in the router domain's children, rather
  * than holding it in a field here, is what keeps a config safe to elaborate
  * more than once: the multi-chip test harness builds several IrisTops from one
  * Parameters, and each gets its own router domain and hence its own wrapper.
  */
case class UcieComplexPort(ucie: UcieTLParams)
    extends ChipletLinkParams
    with ChipletLinkWrapperInstantiationLike {
  def managerBusWhere: TLBusWrapperLocation = ucie.managerBusWhere
  // The link gives the router no control node to attach; see
  // [[CanHaveUcieRegisters]] for where its registers go instead.
  def controlManagerBusWhere: Option[TLBusWrapperLocation] = None

  def instantiate(params: OffchipSubsystemParams, id: Int)(implicit
      p: Parameters
  ): ChipletLinkWrapper = {
    val routerDomain = LazyModule.getScope.getOrElse(
      throw new IllegalStateException(
        "UcieComplexPort.instantiate was called outside a LazyModule scope, " +
          "so there is nothing to hang the UcieComplex wrapper off of"
      )
    )
    val complex = routerDomain.getChildren
      .collectFirst { case c: UcieComplex => c }
      .getOrElse {
        val d2d_ports = LazyModule(new UcieComplex)
        d2d_ports.suggestName("d2d_ports")
        d2d_ports
      }
    complex { LazyModule(new UcieComplexLink(ucie, params)) }
  }
}

object UciePort {

  /** The UCIe parameters of a chiplet-router port. */
  def unapply(link: ChipletLinkParams): Option[UcieTLParams] = link match {
    case wrapped: UcieComplexPort => Some(wrapped.ucie)
    case _                        => None
  }

  /** The UCIe ports of the chiplet router, in port order. */
  def all(p: Parameters): Seq[UcieTLParams] = p(ChipletRoutingKey)
    .map(_.ports.collect { case UciePort(u) => u })
    .getOrElse(Nil)
}

/** Attaches the register blocks of every UCIe link directly to the chiplet
  * router's control bus.
  *
  * [[UcieComplexLink]] gives the router no control node, so this is the only
  * place they are attached. Both of its register ports are on the link's
  * digital clock, which the router already connects the link's data ports to
  * the buses on without a crossing, and the link does its own crossing into
  * the PHY's clock.
  *
  * The router keeps its ports to itself, so the links are found by walking the
  * subsystem's children. Extending [[CanHaveChipletRouting]] makes sure they
  * exist by the time this runs.
  */
trait CanHaveUcieRegisters extends CanHaveChipletRouting {
  this: BaseSubsystem =>
  p(ChipletRoutingKey).foreach { params =>
    // The UCIe repo's own link hands its main register block to the router and
    // has no way to hand over the clock register block, which would be left
    // dangling.
    require(
      !params.ports.exists(_.isInstanceOf[UcieTLParams]),
      "UCIe chiplet-router ports must be wrapped in UcieComplexPort, so that " +
        "CanHaveUcieRegisters can attach their register blocks"
    )

    def links(lm: LazyModule): Seq[UcieComplexLink] =
      lm.getChildren.reverse.flatMap {
        case link: UcieComplexLink => Seq(link)
        case child                 => links(child)
      }
    val ucieLinks = links(this)
    require(
      ucieLinks.size == UciePort.all(p).size,
      s"found ${ucieLinks.size} UCIe links under the subsystem, but the " +
        s"chiplet router has ${UciePort.all(p).size} UCIe ports"
    )

    val cbus = locateTLBusWrapper(params.controlBusWhere)
    ucieLinks.foreach { link =>
      Seq(
        "control" -> link.regNode,
        "clk_control" -> link.clkRegNode
      ).foreach { case (suffix, node) =>
        // The register nodes are as wide as the link's manager bus, which is
        // wider than the control bus.
        cbus.coupleTo(s"${link.name}_$suffix") {
          node := TLWidthWidget(cbus.beatBytes) := TLBuffer() :=
            TLFragmenter(cbus) := _
        }
      }
    }
  }
}
