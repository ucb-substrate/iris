package edu.berkeley.cs.iris

import org.chipsalliance.diplomacy.lazymodule._
import _root_.circt.stage.ChiselStage
import org.scalatest.funspec.AnyFunSpec

class OPUSpec extends AnyFunSpec {
  describe("OPU") {
    it("should generate valid System Verilog for OPUV128D64") {
      val targetDir = Utils.buildRoot / "OPU_V128D64_should_generate_valid_System_Verilog"
      implicit val p = new OPUV128D64IrisConfig
      ChiselStage.emitSystemVerilogFile(
        LazyModule(new OPUTop).module,
        args = Array("--target-dir", targetDir.toString())
      )
      freechips.rocketchip.util.ElaborationArtefacts.files.foreach { case (extension, contents) =>
        os.write.over(targetDir / s"OPUV128D64.${extension}", contents())
      }
    }

    it("should generate valid System Verilog for OPUV512D256") {
      val targetDir = Utils.buildRoot / "OPU_V512D256_should_generate_valid_System_Verilog"
      implicit val p = new OPUV512D256IrisConfig
      ChiselStage.emitSystemVerilogFile(
        LazyModule(new OPUTop).module,
        args = Array("--target-dir", targetDir.toString())
      )
      freechips.rocketchip.util.ElaborationArtefacts.files.foreach { case (extension, contents) =>
        os.write.over(targetDir / s"OPUV512D256.${extension}", contents())
      }
    }
  }
}
