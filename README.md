# Iris

Iris is a chip consisting of two Shuttle cores with outer product units and two UCIe modules.

This repo only contains the process-agnostic portions of the chip. Process-specific tapeout collateral is only accessible to BWRC members.

## Usage

Install [espresso](https://github.com/chipsalliance/espresso) for NoC generation. If using a Chipyard environment, espresso should already be on PATH.

Then, compile Iris:

```bash
git clone git@github.com:ucb-substrate/iris.git
cd iris
git submodule update --init
./mill compile
```

To generate top-level Verilog, run the following:

```
./mill test.testOnly edu.berkeley.cs.iris.IrisSpec -- -z Verilog
```
