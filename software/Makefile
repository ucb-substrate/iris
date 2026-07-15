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

# rv64gcv_zfh_zvfh (not $(ARCH)): ucie-vec-memcpy.c's RVV kernel needs the
# vector extension. Kept separate so it doesn't force V onto every program.
VEC_ARCHFLAGS = -march=rv64gcv_zfh_zvfh -mabi=$(ABI)

CFLAGS  = -std=gnu99 -O2 -fno-common -fno-builtin-printf -Wall -specs=htif_nano.specs
LDFLAGS = -specs=htif_nano.specs

PROGRAMS = hello hello0 hello1 router ring-hello ucie-loopback tacit-always tacit-dma ucie-vec-memcpy

.DEFAULT_GOAL := default

#################################
# Build
#################################

%.o: %.S
	$(GCC) $(CFLAGS) $(ARCHFLAGS) -D__ASSEMBLY__=1 -c $< -o $@

%.o: %.c
	$(GCC) $(CFLAGS) $(ARCHFLAGS) -c $< -o $@

%.riscv: %.o
	$(GCC) $(LDFLAGS) $< -o $@

ucie-vec-memcpy.o: ucie-vec-memcpy.c
	$(GCC) $(CFLAGS) $(VEC_ARCHFLAGS) -c $< -o $@

ucie-vec-memcpy.riscv: ucie-vec-memcpy.o
	$(GCC) $(LDFLAGS) $(VEC_ARCHFLAGS) $< -o $@

%.dump: %.riscv
	$(OBJDUMP) -D $< > $@


#################################
# Recipes
#################################

.PHONY: clean
clean:
	rm -f *.riscv *.o *.dump

.PHONY: default
default: $(addsuffix .riscv, $(PROGRAMS))

.PHONY: dumps
dumps: $(addsuffix .dump, $(PROGRAMS))

