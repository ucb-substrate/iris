#include <stdio.h>
#include <stdint.h>
#include "mmio.h"

#define TRACE_CTRL_BASE(tile) (0x03000000UL + ((uintptr_t)(tile) * 0x1000UL))
#define TRACE_CTRL_CONTROL    0x00
#define TRACE_CTRL_TARGET     0x20
#define TRACE_CTRL_BP_MODE    0x24

static void tacit_select_always(unsigned tile)
{
  uintptr_t base = TRACE_CTRL_BASE(tile);
  reg_write32(base + TRACE_CTRL_TARGET, 0);
  reg_write32(base + TRACE_CTRL_BP_MODE, 0);
  reg_write32(base + TRACE_CTRL_CONTROL, 3);
}

static volatile uint64_t sink;

static void branch_workload(void)
{
  uint64_t acc = 0x1234;
  for (uint64_t i = 0; i < 4096; i++) {
    if ((i & 3) == 0) {
      acc += i ^ 0x55;
    } else if ((i & 3) == 1) {
      acc ^= (i << 1);
    } else if ((i & 3) == 2) {
      acc -= i | 0x33;
    } else {
      acc += (acc >> 3) ^ i;
    }
  }
  sink = acc;
}

int main(void)
{
  for (unsigned tile = 0; tile < 3; tile++) {
    tacit_select_always(tile);
  }
  branch_workload();
  printf("tacit always done sink=%lu\n", sink);
  return 0;
}
