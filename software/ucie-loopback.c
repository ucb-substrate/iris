#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include "marchid.h"
#include "mmio.h"
#include "router.h"
#include "ucie.h"

#define UCIE0_REG_BASE 0x8000UL
#define UCIE1_REG_BASE 0xc000UL
#define ROUTER_MMIO 0x4000
#define CHIP_ID_ADDR 0x4080
#define OFFCHIP_OFFSET 0x800000000L

uint32_t src[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
uint32_t dest[10];
uint32_t test[10];

int rw_mem(uint64_t offset) {
  // remote = dest on the other chip: same local address as our dest, tagged with target chip ID.
  // offset = OFFCHIP_OFFSET * target_chipId (a byte offset, added via uint8_t* cast).
  void *remote = (void *)((uint8_t *)dest + offset);

  size_t write_start = rdcycle();
  memcpy(remote, src, sizeof(src));
  size_t write_end = rdcycle();

  printf("Wrote %ld bytes in %ld cycles\n", sizeof(src), write_end - write_start);

  size_t read_start = rdcycle();
  memcpy(test, remote, sizeof(src));
  size_t read_end = rdcycle();

  for (int i = 0; i < 10; i++) {
      if (src[i] != test[i]) {
        printf("Remote write/read failed at index %d: wrote %x, read %x\n", i, src[i], test[i]);
        exit(1);
      }
  }

  printf("Read %ld bytes in %ld cycles\n", sizeof(src), read_end - read_start);

  return 0;
}

int main(void) {

  int chip_id = reg_read64(CHIP_ID_ADDR);

  printf("Got chip ID: %d\n", chip_id);

  printf("Setting up UCIe0\n");
  setup_ucie(UCIE0_REG_BASE);
  printf("Setting up UCIe1\n");
  setup_ucie(UCIE1_REG_BASE);

  printf("Testing UCIe at port 0\n");
  program_router(0, 1, 0);
  rw_mem(OFFCHIP_OFFSET);
  
  printf("Testing UCIe at port 1\n");
  program_router(0, 1, 1);
  rw_mem(OFFCHIP_OFFSET);

  printf("UCIe loopback test complete\n");

  return 0;
}