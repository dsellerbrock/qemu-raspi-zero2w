/*
 * Raspberry Pi VideoCore IV V3D register block.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/boards.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/bcm2835_v3d.h"
#include "bcm2835_v3d_qpu.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "qemu/timer.h"
#include "system/dma.h"
#include "trace.h"
#ifdef CONFIG_V3D_WGPU
#include "bcm2835_v3d_wgpu.h"
#endif
#include <math.h>
#include <unistd.h>

#define V3D_IDENT0             0x000
#define V3D_IDENT1             0x004
#define V3D_IDENT2             0x008
#define V3D_CT0CS              0x100
#define V3D_CT1CS              0x104
#define V3D_CT0EA              0x108
#define V3D_CT1EA              0x10c
#define V3D_CT0CA              0x110
#define V3D_CT1CA              0x114
#define V3D_CT00RA0            0x118
#define V3D_CT01RA0            0x11c
#define V3D_CT0LC              0x120
#define V3D_CT1LC              0x124
#define V3D_CT0PC              0x128
#define V3D_CT1PC              0x12c
#define V3D_PCS                0x130
#define V3D_BFC                0x134
#define V3D_RFC                0x138
#define V3D_BPCA               0x300
#define V3D_BPCS               0x304
#define V3D_BPOA               0x308
#define V3D_BPOS               0x30c
#define V3D_PCS_BMOOM          (1U << 8)

#define V3D_CS_CTRSTA          (1U << 15)
#define V3D_CS_CTRUN           (1U << 5)
#define V3D_CS_CTSUBS          (1U << 4)
#define V3D_CS_CTERR           (1U << 3)
#define V3D_CS_CTMODE          (1U << 0)
#define V3D_CS_SEMA_SHIFT      12
#define V3D_CS_SEMA_MASK       (7U << V3D_CS_SEMA_SHIFT)

#define V3D_INT_SPILLUSE       (1U << 3)
#define V3D_INT_OUTOMEM        (1U << 2)
#define V3D_INT_FLDONE         (1U << 1)
#define V3D_INT_FRDONE         (1U << 0)
#define V3D_INT_MASK           0x0f

#define V3D_BIN_FLAG_AUTO_INIT_TSDA (1U << 2)
#define V3D_BIN_FLAG_UNSUPPORTED   ((1U << 7) | (1U << 1) | (1U << 0))
#define V3D_BIN_ALLOC_ALIGN        256

#define V3D_PACKET_HALT        0
#define V3D_PACKET_NOP         1
#define V3D_PACKET_FLUSH       4
#define V3D_PACKET_FLUSH_ALL   5
#define V3D_PACKET_START_BIN   6
#define V3D_PACKET_INC_SEMA    7
#define V3D_PACKET_WAIT_SEMA   8
#define V3D_PACKET_BRANCH      16
#define V3D_PACKET_BRANCH_SUB  17
#define V3D_PACKET_RETURN      18
#define V3D_PACKET_STORE_MS    24
#define V3D_PACKET_STORE_MS_EOF 25
#define V3D_PACKET_STORE_TILE  28
#define V3D_PACKET_LOAD_TILE   29
#define V3D_PACKET_ARRAY_PRIM  33
#define V3D_PACKET_INDEX_PRIM  32
#define V3D_PACKET_GL_SHADER_STATE 64
#define V3D_PACKET_PRIMITIVE_FORMAT 56
#define V3D_PRIMITIVE_LIST_16_INDEX_TRIANGLES 0x12
#define V3D_PRIMITIVE_LIST_32_XY_TRIANGLES 0x32
#define V3D_PRIMITIVE_TRIANGLES 4
#define V3D_PRIMITIVE_TRIANGLE_FAN 6
#define V3D_PACKET_COMPRESSED  48
#define V3D_PACKET_CLIPPED     49
#define V3D_PACKET_BIN_CONFIG  112
#define V3D_PACKET_RENDER_CONFIG 113
#define V3D_PACKET_CLEAR_COLORS 114
#define V3D_PACKET_TILE_COORDS 115
#define V3D_PACKET_Z_CLIP     104
#define V3D_PACKET_CLIPPER_XY_SCALE 105
#define V3D_PACKET_VIEWPORT_OFFSET 103

#define V3D_SHADER_STATE_CLIP  (1U << 2)

#define V3D_MAX_CL_COMMANDS    (1U << 16)
#define V3D_MAX_PRIMITIVE_INDICES (1U << 20)
#define V3D_MAX_SHADER_VERTICES 4096
#define V3D_MAX_QPU_INSTRUCTIONS 4096
#define V3D_QPU_INSTRUCTION_BUDGET (1U << 16)
#define V3D_MAX_RASTER_WORK       (1U << 22)
#define V3D_MAX_RASTER_TILES      BCM2835_V3D_MAX_TILES
#define V3D_MAX_CLIPPED_VERTICES  12
/* shortcut: max 16 varyings; raise when VPM and raster work are bounded. */
#define V3D_MAX_FRAGMENT_VARYINGS 16
#define V3D_TILE_SIZE          64
#define V3D_T_MEMORY_TILE_SIZE 32
#define V3D_UTILE_PIXELS       16

typedef struct BCM2835V3DVertex {
    uint32_t word[6 + V3D_MAX_FRAGMENT_VARYINGS];
} BCM2835V3DVertex;

typedef struct BCM2835V3DFragmentVarying {
    uint32_t partial[V3D_MAX_FRAGMENT_VARYINGS]
                    [BCM2835_V3D_QPU_LANES];
    unsigned count;
} BCM2835V3DFragmentVarying;

typedef struct BCM2835V3DFragmentWorkerSlot {
    BCM2835V3DQPUResult result;
    uint32_t varying_index;
    uint16_t color_write_mask;
    uint32_t colors[BCM2835_V3D_QPU_LANES];
    bool fallback;
} BCM2835V3DFragmentWorkerSlot;

typedef struct BCM2835V3DFragmentWorkerJob {
    BCM2835V3DState *s;
    const uint8_t *code;
    uint32_t code_size;
    const uint16_t *pixels;
    const uint32_t *varying_partial;
    const uint32_t *fragment_w;
    unsigned count;
    unsigned batches;
    unsigned varying_count;
    uint32_t uniforms_address;
    BCM2835V3DFragmentWorkerSlot *slots;
    int next_batch;
} BCM2835V3DFragmentWorkerJob;

static GPrivate bcm2835_v3d_fragment_worker_slot = G_PRIVATE_INIT(NULL);

static void bcm2835_v3d_fragment_worker_fallback(void)
{
    BCM2835V3DFragmentWorkerSlot *slot =
        g_private_get(&bcm2835_v3d_fragment_worker_slot);

    if (slot) {
        slot->fallback = true;
    }
}

/* VC4 packet 49 and compressed-list relative branches are not decoded yet. */
static unsigned bcm2835_v3d_packet_size(uint8_t opcode)
{
    switch (opcode) {
    case V3D_PACKET_HALT:
    case V3D_PACKET_NOP:
    case V3D_PACKET_FLUSH:
    case V3D_PACKET_FLUSH_ALL:
    case V3D_PACKET_START_BIN:
    case V3D_PACKET_INC_SEMA:
    case V3D_PACKET_WAIT_SEMA:
    case V3D_PACKET_RETURN:
    case V3D_PACKET_STORE_MS:
    case V3D_PACKET_STORE_MS_EOF:
        return 1;
    case V3D_PACKET_BRANCH:
    case V3D_PACKET_BRANCH_SUB:
    case 26: /* Store Full Resolution Tile Buffer */
    case 27: /* Load Full Resolution Tile Buffer */
    case V3D_PACKET_GL_SHADER_STATE:
    case 65: /* NV Shader State */
    case 66: /* VG Shader State */
    case 97: /* Flat Shade Flags */
    case 98: /* Point Size */
    case 99: /* Line Width */
    case 101: /* Depth Offset */
    case 103: /* Viewport Offset */
        return 5;
    case V3D_PACKET_STORE_TILE:
    case 29: /* Load Tile Buffer General */
        return 7;
    case V3D_PACKET_ARRAY_PRIM:
        return 10;
    case V3D_PACKET_INDEX_PRIM:
        return 14;
    case V3D_PACKET_PRIMITIVE_FORMAT:
        return 2;
    case 96: /* Configuration Bits */
        return 4;
    case 100: /* RHT X Boundary */
        return 3;
    case 102: /* Clip Window */
    case 104: /* Z Clipping */
    case 105: /* Clipper XY Scaling */
    case 106: /* Clipper Z Scaling */
        return 9;
    case V3D_PACKET_BIN_CONFIG:
        return 16;
    case V3D_PACKET_RENDER_CONFIG:
        return 11;
    case V3D_PACKET_CLEAR_COLORS:
        return 14;
    case V3D_PACKET_TILE_COORDS:
        return 3;
    case V3D_PACKET_COMPRESSED:
    case V3D_PACKET_CLIPPED:
    default:
        return 0;
    }
}

static void bcm2835_v3d_update_irq(BCM2835V3DState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->irq_enable) != 0);
}

static void bcm2835_v3d_raise_irq(BCM2835V3DState *s, uint32_t bits)
{
    s->irq_status |= bits & V3D_INT_MASK;
    bcm2835_v3d_update_irq(s);
}

static void bcm2835_v3d_update_semaphore(BCM2835V3DState *s, unsigned thread)
{
    s->ct_cs[thread] &= ~V3D_CS_SEMA_MASK;
    s->ct_cs[thread] |= (s->ct_semaphore[thread] << V3D_CS_SEMA_SHIFT) &
                        V3D_CS_SEMA_MASK;
}

static bool bcm2835_v3d_wgpu_batch_flush_pending(BCM2835V3DState *s);
static void bcm2835_v3d_wgpu_batch_free(BCM2835V3DState *s);

static bool bcm2835_v3d_dma_read(BCM2835V3DState *s, uint32_t addr,
                                 void *buf, size_t len)
{
    if (g_private_get(&bcm2835_v3d_fragment_worker_slot)) {
        hwaddr xlat, plen = len;
        MemoryRegion *mr;

        if ((uint64_t)addr + len > UINT32_MAX + 1ULL) {
            bcm2835_v3d_fragment_worker_fallback();
            return false;
        }
        RCU_READ_LOCK_GUARD();
        mr = address_space_translate(&s->dma_as, addr, &xlat, &plen, false,
                                     MEMTXATTRS_UNSPECIFIED);
        if (!mr || plen < len || !memory_region_is_ram(mr) ||
            dma_memory_read(&s->dma_as, addr, buf, len,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            bcm2835_v3d_fragment_worker_fallback();
            return false;
        }
        return true;
    }
    MemTxResult result = dma_memory_read(&s->dma_as, addr, buf, len,
                                          MEMTXATTRS_UNSPECIFIED);

    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: DMA read at 0x%08x len %zu failed: 0x%x\n",
                      addr, len, result);
        return false;
    }
    return true;
}

static uint16_t bcm2835_v3d_pack_bgr565(uint32_t rgba)
{
    uint8_t red = rgba;
    uint8_t green = rgba >> 8;
    uint8_t blue = rgba >> 16;

    return ((uint16_t)(blue >> 3) << 11) |
           ((uint16_t)(green >> 2) << 5) | (red >> 3);
}

static uint32_t bcm2835_v3d_load_pixel(const uint8_t *src,
                                        unsigned color_format)
{
    uint16_t pixel;
    uint8_t red;
    uint8_t green;
    uint8_t blue;

    if (color_format == 0) {
        return ldl_le_p(src);
    }

    pixel = lduw_le_p(src);
    red = pixel & 0x1f;
    green = (pixel >> 5) & 0x3f;
    blue = (pixel >> 11) & 0x1f;

    red = (red << 3) | (red >> 2);
    green = (green << 2) | (green >> 4);
    blue = (blue << 3) | (blue >> 2);
    return 0xff000000 | ((uint32_t)blue << 16) |
           ((uint32_t)green << 8) | red;
}

static void bcm2835_v3d_store_pixel(uint8_t *dst, unsigned color_format,
                                    uint32_t color)
{
    if (color_format == 0) {
        stl_le_p(dst, color);
    } else {
        stw_le_p(dst, bcm2835_v3d_pack_bgr565(color));
    }
}

static uint64_t bcm2835_v3d_memory_pixel_addr(uint32_t base,
                                              unsigned memory_format,
                                              unsigned bytes_per_pixel,
                                              unsigned image_width,
                                              unsigned global_x,
                                              unsigned global_y)
{
    if (memory_format == 0) {
        return (uint64_t)base +
               ((uint64_t)global_y * image_width + global_x) *
               bytes_per_pixel;
    }

    if (memory_format == 2) {
        uint32_t stride = DIV_ROUND_UP(image_width, 4) * 4 *
                          bytes_per_pixel;

        return (uint64_t)base + (global_y & ~3U) * (uint64_t)stride +
               (global_x & ~3U) * bytes_per_pixel * 4 +
               ((global_y & 3) * 4 + (global_x & 3)) * bytes_per_pixel;
    }

    {
        static const uint8_t even_stile_map[4] = { 0, 3, 1, 2 };
        static const uint8_t odd_stile_map[4] = { 2, 1, 3, 0 };
        uint32_t tile_stride = DIV_ROUND_UP(image_width,
                                            V3D_T_MEMORY_TILE_SIZE);
        uint32_t memory_tile_x = global_x / V3D_T_MEMORY_TILE_SIZE;
        uint32_t memory_tile_y = global_y / V3D_T_MEMORY_TILE_SIZE;
        bool odd_tile_y = memory_tile_y & 1;
        uint32_t tile_x = odd_tile_y ? tile_stride - memory_tile_x - 1 :
                                       memory_tile_x;
        uint32_t tile_bytes = V3D_T_MEMORY_TILE_SIZE *
                              V3D_T_MEMORY_TILE_SIZE *
                              bytes_per_pixel;
        uint32_t utile_bytes = V3D_UTILE_PIXELS * bytes_per_pixel;
        uint32_t stile_bytes = 16 * 16 * bytes_per_pixel;
        uint32_t local_x = global_x % V3D_T_MEMORY_TILE_SIZE;
        uint32_t local_y = global_y % V3D_T_MEMORY_TILE_SIZE;
        uint32_t utile_x = global_x / 4;
        uint32_t utile_y = global_y / 4;
        uint32_t stile_index = (((local_y / 16) & 1) << 1) +
                               ((local_x / 16) & 1);
        uint32_t utile_in_stile = (utile_y & 3) * 4 + (utile_x & 3);
        uint32_t pixel_in_utile = (global_y & 3) * 4 + (global_x & 3);
        const uint8_t *stile_map = odd_tile_y ? odd_stile_map :
                                                 even_stile_map;
        uint64_t tile_base = (uint64_t)base +
                             ((uint64_t)memory_tile_y * tile_stride +
                              tile_x) *
                             tile_bytes;

        return tile_base + stile_map[stile_index] * stile_bytes +
               utile_in_stile * utile_bytes +
               pixel_in_utile * bytes_per_pixel;
    }
}

static uint64_t bcm2835_v3d_tile_pixel_addr(BCM2835V3DState *s,
                                            uint32_t base,
                                            unsigned memory_format,
                                            unsigned bytes_per_pixel,
                                            unsigned x, unsigned y)
{
    unsigned global_x = (uint32_t)s->tile_x * V3D_TILE_SIZE + x;
    unsigned global_y = (uint32_t)s->tile_y * V3D_TILE_SIZE + y;

    return bcm2835_v3d_memory_pixel_addr(base, memory_format,
                                          bytes_per_pixel, s->render_width,
                                          global_x, global_y);
}

static uint64_t bcm2835_v3d_tile_utile_addr(BCM2835V3DState *s,
                                             uint32_t base,
                                             unsigned memory_format,
                                             unsigned bytes_per_pixel,
                                             unsigned utile_x,
                                             unsigned utile_y)
{
    uint32_t global_x = (uint32_t)s->tile_x * V3D_TILE_SIZE + utile_x * 4;
    uint32_t global_y = (uint32_t)s->tile_y * V3D_TILE_SIZE + utile_y * 4;

    if (memory_format == 2) {
        uint32_t stride = DIV_ROUND_UP(s->render_width, 4) * 4 *
                          bytes_per_pixel;

        return (uint64_t)base + global_y * (uint64_t)stride +
               global_x * bytes_per_pixel * 4;
    }

    return bcm2835_v3d_tile_pixel_addr(s, base, memory_format,
                                        bytes_per_pixel,
                                        utile_x * 4, utile_y * 4);
}

static bool bcm2835_v3d_copy_tile(BCM2835V3DState *s, uint32_t base,
                                  unsigned memory_format,
                                  unsigned color_format, bool to_guest)
{
    uint8_t buffer[V3D_TILE_SIZE * sizeof(uint32_t)];
    unsigned bytes_per_pixel = color_format == 0 ? sizeof(uint32_t) :
                                                   sizeof(uint16_t);
    uint32_t x0 = (uint32_t)s->tile_x * V3D_TILE_SIZE;
    uint32_t y0 = (uint32_t)s->tile_y * V3D_TILE_SIZE;
    uint32_t width = MIN(V3D_TILE_SIZE, s->render_width -
                         MIN(x0, s->render_width));
    uint32_t height = MIN(V3D_TILE_SIZE, s->render_height -
                          MIN(y0, s->render_height));

    if (memory_format > 2 || color_format > 2 ||
        (to_guest && color_format == 1)) {
        return false;
    }
    if (!width || !height) {
        return true;
    }

    if (memory_format == 0) {
        for (unsigned y = 0; y < height; y++) {
            uint64_t addr = (uint64_t)base +
                            ((uint64_t)(y0 + y) * s->render_width + x0) *
                            bytes_per_pixel;
            size_t row_size = width * bytes_per_pixel;

            if (addr + row_size > UINT32_MAX + 1ULL) {
                return false;
            }
            if (!to_guest && !bcm2835_v3d_dma_read(s, addr, buffer,
                                                    row_size)) {
                return false;
            }
            for (unsigned x = 0; x < width; x++) {
                unsigned tile_index = y * V3D_TILE_SIZE + x;
                uint8_t *pixel = buffer + x * bytes_per_pixel;

                if (to_guest) {
                    bcm2835_v3d_store_pixel(pixel, color_format,
                                            s->tile_color[tile_index]);
                } else {
                    s->tile_color[tile_index] =
                        bcm2835_v3d_load_pixel(pixel, color_format);
                }
            }
            if (to_guest &&
                dma_memory_write(&s->dma_as, addr, buffer, row_size,
                                 MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                return false;
            }
        }
        return true;
    }

    for (unsigned utile_y = 0; utile_y < V3D_TILE_SIZE / 4; utile_y++) {
        uint32_t global_y = y0 + utile_y * 4;

        if (global_y >= s->render_height) {
            break;
        }
        for (unsigned utile_x = 0; utile_x < V3D_TILE_SIZE / 4; utile_x++) {
            uint32_t global_x = x0 + utile_x * 4;
            uint32_t valid_width;
            uint32_t valid_height;
            size_t utile_size = V3D_UTILE_PIXELS * bytes_per_pixel;
            uint64_t addr;

            if (global_x >= s->render_width) {
                break;
            }
            valid_width = MIN(4, s->render_width - global_x);
            valid_height = MIN(4, s->render_height - global_y);
            addr = bcm2835_v3d_tile_utile_addr(s, base, memory_format,
                                                bytes_per_pixel,
                                                utile_x, utile_y);
            if (addr + utile_size > UINT32_MAX + 1ULL) {
                return false;
            }
            if (!to_guest || valid_width < 4 || valid_height < 4) {
                if (!bcm2835_v3d_dma_read(s, addr, buffer, utile_size)) {
                    return false;
                }
            }
            for (unsigned y = 0; y < valid_height; y++) {
                for (unsigned x = 0; x < valid_width; x++) {
                    unsigned tile_index = (utile_y * 4 + y) *
                                          V3D_TILE_SIZE + utile_x * 4 + x;
                    uint8_t *pixel = buffer +
                                     (y * 4 + x) * bytes_per_pixel;

                    if (to_guest) {
                        bcm2835_v3d_store_pixel(pixel, color_format,
                                                s->tile_color[tile_index]);
                    } else {
                        s->tile_color[tile_index] =
                            bcm2835_v3d_load_pixel(pixel, color_format);
                    }
                }
            }
            if (to_guest &&
                dma_memory_write(&s->dma_as, addr, buffer, utile_size,
                                 MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                return false;
            }
        }
    }
    return true;
}

static bool bcm2835_v3d_tile_buffer_packet(BCM2835V3DState *s,
                                            const uint8_t *packet,
                                            bool store)
{
    uint64_t params = (uint32_t)ldl_le_p(packet + 1) |
                      ((uint64_t)(uint16_t)lduw_le_p(packet + 5) << 32);
    uint32_t base = ((params >> 20) & 0x0fffffff) << 4;
    unsigned buffer = params & 7;
    unsigned memory_format = (params >> 4) & 3;
    unsigned mode = (params >> 6) & 3;
    unsigned color_format = (params >> 8) & 3;

    if (buffer == 0 || (buffer == 1 && (params & (1ULL << 16)))) {
        return true;
    }
    /* shortcut: color only/single sample; add depth, MSAA, and dither later. */
    if (buffer != 1 || mode != 0 || memory_format > 2 ||
        color_format > 2 || (store && color_format == 1) ||
        !s->render_config_valid || !s->render_width || !s->render_height) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: tile buffer %s unsupported: buffer=%u mode=%u memfmt=%u colorfmt=%u base=0x%08x params=0x%012" PRIx64 " render_valid=%d %ux%u config=0x%04x\n",
                      store ? "store" : "load", buffer, mode,
                      memory_format, color_format, base, params,
                      s->render_config_valid, s->render_width,
                      s->render_height, s->render_config);
        return false;
    }

    if (store) {
        if (!s->tile_color_valid) {
            if (!s->clear_color_valid) {
                return false;
            }
            for (unsigned i = 0; i < G_N_ELEMENTS(s->tile_color); i++) {
                s->tile_color[i] = s->clear_color[0];
            }
            s->tile_color_valid = true;
        }
        if (!bcm2835_v3d_copy_tile(s, base, memory_format,
                                    color_format, true)) {
            return false;
        }
        if (!(params & (1ULL << 13))) {
            s->tile_color_valid = false;
        }
        if (params & (1ULL << 19)) {
            s->regs[V3D_RFC >> 2]++;
            bcm2835_v3d_raise_irq(s, V3D_INT_FRDONE);
        }
    } else {
        s->pending_tile_load_base = base;
        s->pending_tile_load_memory_format = memory_format;
        s->pending_tile_load_color_format = color_format;
        s->pending_tile_load = true;
    }
    return true;
}

