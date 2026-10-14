/*
 * Raspberry Pi VideoCore IV V3D block.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef HW_MISC_BCM2835_V3D_H
#define HW_MISC_BCM2835_V3D_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_BCM2835_V3D "bcm2835_v3d"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835V3DState, BCM2835_V3D)

#define BCM2835_V3D_REG_SIZE 0x1000
#define BCM2835_V3D_INTCTL 0x030
#define BCM2835_V3D_INTENA 0x034
#define BCM2835_V3D_INTDIS 0x038

struct BCM2835V3DState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    qemu_irq irq;
    uint32_t regs[BCM2835_V3D_REG_SIZE / sizeof(uint32_t)];
    uint32_t irq_status;
    uint32_t irq_enable;

    uint32_t ct_cs[2];
    uint32_t ct_ca[2];
    uint32_t ct_ea[2];
    uint32_t ct_ra0[2];
    uint32_t ct_lc[2];
    uint32_t ct_pc[2];
    uint32_t ct_return_addr[4];
    uint8_t ct_return_depth[2];
    uint8_t ct_semaphore[2];
    bool ct_waiting[2];

    uint32_t render_base;
    uint16_t render_width;
    uint16_t render_height;
    uint16_t render_config;
    uint8_t tile_x;
    uint8_t tile_y;
    uint32_t clear_color[2];
    bool render_config_valid;
    bool clear_color_valid;
};

#endif /* HW_MISC_BCM2835_V3D_H */
