#include <stdio.h>
#include <riscv-pk/encoding.h>
#include <stdint.h>
#include "mmio.h"

#define CHIP_ID_ADDR 0x4080

int main(void) {
  int chip_id = reg_read64(CHIP_ID_ADDR);
  printf("Hello world chip %d\n", chip_id);
  return 0;
}