static void bcm2835_v3d_thread_error(BCM2835V3DState *s, unsigned thread,
                                     uint32_t addr, const char *reason)
{
#ifdef CONFIG_V3D_WGPU
    if (thread == 1 && s->wgpu_batch) {
        /* Finish queued pixels before reporting an unrelated list error. */
        if (!bcm2835_v3d_wgpu_batch_flush_pending(s)) {
            bcm2835_v3d_wgpu_batch_free(s);
        }
    }
#endif
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bcm2835_v3d: CT%u stopped at 0x%08x: %s\n",
                  thread, addr, reason);
    s->ct_cs[thread] &= ~V3D_CS_CTRUN;
    s->ct_cs[thread] |= V3D_CS_CTSUBS | V3D_CS_CTERR;
    s->ct_waiting[thread] = false;
    s->ct_return_depth[thread] = 0;
    if (thread == 0) {
        s->bin_draw_append_count = 0;
        s->bin_draw_resume = false;
        s->bin_oom_waiting = false;
    }
}

static bool bcm2835_v3d_clear_tile(BCM2835V3DState *s)
{
    uint32_t x0 = s->tile_x * V3D_TILE_SIZE;
    uint32_t y0 = s->tile_y * V3D_TILE_SIZE;
    uint32_t width = MIN(V3D_TILE_SIZE, s->render_width -
                         MIN(x0, s->render_width));
    uint32_t height = MIN(V3D_TILE_SIZE, s->render_height -
                          MIN(y0, s->render_height));
    uint32_t color = s->clear_color[0];
    unsigned format = (s->render_config >> 2) & 3;
    unsigned memory_format = (s->render_config >> 6) & 3;

    if (!s->render_config_valid ||
        (!s->clear_color_valid && !s->tile_color_valid) || !width || !height) {
        return true;
    }

    if ((format != 1 && format != 2) || memory_format > 2) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: clear skipped for render format %u/%u\n",
                      memory_format, format);
        return false;
    }

    if (s->tile_color_valid) {
        return bcm2835_v3d_copy_tile(s, s->render_base, memory_format,
                                      format == 1 ? 0 : 2, true);
    }

    for (unsigned i = 0; i < G_N_ELEMENTS(s->tile_color); i++) {
        s->tile_color[i] = color;
    }
    s->tile_color_valid = true;
    return bcm2835_v3d_copy_tile(s, s->render_base, memory_format,
                                  format == 1 ? 0 : 2, true);
}

static void bcm2835_v3d_run_thread(BCM2835V3DState *s, unsigned thread);
static void bcm2835_v3d_start_thread(BCM2835V3DState *s, unsigned thread);

static bool bcm2835_v3d_validate_vertex_arrays(BCM2835V3DState *s,
                                               uint32_t max_index)
{
    unsigned attributes = s->shader_attribute_count;
    uint8_t selected = s->shader_record[14] | s->shader_record[26];

    if (attributes < 8 && (selected >> attributes)) {
        return false;
    }

    for (unsigned i = 0; i < attributes; i++) {
        uint32_t offset = 36 + i * 8;
        uint32_t base;
        uint32_t stride;
        uint32_t attr_size;
        uint64_t last_offset;
        uint64_t end;
        uint8_t data[256];

        if (!(selected & (1U << i))) {
            continue;
        }

        base = ldl_le_p(s->shader_record + offset);
        attr_size = s->shader_record[offset + 4] + 1;
        stride = s->shader_record[offset + 5];
        if (s->shader_record_extended) {
            stride |= ldl_le_p(s->shader_record + 100 + i * 4) & ~0xffU;
        }

        last_offset = stride ? (uint64_t)max_index * stride : 0;
        end = (uint64_t)base + last_offset + attr_size;
        if (end > UINT32_MAX + 1ULL ||
            !bcm2835_v3d_dma_read(s, base, data, attr_size) ||
            (last_offset &&
             !bcm2835_v3d_dma_read(s, base + last_offset, data,
                                    attr_size))) {
            return false;
        }
    }

    return true;
}

static bool bcm2835_v3d_validate_draw(BCM2835V3DState *s,
                                      const uint8_t *packet,
                                      uint8_t opcode)
{
    unsigned mode = packet[1] & 0xf;
    uint32_t count = ldl_le_p(packet + 2);
    uint32_t max_index;

    if (mode > 6) {
        return false;
    }
    if (!count) {
        return true;
    }
    if (!s->shader_state_valid) {
        return false;
    }

    if (opcode == V3D_PACKET_ARRAY_PRIM) {
        uint32_t base_index = ldl_le_p(packet + 6);

        if ((uint64_t)base_index + count - 1 > UINT32_MAX) {
            return false;
        }
        max_index = base_index + count - 1;
    } else {
        unsigned index_size = (packet[1] >> 4) + 1;
        uint32_t index_addr = ldl_le_p(packet + 6);
        uint32_t declared_max = ldl_le_p(packet + 10);
        uint64_t index_end = (uint64_t)index_addr +
                             (uint64_t)count * index_size;
        uint8_t indices[512];
        uint32_t index = 0;

        /* Keep malformed guest lists from monopolizing the QEMU main loop. */
        if (index_size > 2 || count > V3D_MAX_PRIMITIVE_INDICES ||
            index_end > UINT32_MAX + 1ULL) {
            return false;
        }

        while (index < count) {
            uint32_t batch = MIN(count - index,
                                 sizeof(indices) / index_size);
            size_t bytes = batch * index_size;

            if (!bcm2835_v3d_dma_read(s, index_addr + index * index_size,
                                      indices, bytes)) {
                return false;
            }
            for (uint32_t i = 0; i < batch; i++) {
                uint32_t value = index_size == 1 ? indices[i] :
                                 lduw_le_p(indices + i * index_size);

                if (value > declared_max) {
                    return false;
                }
            }
            index += batch;
        }
        max_index = declared_max;
    }

    return bcm2835_v3d_validate_vertex_arrays(s, max_index);
}

static bool bcm2835_v3d_qpu_read_uniform(void *opaque, uint32_t address,
                                          uint32_t *value)
{
    BCM2835V3DState *s = opaque;
    uint8_t bytes[sizeof(uint32_t)];

    if ((uint64_t)address + sizeof(bytes) > UINT32_MAX + 1ULL ||
        !bcm2835_v3d_dma_read(s, address, bytes, sizeof(bytes))) {
        return false;
    }
    *value = ldl_le_p(bytes);
    return true;
}

static bool bcm2835_v3d_qpu_write_memory(void *opaque, uint32_t address,
                                          uint32_t value)
{
    BCM2835V3DState *s = opaque;
    uint8_t bytes[sizeof(uint32_t)];

    if (g_private_get(&bcm2835_v3d_fragment_worker_slot)) {
        bcm2835_v3d_fragment_worker_fallback();
        return false;
    }

    if ((uint64_t)address + sizeof(bytes) > UINT32_MAX + 1ULL) {
        return false;
    }
    stl_le_p(bytes, value);
    return dma_memory_write(&s->dma_as, address, bytes, sizeof(bytes),
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}

static bool bcm2835_v3d_qpu_read_texture(void *opaque, uint32_t base,
                                          unsigned memory_format,
                                          unsigned width, unsigned height,
                                          unsigned x, unsigned y,
                                          uint32_t *value)
{
    BCM2835V3DState *s = opaque;
    uint8_t pixel[sizeof(uint32_t)];
    uint64_t address;

    if (memory_format > 2 || x >= width || y >= height) {
        return false;
    }
    address = bcm2835_v3d_memory_pixel_addr(base, memory_format,
                                             sizeof(uint32_t), width, x, y);
    if (address + sizeof(pixel) > UINT32_MAX + 1ULL ||
        !bcm2835_v3d_dma_read(s, address, pixel, sizeof(pixel))) {
        return false;
    }
    *value = ldl_le_p(pixel);
    return true;
}

static bool bcm2835_v3d_qpu_read_varying(
    void *opaque, unsigned index,
    uint32_t partial[BCM2835_V3D_QPU_LANES],
    uint32_t c[BCM2835_V3D_QPU_LANES])
{
    BCM2835V3DFragmentVarying *varying = opaque;

    if (index >= varying->count) {
        return false;
    }
    memcpy(partial, varying->partial[index], sizeof(varying->partial[index]));
    memset(c, 0, sizeof(varying->partial[index]));
    return true;
}

static bool bcm2835_v3d_load_qpu_program(BCM2835V3DState *s,
                                         uint32_t address,
                                         uint8_t **code, uint32_t *code_size)
{
    uint8_t *program = g_malloc(V3D_MAX_QPU_INSTRUCTIONS * sizeof(uint64_t));

    address &= ~7U;
    for (uint32_t i = 0; i < V3D_MAX_QPU_INSTRUCTIONS; i++) {
        uint8_t *instruction = program + i * sizeof(uint64_t);
        uint64_t guest_address = (uint64_t)address +
                                (uint64_t)i * sizeof(uint64_t);

        if (guest_address + sizeof(uint64_t) > UINT32_MAX + 1ULL ||
            !bcm2835_v3d_dma_read(s, guest_address, instruction,
                                   sizeof(uint64_t))) {
            g_free(program);
            return false;
        }
        if ((ldq_le_p(instruction) >> 60) == 3) {
            *code = program;
            *code_size = (i + 1) * sizeof(uint64_t);
            return true;
        }
    }

    g_free(program);
    return false;
}

static bool bcm2835_v3d_load_qpu_attributes(BCM2835V3DState *s,
                                            BCM2835V3DQPUState *qpu,
                                            const uint32_t *indices,
                                            unsigned count, unsigned first,
                                            uint8_t selected,
                                            unsigned vpm_offset_byte)
{
    for (unsigned attr = 0; attr < s->shader_attribute_count; attr++) {
        uint32_t record_offset = 36 + attr * 8;
        uint32_t base;
        uint32_t stride;
        unsigned attr_size;
        unsigned vpm_offset;

        if (!(selected & (1U << attr))) {
            continue;
        }
        base = ldl_le_p(s->shader_record + record_offset);
        attr_size = s->shader_record[record_offset + 4] + 1;
        stride = s->shader_record[record_offset + 5];
        if (s->shader_record_extended) {
            stride |= ldl_le_p(s->shader_record + 100 + attr * 4) & ~0xffU;
        }
        vpm_offset = s->shader_record[record_offset + vpm_offset_byte];
        if ((attr_size & 3) || attr_size > 64 || (vpm_offset & 3) ||
            vpm_offset + attr_size > BCM2835_V3D_QPU_VPM_ROWS * 4) {
            return false;
        }

        for (unsigned component = 0; component < attr_size / 4; component++) {
            unsigned row = vpm_offset / 4 + component;

            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint8_t value[sizeof(uint32_t)] = { 0 };
                uint64_t address;

                if (first + lane >= count) {
                    qpu->vpm[row][lane] = 0;
                    continue;
                }
                address = (uint64_t)base +
                          (uint64_t)indices[first + lane] * stride +
                          component * sizeof(uint32_t);
                if (address + sizeof(value) > UINT32_MAX + 1ULL ||
                    !bcm2835_v3d_dma_read(s, address, value, sizeof(value))) {
                    return false;
                }
                qpu->vpm[row][lane] = ldl_le_p(value);
            }
        }
    }
    return true;
}

static bool bcm2835_v3d_run_qpu_stage(BCM2835V3DState *s,
                                       const uint32_t *indices,
                                       unsigned count, unsigned code_offset,
                                       unsigned uniforms_offset,
                                       unsigned attributes_offset,
                                       unsigned vpm_offset_byte,
                                       const uint32_t (*input_vpm)[
                                           BCM2835_V3D_QPU_VPM_ROWS]
                                           [BCM2835_V3D_QPU_LANES],
                                       uint32_t (*output_vpm)[
                                           BCM2835_V3D_QPU_VPM_ROWS]
                                           [BCM2835_V3D_QPU_LANES],
                                       BCM2835V3DVertex *vertices)
{
    uint32_t code_address = ldl_le_p(s->shader_record + code_offset) & ~7U;
    uint32_t uniforms_address = ldl_le_p(s->shader_record + uniforms_offset);
    uint8_t *code;
    uint32_t code_size;

    if (!code_address) {
        return vertices == NULL;
    }
    if (count > V3D_MAX_SHADER_VERTICES) {
        return false;
    }
    if (!bcm2835_v3d_load_qpu_program(s, code_address, &code, &code_size)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: QPU program read failed at 0x%08x\n",
                      code_address);
        return false;
    }

    for (unsigned first = 0; first < count;
         first += BCM2835_V3D_QPU_LANES) {
        BCM2835V3DQPUState qpu;
        uint32_t executed;
        BCM2835V3DQPUResult result;

        bcm2835_v3d_qpu_init(&qpu);
        if (output_vpm || vertices) {
            qpu.separate_vpm_output = true;
            memset(qpu.vpm_out, 0, sizeof(qpu.vpm_out));
        }
        qpu.uniform_address = uniforms_address;
        qpu.read_uniform = bcm2835_v3d_qpu_read_uniform;
        qpu.uniform_opaque = s;
        qpu.read_memory = bcm2835_v3d_qpu_read_uniform;
        qpu.memory_opaque = s;
        qpu.write_memory = bcm2835_v3d_qpu_write_memory;
        qpu.read_texture = bcm2835_v3d_qpu_read_texture;
        qpu.texture_opaque = s;
        if (input_vpm) {
            memcpy(qpu.vpm, input_vpm[first / BCM2835_V3D_QPU_LANES],
                   sizeof(qpu.vpm));
        }
        if (!bcm2835_v3d_load_qpu_attributes(
                s, &qpu, indices, count, first,
                s->shader_record[attributes_offset], vpm_offset_byte)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: QPU attribute load failed for program 0x%08x\n",
                          code_address);
            g_free(code);
            return false;
        }
        result = bcm2835_v3d_qpu_run(code, code_size,
                                      V3D_QPU_INSTRUCTION_BUDGET, &qpu,
                                      &executed);
        if (result != BCM2835_V3D_QPU_END) {
            uint64_t instruction = 0;
            char trace[16 * 17 + 1] = { 0 };
            size_t trace_size = 0;
            unsigned trace_words = MIN(qpu.pc + 1, 16);

            if ((uint64_t)qpu.pc * sizeof(uint64_t) + sizeof(uint64_t) <=
                code_size) {
                instruction = ldq_le_p(code + qpu.pc * sizeof(uint64_t));
            }
            for (unsigned i = 0; i < trace_words; i++) {
                trace_size += g_snprintf(trace + trace_size,
                                         sizeof(trace) - trace_size,
                                         "%s%016" PRIx64,
                                         i ? " " : "",
                                         ldq_le_p(code + i * sizeof(uint64_t)));
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: QPU program 0x%08x returned %u at pc %u, instruction 0x%016" PRIx64 " acc2[0]=0x%08x code=%s\n",
                          code_address, result, qpu.pc, instruction,
                          qpu.acc[2][0], trace);
            g_free(code);
            return false;
        }
        if (output_vpm) {
            memcpy(output_vpm[first / BCM2835_V3D_QPU_LANES], qpu.vpm_out,
                   sizeof(qpu.vpm_out));
        }
        if (vertices) {
            if (qpu.vpm_write_total != 7 ||
                qpu.vpm_write_history_count != 7) {
                char instructions[16 * 17 + 1] = { 0 };
                char rows[4 * BCM2835_V3D_QPU_VPM_WRITE_HISTORY + 1] = { 0 };
                size_t instruction_size = 0;
                size_t row_size = 0;

                for (unsigned i = 0;
                     i < MIN(code_size / sizeof(uint64_t), 16); i++) {
                    instruction_size += g_snprintf(
                        instructions + instruction_size,
                        sizeof(instructions) - instruction_size,
                        "%s%016" PRIx64, i ? " " : "",
                        ldq_le_p(code + i * sizeof(uint64_t)));
                }
                for (unsigned i = 0; i < qpu.vpm_write_history_count; i++) {
                    row_size += g_snprintf(rows + row_size,
                                           sizeof(rows) - row_size,
                                           "%s%u", i ? "," : "",
                                           qpu.vpm_write_history[i]);
                }
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bcm2835_v3d: QPU coordinate output mismatch: shader=0x%08x wrote=%u history=%u expected=7 rows=[%s] code=%s\n",
                              code_address,
                              qpu.vpm_write_total,
                              qpu.vpm_write_history_count,
                              rows, instructions);
                g_free(code);
                return false;
            }
            for (unsigned word = 0; word < 4; word++) {
                uint8_t coordinate_row = qpu.vpm_write_history[word];

                for (unsigned lane = 0; lane <
                     BCM2835_V3D_QPU_LANES; lane++) {
                    if (first + lane < count) {
                        vertices[first + lane].word[word] =
                            qpu.vpm_out[coordinate_row][lane];
                    }
                }
            }
            /* Mesa emits packed viewport XY at row 4 and reciprocal W at 6. */
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                if (first + lane < count) {
                    vertices[first + lane].word[4] =
                        qpu.vpm_out[qpu.vpm_write_history[4]][lane];
                    vertices[first + lane].word[
                        5 + s->shader_record[3]] =
                        qpu.vpm_out[qpu.vpm_write_history[6]][lane];
                }
            }
            if (input_vpm) {
                const uint32_t (*batch_vpm)[BCM2835_V3D_QPU_LANES] =
                    input_vpm[first / BCM2835_V3D_QPU_LANES];

                for (unsigned varying = 0; varying <
                     s->shader_record[3]; varying++) {
                    for (unsigned lane = 0; lane <
                         BCM2835_V3D_QPU_LANES; lane++) {
                        if (first + lane < count) {
                            vertices[first + lane].word[5 + varying] =
                                batch_vpm[3 + varying][lane];
                        }
                    }
                }
            }
        }
    }
    g_free(code);
    return true;
}

static bool bcm2835_v3d_load_shader_state(BCM2835V3DState *s,
                                          uint32_t pointer)
{
    uint32_t address = pointer & ~0xfU;
    unsigned attributes = pointer & 7;
    bool extended = pointer & 8;
    uint16_t record_size;

    if (!attributes) {
        attributes = 8;
    }
    record_size = extended ? 100 + attributes * 4 : 36 + attributes * 8;
    if ((uint64_t)address + record_size > UINT32_MAX + 1ULL ||
        !bcm2835_v3d_dma_read(s, address, s->shader_record, record_size)) {
        return false;
    }
    if (record_size < sizeof(s->shader_record)) {
        memset(s->shader_record + record_size, 0,
               sizeof(s->shader_record) - record_size);
    }
    s->shader_state = pointer;
    s->shader_record_size = record_size;
    s->shader_attribute_count = attributes;
    s->shader_record_extended = extended;
    s->shader_state_valid = true;
    return true;
}

static int32_t bcm2835_v3d_floor_12_4(int32_t value)
{
    return value < 0 ? -((-value + 15) / 16) : value / 16;
}

