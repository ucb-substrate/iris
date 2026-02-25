# Iris Digital

Iris consists of two Shuttle cores with outer product units and two UCIe modules.

## Usage

Iris uses [Chippy](https://github.com/ucb-substrate/chippy) to enable standalone compilation.

First, install Chippy:

```bash
git clone https://github.com/ucb-substrate/chippy.git
cd chippy
./mill __.publishLocal
cd ..
```

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
