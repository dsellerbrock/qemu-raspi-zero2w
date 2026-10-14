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
#define BCM2835_V3D_MAX_TILES 4096

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
    uint32_t tile_color[64 * 64];
    bool render_config_valid;
    bool clear_color_valid;
    bool tile_color_valid;

    uint32_t bin_alloc_base;
    uint32_t bin_alloc_size;
    uint32_t bin_state_base;
    uint8_t bin_tiles_x;
    uint8_t bin_tiles_y;
    uint8_t bin_flags;
    uint64_t bin_alloc_next;
    uint16_t bin_alloc_block_size;
    uint16_t bin_alloc_initial_block_size;
    uint32_t bin_draw_append_count;
    bool bin_draw_resume;
    bool bin_oom_waiting;
    bool bin_spilluse;
    uint32_t bin_tile_list_addr[BCM2835_V3D_MAX_TILES];
    uint16_t bin_tile_list_used[BCM2835_V3D_MAX_TILES];
    uint32_t bin_list_stride;
    bool bin_list_indirect;
    bool bin_config_valid;
    bool binning_started;
    uint32_t shader_state;
    bool shader_state_valid;
    uint8_t shader_record[132];
    uint16_t shader_record_size;
    uint8_t shader_attribute_count;
    bool shader_record_extended;
    uint32_t clipper_xy_scale[2];
    uint32_t clipper_z_min;
    uint32_t clipper_z_max;
    int16_t viewport_center_x;
    int16_t viewport_center_y;
    bool clipper_xy_scale_valid;
    bool clipper_z_clip_valid;
    bool viewport_offset_valid;
    uint8_t tile_primitive_format;
    bool tile_primitive_format_valid;
    uint32_t pending_tile_load_base;
    uint8_t pending_tile_load_memory_format;
    uint8_t pending_tile_load_color_format;
    bool pending_tile_load;
};

#endif /* HW_MISC_BCM2835_V3D_H */