static int64_t bcm2835_v3d_edge(int32_t ax, int32_t ay,
                                int32_t bx, int32_t by,
                                int32_t px, int32_t py)
{
    return (int64_t)(px - ax) * (by - ay) -
           (int64_t)(py - ay) * (bx - ax);
}

static int64_t bcm2835_v3d_ceil_div(int64_t numerator, int64_t denominator)
{
    return numerator / denominator + (numerator % denominator > 0);
}

static int64_t bcm2835_v3d_floor_div(int64_t numerator, int64_t denominator)
{
    return numerator / denominator - (numerator % denominator < 0);
}

static void bcm2835_v3d_init_fragment_qpu(
    BCM2835V3DState *s, BCM2835V3DQPUState *qpu,
    BCM2835V3DFragmentVarying *varying, const uint16_t *pixels,
    const uint32_t *varying_partial, const uint32_t *fragment_w,
    uint32_t uniform_address, unsigned varying_count,
    unsigned first, unsigned batch)
{
    bcm2835_v3d_qpu_init(qpu);
    qpu->uniform_address = uniform_address;
    qpu->read_uniform = bcm2835_v3d_qpu_read_uniform;
    qpu->uniform_opaque = s;
    qpu->read_memory = bcm2835_v3d_qpu_read_uniform;
    qpu->memory_opaque = s;
    qpu->write_memory = bcm2835_v3d_qpu_write_memory;
    qpu->read_texture = bcm2835_v3d_qpu_read_texture;
    qpu->texture_opaque = s;
    qpu->fragment_shader = true;
    if (varying_count) {
        varying->count = varying_count;
        qpu->read_varying = bcm2835_v3d_qpu_read_varying;
        qpu->varying_opaque = varying;
    }
    for (unsigned lane = 0; lane < batch; lane++) {
        unsigned pixel = pixels[first + lane];

        qpu->fragment_coord[0][lane] =
            (s->tile_x * V3D_TILE_SIZE + pixel % V3D_TILE_SIZE) & 0x7ff;
        qpu->fragment_coord[1][lane] =
            (s->tile_y * V3D_TILE_SIZE + pixel / V3D_TILE_SIZE) & 0x7ff;
        qpu->io[45][lane] = s->tile_color[pixel];
        qpu->fragment_w[lane] = fragment_w[first + lane];
        for (unsigned index = 0; index < varying->count; index++) {
            varying->partial[index][lane] =
                varying_partial[(first + lane) * varying->count + index];
        }
    }
}

static void bcm2835_v3d_fragment_worker_run_batches(
    BCM2835V3DFragmentWorkerJob *job)
{
    int index;

    while ((index = qatomic_fetch_inc(&job->next_batch)) < job->batches) {
        BCM2835V3DFragmentWorkerSlot *slot = &job->slots[index];
        BCM2835V3DState *s = job->s;
        BCM2835V3DQPUState qpu;
        BCM2835V3DFragmentVarying varying = { 0 };
        uint32_t first = index * BCM2835_V3D_QPU_LANES;
        uint32_t count = MIN(job->count - first,
                             BCM2835_V3D_QPU_LANES);
        uint32_t executed;

        g_private_set(&bcm2835_v3d_fragment_worker_slot, slot);
        bcm2835_v3d_qpu_set_worker_mode(true);
        bcm2835_v3d_init_fragment_qpu(
            s, &qpu, &varying, job->pixels, job->varying_partial,
            job->fragment_w, job->uniforms_address, job->varying_count,
            first, count);
        slot->result = bcm2835_v3d_qpu_run(
            job->code, job->code_size, V3D_QPU_INSTRUCTION_BUDGET,
            &qpu, &executed);
        slot->varying_index = qpu.varying_index;
        slot->color_write_mask = qpu.tlb_color_write_mask;
        memcpy(slot->colors, qpu.io[46], sizeof(slot->colors));
        bcm2835_v3d_qpu_set_worker_mode(false);
        g_private_set(&bcm2835_v3d_fragment_worker_slot, NULL);
    }
}

static void *bcm2835_v3d_fragment_worker_thread(void *opaque)
{
    BCM2835V3DState *s = opaque;
    uint64_t generation = 0;

    rcu_register_thread();
    qemu_mutex_lock(&s->fragment_worker_mutex);
    while (!s->fragment_worker_quit) {
        BCM2835V3DFragmentWorkerJob *job;

        while (!s->fragment_worker_quit &&
               generation == s->fragment_worker_generation) {
            qemu_cond_wait(&s->fragment_worker_cond,
                           &s->fragment_worker_mutex);
        }
        if (s->fragment_worker_quit) {
            break;
        }
        generation = s->fragment_worker_generation;
        job = s->fragment_worker_job;
        qemu_mutex_unlock(&s->fragment_worker_mutex);
        bcm2835_v3d_fragment_worker_run_batches(job);
        qemu_mutex_lock(&s->fragment_worker_mutex);
        s->fragment_worker_completed++;
        if (s->fragment_worker_completed == s->fragment_workers) {
            qemu_cond_broadcast(&s->fragment_worker_done);
        }
    }
    qemu_mutex_unlock(&s->fragment_worker_mutex);
    rcu_unregister_thread();
    return NULL;
}

static bool bcm2835_v3d_fragment_pixels_unique(const uint16_t *pixels,
                                               unsigned count)
{
    uint64_t seen[64] = { 0 };

    for (unsigned i = 0; i < count; i++) {
        unsigned pixel = pixels[i];
        uint64_t bit;

        if (pixel >= 64 * 64) {
            return false;
        }
        bit = UINT64_C(1) << (pixel % 64);
        if (seen[pixel / 64] & bit) {
            return false;
        }
        seen[pixel / 64] |= bit;
    }
    return true;
}

static bool bcm2835_v3d_fragment_try_workers(
    BCM2835V3DState *s, const uint16_t *pixels,
    const uint32_t *varying_partial, const uint32_t *fragment_w,
    unsigned count, uint32_t uniforms_address, unsigned varying_count,
    const uint8_t *code, uint32_t code_size)
{
    BCM2835V3DFragmentWorkerJob job = {
        .s = s,
        .code = code,
        .code_size = code_size,
        .pixels = pixels,
        .varying_partial = varying_partial,
        .fragment_w = fragment_w,
        .count = count,
        .batches = DIV_ROUND_UP(count, BCM2835_V3D_QPU_LANES),
        .varying_count = varying_count,
        .uniforms_address = uniforms_address,
    };

    if (!s->fragment_workers || job.batches < 2 || s->wgpu_context ||
        !bcm2835_v3d_fragment_pixels_unique(pixels, count)) {
        return false;
    }
    g_autofree BCM2835V3DFragmentWorkerSlot *slots =
        g_new0(BCM2835V3DFragmentWorkerSlot, job.batches);

    job.slots = slots;
    qemu_mutex_lock(&s->fragment_worker_mutex);
    s->fragment_worker_job = &job;
    s->fragment_worker_completed = 0;
    s->fragment_worker_generation++;
    qemu_cond_broadcast(&s->fragment_worker_cond);
    qemu_mutex_unlock(&s->fragment_worker_mutex);

    bcm2835_v3d_fragment_worker_run_batches(&job);

    qemu_mutex_lock(&s->fragment_worker_mutex);
    while (s->fragment_worker_completed < s->fragment_workers) {
        qemu_cond_wait(&s->fragment_worker_done, &s->fragment_worker_mutex);
    }
    s->fragment_worker_job = NULL;
    qemu_mutex_unlock(&s->fragment_worker_mutex);

    for (unsigned i = 0; i < job.batches; i++) {
        BCM2835V3DFragmentWorkerSlot *slot = &job.slots[i];
        unsigned first = i * BCM2835_V3D_QPU_LANES;
        unsigned batch = MIN(count - first, BCM2835_V3D_QPU_LANES);
        uint16_t expected_mask = batch == BCM2835_V3D_QPU_LANES ?
                                 UINT16_MAX : (1U << batch) - 1;

        if (slot->fallback || slot->result != BCM2835_V3D_QPU_END ||
            slot->varying_index != varying_count ||
            (slot->color_write_mask & expected_mask) != expected_mask) {
            return false;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        unsigned batch = i / BCM2835_V3D_QPU_LANES;
        unsigned lane = i % BCM2835_V3D_QPU_LANES;

        s->tile_color[pixels[i]] = job.slots[batch].colors[lane];
    }
    return true;
}

#define BCM2835_V3D_WGPU_TILES_PER_SLICE 4

#ifdef CONFIG_V3D_WGPU
#define BCM2835_V3D_QPU_READ_CACHE_ENTRIES 4096
G_STATIC_ASSERT((BCM2835_V3D_QPU_READ_CACHE_ENTRIES &
                 (BCM2835_V3D_QPU_READ_CACHE_ENTRIES - 1)) == 0);

typedef struct BCM2835V3DQPUReadCacheEntry {
    uint32_t address;
    uint32_t value;
    bool valid;
} BCM2835V3DQPUReadCacheEntry;

typedef struct BCM2835V3DQPUReadCache {
    BCM2835V3DState *s;
    BCM2835V3DQPUReadCacheEntry entries[
        BCM2835_V3D_QPU_READ_CACHE_ENTRIES];
    uint32_t texture_hits;
    uint32_t texture_misses;
} BCM2835V3DQPUReadCache;

static bool bcm2835_v3d_qpu_read_cached(BCM2835V3DQPUReadCache *cache,
                                         uint32_t address, uint32_t *value)
{
    unsigned index = ((address >> 2) ^ (address >> 12)) &
                     (BCM2835_V3D_QPU_READ_CACHE_ENTRIES - 1);
    BCM2835V3DQPUReadCacheEntry *entry = &cache->entries[index];

    if (entry->valid && entry->address == address) {
        *value = entry->value;
        cache->texture_hits++;
        return true;
    }

    if ((uint64_t)address + sizeof(uint32_t) > UINT32_MAX + 1ULL) {
        return false;
    }
    cache->texture_misses++;
    uint8_t bytes[sizeof(uint32_t)];

    if (!bcm2835_v3d_dma_read(cache->s, address, bytes, sizeof(bytes))) {
        return false;
    }
    *value = ldl_le_p(bytes);
    entry->address = address;
    entry->value = *value;
    entry->valid = true;
    return true;
}

static bool bcm2835_v3d_qpu_read_memory_cached(void *opaque,
                                                uint32_t address,
                                                uint32_t *value)
{
    return bcm2835_v3d_qpu_read_cached(opaque, address, value);
}

static bool bcm2835_v3d_qpu_read_texture_cached(
    void *opaque, uint32_t base, unsigned memory_format,
    unsigned width, unsigned height, unsigned x, unsigned y,
    uint32_t *value)
{
    BCM2835V3DQPUReadCache *cache = opaque;
    uint64_t address;

    if (memory_format > 2 || x >= width || y >= height) {
        return false;
    }
    address = bcm2835_v3d_memory_pixel_addr(base, memory_format,
                                            sizeof(uint32_t), width, x, y);
    if (address + sizeof(uint32_t) > UINT32_MAX + 1ULL) {
        return false;
    }
    return bcm2835_v3d_qpu_read_cached(cache, address, value);
}
#endif

typedef struct BCM2835V3DWGPUTileBatch {
    uint8_t x;
    uint8_t y;
    bool stored;
    uint32_t pending_pixels;
    uint8_t store_packet[7];
    uint64_t blend_pixels[BCM2835_V3D_WGPU_MAX_TILE_PIXELS / 64];
    uint32_t colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
} BCM2835V3DWGPUTileBatch;

typedef struct BCM2835V3DWGPUFragmentBatch {
    uint8_t *code;
    uint32_t code_size;
    uint32_t uniform_address;
    uint32_t uniforms[6];
    unsigned varying_count;
    unsigned deferred_stores;
    GArray *pixels;
    GArray *tile_ids;
    GArray *tiles;
    GArray *input_colors;
    GArray *tmu0_colors;
    GArray *varying_partial;
    GArray *fragment_w;
} BCM2835V3DWGPUFragmentBatch;

#ifdef CONFIG_V3D_WGPU
typedef enum BCM2835V3DWGPUQueueResult {
    BCM2835_V3D_WGPU_NOT_QUEUED,
    BCM2835_V3D_WGPU_QUEUED,
    BCM2835_V3D_WGPU_QUEUE_ERROR,
} BCM2835V3DWGPUQueueResult;

static int bcm2835_v3d_wgpu_batch_find_tile(
    BCM2835V3DWGPUFragmentBatch *batch, uint8_t x, uint8_t y)
{
    for (unsigned i = 0; batch->tiles && i < batch->tiles->len; i++) {
        BCM2835V3DWGPUTileBatch *tile =
            &g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch, i);

        if (tile->x == x && tile->y == y) {
            return i;
        }
    }
    return -1;
}

static bool bcm2835_v3d_wgpu_batch_defer_across_tile(
    BCM2835V3DState *s, uint8_t next_x, uint8_t next_y)
{
    BCM2835V3DWGPUFragmentBatch *batch = s->wgpu_batch;
    int index;

    if (s->pending_tile_load || !batch ||
        (index = bcm2835_v3d_wgpu_batch_find_tile(
                     batch, s->tile_x, s->tile_y)) < 0 ||
        (next_x == s->tile_x && next_y == s->tile_y)) {
        return false;
    }
    return g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch,
                         index).stored;
}

static bool bcm2835_v3d_wgpu_batch_defer_store(
    BCM2835V3DState *s, const uint8_t packet[7])
{
    BCM2835V3DWGPUFragmentBatch *batch = s->wgpu_batch;
    int index;
    BCM2835V3DWGPUTileBatch *tile;
    uint64_t params;

    if (!batch || !batch->pixels || !batch->pixels->len ||
        (index = bcm2835_v3d_wgpu_batch_find_tile(
                     batch, s->tile_x, s->tile_y)) < 0) {
        return false;
    }
    params = (uint32_t)ldl_le_p(packet + 1) |
             ((uint64_t)(uint16_t)lduw_le_p(packet + 5) << 32);
    if ((params & 7) != 1 || (params & (1ULL << 16)) ||
        ((params >> 6) & 3) != 0 || ((params >> 4) & 3) > 2 ||
        ((params >> 8) & 3) > 2 || ((params >> 8) & 3) == 1 ||
        (params & (1ULL << 19)) || !s->render_config_valid ||
        !s->render_width || !s->render_height) {
        return false;
    }
    tile = &g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch, index);
    if (!tile->pending_pixels || tile->stored) {
        return false;
    }
    memcpy(tile->store_packet, packet, sizeof(tile->store_packet));
    tile->stored = true;
    batch->deferred_stores++;
    return true;
}

static int bcm2835_v3d_wgpu_batch_add_tile(
    BCM2835V3DState *s, BCM2835V3DWGPUFragmentBatch *batch)
{
    BCM2835V3DWGPUTileBatch tile = {
        .x = s->tile_x,
        .y = s->tile_y,
    };

    if (!batch->tiles) {
        batch->tiles = g_array_sized_new(
            FALSE, FALSE, sizeof(tile),
            MIN(BCM2835_V3D_WGPU_MAX_BATCH_TILES, 64));
    }
    if (batch->tiles->len >= BCM2835_V3D_WGPU_MAX_BATCH_TILES) {
        return -1;
    }
    memcpy(tile.colors, s->tile_color, sizeof(tile.colors));
    g_array_append_val(batch->tiles, tile);
    return batch->tiles->len - 1;
}

static bool bcm2835_v3d_wgpu_batch_commit_stores(
    BCM2835V3DState *s, BCM2835V3DWGPUFragmentBatch *batch)
{
    uint8_t active_x = s->tile_x;
    uint8_t active_y = s->tile_y;
    uint32_t active_colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    bool active_valid = s->tile_color_valid;
    int active_index = bcm2835_v3d_wgpu_batch_find_tile(
        batch, active_x, active_y);

    if (active_index >= 0) {
        BCM2835V3DWGPUTileBatch *tile =
            &g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch,
                           active_index);

        memcpy(active_colors, tile->colors, sizeof(active_colors));
        active_valid = true;
    } else {
        memcpy(active_colors, s->tile_color, sizeof(active_colors));
    }

    for (unsigned i = 0; batch->tiles && i < batch->tiles->len; i++) {
        BCM2835V3DWGPUTileBatch *tile =
            &g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch, i);

        if (!tile->stored) {
            continue;
        }
        s->tile_x = tile->x;
        s->tile_y = tile->y;
        s->tile_color_valid = true;
        memcpy(s->tile_color, tile->colors, sizeof(s->tile_color));
        if (!bcm2835_v3d_tile_buffer_packet(s, tile->store_packet, true)) {
            s->tile_x = active_x;
            s->tile_y = active_y;
            memcpy(s->tile_color, active_colors, sizeof(s->tile_color));
            s->tile_color_valid = active_valid;
            return false;
        }
        if ((int)i == active_index) {
            active_valid = s->tile_color_valid;
        }
    }

    s->tile_x = active_x;
    s->tile_y = active_y;
    memcpy(s->tile_color, active_colors, sizeof(s->tile_color));
    s->tile_color_valid = active_valid;
    g_clear_pointer(&batch->tiles, g_array_unref);
    if (batch->pixels) {
        g_array_set_size(batch->pixels, 0);
        g_array_set_size(batch->tile_ids, 0);
        if (batch->input_colors) {
            g_array_set_size(batch->input_colors, 0);
        }
        g_array_set_size(batch->tmu0_colors, 0);
        g_array_set_size(batch->varying_partial, 0);
        g_array_set_size(batch->fragment_w, 0);
    }
    batch->deferred_stores = 0;
    return true;
}

static bool bcm2835_v3d_wgpu_batch_flush(
    BCM2835V3DState *s, BCM2835V3DWGPUFragmentBatch *batch)
{
    unsigned count = batch->pixels ? batch->pixels->len : 0;
    uint16_t *pixels;
    uint32_t *input_colors = NULL;
    g_autofree uint32_t *colors = NULL;
    int64_t scatter_start_us;
    unsigned tile_groups = 0;
    bool colors_ready = false;

    pixels = count ? (uint16_t *)batch->pixels->data : NULL;
    if (count) {
        g_assert(batch->tile_ids && batch->tiles &&
                 batch->tile_ids->len == count);
        if (bcm2835_v3d_wgpu_needs_color_load(batch->code,
                                               batch->code_size)) {
            if (!batch->input_colors ||
                batch->input_colors->len != count) {
                return false;
            }
            input_colors = (uint32_t *)batch->input_colors->data;
        }
    }
    if (count && count < BCM2835_V3D_WGPU_MIN_BATCH_PIXELS) {
        /* shortcut: exact translated shaders only; extend for new kernels. */
        colors = g_new(uint32_t, count);
        colors_ready = bcm2835_v3d_wgpu_run_software(
            batch->code, batch->code_size, batch->uniforms, count,
            input_colors, (uint32_t *)batch->tmu0_colors->data, colors);
    }
    if (!colors_ready && count >= BCM2835_V3D_WGPU_MIN_BATCH_PIXELS) {
        colors = g_new(uint32_t, count);
        colors_ready = bcm2835_v3d_wgpu_run(
            s->wgpu_context, batch->code, batch->code_size, batch->uniforms,
            count, input_colors, (uint32_t *)batch->tmu0_colors->data,
            NULL, colors);
    }
    if (colors_ready) {
        uint16_t *tile_ids = (uint16_t *)batch->tile_ids->data;

        scatter_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        for (unsigned first = 0; first < count;) {
            uint16_t tile_id = tile_ids[first];
            BCM2835V3DWGPUTileBatch *tile;
            unsigned end = first + 1;

            if (tile_id >= batch->tiles->len) {
                return false;
            }
            tile = &g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch,
                                  tile_id);
            while (end < count && tile_ids[end] == tile_id) {
                end++;
            }
            for (unsigned i = first; i < end; i++) {
                tile->colors[pixels[i]] = colors[i];
            }
            tile_groups++;
            first = end;
        }
        trace_bcm2835_v3d_wgpu_scatter(
            count, tile_groups,
            qemu_clock_get_us(QEMU_CLOCK_REALTIME) - scatter_start_us);
    } else if (count) {
        uint8_t saved_x = s->tile_x;
        uint8_t saved_y = s->tile_y;
        bool saved_valid = s->tile_color_valid;
        uint32_t saved_colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];

        memcpy(saved_colors, s->tile_color, sizeof(saved_colors));
        /* Small or failed GPU batches replay through the reference QPU. */
        for (unsigned first = 0; first < count;) {
            uint16_t tile_id = g_array_index(batch->tile_ids, uint16_t, first);
            BCM2835V3DWGPUTileBatch *tile;
            BCM2835V3DQPUState qpu;
            BCM2835V3DFragmentVarying varying = { 0 };
            uint32_t executed;
            unsigned size = 0;
            uint16_t expected_mask;

            if (tile_id >= batch->tiles->len) {
                return false;
            }
            tile = &g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch,
                                  tile_id);
            while (size < BCM2835_V3D_QPU_LANES && first + size < count &&
                   g_array_index(batch->tile_ids, uint16_t, first + size) ==
                       tile_id) {
                size++;
            }
            expected_mask = size == BCM2835_V3D_QPU_LANES ?
                            UINT16_MAX : (1U << size) - 1;
            s->tile_x = tile->x;
            s->tile_y = tile->y;
            s->tile_color_valid = true;
            memcpy(s->tile_color, tile->colors, sizeof(s->tile_color));
            bcm2835_v3d_init_fragment_qpu(
                s, &qpu, &varying, pixels,
                (uint32_t *)batch->varying_partial->data,
                (uint32_t *)batch->fragment_w->data,
                batch->uniform_address, batch->varying_count, first, size);
            if (bcm2835_v3d_qpu_run(batch->code, batch->code_size,
                                    V3D_QPU_INSTRUCTION_BUDGET, &qpu,
                                    &executed) != BCM2835_V3D_QPU_END ||
                qpu.varying_index != varying.count ||
                (qpu.tlb_color_write_mask & expected_mask) != expected_mask) {
                s->tile_x = saved_x;
                s->tile_y = saved_y;
                s->tile_color_valid = saved_valid;
                memcpy(s->tile_color, saved_colors, sizeof(s->tile_color));
                return false;
            }
            for (unsigned lane = 0; lane < size; lane++) {
                tile->colors[pixels[first + lane]] = qpu.io[46][lane];
            }
            first += size;
        }
        s->tile_x = saved_x;
        s->tile_y = saved_y;
        s->tile_color_valid = saved_valid;
        memcpy(s->tile_color, saved_colors, sizeof(s->tile_color));
    }

    return bcm2835_v3d_wgpu_batch_commit_stores(s, batch);
}

