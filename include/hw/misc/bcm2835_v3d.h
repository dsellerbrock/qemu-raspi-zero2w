/*
 * Raspberry Pi VideoCore IV V3D block.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef HW_MISC_BCM2835_V3D_H
#define HW_MISC_BCM2835_V3D_H

#include "hw/core/sysbus.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_BCM2835_V3D "bcm2835_v3d"
OBJECT_DECLARE_SIMPLE_TYPE(BCM2835V3DState, BCM2835_V3D)

#define BCM2835_V3D_REG_SIZE 0x1000
#define BCM2835_V3D_INTCTL 0x030
#define BCM2835_V3D_INTENA 0x034
#define BCM2835_V3D_INTDIS 0x038
#define BCM2835_V3D_MAX_TILES 4096

typedef struct BCM2835V3DDeferredMMIO {
    struct BCM2835V3DDeferredMMIO *next;
    hwaddr offset;
    uint64_t value;
    unsigned size;
} BCM2835V3DDeferredMMIO;

struct BCM2835V3DState {
    SysBusDevice parent_obj;

    QEMUBH *ct1_bh;
    QEMUTimer *ct1_timer;
    int64_t ct1_start_us;
    void *wgpu_context;
    void *wgpu_batch;
    bool wgpu_verify;
    MemoryRegion iomem;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    qemu_irq irq;
    uint32_t regs[BCM2835_V3D_REG_SIZE / sizeof(uint32_t)];
    uint32_t irq_status;
    uint32_t irq_enable;
    uint32_t fragment_dump_count;
    uint32_t fragment_workers_requested;
    uint32_t fragment_workers;
    QemuThread *fragment_worker_threads;
    bool profile_enabled;
    struct {
        uint64_t bql_parse_us;
        uint64_t bql_unlocked_us;
        uint64_t worker_wait_us;
        uint64_t bql_worker_wait_us;
        uint64_t publish_us;
        uint64_t idle_us;
        uint64_t worker_busy_us;
        uint64_t worker_window_us;
        uint64_t last_end_us;
        unsigned tiles;
        unsigned lists;
    } profile;
    QemuMutex fragment_worker_mutex;
    QemuCond fragment_worker_cond;
    QemuCond fragment_worker_done;
    uint64_t fragment_worker_generation;
    unsigned fragment_worker_completed;
    void *fragment_worker_job;
    bool fragment_worker_quit;
    bool fragment_unlock_requested;
    bool ct1_top_level;
    bool mmio_deferred;
    bool debug_defer_probe_requested;
    bool debug_defer_probe_pending;
    bool debug_worker_fallback_requested;
    uint32_t mmio_deferred_count;
    uint32_t debug_worker_fallback_count;
    BCM2835V3DDeferredMMIO *deferred_mmio_head;
    BCM2835V3DDeferredMMIO **deferred_mmio_tail;

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
    uint32_t clear_z;
    uint8_t clear_stencil;
    uint32_t tile_color[64 * 64];
    uint32_t tile_zs[64 * 64];
    bool render_config_valid;
    bool clear_color_valid;
    bool tile_color_valid;
    bool tile_zs_valid;

    uint32_t bin_alloc_base;
    uint32_t bin_alloc_size;
    uint32_t bin_state_base;
    uint8_t bin_tiles_x;
    uint8_t bin_tiles_y;
    uint8_t bin_flags;
    uint64_t bin_alloc_next;
    uint32_t bin_overflow_next;
    uint32_t bin_overflow_remaining;
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
    bool pending_tile_load_zs;
};

#endif /* HW_MISC_BCM2835_V3D_H */
