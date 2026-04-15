package edu.berkeley.cs.iris

import os.Path
import circt.stage.ChiselStage
import java.nio.file.Paths
import testchipip.dram.SimDRAM
import org.chipsalliance.cde.config.Parameters

object Utils {
  val root = Path(
    Paths.get(sys.env("MILL_TEST_RESOURCE_DIR")).toAbsolutePath
  ) / os.up / os.up
  val buildRoot = root / "build"

  def writeSourceFilesList(path: Path, sourceFiles: Seq[Path]) = {
    os.makeDir.all(path / os.up)
    os.write.over(path, sourceFiles.map(_.toString).mkString("\n"))
  }

  def writeVerilatorSimScript(
      path: Path,
      topModule: String,
      sourceFilesList: Path,
      incDirs: Seq[Path] = Seq.empty,
      optLevel: Option[String] = None
  ) = {
    os.makeDir.all(path / os.up)
    os.write.over(
      path,
      s"""#!/bin/bash
set -ex -o pipefail
verilator \\
  --cc \\
  --exe \\
  --build \\
  --main \\
  -o ../simulation \\
  --top-module ${topModule} \\
  --Mdir verilated-sources \\
  --assert \\
  --timing \\
  --max-num-width 1048576 \\${optLevel match {
          case Some(v) => s"\n  $v \\"
          case None    => ""
        }}${incDirs.map(dir => s"\n  +incdir+$dir \\").mkString("")}
  --vpi \\
  +define+layer$$Verification$$Assert$$Temporal \\
  +define+layer$$Verification$$Assume$$Temporal \\
  +define+layer$$Verification$$Cover$$Temporal \\
  +define+VERILATOR \\
  -Wno-fatal \\
  -CFLAGS "$$CXXFLAGS -O3 -std=c++17 -DVERILATOR -I$$RISCV/include" \\
  -LDFLAGS "$$LDFLAGS -L$$RISCV/lib -Wl,-rpath,$$RISCV/lib -lriscv -lfesvr" \\
  -F ${sourceFilesList.toString}
script -f -c "./simulation </dev/null 2> >(spike-dasm > simulation.out)" simulation.log
"""
    )
    path.toIO.setExecutable(true)
  }

  def writeVcsSimScript(
      path: Path,
      topModule: String,
      sourceFilesList: Path,
      incDirs: Seq[Path] = Seq.empty,
      loadmem: Option[Path] = None,
      debug: Boolean = false
  ) = {
    val dramsim_ini = root / "testchipip" / "src" / "main" / "resources" / "dramsim2_ini"
    val debugCompileFlags = if (debug) " +define+DEBUG -debug_access+all -kdb -lca" else ""
    val debugRuntimeFlag = if (debug) " +fsdbfile=waveform.fsdb" else ""
    os.makeDir.all(path / os.up)
    os.write.over(
      path,
      s"""#!/bin/bash
set -ex -o pipefail
vcs \\
  -full64 -j16 -fgp \\
  -CFLAGS "$$CXXFLAGS -O3 -std=c++17 -I$$RISCV/include -I${root.toString}/DRAMSim2" \\
  -LDFLAGS "$$LDFLAGS -L$$RISCV/lib -Wl,-rpath,$$RISCV/lib" \\
  -lriscv -lfesvr -ldramsim \\
  -notice -line +lint=all,noVCDE,noONGS,noUI -error=PCWM-L -error=noZMMCM \\
  -timescale=1ns/10ps -quiet -q +rad +vcs+lic+wait +vc+list \\
  -f ${sourceFilesList.toString} -sverilog +systemverilogext+.sv+.svi+.svh+.svt -assert svaext +libext+.sv +v2k +verilog2001ext+.v95+.vt+.vp +libext+.v \\
  -debug_pp \\
  -top $topModule \\${incDirs.map(dir => s"\n  +incdir+$dir \\").mkString("")}
  +define+layer$$Verification$$Assert$$Temporal \\
  +define+layer$$Verification$$Assume$$Temporal \\
  +define+layer$$Verification$$Cover$$Temporal \\
  +define+VCS +define+FSDB +define+RANDOMIZE_MEM_INIT +define+RANDOMIZE_REG_INIT +define+RANDOMIZE_GARBAGE_ASSIGN +define+RANDOMIZE_INVALID_ASSIGN$debugCompileFlags \\
  -o simulation -Mdir=vcs-sources
script -f -c "./simulation +permissive +vcs+thread+16 +dramsim +dramsim_ini_dir=${dramsim_ini.toString}${loadmem.map(p => s" +loadmem=${p.toString}").getOrElse("")}$debugRuntimeFlag +permissive-off placeholder-binary </dev/null 2> >(spike-dasm > simulation.out)" simulation.log
"""
    )
    path.toIO.setExecutable(true)
  }

  /** Finds source files within a given source directory with the given file
    * extensions.
    */
  def getSourceFiles(
      sourceDir: Path,
      fileExtensions: Seq[String] = Seq(".v", ".sv", ".cc", ".vams")
  ): Seq[Path] = {
    os
      .walk(sourceDir)
      .filter(os.isFile)
      .filter(path => fileExtensions.exists(ext => path.last.endsWith(ext)))
  }

  def simulateTopWithBinaries(
      workDir: Path,
      nChips: Int,
      binaryPaths: Seq[Path],
      plusArgs: Seq[Seq[String]] = Seq.empty,
      fast: Boolean = false,
      debug: Boolean = false
  )(implicit p: Parameters) = {
    binaryPaths.zipWithIndex.foreach { case (path, i) =>
      assert(
        os.exists(path),
        s"The provided chip $i binary ($path) does not exist. You may have to run `make` in the `software/` directory to make the binary first"
      )
    }

    os.makeDir.all(workDir)

    val sourceDir = workDir / "src"
    val simDir = workDir / "sim"
    val artifactsDir = workDir / "artifacts"
    os.remove.all(sourceDir)
    os.makeDir.all(simDir)
    os.makeDir.all(artifactsDir)

    ChiselStage.emitSystemVerilogFile(
      new SimTop(nChips, binaryPaths, plusArgs, fast),
      args = Array(
        "--target-dir",
        sourceDir.toString
      )
    )
    freechips.rocketchip.util.ElaborationArtefacts.files.foreach { case (extension, contents) =>
      os.write.over(artifactsDir / s"Iris.${extension}", contents ())
    }
    val sourceFiles = getSourceFiles(sourceDir)

    val sourceFilesList = simDir / "sourceFiles.F"
    val simScript = simDir / "simulate.sh"

    writeSourceFilesList(sourceFilesList, sourceFiles)

    writeVcsSimScript(
      simScript,
      "SimTop",
      sourceFilesList,
      incDirs = os.walk(sourceDir).filter(os.isDir) ++ Seq(sourceDir),
      loadmem = if (fast) Some(binaryPaths(0)) else None,
      debug = debug
    )

    os.proc(
      "/bin/bash",
      simScript
    ).call(stdout = os.Inherit, stderr = os.Inherit, cwd = simDir)
  }
}