static BCM2835V3DWGPUQueueResult bcm2835_v3d_wgpu_batch_queue(
    BCM2835V3DState *s, BCM2835V3DWGPUFragmentBatch *batch,
    const uint8_t *code, uint32_t code_size, const uint32_t uniforms[6],
    const uint16_t *pixels, const uint32_t *varying_partial,
    const uint32_t *fragment_w, unsigned count)
{
    uint32_t samples[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    uint16_t tile_ids[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    g_autofree BCM2835V3DQPUReadCache *read_cache = NULL;
    unsigned varying_count = s->shader_record[3];
    unsigned queued = batch->pixels ? batch->pixels->len : 0;
    BCM2835V3DQPUState sampler;
    int tile_index;
    int64_t sample_start_us;
    bool needs_color_load = bcm2835_v3d_wgpu_needs_color_load(code,
                                                               code_size);
    bool same_shader = queued && batch->code_size == code_size &&
        batch->varying_count == varying_count &&
        !memcmp(batch->code, code, code_size) &&
        !memcmp(batch->uniforms, uniforms, sizeof(batch->uniforms));

    if (count > G_N_ELEMENTS(samples)) {
        return BCM2835_V3D_WGPU_NOT_QUEUED;
    }
    if (queued && (!same_shader ||
                   queued + count > BCM2835_V3D_WGPU_MAX_BATCH_PIXELS)) {
        if (!bcm2835_v3d_wgpu_batch_flush(s, batch)) {
            return BCM2835_V3D_WGPU_QUEUE_ERROR;
        }
        queued = 0;
    }
    tile_index = bcm2835_v3d_wgpu_batch_find_tile(
        batch, s->tile_x, s->tile_y);
    if ((tile_index >= 0 &&
         g_array_index(batch->tiles, BCM2835V3DWGPUTileBatch,
                       tile_index).stored) ||
        (tile_index < 0 && batch->tiles &&
         batch->tiles->len >= BCM2835_V3D_WGPU_MAX_BATCH_TILES)) {
        if (!bcm2835_v3d_wgpu_batch_flush(s, batch)) {
            return BCM2835_V3D_WGPU_QUEUE_ERROR;
        }
        queued = 0;
        tile_index = -1;
    }
    if (needs_color_load && tile_index >= 0) {
        uint64_t seen[BCM2835_V3D_WGPU_MAX_TILE_PIXELS / 64] = { 0 };
        BCM2835V3DWGPUTileBatch *tile = &g_array_index(
            batch->tiles, BCM2835V3DWGPUTileBatch, tile_index);
        bool overlap = false;
        bool duplicate = false;

        for (unsigned i = 0; i < count; i++) {
            unsigned pixel = pixels[i];
            unsigned word = pixel / 64;
            uint64_t bit = UINT64_C(1) << (pixel % 64);

            if (word >= G_N_ELEMENTS(seen)) {
                return BCM2835_V3D_WGPU_NOT_QUEUED;
            }
            duplicate |= (seen[word] & bit) != 0;
            overlap |= (tile->blend_pixels[word] & bit) != 0;
            seen[word] |= bit;
        }
        if (duplicate) {
            if (queued && !bcm2835_v3d_wgpu_batch_flush(s, batch)) {
                return BCM2835_V3D_WGPU_QUEUE_ERROR;
            }
            return BCM2835_V3D_WGPU_NOT_QUEUED;
        }
        /* shortcut: overlaps split batches; chain on measured overdraw. */
        if (overlap) {
            if (!bcm2835_v3d_wgpu_batch_flush(s, batch)) {
                return BCM2835_V3D_WGPU_QUEUE_ERROR;
            }
            queued = 0;
            tile_index = -1;
        }
    }

    /* shortcut: no tile-time writes; add invalidation if guests require them. */
    read_cache = g_new0(BCM2835V3DQPUReadCache, 1);
    read_cache->s = s;

    sampler.read_memory = bcm2835_v3d_qpu_read_memory_cached;
    sampler.memory_opaque = read_cache;
    sampler.read_texture = bcm2835_v3d_qpu_read_texture_cached;
    sampler.texture_opaque = read_cache;
    sample_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (!bcm2835_v3d_qpu_sample_tmu0_batch(
            &sampler, uniforms[0], uniforms[1], varying_partial,
            fragment_w, varying_count, count, samples)) {
        return BCM2835_V3D_WGPU_NOT_QUEUED;
    }
    trace_bcm2835_v3d_qpu_sample_batch(
        count, qemu_clock_get_us(QEMU_CLOCK_REALTIME) - sample_start_us);
    trace_bcm2835_v3d_qpu_texture_cache(read_cache->texture_hits,
                                        read_cache->texture_misses);

    if (!batch->pixels) {
        batch->pixels = g_array_sized_new(FALSE, FALSE, sizeof(uint16_t),
                                          BCM2835_V3D_WGPU_MAX_BATCH_PIXELS);
        batch->tmu0_colors = g_array_sized_new(FALSE, FALSE,
                                               sizeof(uint32_t),
                                               BCM2835_V3D_WGPU_MAX_BATCH_PIXELS);
        batch->varying_partial = g_array_sized_new(
            FALSE, FALSE, sizeof(uint32_t),
            BCM2835_V3D_WGPU_MAX_BATCH_PIXELS * varying_count);
        batch->fragment_w = g_array_sized_new(
            FALSE, FALSE, sizeof(uint32_t),
            BCM2835_V3D_WGPU_MAX_BATCH_PIXELS);
        batch->tile_ids = g_array_sized_new(
            FALSE, FALSE, sizeof(uint16_t),
            BCM2835_V3D_WGPU_MAX_BATCH_PIXELS);
    }
    if (needs_color_load && !batch->input_colors) {
        batch->input_colors = g_array_sized_new(
            FALSE, FALSE, sizeof(uint32_t),
            BCM2835_V3D_WGPU_MAX_BATCH_PIXELS);
    }
    if (tile_index < 0) {
        tile_index = bcm2835_v3d_wgpu_batch_add_tile(s, batch);
        if (tile_index < 0) {
            return BCM2835_V3D_WGPU_NOT_QUEUED;
        }
    }
    if (!queued) {
        g_free(batch->code);
        batch->code = g_memdup2(code, code_size);
        batch->code_size = code_size;
        batch->uniform_address = ldl_le_p(s->shader_record + 8);
        memcpy(batch->uniforms, uniforms, sizeof(batch->uniforms));
        batch->varying_count = varying_count;
    }
    g_array_append_vals(batch->pixels, pixels, count);
    for (unsigned i = 0; i < count; i++) {
        tile_ids[i] = tile_index;
    }
    g_array_append_vals(batch->tile_ids, tile_ids, count);
    g_array_append_vals(batch->tmu0_colors, samples, count);
    BCM2835V3DWGPUTileBatch *tile = &g_array_index(
        batch->tiles, BCM2835V3DWGPUTileBatch, tile_index);
    if (needs_color_load) {
        uint32_t inputs[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];

        for (unsigned i = 0; i < count; i++) {
            inputs[i] = tile->colors[pixels[i]];
            tile->blend_pixels[pixels[i] / 64] |=
                UINT64_C(1) << (pixels[i] % 64);
        }
        g_array_append_vals(batch->input_colors, inputs, count);
    }
    tile->pending_pixels += count;
    g_array_append_vals(batch->fragment_w, fragment_w, count);
    if (varying_count) {
        g_array_append_vals(batch->varying_partial, varying_partial,
                            count * varying_count);
    }
    g_assert(batch->pixels->len == batch->tile_ids->len);
    g_assert(batch->tiles->len <= BCM2835_V3D_WGPU_MAX_BATCH_TILES);
    return BCM2835_V3D_WGPU_QUEUED;
}
#endif

static void bcm2835_v3d_wgpu_batch_clear(
    BCM2835V3DWGPUFragmentBatch *batch)
{
#ifdef CONFIG_V3D_WGPU
    g_free(batch->code);
    g_clear_pointer(&batch->pixels, g_array_unref);
    g_clear_pointer(&batch->tile_ids, g_array_unref);
    g_clear_pointer(&batch->tiles, g_array_unref);
    g_clear_pointer(&batch->input_colors, g_array_unref);
    g_clear_pointer(&batch->tmu0_colors, g_array_unref);
    g_clear_pointer(&batch->varying_partial, g_array_unref);
    g_clear_pointer(&batch->fragment_w, g_array_unref);
#else
    (void)batch;
#endif
}

static bool bcm2835_v3d_wgpu_batch_flush_pending(BCM2835V3DState *s)
{
#ifdef CONFIG_V3D_WGPU
    BCM2835V3DWGPUFragmentBatch *batch = s->wgpu_batch;

    return !batch || bcm2835_v3d_wgpu_batch_flush(s, batch);
#else
    return true;
#endif
}

static void bcm2835_v3d_wgpu_batch_free(BCM2835V3DState *s)
{
#ifdef CONFIG_V3D_WGPU
    BCM2835V3DWGPUFragmentBatch *batch = s->wgpu_batch;

    if (batch) {
        bcm2835_v3d_wgpu_batch_clear(batch);
        g_free(batch);
        s->wgpu_batch = NULL;
    }
#endif
}

static bool bcm2835_v3d_wgpu_batch_flush_before(
    BCM2835V3DState *s, unsigned thread, uint32_t addr)
{
    if (thread != 1 || bcm2835_v3d_wgpu_batch_flush_pending(s)) {
        return true;
    }
    bcm2835_v3d_wgpu_batch_free(s);
    bcm2835_v3d_thread_error(s, thread, addr,
                             "fragment shader batch could not be committed");
    return false;
}

static bool bcm2835_v3d_run_fragment_shader(BCM2835V3DState *s,
                                             const uint16_t *pixels,
                                             const uint32_t *varying_partial,
                                             const uint32_t *fragment_w,
                                             unsigned count,
                                             BCM2835V3DWGPUFragmentBatch *batch)
{
#ifndef CONFIG_V3D_WGPU
    (void)batch;
#endif
    uint32_t code_address = ldl_le_p(s->shader_record + 4) & ~7U;
    uint32_t uniforms_address = ldl_le_p(s->shader_record + 8);
    uint8_t *code;
    uint32_t code_size;
#ifdef CONFIG_V3D_WGPU
    uint32_t wgpu_uniforms[6];
    uint32_t wgpu_colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    uint32_t wgpu_input_colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    uint32_t wgpu_tmu0_colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    uint32_t wgpu_software_colors[BCM2835_V3D_WGPU_MAX_TILE_PIXELS];
    bool wgpu_uniforms_readable = false;
    bool wgpu_shader_supported = false;
    bool wgpu_shader_needs_tmu0 = false;
    bool wgpu_shader_can_batch = false;
#endif

    if (!code_address || !count ||
        !bcm2835_v3d_load_qpu_program(s, code_address, &code, &code_size)) {
        return false;
    }

#ifdef CONFIG_V3D_WGPU
    if (count <= BCM2835_V3D_WGPU_MAX_TILE_PIXELS && s->wgpu_context) {
        wgpu_shader_supported =
            bcm2835_v3d_wgpu_supports(code, code_size);
        wgpu_shader_needs_tmu0 =
            bcm2835_v3d_wgpu_needs_tmu0(code, code_size);
        wgpu_shader_can_batch =
            bcm2835_v3d_wgpu_can_batch(code, code_size);
        if (!wgpu_shader_supported) {
            bcm2835_v3d_wgpu_report_unsupported(
                s->wgpu_context, code_address, code, code_size);
        }
    }
    if (wgpu_shader_supported) {
        wgpu_uniforms_readable = true;
        for (unsigned i = 0; i < G_N_ELEMENTS(wgpu_uniforms); i++) {
            uint64_t address = (uint64_t)uniforms_address + i * 4;

            if (address > UINT32_MAX ||
                !bcm2835_v3d_qpu_read_uniform(s, address,
                                              &wgpu_uniforms[i])) {
                wgpu_uniforms_readable = false;
                break;
            }
        }
        if (wgpu_uniforms_readable) {
            if (!wgpu_shader_can_batch && batch->pixels &&
                batch->pixels->len &&
                !bcm2835_v3d_wgpu_batch_flush(s, batch)) {
                g_free(code);
                return false;
            }
            if (s->wgpu_verify || !wgpu_shader_can_batch) {
                for (unsigned i = 0; i < count; i++) {
                    wgpu_input_colors[i] = s->tile_color[pixels[i]];
                }
            }
            if (!s->wgpu_verify && wgpu_shader_can_batch) {
                BCM2835V3DWGPUQueueResult queued =
                    bcm2835_v3d_wgpu_batch_queue(
                        s, batch, code, code_size, wgpu_uniforms, pixels,
                        varying_partial, fragment_w, count);

                if (queued == BCM2835_V3D_WGPU_QUEUED) {
                    g_free(code);
                    return true;
                }
                if (queued == BCM2835_V3D_WGPU_QUEUE_ERROR) {
                    g_free(code);
                    return false;
                }
                if (bcm2835_v3d_wgpu_needs_color_load(code, code_size)) {
                    for (unsigned i = 0; i < count; i++) {
                        wgpu_input_colors[i] = s->tile_color[pixels[i]];
                    }
                }
            }
            /* shortcut: other independent shaders wait for tile batching. */
            if (!s->wgpu_verify &&
                bcm2835_v3d_wgpu_needs_color_load(code, code_size)) {
                bool samples_ready = true;

                if (wgpu_shader_needs_tmu0) {
                    for (unsigned first = 0; first < count;
                         first += BCM2835_V3D_QPU_LANES) {
                        BCM2835V3DQPUState qpu;
                        BCM2835V3DFragmentVarying varying = { 0 };
                        uint32_t executed;
                        uint32_t batch = MIN(count - first,
                                             BCM2835_V3D_QPU_LANES);
                        BCM2835V3DQPUResult result;

                        bcm2835_v3d_init_fragment_qpu(
                            s, &qpu, &varying, pixels, varying_partial,
                            fragment_w, uniforms_address,
                            s->shader_record[3], first, batch);
                        result = bcm2835_v3d_qpu_run_until_tmu0(
                            code, code_size, V3D_QPU_INSTRUCTION_BUDGET,
                            &qpu, &executed);
                        if (result != BCM2835_V3D_QPU_TMU0_LOADED ||
                            qpu.varying_index != varying.count) {
                            samples_ready = false;
                            break;
                        }
                        for (unsigned lane = 0; lane < batch; lane++) {
                            wgpu_tmu0_colors[first + lane] =
                                qpu.acc[4][lane];
                        }
                    }
                }
                if (samples_ready &&
                    bcm2835_v3d_wgpu_run(
                        s->wgpu_context, code, code_size, wgpu_uniforms,
                        count, wgpu_input_colors,
                        wgpu_shader_needs_tmu0 ? wgpu_tmu0_colors : NULL,
                        NULL, wgpu_colors)) {
                    for (unsigned i = 0; i < count; i++) {
                        s->tile_color[pixels[i]] = wgpu_colors[i];
                    }
                    g_free(code);
                    return true;
                }
            }
        }
    }
#endif

#ifdef CONFIG_V3D_WGPU
    if (batch->pixels && batch->pixels->len &&
        !bcm2835_v3d_wgpu_batch_flush(s, batch)) {
        g_free(code);
        return false;
    }
#endif

    if (s->fragment_workers) {
        if (bcm2835_v3d_fragment_try_workers(
                s, pixels, varying_partial, fragment_w, count,
                uniforms_address, s->shader_record[3], code, code_size)) {
            g_free(code);
            return true;
        }
    }

    for (unsigned first = 0; first < count;
         first += BCM2835_V3D_QPU_LANES) {
        BCM2835V3DQPUState qpu;
        BCM2835V3DFragmentVarying varying = { 0 };
        uint32_t executed;
        uint32_t batch = MIN(count - first, BCM2835_V3D_QPU_LANES);
        uint16_t expected_mask = batch == BCM2835_V3D_QPU_LANES ?
                                 UINT16_MAX : (1U << batch) - 1;
        BCM2835V3DQPUResult result;

        bcm2835_v3d_init_fragment_qpu(s, &qpu, &varying, pixels,
                                      varying_partial, fragment_w,
                                      uniforms_address, s->shader_record[3],
                                      first, batch);
        result = bcm2835_v3d_qpu_run(code, code_size,
                                      V3D_QPU_INSTRUCTION_BUDGET, &qpu,
                                      &executed);
        if (result != BCM2835_V3D_QPU_END ||
            qpu.varying_index != varying.count ||
            (qpu.tlb_color_write_mask & expected_mask) !=
            expected_mask) {
            uint64_t instruction = 0;

            if ((uint64_t)qpu.pc * sizeof(uint64_t) + sizeof(uint64_t) <=
                code_size) {
                instruction = ldq_le_p(code + qpu.pc * sizeof(uint64_t));
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: fragment QPU 0x%08x result=%u pc=%u instruction=0x%016" PRIx64 " varying=%u/%u color-mask=0x%04x expected=0x%04x src_line=%u tmu0 has_t=%d has_b=%d wm=%04x/%04x/%04x/%04x uniform_addr=0x%08x\n",
                          code_address, result, qpu.pc, instruction,
                          qpu.varying_index, varying.count,
                          qpu.tlb_color_write_mask, expected_mask,
                          qpu.unsupported_line, qpu.tmu[0].has_t,
                          qpu.tmu[0].has_b,
                          qpu.tmu[0].instruction_write_mask[0],
                          qpu.tmu[0].instruction_write_mask[1],
                          qpu.tmu[0].instruction_write_mask[2],
                          qpu.tmu[0].instruction_write_mask[3],
                          qpu.uniform_address);
            if (s->fragment_dump_count < 4) {
                s->fragment_dump_count++;
                for (uint32_t i = 0; i < 128 &&
                     (uint64_t)(i + 1) * sizeof(uint64_t) <= code_size; i++) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "bcm2835_v3d: dump code 0x%08x [%u] 0x%016" PRIx64 "\n",
                                  code_address, i,
                                  ldq_le_p(code + i * sizeof(uint64_t)));
                }
                for (unsigned i = 0; i < 16; i++) {
                    uint32_t u = 0;
                    bool ok = bcm2835_v3d_dma_read(s, uniforms_address + i * 4,
                                                   &u, sizeof(u));

                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "bcm2835_v3d: dump uniform 0x%08x [%u] 0x%08x%s\n",
                                  uniforms_address, i, le32_to_cpu(u),
                                  ok ? "" : " (read failed)");
                }
            }
            g_free(code);
            return false;
        }
        for (unsigned lane = 0; lane < batch; lane++) {
#ifdef CONFIG_V3D_WGPU
            if (s->wgpu_context && s->wgpu_verify &&
                wgpu_uniforms_readable) {
                wgpu_software_colors[first + lane] = qpu.io[46][lane];
                if (wgpu_shader_needs_tmu0) {
                    wgpu_tmu0_colors[first + lane] =
                        qpu.tmu[0].result[lane];
                }
            }
#endif
            s->tile_color[pixels[first + lane]] = qpu.io[46][lane];
        }
    }
