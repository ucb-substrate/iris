#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include "marchid.h"
#include "mmio.h"

#define ROUTER_MMIO 0x4000
#define CHIP_ID_ADDR 0x4080
#define OFFCHIP_OFFSET 0x800000000L

void program_router(uint64_t chip_id, uint64_t port, uint64_t table_entry) {
  uint64_t base = ROUTER_MMIO + table_entry * 32;
  reg_write64(base + 0,  1);        // valid
  reg_write64(base + 8,  chip_id);  // chipID
  reg_write64(base + 16, port);     // port
}

int main(void) {

  int chip_id = reg_read64(CHIP_ID_ADDR);

  printf("Got chip ID: %d\n", chip_id);

  int next = (chip_id % 4) + 1;
  int prev = ((chip_id + 2) % 4) + 1;

  program_router(next, 1, 0);
  program_router(prev, 0, 1);

  int neighbor0 = reg_read64(CHIP_ID_ADDR + OFFCHIP_OFFSET * next);
  int neighbor1 = reg_read64(CHIP_ID_ADDR + OFFCHIP_OFFSET * prev);

  printf("I am chip %d and my neighbors are %d and %d\n", chip_id, neighbor0, neighbor1);

  return 0;
}