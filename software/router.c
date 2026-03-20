#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include "marchid.h"
#include "mmio.h"

#define ROUTER_MMIO 0x4000
#define CHIP_ID_ADDR 0x4080
#define OFFCHIP_OFFSET 0x200000000L

uint32_t src[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
uint32_t dest[10];
uint32_t test[10];

void program_router(uint64_t chip_id, uint64_t port, uint64_t table_entry) {
  uint64_t base = ROUTER_MMIO + table_entry * 32;
  reg_write64(base + 0,  1);        // valid
  reg_write64(base + 8,  chip_id);  // chipID
  reg_write64(base + 16, port);     // port
}

int rw_mem(uint64_t offset) {
  size_t write_start = rdcycle();
  memcpy(dest + offset, src, sizeof(src));
  size_t write_end = rdcycle();

  printf("Wrote %ld bytes in %ld cycles\n", sizeof(src), write_end - write_start);

  size_t read_start = rdcycle();
  memcpy(test, dest + offset, sizeof(src));
  size_t read_end = rdcycle();

  for (int i = 0; i < sizeof(src); i++) {
      if (src[i] != test[i]) {
      printf("Remote write/read failed at %p %p %p %x %x\n", src+i, test+i, dest + offset + i, src[i], test[i]);
      exit(1);
      }
  }

  printf("Read %ld bytes in %ld cycles\n", sizeof(src), read_end - read_start);

  return 0;
}

int main(void) {

  int chip_id = reg_read64(CHIP_ID_ADDR);

  printf("Got chip ID: %d\n", chip_id);

  if (chip_id == 1) {
    program_router(2, 0, 0);
    rw_mem(OFFCHIP_OFFSET * 2);
    printf("Chip 1 DONE\n");
  } else {
    program_router(1, 0, 0);
    rw_mem(OFFCHIP_OFFSET * 1);
    printf("Chip 2 DONE\n");
  }

  return 0;
}