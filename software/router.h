#ifndef __ROUTER_H__
#define __ROUTER_H__

#include <stdint.h>
#include "mmio.h"

#define ROUTER_MMIO  0x4000
#define CHIP_ID_ADDR 0x4080

static inline void program_router(uint64_t table_entry, uint64_t chip_id, uint64_t port) {
  uint64_t base = ROUTER_MMIO + table_entry * 32;
  reg_write64(base + 0,  1);        // valid
  reg_write64(base + 8,  chip_id);  // chipID
  reg_write64(base + 16, port);     // port
}

#endif