#ifdef CONFIG_V3D_WGPU
    if (s->wgpu_context && s->wgpu_verify && wgpu_uniforms_readable &&
        count <= BCM2835_V3D_WGPU_MAX_TILE_PIXELS &&
        bcm2835_v3d_wgpu_run(s->wgpu_context, code, code_size,
                             wgpu_uniforms, count, wgpu_input_colors,
                             wgpu_shader_needs_tmu0 ?
                             wgpu_tmu0_colors : NULL,
                             wgpu_software_colors, wgpu_colors)) {
        for (unsigned i = 0; i < count; i++) {
            s->tile_color[pixels[i]] = wgpu_colors[i];
        }
    }
#endif
    g_free(code);
    return true;
}

static bool bcm2835_v3d_init_tile_lists(BCM2835V3DState *s)
{
    uint32_t tile_count = (uint32_t)s->bin_tiles_x * s->bin_tiles_y;
    uint64_t state_size = (uint64_t)tile_count * 48;
    uint64_t initial_size = (uint64_t)tile_count *
                            s->bin_alloc_initial_block_size;
    uint64_t pool_address = ((uint64_t)s->bin_alloc_base + initial_size +
                             V3D_BIN_ALLOC_ALIGN - 1) &
                            ~(uint64_t)(V3D_BIN_ALLOC_ALIGN - 1);
    uint64_t pool_offset = pool_address - s->bin_alloc_base;
    uint64_t alloc_end = (uint64_t)s->bin_alloc_base + s->bin_alloc_size;

    if (!tile_count || tile_count > V3D_MAX_RASTER_TILES ||
        !s->bin_alloc_block_size || !s->bin_alloc_initial_block_size ||
        pool_offset > s->bin_alloc_size ||
        (uint64_t)s->bin_state_base + state_size > UINT32_MAX + 1ULL) {
        return false;
    }
    if ((s->bin_flags & V3D_BIN_FLAG_AUTO_INIT_TSDA) &&
        dma_memory_set(&s->dma_as, s->bin_state_base, 0, state_size,
                       MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }

    /* The PTB starts each tile in its initial block, then shares the rest. */
    s->bin_alloc_next = pool_address;
    s->bin_overflow_next = 0;
    s->bin_overflow_remaining = 0;
    s->bin_draw_append_count = 0;
    s->bin_draw_resume = false;
    s->bin_oom_waiting = false;
    s->bin_spilluse = false;
    s->regs[V3D_PCS >> 2] &= ~V3D_PCS_BMOOM;
    s->bin_list_stride = 0;
    s->bin_list_indirect = false;

    for (uint32_t tile = 0; tile < tile_count; tile++) {
        uint64_t tile_cl = (uint64_t)s->bin_alloc_base + tile *
                           s->bin_alloc_initial_block_size;
        uint8_t empty_list = V3D_PACKET_RETURN;

        if (tile_cl + s->bin_alloc_initial_block_size > alloc_end ||
            tile_cl + s->bin_alloc_initial_block_size > UINT32_MAX + 1ULL) {
            return false;
        }
        if (dma_memory_write(&s->dma_as, tile_cl, &empty_list,
                             sizeof(empty_list),
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return false;
        }
        s->bin_tile_list_addr[tile] = tile_cl;
        s->bin_tile_list_used[tile] = 1;
    }
    return true;
}

static bool bcm2835_v3d_overflow_range_valid(BCM2835V3DState *s,
                                             uint32_t base, uint32_t size)
{
    uint64_t end = (uint64_t)base + size;
    uint32_t memory_window = s->bin_alloc_base & 0xf0000000;

    return base && size >= s->bin_alloc_block_size && !(base & 15) &&
           end <= UINT32_MAX + 1ULL && (base & 0xf0000000) == memory_window &&
           ((end - 1) & 0xf0000000) == memory_window;
}

static bool bcm2835_v3d_alloc_tile_block(BCM2835V3DState *s,
                                         uint32_t *address)
{
    uint64_t alloc_end = (uint64_t)s->bin_alloc_base + s->bin_alloc_size;

    if (s->bin_alloc_next + s->bin_alloc_block_size <= alloc_end) {
        *address = s->bin_alloc_next;
        s->bin_alloc_next += s->bin_alloc_block_size;
        return true;
    }

    if (s->bin_overflow_remaining < s->bin_alloc_block_size) {
        s->bin_overflow_next = 0;
        s->bin_overflow_remaining = 0;
    }

    if (!s->bin_overflow_remaining) {
        uint32_t overflow_base = s->regs[V3D_BPOA >> 2];
        uint32_t overflow_size = s->regs[V3D_BPOS >> 2];

        if (bcm2835_v3d_overflow_range_valid(s, overflow_base,
                                             overflow_size)) {
            s->bin_overflow_next = overflow_base;
            s->bin_overflow_remaining = overflow_size;
            s->regs[V3D_BPOS >> 2] = 0;
            if (!s->bin_spilluse) {
                s->bin_spilluse = true;
                bcm2835_v3d_raise_irq(s, V3D_INT_SPILLUSE);
            }
        }
    }

    if (s->bin_overflow_remaining >= s->bin_alloc_block_size) {
        *address = s->bin_overflow_next;
        s->bin_overflow_next += s->bin_alloc_block_size;
        s->bin_overflow_remaining -= s->bin_alloc_block_size;
        return true;
    }

    s->bin_oom_waiting = true;
    s->regs[V3D_PCS >> 2] |= V3D_PCS_BMOOM;
    bcm2835_v3d_raise_irq(s, V3D_INT_OUTOMEM);
    return false;
}

static bool bcm2835_v3d_append_tile_triangle(BCM2835V3DState *s,
                                             uint32_t tile_x,
                                             uint32_t tile_y,
                                             const BCM2835V3DVertex *triangle)
{
    uint32_t tile_index = tile_y * s->bin_tiles_x + tile_x;
    uint8_t commands[23 + 3 * (V3D_MAX_FRAGMENT_VARYINGS + 1) *
                     sizeof(uint32_t)];
    uint32_t list_address;
    uint32_t used;
    uint32_t list_capacity;
    uint64_t pool_start;
    size_t command_size = 23;
    size_t offset = 0;

    if (tile_x >= s->bin_tiles_x || tile_y >= s->bin_tiles_y ||
        tile_index >= V3D_MAX_RASTER_TILES ||
        s->shader_record[3] > V3D_MAX_FRAGMENT_VARYINGS) {
        return false;
    }
    if (s->shader_record[3]) {
        command_size += 3 * (s->shader_record[3] + 1) * sizeof(uint32_t);
    }
    list_address = s->bin_tile_list_addr[tile_index];
    used = s->bin_tile_list_used[tile_index];
    pool_start = ((uint64_t)s->bin_alloc_base +
                  (uint64_t)s->bin_tiles_x * s->bin_tiles_y *
                  s->bin_alloc_initial_block_size + V3D_BIN_ALLOC_ALIGN - 1) &
                 ~(uint64_t)(V3D_BIN_ALLOC_ALIGN - 1);
    list_capacity = list_address < pool_start ?
                    s->bin_alloc_initial_block_size :
                    s->bin_alloc_block_size;
    if (!used || used > list_capacity) {
        return false;
    }

    uint8_t branch[5] = { V3D_PACKET_BRANCH };

    if (command_size - 1 + sizeof(branch) - 1 > list_capacity - used) {
        uint32_t next_block;
        uint8_t empty_list = V3D_PACKET_RETURN;

        if (used - 1 + sizeof(branch) > list_capacity ||
            !bcm2835_v3d_alloc_tile_block(s, &next_block)) {
            return false;
        }
        stl_le_p(branch + 1, next_block);
        if (dma_memory_write(&s->dma_as, next_block, &empty_list,
                             sizeof(empty_list),
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
            dma_memory_write(&s->dma_as, list_address + used - 1, branch,
                             sizeof(branch),
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return false;
        }
        s->bin_tile_list_addr[tile_index] = next_block;
        s->bin_tile_list_used[tile_index] = 1;
        list_address = next_block;
        used = 1;
        list_capacity = s->bin_alloc_block_size;
    }
    if (command_size - 1 > list_capacity - used ||
        (uint64_t)list_address + used + command_size - 1 >
            UINT32_MAX + 1ULL) {
        return false;
    }

    commands[offset++] = V3D_PACKET_PRIMITIVE_FORMAT;
    commands[offset++] = V3D_PRIMITIVE_LIST_32_XY_TRIANGLES;
    commands[offset++] = V3D_PACKET_GL_SHADER_STATE;
    stl_le_p(commands + offset, s->shader_state);
    offset += sizeof(uint32_t);
    commands[offset++] = V3D_PACKET_COMPRESSED;
    commands[offset++] = 129; /* absolute triangle coordinates */
    for (unsigned i = 0; i < 3; i++) {
        stl_le_p(commands + offset, triangle[i].word[4]);
        offset += sizeof(uint32_t);
    }
    if (s->shader_record[3]) {
        /* Prototype CT1 consumes these per-vertex interpolation inputs. */
        for (unsigned varying = 0; varying < s->shader_record[3]; varying++) {
            for (unsigned i = 0; i < 3; i++) {
                stl_le_p(commands + offset,
                         triangle[i].word[5 + varying]);
                offset += sizeof(uint32_t);
            }
        }
        for (unsigned i = 0; i < 3; i++) {
            stl_le_p(commands + offset,
                     triangle[i].word[5 + s->shader_record[3]]);
            offset += sizeof(uint32_t);
        }
    }
    commands[offset++] = 128; /* compressed-list escape */
    commands[offset++] = V3D_PACKET_RETURN;
    g_assert(offset == command_size);

    if (dma_memory_write(&s->dma_as, list_address + used - 1, commands,
                         command_size,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }
    s->bin_tile_list_used[tile_index] = used + command_size - 1;
    return true;
}

static float bcm2835_v3d_bits_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t bcm2835_v3d_float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool bcm2835_v3d_project_clipped_vertex(BCM2835V3DState *s,
                                               BCM2835V3DVertex *vertex)
{
    float x = bcm2835_v3d_bits_float(vertex->word[0]);
    float y = bcm2835_v3d_bits_float(vertex->word[1]);
    float w = bcm2835_v3d_bits_float(vertex->word[3]);
    float scale_x = bcm2835_v3d_bits_float(s->clipper_xy_scale[0]);
    float scale_y = bcm2835_v3d_bits_float(s->clipper_xy_scale[1]);
    float screen_x;
    float screen_y;
    long fixed_x;
    long fixed_y;

    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(scale_x) ||
        !isfinite(scale_y) || w == 0.0f || scale_x <= 0.0f ||
        scale_y == 0.0f) {
        return false;
    }
    screen_x = x / w * scale_x;
    screen_y = y / w * scale_y;
    if (!isfinite(screen_x) || !isfinite(screen_y) ||
        screen_x < INT16_MIN || screen_x > INT16_MAX ||
        screen_y < INT16_MIN || screen_y > INT16_MAX) {
        return false;
    }
    fixed_x = lrintf(screen_x);
    fixed_y = lrintf(screen_y);
    if (fixed_x < INT16_MIN || fixed_x > INT16_MAX ||
        fixed_y < INT16_MIN || fixed_y > INT16_MAX) {
        return false;
    }
    vertex->word[4] = ((uint32_t)(uint16_t)(int16_t)fixed_y << 16) |
                      (uint16_t)(int16_t)fixed_x;
    vertex->word[5 + s->shader_record[3]] =
        bcm2835_v3d_float_bits(1.0f / w);
    return true;
}

static float bcm2835_v3d_clip_distance(BCM2835V3DState *s,
                                       const BCM2835V3DVertex *vertex,
                                       unsigned plane)
{
    float x = bcm2835_v3d_bits_float(vertex->word[0]);
    float y = bcm2835_v3d_bits_float(vertex->word[1]);
    float z = bcm2835_v3d_bits_float(vertex->word[2]);
    float w = bcm2835_v3d_bits_float(vertex->word[3]);

    switch (plane) {
    case 0:
        return x + w;
    case 1:
        return w - x;
    case 2:
        return y + w;
    case 3:
        return w - y;
    case 4:
        return z - bcm2835_v3d_bits_float(s->clipper_z_min) * w;
    case 5:
        return bcm2835_v3d_bits_float(s->clipper_z_max) * w - z;
    default:
        g_assert_not_reached();
    }
}

static BCM2835V3DVertex bcm2835_v3d_lerp_vertex(
    const BCM2835V3DVertex *a, const BCM2835V3DVertex *b, float t,
    unsigned varying_count)
{
    BCM2835V3DVertex result;

    for (unsigned i = 0; i < 4; i++) {
        float av = bcm2835_v3d_bits_float(a->word[i]);
        float bv = bcm2835_v3d_bits_float(b->word[i]);

        result.word[i] = bcm2835_v3d_float_bits(av + (bv - av) * t);
    }
    result.word[4] = 0;
    for (unsigned i = 5; i < 6 + varying_count; i++) {
        float av = bcm2835_v3d_bits_float(a->word[i]);
        float bv = bcm2835_v3d_bits_float(b->word[i]);

        result.word[i] = bcm2835_v3d_float_bits(av + (bv - av) * t);
    }
    return result;
}

static bool bcm2835_v3d_clip_triangle(BCM2835V3DState *s,
                                      const BCM2835V3DVertex *triangle,
                                      BCM2835V3DVertex *polygon,
                                      unsigned *polygon_count)
{
    BCM2835V3DVertex current[V3D_MAX_CLIPPED_VERTICES];
    BCM2835V3DVertex next[V3D_MAX_CLIPPED_VERTICES];
    bool created[V3D_MAX_CLIPPED_VERTICES] = { false };
    bool next_created[V3D_MAX_CLIPPED_VERTICES];
    unsigned current_count = 3;
    float scale_x = bcm2835_v3d_bits_float(s->clipper_xy_scale[0]);
    float scale_y = bcm2835_v3d_bits_float(s->clipper_xy_scale[1]);
    float z_min = bcm2835_v3d_bits_float(s->clipper_z_min);
    float z_max = bcm2835_v3d_bits_float(s->clipper_z_max);

    if (!s->clipper_xy_scale_valid || !isfinite(scale_x) ||
        !isfinite(scale_y) || scale_x <= 0.0f || scale_y == 0.0f ||
        (s->clipper_z_clip_valid &&
         (!isfinite(z_min) || !isfinite(z_max) || z_min > z_max))) {
        return false;
    }
    memcpy(current, triangle, sizeof(BCM2835V3DVertex) * current_count);
    for (unsigned vertex = 0; vertex < current_count; vertex++) {
        for (unsigned word = 0; word < 4; word++) {
            if (!isfinite(bcm2835_v3d_bits_float(
                    current[vertex].word[word]))) {
                return false;
            }
        }
    }

    /* Project only after all planes: intermediate intersections may lie far
     * outside the remaining planes (e.g. near w = 0) and are clipped later. */
    for (unsigned plane = 0;
         plane < (s->clipper_z_clip_valid ? 6 : 4) && current_count;
         plane++) {
        unsigned next_count = 0;
        BCM2835V3DVertex previous = current[current_count - 1];
        float previous_distance = bcm2835_v3d_clip_distance(
            s, &previous, plane);
        bool previous_inside;

        if (!isfinite(previous_distance)) {
            return false;
        }
        previous_inside = previous_distance >= 0.0f;

        for (unsigned i = 0; i < current_count; i++) {
            BCM2835V3DVertex vertex = current[i];
            float distance = bcm2835_v3d_clip_distance(s, &vertex, plane);
            bool inside;

            if (!isfinite(distance)) {
                return false;
            }
            inside = distance >= 0.0f;

            if (inside != previous_inside) {
                float denominator = previous_distance - distance;
                float t;
                BCM2835V3DVertex intersection;

                if (!isfinite(denominator) || denominator == 0.0f ||
                    next_count == V3D_MAX_CLIPPED_VERTICES) {
                    return false;
                }
                t = previous_distance / denominator;
                if (!isfinite(t) || t < 0.0f || t > 1.0f) {
                    return false;
                }
                intersection = bcm2835_v3d_lerp_vertex(
                    &previous, &vertex, t, s->shader_record[3]);
                next[next_count] = intersection;
                next_created[next_count++] = true;
            }
            if (inside) {
                if (next_count == V3D_MAX_CLIPPED_VERTICES) {
                    return false;
                }
                next[next_count] = vertex;
                next_created[next_count++] = created[i];
            }
            previous = vertex;
            previous_distance = distance;
            previous_inside = inside;
        }
        memcpy(current, next, sizeof(BCM2835V3DVertex) * next_count);
        memcpy(created, next_created, sizeof(bool) * next_count);
        current_count = next_count;
    }
    for (unsigned i = 0; i < current_count; i++) {
        if (created[i] && !bcm2835_v3d_project_clipped_vertex(s,
                                                              &current[i])) {
            return false;
        }
    }
    memcpy(polygon, current, sizeof(BCM2835V3DVertex) * current_count);
    *polygon_count = current_count;
    return true;
}

static bool bcm2835_v3d_bin_screen_triangle(BCM2835V3DState *s,
                                             const BCM2835V3DVertex *triangle,
                                             uint64_t *binned_tiles,
                                             uint32_t *append_index)
{
    int32_t center_x = s->viewport_offset_valid ? s->viewport_center_x :
                         s->bin_tiles_x * V3D_TILE_SIZE * 8;
    int32_t center_y = s->viewport_offset_valid ? s->viewport_center_y :
                         s->bin_tiles_y * V3D_TILE_SIZE * 8;
    int32_t bin_width = s->bin_tiles_x * V3D_TILE_SIZE;
    int32_t bin_height = s->bin_tiles_y * V3D_TILE_SIZE;
    int32_t x[3];
    int32_t y[3];
    int32_t min_x, min_y, max_x, max_y;
    int32_t pixel_min_x, pixel_min_y, pixel_max_x, pixel_max_y;
    int64_t area;
    uint32_t screen_xy[3];

    for (unsigned i = 0; i < 3; i++) {
        screen_xy[i] = triangle[i].word[4];
        x[i] = center_x + (int16_t)screen_xy[i];
        /*
         * VC4 screen Y is viewport centre + scaled Y; Mesa negates the scale
         * for Y-flipped targets.
         */
        y[i] = center_y + (int16_t)(screen_xy[i] >> 16);
    }
    area = bcm2835_v3d_edge(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (!area) {
        return true;
    }
    min_x = MIN(x[0], MIN(x[1], x[2]));
    max_x = MAX(x[0], MAX(x[1], x[2]));
    min_y = MIN(y[0], MIN(y[1], y[2]));
    max_y = MAX(y[0], MAX(y[1], y[2]));
    pixel_min_x = MAX(0, bcm2835_v3d_floor_12_4(min_x));
    pixel_min_y = MAX(0, bcm2835_v3d_floor_12_4(min_y));
    pixel_max_x = MIN(bin_width - 1, bcm2835_v3d_floor_12_4(max_x));
    pixel_max_y = MIN(bin_height - 1, bcm2835_v3d_floor_12_4(max_y));
    if (pixel_min_x > pixel_max_x || pixel_min_y > pixel_max_y) {
        return true;
    }

    for (uint32_t tile_y = pixel_min_y / V3D_TILE_SIZE;
         tile_y <= (uint32_t)pixel_max_y / V3D_TILE_SIZE; tile_y++) {
        for (uint32_t tile_x = pixel_min_x / V3D_TILE_SIZE;
             tile_x <= (uint32_t)pixel_max_x / V3D_TILE_SIZE; tile_x++) {
            if (++*binned_tiles > V3D_MAX_RASTER_TILES) {
                return false;
            }
            if ((*append_index)++ < s->bin_draw_append_count) {
                continue;
            }
            if (!bcm2835_v3d_append_tile_triangle(s, tile_x, tile_y,
                                                  triangle)) {
                s->bin_draw_resume = s->bin_oom_waiting;
                return false;
            }
            s->bin_draw_append_count++;
        }
    }
    return true;
}

/* CT0 clips coordinate triangles, then emits the visible fan per tile. */
static bool bcm2835_v3d_bin_triangles(BCM2835V3DState *s,
                                      const BCM2835V3DVertex *vertices,
                                      unsigned count)
{
    uint64_t binned_tiles = 0;
    uint32_t append_index = 0;

    if (!s->bin_draw_resume) {
        s->bin_draw_append_count = 0;
    }

    if (!s->bin_config_valid || !s->binning_started ||
        !s->bin_tiles_x || !s->bin_tiles_y) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: invalid bin setup bin=%u/%u/%u/%u\n",
                      s->bin_config_valid,
                      s->binning_started, s->bin_tiles_x, s->bin_tiles_y);
        return false;
    }

    for (unsigned vertex = 0; vertex < count; vertex += 3) {
        if (s->shader_record[0] & V3D_SHADER_STATE_CLIP) {
            BCM2835V3DVertex polygon[V3D_MAX_CLIPPED_VERTICES];
            unsigned polygon_count;

            if (!bcm2835_v3d_clip_triangle(s, vertices + vertex, polygon,
                                           &polygon_count)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bcm2835_v3d: triangle clipping failed at %u: xy-scale=%08x/%08x clip-z=%u/%08x/%08x v0=%08x,%08x,%08x,%08x v1=%08x,%08x,%08x,%08x v2=%08x,%08x,%08x,%08x\n",
                              vertex, s->clipper_xy_scale[0],
                              s->clipper_xy_scale[1],
                              s->clipper_z_clip_valid, s->clipper_z_min,
                              s->clipper_z_max,
                              vertices[vertex].word[0],
                              vertices[vertex].word[1],
                              vertices[vertex].word[2],
                              vertices[vertex].word[3],
                              vertices[vertex + 1].word[0],
                              vertices[vertex + 1].word[1],
                              vertices[vertex + 1].word[2],
                              vertices[vertex + 1].word[3],
                              vertices[vertex + 2].word[0],
                              vertices[vertex + 2].word[1],
                              vertices[vertex + 2].word[2],
                              vertices[vertex + 2].word[3]);
                return false;
            }
            for (unsigned i = 1; i + 1 < polygon_count; i++) {
                BCM2835V3DVertex triangle[3] = {
                    polygon[0], polygon[i], polygon[i + 1],
                };

                if (!bcm2835_v3d_bin_screen_triangle(s, triangle,
                                                     &binned_tiles,
                                                     &append_index)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "bcm2835_v3d: clipped triangle bin failed at vertex %u\n",
                                  vertex);
                    return false;
                }
            }
        } else if (!bcm2835_v3d_bin_screen_triangle(s, vertices + vertex,
                                                    &binned_tiles,
                                                    &append_index)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: triangle bin failed at vertex %u\n",
                          vertex);
            return false;
        }
    }
    s->bin_draw_append_count = 0;
    s->bin_draw_resume = false;
    return true;
}

