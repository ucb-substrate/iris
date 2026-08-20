#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <riscv-pk/encoding.h>
#include "marchid.h"
#include "mmio.h"
#include "router.h"
#include "ucie.h"

#define OFFCHIP_OFFSET 0x800000000L
#define UCIE0_REG_BASE  0x200000UL
#define UCIE1_REG_BASE  0x208000UL

int main(void) {

  int chip_id = reg_read64(CHIP_ID_ADDR);

  printf("Got chip ID: %d\n", chip_id);

  setup_ucie(UCIE0_REG_BASE);
  reg_write64(UCIE0_REG_BASE + UCIE_MAINBAND_MODE, UCIE_BAND_MODE_TL);
  setup_ucie(UCIE1_REG_BASE);
  reg_write64(UCIE1_REG_BASE + UCIE_MAINBAND_MODE, UCIE_BAND_MODE_TL);

  int next = (chip_id % 4) + 1;
  int prev = ((chip_id + 2) % 4) + 1;

  program_router(0, next, 1);
  program_router(1, prev, 0);

  int neighbor0 = reg_read64(CHIP_ID_ADDR + OFFCHIP_OFFSET * next);
  int neighbor1 = reg_read64(CHIP_ID_ADDR + OFFCHIP_OFFSET * prev);

  printf("I am chip %d and my neighbors are %d and %d\n", chip_id, neighbor0, neighbor1);

  return 0;
}
