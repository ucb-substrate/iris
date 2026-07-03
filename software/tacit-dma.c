#include <stdio.h>
#include <stdint.h>
#include "mmio.h"

#define TRACE_CTRL_BASE(tile) (0x03000000UL + ((uintptr_t)(tile) * 0x1000UL))
#define TRACE_CTRL_CONTROL    0x00
#define TRACE_CTRL_TARGET     0x20
#define TRACE_CTRL_BP_MODE    0x24

#define TRACE_DMA_BASE(tile)  (0x03010000UL + ((uintptr_t)(tile) * 0x1000UL))
#define TRACE_DMA_START_ADDR 0x00
#define TRACE_DMA_COUNTER    0x08
#define TRACE_DMA_MAX_SIZE   0x10
#define TRACE_DMA_RESET      0x18
#define TRACE_DMA_MODE       0x1c
#define TRACE_DMA_WRAP_COUNT 0x20
#define TRACE_DMA_SRC_STALLS 0x24

#define TRACE_DMA_MODE_OVERFLOW 0

#define TRACE_DUMP_BASE      0x90000000UL
#define TRACE_DUMP_STRIDE    0x00100000UL
#define TRACE_DUMP_MAX_SIZE  0x00010000UL

static void delay_cycles(unsigned cycles)
{
  for (volatile unsigned i = 0; i < cycles; i++) {
    __asm__ volatile ("nop");
  }
}

static void tacit_select_dma(unsigned tile)
{
  uintptr_t ctrl = TRACE_CTRL_BASE(tile);
  uintptr_t dma = TRACE_DMA_BASE(tile);

  reg_write32(ctrl + TRACE_CTRL_CONTROL, 1);
  reg_write32(dma + TRACE_DMA_RESET, 1);
  delay_cycles(64);

  reg_write64(dma + TRACE_DMA_START_ADDR, TRACE_DUMP_BASE + tile * TRACE_DUMP_STRIDE);
  reg_write64(dma + TRACE_DMA_MAX_SIZE, TRACE_DUMP_MAX_SIZE);
  reg_write32(dma + TRACE_DMA_MODE, TRACE_DMA_MODE_OVERFLOW);

  reg_write32(ctrl + TRACE_CTRL_TARGET, 1);
  reg_write32(ctrl + TRACE_CTRL_BP_MODE, 0);
  reg_write32(ctrl + TRACE_CTRL_CONTROL, 3);
}

static void tacit_stop_trace(unsigned tile)
{
  uintptr_t ctrl = TRACE_CTRL_BASE(tile);
  reg_write32(ctrl + TRACE_CTRL_CONTROL, 1);
  delay_cycles(1024);
  __asm__ volatile ("fence" ::: "memory");
}

static volatile uint64_t sink;

static void branch_workload(void)
{
  uint64_t acc = 0x5678;
  for (uint64_t i = 0; i < 128; i++) {
    if ((i & 7) < 3) {
      acc += (i * 17) ^ 0xabc;
    } else if ((i & 7) < 6) {
      acc ^= (acc << 5) + i;
    } else {
      acc -= (i << 2) | 0x5a;
    }
  }
  sink = acc;
}

static void dump_trace_hex(unsigned tile, uint64_t count)
{
  volatile uint64_t *trace = (volatile uint64_t *)(TRACE_DUMP_BASE + tile * TRACE_DUMP_STRIDE);
  uint64_t words = count / 8;

  printf("TACIT_DMA_HEX64LE_BEGIN tile=%u bytes=%lu words=%lu\n", tile, count, words);
  for (uint64_t i = 0; i < words; i++) {
    printf("%016lx", trace[i]);
    if ((i & 3) == 3) {
      printf("\n");
    }
  }
  if ((words & 3) != 0) {
    printf("\n");
  }
  printf("TACIT_DMA_HEX64LE_END tile=%u\n", tile);
}

int main(void)
{
  const unsigned tile = 0;

  tacit_select_dma(tile);
  branch_workload();
  tacit_stop_trace(tile);

  uint64_t count = reg_read64(TRACE_DMA_BASE(tile) + TRACE_DMA_COUNTER);
  uint32_t wraps = reg_read32(TRACE_DMA_BASE(tile) + TRACE_DMA_WRAP_COUNT);
  uint32_t stalls = reg_read32(TRACE_DMA_BASE(tile) + TRACE_DMA_SRC_STALLS);

  printf("tacit dma done sink=%lu count=%lu wraps=%u stalls=%u\n", sink, count, wraps, stalls);
  dump_trace_hex(tile, count);
  return 0;
}