static bool bcm2835_v3d_rasterize_tile_triangle(BCM2835V3DState *s,
                                                 const uint32_t *screen_xy,
                                                 const uint32_t *varying,
                                                 const uint32_t *reciprocal_w,
                                                 uint64_t *work,
                                                 BCM2835V3DWGPUFragmentBatch *batch)
{
    int32_t center_x = s->viewport_offset_valid ? s->viewport_center_x :
                                                 s->render_width * 8;
    int32_t center_y = s->viewport_offset_valid ? s->viewport_center_y :
                                                 s->render_height * 8;
    int32_t x[3];
    int32_t y[3];
    int32_t x0 = (int32_t)s->tile_x * V3D_TILE_SIZE;
    int32_t y0 = (int32_t)s->tile_y * V3D_TILE_SIZE;
    int32_t x1 = MIN(x0 + V3D_TILE_SIZE, s->render_width);
    int32_t y1 = MIN(y0 + V3D_TILE_SIZE, s->render_height);
    int32_t min_x, min_y, max_x, max_y;
    int64_t area;
    int64_t edge_row[3], edge_x_step[3], edge_y_step[3];
    int64_t normalized_area, edge_sign;
    bool edge_top_left[3];
    uint32_t color = s->clear_color_valid ? s->clear_color[0] : UINT32_MAX;
    uint16_t covered[64 * 64];
    uint32_t *covered_partial = NULL;
    uint32_t covered_w[64 * 64];
    float vertex_varying[V3D_MAX_FRAGMENT_VARYINGS][3] = { 0 };
    float vertex_reciprocal_w[3] = { 1.0f, 1.0f, 1.0f };
    unsigned covered_count = 0;
    int64_t raster_start_us;

    if (s->shader_record[3]) {
        if (!varying || !reciprocal_w) {
            return false;
        }
        covered_partial = g_new(uint32_t,
                                64 * 64 * s->shader_record[3]);
        for (unsigned index = 0; index < s->shader_record[3]; index++) {
            for (unsigned i = 0; i < 3; i++) {
                vertex_varying[index][i] = bcm2835_v3d_bits_float(
                    varying[index * 3 + i]);
                if (!isfinite(vertex_varying[index][i])) {
                    g_free(covered_partial);
                    return false;
                }
            }
        }
        for (unsigned i = 0; i < 3; i++) {
            vertex_reciprocal_w[i] =
                bcm2835_v3d_bits_float(reciprocal_w[i]);
            if (!isfinite(vertex_reciprocal_w[i]) ||
                vertex_reciprocal_w[i] == 0.0f) {
                g_free(covered_partial);
                return false;
            }
        }
    }

    if (!s->tile_color_valid) {
        for (unsigned i = 0; i < G_N_ELEMENTS(s->tile_color); i++) {
            s->tile_color[i] = color;
        }
        s->tile_color_valid = true;
    }
    for (unsigned i = 0; i < 3; i++) {
        uint32_t packed = screen_xy[i];

        x[i] = center_x + (int16_t)packed;
        /*
         * VC4 screen Y is viewport centre + scaled Y; Mesa negates the scale
         * for Y-flipped targets.
         */
        y[i] = center_y + (int16_t)(packed >> 16);
    }
    area = bcm2835_v3d_edge(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (!area) {
        g_free(covered_partial);
        return true;
    }
    min_x = MAX(x0, MAX(0, bcm2835_v3d_floor_12_4(
                              MIN(x[0], MIN(x[1], x[2])))));
    min_y = MAX(y0, MAX(0, bcm2835_v3d_floor_12_4(
                              MIN(y[0], MIN(y[1], y[2])))));
    max_x = MIN(x1 - 1, bcm2835_v3d_floor_12_4(
                             MAX(x[0], MAX(x[1], x[2]))));
    max_y = MIN(y1 - 1, bcm2835_v3d_floor_12_4(
                             MAX(y[0], MAX(y[1], y[2]))));
    if (min_x > max_x || min_y > max_y) {
        g_free(covered_partial);
        return true;
    }
    *work += (uint64_t)(max_x - min_x + 1) * (max_y - min_y + 1);
    if (*work > V3D_MAX_RASTER_WORK) {
        g_free(covered_partial);
        return false;
    }

    edge_sign = area < 0 ? -1 : 1;
    normalized_area = area * edge_sign;
    int32_t first_px_fixed = min_x * 16 + 8;
    int32_t first_py_fixed = min_y * 16 + 8;
    for (unsigned i = 0; i < 3; i++) {
        unsigned next = (i + 1) % 3;
        int32_t dx = x[next] - x[i];
        int32_t dy = y[next] - y[i];
        int32_t oriented_dx = dx * edge_sign;
        int32_t oriented_dy = dy * edge_sign;

        edge_row[i] = bcm2835_v3d_edge(x[i], y[i], x[next], y[next],
                                       first_px_fixed, first_py_fixed) *
                      edge_sign;
        edge_x_step[i] = 16LL * oriented_dy;
        edge_y_step[i] = -16LL * oriented_dx;
        edge_top_left[i] = oriented_dy > 0 ||
                           (oriented_dy == 0 && oriented_dx < 0);
    }

    raster_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    for (int32_t py = min_y; py <= max_y; py++) {
        int64_t span_start = 0;
        int64_t span_end = max_x - min_x;
        bool row_covered = true;

        for (unsigned i = 0; i < 3; i++) {
            int64_t threshold = edge_top_left[i] ? 0 : 1;

            if (edge_x_step[i] > 0) {
                span_start = MAX(span_start,
                    bcm2835_v3d_ceil_div(threshold - edge_row[i],
                                        edge_x_step[i]));
            } else if (edge_x_step[i] < 0) {
                span_end = MIN(span_end,
                    bcm2835_v3d_floor_div(edge_row[i] - threshold,
                                          -edge_x_step[i]));
            } else if (edge_row[i] < threshold) {
                row_covered = false;
                break;
            }
        }

        if (row_covered && span_start <= span_end) {
            int32_t first_px = min_x + (int32_t)span_start;
            int32_t last_px = min_x + (int32_t)span_end;
            int64_t e0 = edge_row[0] + span_start * edge_x_step[0];
            int64_t e1 = edge_row[1] + span_start * edge_x_step[1];
            int64_t e2 = edge_row[2] + span_start * edge_x_step[2];

            for (int32_t px = first_px; px <= last_px; px++) {
                unsigned tile_pixel = (py - y0) * V3D_TILE_SIZE + px - x0;
                float fragment_w = 1.0f;

                if (covered_count >= G_N_ELEMENTS(covered)) {
                    g_free(covered_partial);
                    return false;
                }
                if (s->shader_record[3]) {
                    double b0 = (double)e1 / normalized_area;
                    double b1 = (double)e2 / normalized_area;
                    double b2 = (double)e0 / normalized_area;
                    double inv_w = b0 * vertex_reciprocal_w[0] +
                                   b1 * vertex_reciprocal_w[1] +
                                   b2 * vertex_reciprocal_w[2];
                    double value;

                    if (!isfinite(inv_w) || inv_w == 0.0) {
                        g_free(covered_partial);
                        return false;
                    }
                    fragment_w = 1.0 / inv_w;
                    if (!isfinite(fragment_w)) {
                        g_free(covered_partial);
                        return false;
                    }
                    for (unsigned index = 0;
                         index < s->shader_record[3]; index++) {
                        value = b0 * vertex_varying[index][0] *
                                    vertex_reciprocal_w[0] +
                                b1 * vertex_varying[index][1] *
                                    vertex_reciprocal_w[1] +
                                b2 * vertex_varying[index][2] *
                                    vertex_reciprocal_w[2];
                        if (!isfinite(value)) {
                            g_free(covered_partial);
                            return false;
                        }
                        covered_partial[covered_count *
                                        s->shader_record[3] + index] =
                            bcm2835_v3d_float_bits(value);
                    }
                }
                covered[covered_count] = tile_pixel;
                covered_w[covered_count] = bcm2835_v3d_float_bits(fragment_w);
                covered_count++;
                e0 += edge_x_step[0];
                e1 += edge_x_step[1];
                e2 += edge_x_step[2];
            }
        }
        edge_row[0] += edge_y_step[0];
        edge_row[1] += edge_y_step[1];
        edge_row[2] += edge_y_step[2];
    }
    trace_bcm2835_v3d_rasterize_triangle(
        (max_x - min_x + 1) * (max_y - min_y + 1), covered_count,
        qemu_clock_get_us(QEMU_CLOCK_REALTIME) - raster_start_us);
    if (covered_count && ldl_le_p(s->shader_record + 4)) {
        bool success = bcm2835_v3d_run_fragment_shader(
            s, covered, covered_partial, covered_w, covered_count, batch);
        g_free(covered_partial);
        return success;
    }
#ifdef CONFIG_V3D_WGPU
    if (batch->pixels && batch->pixels->len &&
        !bcm2835_v3d_wgpu_batch_flush(s, batch)) {
        g_free(covered_partial);
        return false;
    }
#endif
    for (unsigned i = 0; i < covered_count; i++) {
        s->tile_color[covered[i]] = color;
    }
    g_free(covered_partial);
    return true;
}

static int32_t bcm2835_v3d_sign_extend(uint32_t value, unsigned bits)
{
    uint32_t sign = 1U << (bits - 1);

    return value & sign ? (int32_t)value - (1 << bits) : (int32_t)value;
}

static bool bcm2835_v3d_add_xy_delta(uint32_t xy, int32_t dx, int32_t dy,
                                      uint32_t *result)
{
    int32_t x = (int16_t)xy + dx;
    int32_t y = (int16_t)(xy >> 16) + dy;

    if (x < INT16_MIN || x > INT16_MAX ||
        y < INT16_MIN || y > INT16_MAX) {
        return false;
    }
    *result = (uint16_t)x | ((uint32_t)(uint16_t)y << 16);
    return true;
}

static bool bcm2835_v3d_execute_compressed_xy_body(
    BCM2835V3DState *s, uint32_t *ca,
    BCM2835V3DWGPUFragmentBatch *batch)
{
    uint32_t triangles = 0;
    uint64_t work = 0;
    uint32_t previous[3];
    bool previous_valid = false;

    if (*ca == UINT32_MAX || !s->tile_primitive_format_valid ||
        s->tile_primitive_format != V3D_PRIMITIVE_LIST_32_XY_TRIANGLES ||
        !s->shader_state_valid ||
        s->shader_record[3] > V3D_MAX_FRAGMENT_VARYINGS) {
        return false;
    }

    *ca += 1; /* compressed-list packet id */
    while (triangles < V3D_MAX_RASTER_WORK / (V3D_TILE_SIZE *
                                              V3D_TILE_SIZE)) {
        uint8_t code;
        uint8_t data[13];
        uint8_t varying_bytes[sizeof(uint32_t) *
                              3 * (V3D_MAX_FRAGMENT_VARYINGS + 1)];
        uint32_t coords[3];
        uint32_t varying[3 * V3D_MAX_FRAGMENT_VARYINGS];
        uint32_t reciprocal_w[3];
        size_t varying_size = s->shader_record[3] ?
            3 * (s->shader_record[3] + 1) * sizeof(uint32_t) : 0;
        size_t encoded_size;
        size_t triangle_size;

        if (!bcm2835_v3d_dma_read(s, *ca, &code, sizeof(code))) {
            return false;
        }
        if (code == 128) {
            *ca += 1;
            return true;
        }
        if (code == 130) {
            return false;
        }

        if (code == 129) {
            encoded_size = 13;
        } else if (code == 15) {
            encoded_size = 8;
        } else if ((code & 3) == 3) {
            encoded_size = 3;
        } else {
            encoded_size = 2;
        }

        triangle_size = encoded_size + (code == 129 ? varying_size : 0);
        if ((uint64_t)*ca + triangle_size > UINT32_MAX + 1ULL ||
            !bcm2835_v3d_dma_read(s, *ca, data, encoded_size)) {
            return false;
        }

        if (code == 129) {
            for (unsigned i = 0; i < G_N_ELEMENTS(coords); i++) {
                coords[i] = ldl_le_p(data + 1 + i * sizeof(uint32_t));
            }
        } else {
            uint32_t dx;
            uint32_t dy;
            unsigned orientation;

            if (varying_size) {
                return false;
            }
            if (code == 15) {
                uint64_t encoded = ldq_le_p(data);

                coords[0] = lduw_le_p(data + 4) |
                            ((uint32_t)lduw_le_p(data + 6) << 16);
                dx = (encoded >> 4) & 0x7f;
                dy = (encoded >> 11) & 0x7f;
                uint32_t dx2 = (encoded >> 18) & 0x7f;
                uint32_t dy2 = (encoded >> 25) & 0x7f;
                if (!bcm2835_v3d_add_xy_delta(coords[0],
                        bcm2835_v3d_sign_extend(dx, 7),
                        bcm2835_v3d_sign_extend(dy, 7), &coords[1]) ||
                    !bcm2835_v3d_add_xy_delta(coords[0],
                        bcm2835_v3d_sign_extend(dx2, 7),
                        bcm2835_v3d_sign_extend(dy2, 7), &coords[2])) {
                    return false;
                }
            } else {
                bool wide = (code & 3) == 3;
                uint32_t encoded = wide ?
                    data[0] | ((uint32_t)data[1] << 8) |
                    ((uint32_t)data[2] << 16) : lduw_le_p(data);
                unsigned x_bits = wide ? 10 : 7;
                unsigned x_shift = wide ? 4 : 2;
                unsigned y_shift = wide ? 14 : 9;

                orientation = wide ? (encoded >> 2) & 3 : encoded & 3;
                if (!previous_valid || orientation > 2) {
                    return false;
                }
                dx = (encoded >> x_shift) & ((1U << x_bits) - 1);
                dy = (encoded >> y_shift) & ((1U << (wide ? 10 :
                                                                  7)) - 1);
                if (!wide && bcm2835_v3d_sign_extend(dx, 7) < -62) {
                    return false;
                }
                switch (orientation) {
                case 0:
                    coords[0] = previous[2];
                    coords[1] = previous[1];
                    break;
                case 1:
                    coords[0] = previous[0];
                    coords[1] = previous[2];
                    break;
                default:
                    coords[0] = previous[1];
                    coords[1] = previous[0];
                    break;
                }
                if (!bcm2835_v3d_add_xy_delta(previous[2],
                        bcm2835_v3d_sign_extend(dx, x_bits),
                        bcm2835_v3d_sign_extend(dy, wide ? 10 : 7),
                        &coords[2])) {
                    return false;
                }
            }
        }

        if (varying_size) {
            if (!bcm2835_v3d_dma_read(s, *ca + encoded_size, varying_bytes,
                                      varying_size)) {
                return false;
            }
            for (unsigned index = 0; index < s->shader_record[3]; index++) {
                for (unsigned i = 0; i < 3; i++) {
                    varying[index * 3 + i] = ldl_le_p(varying_bytes +
                        (index * 3 + i) * sizeof(uint32_t));
                }
            }
            for (unsigned i = 0; i < 3; i++) {
                reciprocal_w[i] = ldl_le_p(varying_bytes +
                    (s->shader_record[3] * 3 + i) * sizeof(uint32_t));
            }
        }
        if (!bcm2835_v3d_rasterize_tile_triangle(
                s, coords, varying_size ? varying : NULL,
                varying_size ? reciprocal_w : NULL, &work, batch)) {
            return false;
        }
        memcpy(previous, coords, sizeof(previous));
        previous_valid = true;
        *ca += triangle_size;
        triangles++;
    }
    return false;
}

static bool bcm2835_v3d_execute_compressed_xy(BCM2835V3DState *s,
                                               uint32_t *ca)
{
    BCM2835V3DWGPUFragmentBatch local_batch = { 0 };
    BCM2835V3DWGPUFragmentBatch *batch = &local_batch;
    bool persistent_batch = false;
    bool success;

#ifdef CONFIG_V3D_WGPU
    if (s->wgpu_context) {
        if (!s->wgpu_batch) {
            s->wgpu_batch = g_new0(BCM2835V3DWGPUFragmentBatch, 1);
        }
        batch = s->wgpu_batch;
        persistent_batch = true;
    }
#endif
    success = bcm2835_v3d_execute_compressed_xy_body(s, ca, batch);
    if (!persistent_batch) {
        bcm2835_v3d_wgpu_batch_clear(batch);
    }
    return success;
}

static bool bcm2835_v3d_execute_coordinate_shader(BCM2835V3DState *s,
                                                   const uint8_t *packet,
                                                   uint8_t opcode)
{
    uint32_t count = ldl_le_p(packet + 2);
    unsigned mode = packet[1] & 0xf;
    uint32_t *indices;
    BCM2835V3DVertex *vertices;
    BCM2835V3DVertex *fan_vertices = NULL;
    uint32_t (*vertex_vpm)[BCM2835_V3D_QPU_VPM_ROWS]
                          [BCM2835_V3D_QPU_LANES] = NULL;
    bool success;

    if (!count) {
        return true;
    }
    if (count > V3D_MAX_SHADER_VERTICES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: coordinate shader vertex count %u exceeds limit\n",
                      count);
        return false;
    }
    if (mode == V3D_PRIMITIVE_TRIANGLE_FAN && count < 3) {
        return true;
    }
    if ((mode != V3D_PRIMITIVE_TRIANGLES &&
         mode != V3D_PRIMITIVE_TRIANGLE_FAN) ||
        (mode == V3D_PRIMITIVE_TRIANGLES && count % 3)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: unsupported primitive mode 0x%x for %u vertices\n",
                      mode, count);
        return false;
    }
    if (s->shader_record[3] > V3D_MAX_FRAGMENT_VARYINGS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: shader has unsupported %u varyings\n",
                      s->shader_record[3]);
        return false;
    }
    indices = g_new(uint32_t, count);
    vertices = g_new0(BCM2835V3DVertex, count);
    if (opcode == V3D_PACKET_ARRAY_PRIM) {
        uint32_t first = ldl_le_p(packet + 6);

        for (uint32_t i = 0; i < count; i++) {
            indices[i] = first + i;
        }
    } else {
        unsigned index_size = (packet[1] >> 4) + 1;
        uint32_t address = ldl_le_p(packet + 6);
        uint8_t indices_bytes[512];

        for (uint32_t first = 0; first < count;) {
            uint32_t batch = MIN(count - first,
                                 sizeof(indices_bytes) / index_size);
            size_t bytes = batch * index_size;

            if (!bcm2835_v3d_dma_read(s, address + first * index_size,
                                      indices_bytes, bytes)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "bcm2835_v3d: index read failed at 0x%08x (%zu bytes)\n",
                              address + first * index_size, bytes);
                g_free(indices);
                g_free(vertices);
                return false;
            }
            for (uint32_t i = 0; i < batch; i++) {
                indices[first + i] = index_size == 1 ? indices_bytes[i] :
                    lduw_le_p(indices_bytes + i * index_size);
            }
            first += batch;
        }
    }

    /* GL vertices are shaded before the coordinate transform consumes VPM. */
    if (ldl_le_p(s->shader_record + 16)) {
        vertex_vpm = g_malloc0_n(DIV_ROUND_UP(count,
                                               BCM2835_V3D_QPU_LANES),
                                 sizeof(*vertex_vpm));
        success = bcm2835_v3d_run_qpu_stage(
            s, indices, count, 16, 20, 14, 6, NULL, vertex_vpm, NULL);
    } else {
        success = true;
    }
    if (success) {
        success = bcm2835_v3d_run_qpu_stage(
            s, indices, count, 28, 32, 26, 7,
            vertex_vpm, NULL, vertices);
    }
    g_free(indices);
    g_free(vertex_vpm);
    if (success) {
        if (mode == V3D_PRIMITIVE_TRIANGLE_FAN) {
            unsigned triangle_vertices = 3 * (count - 2);

            fan_vertices = g_new(BCM2835V3DVertex, triangle_vertices);
            for (unsigned triangle = 0; triangle < count - 2; triangle++) {
                fan_vertices[triangle * 3] = vertices[0];
                fan_vertices[triangle * 3 + 1] = vertices[triangle + 1];
                fan_vertices[triangle * 3 + 2] = vertices[triangle + 2];
            }
            success = bcm2835_v3d_bin_triangles(s, fan_vertices,
                                                triangle_vertices);
        } else {
            success = bcm2835_v3d_bin_triangles(s, vertices, count);
        }
        if (!success) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: triangle binning rejected render %ux%u config 0x%08x tiles %ux%u\n",
                          s->render_width, s->render_height,
                          s->render_config, s->bin_tiles_x, s->bin_tiles_y);
        }
    }
    g_free(fan_vertices);
    g_free(vertices);
    return success;
}

