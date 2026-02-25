# Iris Digital

Iris consists of two Shuttle cores with outer product units and two UCIe modules.

## Usage

Iris uses [Chippy](https://github.com/ucb-substrate/chippy) to enable standalone compilation.

First, install Chippy:

```bash
git clone https://github.com/ucb-substrate/chippy.git
git submodule update --init --recursive
cd chippy
./mill __.publishLocal
cd ..
```

Install [espresso](https://github.com/chipsalliance/espresso) for NoC generation. If using a Chipyard environment, espresso should already be on PATH.

Then, compile Iris:

```bash
git clone git@github.com:ucb-substrate/iris-digital.git
cd iris-digital
git submodule update --init
./mill compile
```

To generate top-level Verilog, run the following:

```
./mill test.testOnly "*.DigitalChipSpec" -- -z Verilog
```
