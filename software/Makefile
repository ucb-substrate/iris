#################################
# Paths
#################################

# Repo root, derived from this Makefile's own path rather than the working
# directory, so the mill invocation below works under `make -C`, `make -f`, or
# a plain `make` from this folder.
REPO_ROOT := $(abspath $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/..)

#################################
# RISCV Toolchain
#################################

TARGET = riscv64-unknown-elf

GCC = $(TARGET)-gcc
CXX = $(TARGET)-g++
CP = $(TARGET)-objcopy
OBJDUMP = $(TARGET)-objdump
DG = $(TARGET)-gdb
SIZE = $(TARGET)-size

#################################
# Flags
#################################

# SoC Settings
ARCH = rv64imafdc
ABI = lp64d
ARCHFLAGS = -march=$(ARCH) -mabi=$(ABI)

# rv64gcv_zfh_zvfh (not $(ARCH)): the RVV kernels in ucie-vec-memcpy.c and
# multihart-vec-amo.c need the vector extension. Kept separate so it doesn't
# force V onto every program.
VEC_ARCHFLAGS = -march=rv64gcv_zfh_zvfh -mabi=$(ABI)

CFLAGS  = -std=gnu99 -O2 -fno-common -fno-builtin-printf -Wall -specs=htif_nano.specs -Iinclude
# --wrap=exit routes exit() through exit.c's fix for riscv-tools 1.0.9.
LDFLAGS = -specs=htif_nano.specs -Wl,--wrap=exit

PROGRAMS = hello hello0 hello1 router ring-hello ucie-loopback ucie-digital-loopback tacit-always tacit-dma ucie-vec-memcpy multihart-vec-amo clock-div

.DEFAULT_GOAL := default

#################################
# Build
#################################

%.o: %.S
	$(GCC) $(CFLAGS) $(ARCHFLAGS) -D__ASSEMBLY__=1 -c $< -o $@

%.o: %.c
	$(GCC) $(CFLAGS) $(ARCHFLAGS) -c $< -o $@

# exit.o wraps newlib-nano's exit(); see exit.c.
%.riscv: %.o exit.o
	$(GCC) $(LDFLAGS) $^ -o $@

ucie-vec-memcpy.o: ucie-vec-memcpy.c
	$(GCC) $(CFLAGS) $(VEC_ARCHFLAGS) -c $< -o $@

ucie-vec-memcpy.riscv: ucie-vec-memcpy.o exit.o
	$(GCC) $(LDFLAGS) $(VEC_ARCHFLAGS) $^ -o $@

multihart-vec-amo.o: multihart-vec-amo.c
	$(GCC) $(CFLAGS) $(VEC_ARCHFLAGS) -c $< -o $@

multihart-vec-amo.riscv: multihart-vec-amo.o exit.o
	$(GCC) $(LDFLAGS) $(VEC_ARCHFLAGS) $^ -o $@

# saturn-vectors' MX vector benchmarks, built here so nothing is written into
# that submodule. Same flags and bare-metal runtime (crt.S, syscalls.c, test.ld)
# as its benchmarks/Makefile, against its data.S. A copy in saturn-benchmarks/
# replaces upstream's main.c (vec-mx-matmul, whose check cannot fail upstream).
SATURN_BENCH = $(REPO_ROOT)/saturn-vectors/benchmarks
SATURN_PROGRAMS = vec-mx-binary vec-mx-unary vec-mx-matmul
SATURN_CFLAGS = -DPREALLOCATE=1 -mcmodel=medany -static -O2 -g -ffast-math -fno-common \
	-fno-builtin-printf -fno-tree-loop-distribute-patterns -march=rv64gcv_zfh_zvfh -mabi=$(ABI) -std=gnu99
SATURN_LDFLAGS = -static -nostdlib -nostartfiles -lm -lgcc -T $(SATURN_BENCH)/common/test.ld
SATURN_COMMON = $(wildcard $(SATURN_BENCH)/common/*.c $(SATURN_BENCH)/common/*.S $(SATURN_BENCH)/common/ara/*.c)
saturn_main = $(firstword $(wildcard saturn-benchmarks/$(1).c) $(SATURN_BENCH)/$(1)/main.c)

define saturn_template
$(1).riscv: $(call saturn_main,$(1)) $(SATURN_BENCH)/$(1)/data.S $(wildcard $(SATURN_BENCH)/common/*)
	$$(GCC) -I$(SATURN_BENCH)/env -I$(SATURN_BENCH)/common -I$(SATURN_BENCH)/$(1) $$(SATURN_CFLAGS) \
		-o $$@ $(call saturn_main,$(1)) $(SATURN_BENCH)/$(1)/data.S $$(SATURN_COMMON) $$(SATURN_LDFLAGS)
endef
$(foreach b,$(SATURN_PROGRAMS),$(eval $(call saturn_template,$(b))))

%.dump: %.riscv
	$(OBJDUMP) -D $< > $@


#################################
# Recipes
#################################

# ucie.h holds the UCIe register offsets, generated from the UCIe parameters in
# IrisConfig. It is checked in so building these programs needs only the RISC-V
# toolchain; regenerating it needs the Scala toolchain, so it is not a
# prerequisite of the program rules. Re-run this after bumping the ucie
# submodule or changing the UCIe ports in IrisConfig -- CI checks it is current.
.PHONY: ucie.h
ucie.h:
	cd $(REPO_ROOT) && ./mill runMain edu.berkeley.cs.iris.GenUcieHeader software/ucie.h

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump

.PHONY: default
default: $(addsuffix .riscv, $(PROGRAMS) $(SATURN_PROGRAMS))

.PHONY: dumps
dumps: $(addsuffix .dump, $(PROGRAMS) $(SATURN_PROGRAMS))