static void bcm2835_v3d_run_thread_body(BCM2835V3DState *s, unsigned thread,
                                        bool yield_after_tile)
{
    uint32_t *ca = &s->ct_ca[thread];
    uint32_t ea = s->ct_ea[thread];
#ifdef CONFIG_V3D_WGPU
    unsigned deferred_stores_this_slice = 0;
#endif

    for (uint32_t commands = 0; *ca != ea; commands++) {
        uint8_t packet[16];
        uint8_t opcode;
        unsigned packet_size;
        uint32_t next;
        uint32_t packet_address = *ca;

        if (commands >= V3D_MAX_CL_COMMANDS) {
            bcm2835_v3d_thread_error(s, thread, *ca,
                                     "control list command limit exceeded");
            return;
        }
        if (!bcm2835_v3d_dma_read(s, *ca, &opcode, sizeof(opcode))) {
            bcm2835_v3d_thread_error(s, thread, *ca,
                                     "control list DMA read failed");
            return;
        }
        if (opcode == V3D_PACKET_COMPRESSED) {
            if (thread != 1 ||
                !bcm2835_v3d_execute_compressed_xy(s, ca)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported compressed tile primitive");
                return;
            }
            continue;
        }
        packet_size = bcm2835_v3d_packet_size(opcode);
        if (!packet_size || packet_size > sizeof(packet) ||
            (uint64_t)*ca + packet_size > UINT32_MAX + 1ULL ||
            !bcm2835_v3d_dma_read(s, *ca, packet, packet_size)) {
            bcm2835_v3d_thread_error(s, thread, *ca,
                                     "unsupported or invalid control packet");
            return;
        }
        next = *ca + packet_size;
        *ca = next;

        switch (opcode) {
        case V3D_PACKET_HALT:
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
            s->ct_cs[thread] &= ~V3D_CS_CTRUN;
            s->ct_cs[thread] |= V3D_CS_CTSUBS;
            s->ct_waiting[thread] = false;
            return;
        case V3D_PACKET_START_BIN:
            if (thread == 0) {
                if (!s->bin_config_valid) {
                    bcm2835_v3d_thread_error(s, thread, *ca,
                                             "tile binning started without configuration");
                    return;
                }
                if (!bcm2835_v3d_init_tile_lists(s)) {
                    bcm2835_v3d_thread_error(s, thread, *ca,
                                             "tile list memory initialization failed");
                    return;
                }
                s->binning_started = true;
                s->shader_state_valid = false;
                s->shader_record_size = 0;
                s->shader_attribute_count = 0;
                s->shader_record_extended = false;
                memset(s->shader_record, 0, sizeof(s->shader_record));
                s->ct_cs[thread] &= ~V3D_CS_CTMODE;
            }
            break;
        case V3D_PACKET_INC_SEMA: {
            unsigned target = thread ^ 1;

            if (s->ct_semaphore[target] < 7) {
                s->ct_semaphore[target]++;
                bcm2835_v3d_update_semaphore(s, target);
            }
            if (s->ct_waiting[target]) {
                s->ct_waiting[target] = false;
                s->ct_cs[target] &= ~V3D_CS_CTSUBS;
                bcm2835_v3d_start_thread(s, target);
            }
            break;
        }
        case V3D_PACKET_WAIT_SEMA:
            if (!s->ct_semaphore[thread]) {
                if (!bcm2835_v3d_wgpu_batch_flush_before(
                        s, thread, packet_address)) {
                    return;
                }
                s->ct_cs[thread] |= V3D_CS_CTSUBS;
                s->ct_waiting[thread] = true;
                return;
            }
            s->ct_semaphore[thread]--;
            bcm2835_v3d_update_semaphore(s, thread);
            break;
        case V3D_PACKET_BRANCH:
            *ca = ldl_le_p(packet + 1);
            break;
        case V3D_PACKET_BRANCH_SUB:
            if (s->ct_return_depth[thread] >= 2) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "sub-list stack overflow");
                return;
            }
            s->ct_return_addr[thread * 2 + s->ct_return_depth[thread]++] = next;
            s->ct_ra0[thread] = next;
            *ca = ldl_le_p(packet + 1);
            break;
        case V3D_PACKET_RETURN:
            if (s->ct_return_depth[thread]) {
                s->ct_return_depth[thread]--;
                *ca = s->ct_return_addr[thread * 2 +
                                         s->ct_return_depth[thread]];
                s->ct_ra0[thread] = s->ct_return_depth[thread] ?
                    s->ct_return_addr[thread * 2 +
                                      s->ct_return_depth[thread] - 1] : 0;
            }
            break;
        case V3D_PACKET_BIN_CONFIG: {
            uint32_t alloc_base = ldl_le_p(packet + 1);
            uint32_t alloc_size = ldl_le_p(packet + 5);
            uint32_t state_base = ldl_le_p(packet + 9);
            uint8_t tiles_x = packet[13];
            uint8_t tiles_y = packet[14];
            uint8_t flags = packet[15];
            uint16_t initial_block_size =
                32U << ((flags >> 3) & 3);
            uint16_t block_size = 32U << ((flags >> 5) & 3);
            uint64_t alloc_end = (uint64_t)alloc_base + alloc_size;
            uint64_t state_size = (uint64_t)tiles_x * tiles_y * 48;
            uint64_t state_end = (uint64_t)state_base + state_size;

            if (thread != 0 || !tiles_x || !tiles_y || !alloc_size ||
                (alloc_base & 15) || (state_base & 15) ||
                (flags & V3D_BIN_FLAG_UNSUPPORTED) ||
                alloc_end > UINT32_MAX + 1ULL ||
                state_end > UINT32_MAX + 1ULL) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "invalid tile binning configuration");
                return;
            }

            s->bin_alloc_base = alloc_base;
            s->bin_alloc_size = alloc_size;
            s->bin_state_base = state_base;
            s->bin_tiles_x = tiles_x;
            s->bin_tiles_y = tiles_y;
            s->bin_flags = flags;
            s->bin_alloc_next = 0;
            s->bin_overflow_next = 0;
            s->bin_overflow_remaining = 0;
            s->bin_alloc_block_size = block_size;
            s->bin_alloc_initial_block_size = initial_block_size;
            memset(s->bin_tile_list_addr, 0,
                   sizeof(s->bin_tile_list_addr));
            memset(s->bin_tile_list_used, 0,
                   sizeof(s->bin_tile_list_used));
            s->bin_list_stride = 0;
            s->bin_list_indirect = false;
            s->bin_config_valid = true;
            s->binning_started = false;
            s->shader_state_valid = false;
            s->tile_primitive_format_valid = false;
            break;
        }
        case V3D_PACKET_GL_SHADER_STATE:
            if ((thread == 0 && !s->binning_started) || thread > 1) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "shader state outside tile binning");
                return;
            }
            if (!bcm2835_v3d_load_shader_state(s, ldl_le_p(packet + 1))) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "shader state DMA read failed");
                return;
            }
            break;
        case V3D_PACKET_Z_CLIP:
            if (thread != 0) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "Z clipping state outside binning");
                return;
            }
            s->clipper_z_min = ldl_le_p(packet + 1);
            s->clipper_z_max = ldl_le_p(packet + 5);
            s->clipper_z_clip_valid = true;
            break;
        case V3D_PACKET_VIEWPORT_OFFSET:
            s->viewport_center_x = (int16_t)lduw_le_p(packet + 1);
            s->viewport_center_y = (int16_t)lduw_le_p(packet + 3);
            s->viewport_offset_valid = true;
            break;
        case V3D_PACKET_CLIPPER_XY_SCALE:
            if (thread != 0) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "XY clipper scale outside binning");
                return;
            }
            s->clipper_xy_scale[0] = ldl_le_p(packet + 1);
            s->clipper_xy_scale[1] = ldl_le_p(packet + 5);
            s->clipper_xy_scale_valid = true;
            break;
        case V3D_PACKET_PRIMITIVE_FORMAT:
            if ((thread == 0 &&
                 packet[1] != V3D_PRIMITIVE_LIST_16_INDEX_TRIANGLES &&
                 packet[1] != V3D_PRIMITIVE_LIST_32_XY_TRIANGLES) ||
                (thread == 1 &&
                 packet[1] != V3D_PRIMITIVE_LIST_32_XY_TRIANGLES)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported primitive list format");
                return;
            }
            s->tile_primitive_format = packet[1];
            s->tile_primitive_format_valid = true;
            break;
        case V3D_PACKET_FLUSH:
        case V3D_PACKET_FLUSH_ALL:
            if (thread == 0) {
                s->regs[V3D_BFC >> 2]++;
                bcm2835_v3d_raise_irq(s, V3D_INT_FLDONE);
            }
            break;
        case V3D_PACKET_RENDER_CONFIG:
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
            s->render_base = ldl_le_p(packet + 1);
            s->render_width = lduw_le_p(packet + 5);
            s->render_height = lduw_le_p(packet + 7);
            s->render_config = lduw_le_p(packet + 9);
            s->render_config_valid = true;
            s->tile_color_valid = false;
            break;
        case V3D_PACKET_CLEAR_COLORS:
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
            s->clear_color[0] = ldl_le_p(packet + 1);
            s->clear_color[1] = ldl_le_p(packet + 5);
            s->clear_color_valid = true;
            break;
        case V3D_PACKET_TILE_COORDS:
#ifdef CONFIG_V3D_WGPU
            if ((thread != 1 ||
                 !bcm2835_v3d_wgpu_batch_defer_across_tile(
                     s, packet[1], packet[2])) &&
                !bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
#else
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
#endif
            s->tile_x = packet[1];
            s->tile_y = packet[2];
            s->tile_color_valid = false;
            if (s->pending_tile_load) {
                if (!bcm2835_v3d_copy_tile(
                        s, s->pending_tile_load_base,
                        s->pending_tile_load_memory_format,
                        s->pending_tile_load_color_format, false)) {
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "bcm2835_v3d: CT%u tile load base=0x%08x "
                                  "format=%u color=%u tile=(%u,%u) "
                                  "render=%ux%u\n",
                                  thread, s->pending_tile_load_base,
                                  s->pending_tile_load_memory_format,
                                  s->pending_tile_load_color_format,
                                  s->tile_x, s->tile_y,
                                  s->render_width, s->render_height);
                    bcm2835_v3d_thread_error(s, thread, *ca,
                                             "tile buffer load DMA failed");
                    return;
                }
                s->tile_color_valid = true;
                s->pending_tile_load = false;
            }
            s->tile_primitive_format_valid = false;
            break;
        case V3D_PACKET_STORE_TILE:
#ifdef CONFIG_V3D_WGPU
            if (thread == 1 &&
                bcm2835_v3d_wgpu_batch_defer_store(s, packet)) {
                BCM2835V3DWGPUFragmentBatch *batch = s->wgpu_batch;

                deferred_stores_this_slice++;
                if (yield_after_tile &&
                    batch->deferred_stores >=
                    BCM2835_V3D_WGPU_MAX_BATCH_TILES) {
                    if (!bcm2835_v3d_wgpu_batch_flush_before(
                            s, thread, packet_address)) {
                        return;
                    }
                    return;
                }
                if (yield_after_tile &&
                    deferred_stores_this_slice >=
                    BCM2835_V3D_WGPU_TILES_PER_SLICE) {
                    /* Keep collecting across BH slices; submit every 64 tiles. */
                    return;
                }
                break;
            }
#endif
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
            if (thread != 1 ||
                !bcm2835_v3d_tile_buffer_packet(
                    s, packet, true)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported tile buffer operation or DMA failure");
                return;
            }
            if (yield_after_tile && opcode == V3D_PACKET_STORE_TILE) {
                return;
            }
            break;
        case V3D_PACKET_LOAD_TILE:
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
            if (thread != 1 ||
                !bcm2835_v3d_tile_buffer_packet(s, packet, false)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported tile buffer operation or DMA failure");
                return;
            }
            break;
        case V3D_PACKET_STORE_MS:
        case V3D_PACKET_STORE_MS_EOF:
            if (!bcm2835_v3d_wgpu_batch_flush_before(
                    s, thread, packet_address)) {
                return;
            }
            if (!bcm2835_v3d_clear_tile(s)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported render target or DMA write failed");
                return;
            }
            if (opcode == V3D_PACKET_STORE_MS_EOF) {
                s->regs[V3D_RFC >> 2]++;
                bcm2835_v3d_raise_irq(s, V3D_INT_FRDONE);
            }
            break;
        case V3D_PACKET_INDEX_PRIM:
        case V3D_PACKET_ARRAY_PRIM:
            if (thread != 0 || !s->binning_started ||
                !bcm2835_v3d_validate_draw(s, packet, opcode)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "invalid primitive or vertex input");
                return;
            }
            if (!bcm2835_v3d_execute_coordinate_shader(s, packet, opcode)) {
                if (s->bin_oom_waiting) {
                    *ca = packet_address;
                    s->ct_cs[thread] |= V3D_CS_CTRUN | V3D_CS_CTSUBS;
                    return;
                }
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported QPU shader, triangle mode, or render target");
                return;
            }
            break;
        case V3D_PACKET_COMPRESSED:
        case V3D_PACKET_CLIPPED:
            bcm2835_v3d_thread_error(s, thread, *ca,
                                     "unsupported clipped primitive packet");
            return;
        default:
            break;
        }
    }

    if (!bcm2835_v3d_wgpu_batch_flush_before(s, thread, *ca)) {
        return;
    }
    s->ct_cs[thread] &= ~(V3D_CS_CTRUN | V3D_CS_CTSUBS);
    s->ct_waiting[thread] = false;
}

static void bcm2835_v3d_run_thread(BCM2835V3DState *s, unsigned thread)
{
    int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    trace_bcm2835_v3d_thread_start(thread, s->ct_ca[thread],
                                   s->ct_ea[thread]);
    bcm2835_v3d_run_thread_body(s, thread, false);
    trace_bcm2835_v3d_thread_end(thread, s->ct_ca[thread], s->ct_cs[thread],
                                 qemu_clock_get_us(QEMU_CLOCK_REALTIME) -
                                 start_us);
}

static void bcm2835_v3d_start_thread(BCM2835V3DState *s, unsigned thread)
{
    if (thread != 1) {
        bcm2835_v3d_run_thread(s, thread);
        return;
    }

    if (!s->ct1_start_us) {
        s->ct1_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        trace_bcm2835_v3d_thread_start(thread, s->ct_ca[thread],
                                      s->ct_ea[thread]);
    }
    qemu_bh_schedule(s->ct1_bh);
}

static void bcm2835_v3d_ct1_bh(void *opaque)
{
    BCM2835V3DState *s = opaque;
    int64_t slice_start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    int64_t now_us;

    bcm2835_v3d_run_thread_body(s, 1, true);
    now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    trace_bcm2835_v3d_thread_slice(1, now_us - slice_start_us);

    if (s->ct_waiting[1] || (s->ct_cs[1] & V3D_CS_CTRUN)) {
        if (s->ct_waiting[1]) {
            trace_bcm2835_v3d_thread_end(1, s->ct_ca[1], s->ct_cs[1],
                                         now_us - s->ct1_start_us);
            s->ct1_start_us = 0;
        } else {
            /* shortcut: ordinary tile stores yield promptly; split dense tiles if needed. */
            qemu_bh_schedule(s->ct1_bh);
        }
        return;
    }

    trace_bcm2835_v3d_thread_end(1, s->ct_ca[1], s->ct_cs[1],
                                 now_us - s->ct1_start_us);
    s->ct1_start_us = 0;
}

static void bcm2835_v3d_resume_binner_if_ready(BCM2835V3DState *s)
{
    uint32_t base = s->regs[V3D_BPOA >> 2];
    uint32_t size = s->regs[V3D_BPOS >> 2];

    if (!s->bin_oom_waiting ||
        !bcm2835_v3d_overflow_range_valid(s, base, size)) {
        return;
    }

    s->bin_oom_waiting = false;
    s->ct_cs[0] &= ~V3D_CS_CTSUBS;
    if (s->ct_ca[0] != s->ct_ea[0]) {
        s->ct_cs[0] |= V3D_CS_CTRUN;
        bcm2835_v3d_run_thread(s, 0);
    }
}

static uint32_t bcm2835_v3d_read_thread_reg(BCM2835V3DState *s,
                                             unsigned thread,
                                             hwaddr offset)
{
    switch (offset) {
    case V3D_CT0CS:
    case V3D_CT1CS:
        return s->ct_cs[thread];
    case V3D_CT0EA:
    case V3D_CT1EA:
        return s->ct_ea[thread];
    case V3D_CT0CA:
    case V3D_CT1CA:
        return s->ct_ca[thread];
    case V3D_CT00RA0:
    case V3D_CT01RA0:
        return s->ct_ra0[thread];
    case V3D_CT0LC:
    case V3D_CT1LC:
        return s->ct_lc[thread];
    case V3D_CT0PC:
    case V3D_CT1PC:
        return s->ct_pc[thread];
    default:
        g_assert_not_reached();
    }
}

static uint64_t bcm2835_v3d_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835V3DState *s = opaque;
    unsigned thread;

    assert(size == 4);
    switch (offset) {
    case V3D_IDENT0:
        return 0x02443356;
    case V3D_IDENT1:
        return 0xc0101431;
    case V3D_IDENT2:
        return 0;
    case BCM2835_V3D_INTCTL:
        return s->irq_status;
    case BCM2835_V3D_INTENA:
        return s->irq_enable;
    case BCM2835_V3D_INTDIS:
        return 0;
    case V3D_BPCA:
        return s->bin_alloc_next <= UINT32_MAX ? s->bin_alloc_next : 0;
    case V3D_BPCS: {
        uint64_t alloc_end = (uint64_t)s->bin_alloc_base +
                             s->bin_alloc_size;

        return alloc_end > s->bin_alloc_next ?
               alloc_end - s->bin_alloc_next : 0;
    }
    case V3D_CT0CS: case V3D_CT0EA: case V3D_CT0CA:
    case V3D_CT00RA0: case V3D_CT0LC: case V3D_CT0PC:
        thread = 0;
        return bcm2835_v3d_read_thread_reg(s, thread, offset);
    case V3D_CT1CS: case V3D_CT1EA: case V3D_CT1CA:
    case V3D_CT01RA0: case V3D_CT1LC: case V3D_CT1PC:
        thread = 1;
        return bcm2835_v3d_read_thread_reg(s, thread, offset);
    default:
        return s->regs[offset >> 2];
    }
}

