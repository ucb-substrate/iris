package edu.berkeley.cs.iris

import org.chipsalliance.cde.config.Parameters
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.subsystem.TLBusWrapperLocation
import edu.berkeley.cs.uciedigital.tilelink.UcieTLParams
import testchipip.soc.{
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
  def controlManagerBusWhere: Option[TLBusWrapperLocation] =
    ucie.controlManagerBusWhere

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
    complex { ucie.instantiate(params, id) }
  }
}

object UciePort {

  /** The UCIe parameters of a chiplet-router port, wrapped or not. */
  def unapply(link: ChipletLinkParams): Option[UcieTLParams] = link match {
    case wrapped: UcieComplexPort => Some(wrapped.ucie)
    case direct: UcieTLParams     => Some(direct)
    case _                        => None
  }

  /** The UCIe ports of the chiplet router, in port order. */
  def all(p: Parameters): Seq[UcieTLParams] = p(ChipletRoutingKey)
    .map(_.ports.collect { case UciePort(u) => u })
    .getOrElse(Nil)
}
