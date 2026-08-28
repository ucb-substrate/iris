package edu.berkeley.cs.iris

import org.chipsalliance.cde.config.Parameters
import testchipip.soc.ChipletRoutingKey
import edu.berkeley.cs.uciedigital.tilelink.UcieTLParams

/** Writes the bringup software's `ucie.h` from the UCIe parameters IrisConfig
  * actually uses.
  *
  * The register layout depends on those parameters -- the per-lane arrays are
  * sized from `numLanes`, for one -- so generating from the ucie defaults would
  * silently describe a different chip the moment IrisConfig moves off them.
  *
  * Run as:
  * {{{
  * ./mill runMain edu.berkeley.cs.iris.GenUcieHeader software/ucie.h
  * }}}
  * or `make ucie.h` from `software/`.
  */
object GenUcieHeader {
  val regenCommand =
    "./mill runMain edu.berkeley.cs.iris.GenUcieHeader software/ucie.h"

  /** The UCIe ports of IrisConfig, in chiplet-router port order. */
  def uciePorts: Seq[UcieTLParams] = {
    val p: Parameters = new IrisConfig
    val ports = p(ChipletRoutingKey)
      .map(_.ports.collect { case u: UcieTLParams => u })
      .getOrElse(Nil)
    require(ports.nonEmpty, "IrisConfig has no UCIe chiplet-router ports")
    ports
  }

  def render(): String = {
    val ports = uciePorts
    // The header holds one register layout, so every port has to agree on
    // everything that layout depends on. Address and orientation do not reach
    // it: the offsets are relative to each port's own MMIO base, and the
    // orientation only names modules.
    val canonical = ports.map(
      _.copy(
        address = ports.head.address,
        orientation = ports.head.orientation
      )
    )
    require(
      canonical.forall(_ == canonical.head),
      "UCIe ports must share a register layout, but their parameters differ " +
        s"beyond address and orientation: ${canonical.distinct.mkString("\n")}"
    )
    edu.berkeley.cs.uciedigital.tilelink.GenUcieHeader
      .render(ports.head, regenCommand)
  }

  def main(args: Array[String]): Unit = {
    require(
      args.length == 1,
      s"Usage: GenUcieHeader <output-path>; got ${args.mkString(" ")}"
    )
    val out = os.Path(args(0), os.pwd)
    os.makeDir.all(out / os.up)
    os.write.over(out, render())
    println(s"Wrote $out")
  }
}