static void bcm2835_v3d_write_thread_cs(BCM2835V3DState *s,
                                         unsigned thread, uint32_t value)
{
    bool resume = false;

    if (value & V3D_CS_CTRSTA) {
        if (thread == 1) {
            qemu_bh_cancel(s->ct1_bh);
            s->ct1_start_us = 0;
        }
        s->ct_cs[thread] = 0;
        s->ct_return_depth[thread] = 0;
        s->ct_waiting[thread] = false;
        if (thread == 0) {
            s->bin_draw_append_count = 0;
            s->bin_draw_resume = false;
            s->bin_oom_waiting = false;
            s->regs[V3D_PCS >> 2] &= ~V3D_PCS_BMOOM;
        }
        return;
    }
    if (value & V3D_CS_CTRUN) {
        s->ct_cs[thread] &= ~V3D_CS_CTRUN;
        s->ct_cs[thread] |= V3D_CS_CTSUBS;
        s->ct_waiting[thread] = false;
    }
    if (value & V3D_CS_CTSUBS) {
        s->ct_cs[thread] &= ~(V3D_CS_CTSUBS | V3D_CS_CTERR);
        if (s->ct_ca[thread] != s->ct_ea[thread]) {
            s->ct_cs[thread] |= V3D_CS_CTRUN;
            resume = true;
        }
    }
    s->ct_cs[thread] = (s->ct_cs[thread] & ~V3D_CS_CTMODE) |
                       (value & V3D_CS_CTMODE);
    bcm2835_v3d_update_semaphore(s, thread);
    if (resume) {
        bcm2835_v3d_start_thread(s, thread);
    }
}

static void bcm2835_v3d_write_thread_ea(BCM2835V3DState *s,
                                         unsigned thread, uint32_t value)
{
    s->ct_ea[thread] = value;
    if (!(s->ct_cs[thread] & (V3D_CS_CTRUN | V3D_CS_CTSUBS)) &&
        s->ct_ca[thread] != s->ct_ea[thread]) {
        s->ct_cs[thread] &= ~V3D_CS_CTERR;
        s->ct_cs[thread] |= V3D_CS_CTRUN;
        bcm2835_v3d_start_thread(s, thread);
    }
}

static void bcm2835_v3d_write_thread_ca(BCM2835V3DState *s,
                                         unsigned thread, uint32_t value)
{
    if (s->ct_cs[thread] & V3D_CS_CTRUN) {
        return;
    }
    s->ct_ca[thread] = value;
    s->ct_return_depth[thread] = 0;
    s->ct_cs[thread] &= ~(V3D_CS_CTSUBS | V3D_CS_CTERR);
    s->ct_waiting[thread] = false;
}

static void bcm2835_v3d_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    BCM2835V3DState *s = opaque;
    uint32_t value32 = value;

    assert(size == 4);
    switch (offset) {
    case V3D_IDENT0:
    case V3D_IDENT1:
    case V3D_IDENT2:
        break;
    case BCM2835_V3D_INTCTL:
        s->irq_status &= ~value32;
        bcm2835_v3d_update_irq(s);
        break;
    case BCM2835_V3D_INTENA:
        s->irq_enable |= value32 & V3D_INT_MASK;
        bcm2835_v3d_update_irq(s);
        break;
    case BCM2835_V3D_INTDIS:
        s->irq_enable &= ~(value32 & V3D_INT_MASK);
        bcm2835_v3d_update_irq(s);
        break;
    case V3D_CT0CS:
        bcm2835_v3d_write_thread_cs(s, 0, value32);
        break;
    case V3D_CT1CS:
        bcm2835_v3d_write_thread_cs(s, 1, value32);
        break;
    case V3D_CT0EA:
        bcm2835_v3d_write_thread_ea(s, 0, value32);
        break;
    case V3D_CT1EA:
        bcm2835_v3d_write_thread_ea(s, 1, value32);
        break;
    case V3D_CT0CA:
        bcm2835_v3d_write_thread_ca(s, 0, value32);
        break;
    case V3D_CT1CA:
        bcm2835_v3d_write_thread_ca(s, 1, value32);
        break;
    case V3D_CT00RA0:
    case V3D_CT01RA0:
    case V3D_CT0PC:
    case V3D_CT1PC:
        break;
    case V3D_CT0LC:
        s->ct_lc[0] &= ~((value32 & (1U << 16)) ? (0xffffU << 16) : 0);
        s->ct_lc[0] &= ~((value32 & 1) ? 0xffffU : 0);
        break;
    case V3D_CT1LC:
        s->ct_lc[1] &= ~((value32 & (1U << 16)) ? (0xffffU << 16) : 0);
        s->ct_lc[1] &= ~((value32 & 1) ? 0xffffU : 0);
        break;
    case V3D_BFC:
    case V3D_RFC:
        if (value32 & 1) {
            s->regs[offset >> 2] = 0;
        }
        break;
    case V3D_PCS:
        break;
    case V3D_BPCA:
    case V3D_BPCS:
        break;
    case V3D_BPOA:
    case V3D_BPOS:
        s->regs[offset >> 2] = value32;
        if (offset == V3D_BPOS) {
            s->regs[V3D_PCS >> 2] &= ~V3D_PCS_BMOOM;
        }
        bcm2835_v3d_resume_binner_if_ready(s);
        break;
    default:
        s->regs[offset >> 2] = value32;
        break;
    }
}

static const MemoryRegionOps bcm2835_v3d_ops = {
    .read = bcm2835_v3d_read,
    .write = bcm2835_v3d_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void bcm2835_v3d_reset(DeviceState *dev)
{
    BCM2835V3DState *s = BCM2835_V3D(dev);

    if (s->ct1_bh) {
        qemu_bh_cancel(s->ct1_bh);
    }
    bcm2835_v3d_wgpu_batch_free(s);
    s->ct1_start_us = 0;
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ct_cs, 0, sizeof(s->ct_cs));
    memset(s->ct_ca, 0, sizeof(s->ct_ca));
    memset(s->ct_ea, 0, sizeof(s->ct_ea));
    memset(s->ct_ra0, 0, sizeof(s->ct_ra0));
    memset(s->ct_lc, 0, sizeof(s->ct_lc));
    memset(s->ct_pc, 0, sizeof(s->ct_pc));
    memset(s->ct_return_addr, 0, sizeof(s->ct_return_addr));
    memset(s->ct_return_depth, 0, sizeof(s->ct_return_depth));
    memset(s->ct_semaphore, 0, sizeof(s->ct_semaphore));
    memset(s->ct_waiting, 0, sizeof(s->ct_waiting));
    memset(s->clear_color, 0, sizeof(s->clear_color));
    memset(s->tile_color, 0, sizeof(s->tile_color));
    memset(s->shader_record, 0, sizeof(s->shader_record));
    s->render_base = 0;
    s->render_width = 0;
    s->render_height = 0;
    s->render_config = 0;
    s->tile_x = 0;
    s->tile_y = 0;
    s->render_config_valid = false;
    s->clear_color_valid = false;
    s->tile_color_valid = false;
    s->bin_alloc_base = 0;
    s->bin_alloc_size = 0;
    s->bin_state_base = 0;
    s->bin_tiles_x = 0;
    s->bin_tiles_y = 0;
    s->bin_flags = 0;
    s->bin_alloc_next = 0;
    s->bin_overflow_next = 0;
    s->bin_overflow_remaining = 0;
    s->bin_alloc_block_size = 0;
    s->bin_alloc_initial_block_size = 0;
    s->bin_draw_append_count = 0;
    s->bin_draw_resume = false;
    s->bin_oom_waiting = false;
    s->bin_spilluse = false;
    memset(s->bin_tile_list_addr, 0, sizeof(s->bin_tile_list_addr));
    memset(s->bin_tile_list_used, 0, sizeof(s->bin_tile_list_used));
    s->bin_list_stride = 0;
    s->bin_list_indirect = false;
    s->bin_config_valid = false;
    s->binning_started = false;
    s->shader_state = 0;
    s->shader_state_valid = false;
    s->shader_record_size = 0;
    s->shader_attribute_count = 0;
    s->shader_record_extended = false;
    memset(s->clipper_xy_scale, 0, sizeof(s->clipper_xy_scale));
    s->clipper_z_min = 0;
    s->clipper_z_max = 0;
    s->viewport_center_x = 0;
    s->viewport_center_y = 0;
    s->clipper_xy_scale_valid = false;
    s->clipper_z_clip_valid = false;
    s->viewport_offset_valid = false;
    s->tile_primitive_format = 0;
    s->tile_primitive_format_valid = false;
    s->pending_tile_load_base = 0;
    s->pending_tile_load_memory_format = 0;
    s->pending_tile_load_color_format = 0;
    s->pending_tile_load = false;
    s->irq_status = 0;
    s->irq_enable = 0;
    bcm2835_v3d_update_irq(s);
}

static void bcm2835_v3d_fragment_workers_init(BCM2835V3DState *s)
{
    long host_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned guest_cpus = current_machine ? current_machine->smp.cpus : 1;
    unsigned available = host_cpus > guest_cpus ? host_cpus - guest_cpus : 0;

    s->fragment_workers = MIN(MIN(s->fragment_workers_requested, 4),
                              available);
    if (s->fragment_workers_requested) {
        qemu_log("bcm2835_v3d: fragment workers=%u\n", s->fragment_workers);
    }
    if (!s->fragment_workers) {
        return;
    }

    qemu_mutex_init(&s->fragment_worker_mutex);
    qemu_cond_init(&s->fragment_worker_cond);
    qemu_cond_init(&s->fragment_worker_done);
    s->fragment_worker_threads = g_new0(QemuThread, s->fragment_workers);
    for (unsigned i = 0; i < s->fragment_workers; i++) {
        qemu_thread_create(&s->fragment_worker_threads[i],
                           "v3d-fragment", bcm2835_v3d_fragment_worker_thread,
                           s, QEMU_THREAD_JOINABLE);
    }
}

static void bcm2835_v3d_fragment_workers_destroy(BCM2835V3DState *s)
{
    if (!s->fragment_worker_threads) {
        return;
    }
    qemu_mutex_lock(&s->fragment_worker_mutex);
    s->fragment_worker_quit = true;
    s->fragment_worker_generation++;
    qemu_cond_broadcast(&s->fragment_worker_cond);
    qemu_mutex_unlock(&s->fragment_worker_mutex);
    for (unsigned i = 0; i < s->fragment_workers; i++) {
        qemu_thread_join(&s->fragment_worker_threads[i]);
    }
    g_free(s->fragment_worker_threads);
    s->fragment_worker_threads = NULL;
    qemu_cond_destroy(&s->fragment_worker_done);
    qemu_cond_destroy(&s->fragment_worker_cond);
    qemu_mutex_destroy(&s->fragment_worker_mutex);
}

static void bcm2835_v3d_realize(DeviceState *dev, Error **errp)
{
    BCM2835V3DState *s = BCM2835_V3D(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_V3D "-memory");
    bcm2835_v3d_fragment_workers_init(s);
#ifdef CONFIG_V3D_WGPU
    const char *library = g_getenv("QEMU_V3D_WGPU_LIBRARY");
    const char *verify = g_getenv("QEMU_V3D_WGPU_VERIFY");

    if (library && *library) {
        s->wgpu_context = bcm2835_v3d_wgpu_new(library);
        s->wgpu_verify = verify && *verify;
    }
#endif
    s->ct1_bh = qemu_bh_new(bcm2835_v3d_ct1_bh, s);
    bcm2835_v3d_reset(dev);
}

static void bcm2835_v3d_unrealize(DeviceState *dev)
{
    BCM2835V3DState *s = BCM2835_V3D(dev);

    if (s->ct1_bh) {
        qemu_bh_cancel(s->ct1_bh);
        qemu_bh_delete(s->ct1_bh);
        s->ct1_bh = NULL;
    }
    bcm2835_v3d_fragment_workers_destroy(s);
    bcm2835_v3d_wgpu_batch_free(s);
#ifdef CONFIG_V3D_WGPU
    bcm2835_v3d_wgpu_free(s->wgpu_context);
    s->wgpu_context = NULL;
#endif
    address_space_destroy(&s->dma_as);
}

static int bcm2835_v3d_post_load(void *opaque, int version_id)
{
    BCM2835V3DState *s = opaque;

    if (version_id < 5) {
        memset(s->tile_color, 0, sizeof(s->tile_color));
        s->tile_color_valid = false;
    }
    if (version_id < 6) {
        memset(s->shader_record, 0, sizeof(s->shader_record));
        s->shader_record_size = 0;
        s->shader_attribute_count = 0;
        s->shader_record_extended = false;
        s->shader_state_valid = false;
    }
    if (version_id < 7) {
        s->bin_list_stride = 0;
        s->bin_list_indirect = false;
        s->tile_primitive_format = 0;
        s->tile_primitive_format_valid = false;
        s->pending_tile_load_base = 0;
        s->pending_tile_load_memory_format = 0;
        s->pending_tile_load_color_format = 0;
        s->pending_tile_load = false;
    }
    if (version_id < 8) {
        memset(s->clipper_xy_scale, 0, sizeof(s->clipper_xy_scale));
        s->clipper_z_min = 0;
        s->clipper_z_max = 0;
        s->clipper_xy_scale_valid = false;
        s->clipper_z_clip_valid = false;
    }
    if (version_id < 9) {
        s->viewport_center_x = 0;
        s->viewport_center_y = 0;
        s->viewport_offset_valid = false;
    }
    if (version_id < 10) {
        s->bin_alloc_next = 0;
        s->bin_alloc_block_size = 0;
        s->bin_alloc_initial_block_size = 0;
        memset(s->bin_tile_list_addr, 0, sizeof(s->bin_tile_list_addr));
        memset(s->bin_tile_list_used, 0, sizeof(s->bin_tile_list_used));
    }
    if (version_id < 12) {
        s->bin_overflow_next = 0;
        s->bin_overflow_remaining = 0;
    }
    if (version_id < 11) {
        s->bin_draw_append_count = 0;
        s->bin_draw_resume = false;
        s->bin_oom_waiting = false;
        s->bin_spilluse = false;
    }
    bcm2835_v3d_update_irq(s);
    if ((s->ct_cs[1] & V3D_CS_CTRUN) && !s->ct_waiting[1]) {
        bcm2835_v3d_start_thread(s, 1);
    }
    return 0;
}

static const VMStateDescription vmstate_bcm2835_v3d = {
    .name = TYPE_BCM2835_V3D,
    .version_id = 12,
    .minimum_version_id = 1,
    .post_load = bcm2835_v3d_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2835V3DState,
                             BCM2835_V3D_REG_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32(irq_status, BCM2835V3DState),
        VMSTATE_UINT32(irq_enable, BCM2835V3DState),
        VMSTATE_UINT32_ARRAY_V(ct_cs, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ct_ca, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ct_ea, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ct_ra0, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ct_lc, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ct_pc, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_ARRAY_V(ct_return_addr, BCM2835V3DState, 4, 2),
        VMSTATE_UINT8_ARRAY_V(ct_return_depth, BCM2835V3DState, 2, 2),
        VMSTATE_UINT8_ARRAY_V(ct_semaphore, BCM2835V3DState, 2, 2),
        VMSTATE_BOOL_ARRAY_V(ct_waiting, BCM2835V3DState, 2, 2),
        VMSTATE_UINT32_V(render_base, BCM2835V3DState, 2),
        VMSTATE_UINT16_V(render_width, BCM2835V3DState, 2),
        VMSTATE_UINT16_V(render_height, BCM2835V3DState, 2),
        VMSTATE_UINT16_V(render_config, BCM2835V3DState, 2),
        VMSTATE_UINT8_V(tile_x, BCM2835V3DState, 2),
        VMSTATE_UINT8_V(tile_y, BCM2835V3DState, 2),
        VMSTATE_UINT32_ARRAY_V(clear_color, BCM2835V3DState, 2, 2),
        VMSTATE_BOOL_V(render_config_valid, BCM2835V3DState, 2),
        VMSTATE_BOOL_V(clear_color_valid, BCM2835V3DState, 2),
        VMSTATE_UINT32_V(bin_alloc_base, BCM2835V3DState, 3),
        VMSTATE_UINT32_V(bin_alloc_size, BCM2835V3DState, 3),
        VMSTATE_UINT32_V(bin_state_base, BCM2835V3DState, 3),
        VMSTATE_UINT8_V(bin_tiles_x, BCM2835V3DState, 3),
        VMSTATE_UINT8_V(bin_tiles_y, BCM2835V3DState, 3),
        VMSTATE_UINT8_V(bin_flags, BCM2835V3DState, 3),
        VMSTATE_UINT64_V(bin_alloc_next, BCM2835V3DState, 10),
        VMSTATE_UINT32_V(bin_overflow_next, BCM2835V3DState, 12),
        VMSTATE_UINT32_V(bin_overflow_remaining, BCM2835V3DState, 12),
        VMSTATE_UINT16_V(bin_alloc_block_size, BCM2835V3DState, 10),
        VMSTATE_UINT16_V(bin_alloc_initial_block_size, BCM2835V3DState, 10),
        VMSTATE_UINT32_ARRAY_V(bin_tile_list_addr, BCM2835V3DState,
                               BCM2835_V3D_MAX_TILES, 10),
        VMSTATE_UINT16_ARRAY_V(bin_tile_list_used, BCM2835V3DState,
                               BCM2835_V3D_MAX_TILES, 10),
        VMSTATE_UINT32_V(bin_draw_append_count, BCM2835V3DState, 11),
        VMSTATE_BOOL_V(bin_draw_resume, BCM2835V3DState, 11),
        VMSTATE_BOOL_V(bin_oom_waiting, BCM2835V3DState, 11),
        VMSTATE_BOOL_V(bin_spilluse, BCM2835V3DState, 11),
        VMSTATE_UINT32_V(bin_list_stride, BCM2835V3DState, 7),
        VMSTATE_BOOL_V(bin_list_indirect, BCM2835V3DState, 7),
        VMSTATE_BOOL_V(bin_config_valid, BCM2835V3DState, 3),
        VMSTATE_BOOL_V(binning_started, BCM2835V3DState, 3),
        VMSTATE_UINT32_V(shader_state, BCM2835V3DState, 3),
        VMSTATE_BOOL_V(shader_state_valid, BCM2835V3DState, 3),
        VMSTATE_UINT32_ARRAY_V(tile_color, BCM2835V3DState, 64 * 64, 5),
        VMSTATE_BOOL_V(tile_color_valid, BCM2835V3DState, 5),
        VMSTATE_UINT8_ARRAY_V(shader_record, BCM2835V3DState, 132, 6),
        VMSTATE_UINT16_V(shader_record_size, BCM2835V3DState, 6),
        VMSTATE_UINT8_V(shader_attribute_count, BCM2835V3DState, 6),
        VMSTATE_BOOL_V(shader_record_extended, BCM2835V3DState, 6),
        VMSTATE_UINT32_ARRAY_V(clipper_xy_scale, BCM2835V3DState, 2, 8),
        VMSTATE_UINT32_V(clipper_z_min, BCM2835V3DState, 8),
        VMSTATE_UINT32_V(clipper_z_max, BCM2835V3DState, 8),
        VMSTATE_BOOL_V(clipper_xy_scale_valid, BCM2835V3DState, 8),
        VMSTATE_BOOL_V(clipper_z_clip_valid, BCM2835V3DState, 8),
        VMSTATE_INT16_V(viewport_center_x, BCM2835V3DState, 9),
        VMSTATE_INT16_V(viewport_center_y, BCM2835V3DState, 9),
        VMSTATE_BOOL_V(viewport_offset_valid, BCM2835V3DState, 9),
        VMSTATE_UINT8_V(tile_primitive_format, BCM2835V3DState, 7),
        VMSTATE_BOOL_V(tile_primitive_format_valid, BCM2835V3DState, 7),
        VMSTATE_UINT32_V(pending_tile_load_base, BCM2835V3DState, 7),
        VMSTATE_UINT8_V(pending_tile_load_memory_format, BCM2835V3DState, 7),
        VMSTATE_UINT8_V(pending_tile_load_color_format, BCM2835V3DState, 7),
        VMSTATE_BOOL_V(pending_tile_load, BCM2835V3DState, 7),
        VMSTATE_END_OF_LIST()
    },
};

static void bcm2835_v3d_init(Object *obj)
{
    BCM2835V3DState *s = BCM2835_V3D(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_v3d_ops, s,
                          TYPE_BCM2835_V3D, BCM2835_V3D_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void bcm2835_v3d_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    static const Property properties[] = {
        DEFINE_PROP_UINT32("fragment-workers", BCM2835V3DState,
                           fragment_workers_requested, 0),
    };

    dc->realize = bcm2835_v3d_realize;
    dc->unrealize = bcm2835_v3d_unrealize;
    device_class_set_props(dc, properties);
    device_class_set_legacy_reset(dc, bcm2835_v3d_reset);
    dc->vmsd = &vmstate_bcm2835_v3d;
}

static const TypeInfo bcm2835_v3d_info = {
    .name = TYPE_BCM2835_V3D,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835V3DState),
    .instance_init = bcm2835_v3d_init,
    .class_init = bcm2835_v3d_class_init,
};

static void bcm2835_v3d_register_types(void)
{
    type_register_static(&bcm2835_v3d_info);
}

type_init(bcm2835_v3d_register_types)
