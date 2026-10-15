/*
 * Raspberry Pi VideoCore IV V3D register block.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_v3d.h"
#include "bcm2835_v3d_qpu.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/dma.h"
#include <math.h>

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
#define V3D_TILE_SIZE          64
#define V3D_T_MEMORY_TILE_SIZE 32
#define V3D_UTILE_PIXELS       16

typedef struct BCM2835V3DVertex {
    uint32_t word[7];
} BCM2835V3DVertex;

/* VC4 packets 48 and 49 have data-dependent lengths and are not decoded yet. */
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

static bool bcm2835_v3d_dma_read(BCM2835V3DState *s, uint32_t addr,
                                 void *buf, size_t len)
{
    return dma_memory_read(&s->dma_as, addr, buf, len,
                           MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
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

static uint64_t bcm2835_v3d_tile_pixel_addr(BCM2835V3DState *s,
                                            uint32_t base,
                                            unsigned memory_format,
                                            unsigned bytes_per_pixel,
                                            unsigned x, unsigned y)
{
    uint32_t global_x = (uint32_t)s->tile_x * V3D_TILE_SIZE + x;
    uint32_t global_y = (uint32_t)s->tile_y * V3D_TILE_SIZE + y;

    if (memory_format == 0) {
        return (uint64_t)base +
               ((uint64_t)global_y * s->render_width + global_x) *
               bytes_per_pixel;
    }

    if (memory_format == 2) {
        uint32_t stride = DIV_ROUND_UP(s->render_width, 4) * 4 *
                          bytes_per_pixel;

        return (uint64_t)base + (global_y & ~3U) * (uint64_t)stride +
               (global_x & ~3U) * bytes_per_pixel * 4 +
               ((global_y & 3) * 4 + (global_x & 3)) * bytes_per_pixel;
    }

    {
        static const uint8_t even_stile_map[4] = { 0, 3, 1, 2 };
        static const uint8_t odd_stile_map[4] = { 2, 1, 3, 0 };
        uint32_t tile_stride = DIV_ROUND_UP(s->render_width,
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

    if (memory_format > 2 || (color_format != 0 && color_format != 2)) {
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
    uint64_t params = ldl_le_p(packet + 1) |
                      ((uint64_t)lduw_le_p(packet + 5) << 32);
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
        (color_format != 0 && color_format != 2) ||
        !s->render_config_valid || !s->render_width || !s->render_height) {
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
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bcm2835_v3d: CT%u stopped at 0x%08x: %s\n",
                  thread, addr, reason);
    s->ct_cs[thread] &= ~V3D_CS_CTRUN;
    s->ct_cs[thread] |= V3D_CS_CTSUBS | V3D_CS_CTERR;
    s->ct_waiting[thread] = false;
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
        return false;
    }

    for (unsigned first = 0; first < count;
         first += BCM2835_V3D_QPU_LANES) {
        BCM2835V3DQPUState qpu;
        uint32_t executed;
        BCM2835V3DQPUResult result;

        bcm2835_v3d_qpu_init(&qpu);
        qpu.uniform_address = uniforms_address;
        qpu.read_uniform = bcm2835_v3d_qpu_read_uniform;
        qpu.uniform_opaque = s;
        if (input_vpm) {
            memcpy(qpu.vpm, input_vpm[first / BCM2835_V3D_QPU_LANES],
                   sizeof(qpu.vpm));
        }
        if (!bcm2835_v3d_load_qpu_attributes(
                s, &qpu, indices, count, first,
                s->shader_record[attributes_offset], vpm_offset_byte)) {
            g_free(code);
            return false;
        }
        result = bcm2835_v3d_qpu_run(code, code_size,
                                      V3D_QPU_INSTRUCTION_BUDGET, &qpu,
                                      &executed);
        if (result != BCM2835_V3D_QPU_END) {
            g_free(code);
            return false;
        }
        if (output_vpm) {
            memcpy(output_vpm[first / BCM2835_V3D_QPU_LANES], qpu.vpm,
                   sizeof(qpu.vpm));
        }
        if (vertices) {
            if (qpu.vpm_write_total !=
                    BCM2835_V3D_QPU_VPM_WRITE_HISTORY ||
                qpu.vpm_write_history_count !=
                    BCM2835_V3D_QPU_VPM_WRITE_HISTORY) {
                g_free(code);
                return false;
            }
            for (unsigned word = 0; word <
                 BCM2835_V3D_QPU_VPM_WRITE_HISTORY; word++) {
                uint8_t coordinate_row = qpu.vpm_write_history[word];

                for (unsigned lane = 0; lane <
                     BCM2835_V3D_QPU_LANES; lane++) {
                    if (first + lane < count) {
                        vertices[first + lane].word[word] =
                            qpu.vpm[coordinate_row][lane];
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

static bool bcm2835_v3d_run_fragment_shader(BCM2835V3DState *s,
                                             const uint16_t *pixels,
                                             unsigned count)
{
    uint32_t code_address = ldl_le_p(s->shader_record + 4) & ~7U;
    uint32_t uniforms_address = ldl_le_p(s->shader_record + 8);
    uint8_t *code;
    uint32_t code_size;

    if (!code_address || !count ||
        !bcm2835_v3d_load_qpu_program(s, code_address, &code, &code_size)) {
        return false;
    }

    for (unsigned first = 0; first < count;
         first += BCM2835_V3D_QPU_LANES) {
        BCM2835V3DQPUState qpu;
        uint32_t executed;
        uint32_t batch = MIN(count - first, BCM2835_V3D_QPU_LANES);
        uint16_t expected_mask = batch == BCM2835_V3D_QPU_LANES ?
                                 UINT16_MAX : (1U << batch) - 1;
        BCM2835V3DQPUResult result;

        bcm2835_v3d_qpu_init(&qpu);
        qpu.uniform_address = uniforms_address;
        qpu.read_uniform = bcm2835_v3d_qpu_read_uniform;
        qpu.uniform_opaque = s;
        /* shortcut: fragment W/Z inputs wait for interpolated depth support. */
        for (unsigned lane = 0; lane < batch; lane++) {
            unsigned pixel = pixels[first + lane];

            qpu.fragment_coord[0][lane] =
                (s->tile_x * V3D_TILE_SIZE + pixel % V3D_TILE_SIZE) & 0x7ff;
            qpu.fragment_coord[1][lane] =
                (s->tile_y * V3D_TILE_SIZE + pixel / V3D_TILE_SIZE) & 0x7ff;
        }
        result = bcm2835_v3d_qpu_run(code, code_size,
                                      V3D_QPU_INSTRUCTION_BUDGET, &qpu,
                                      &executed);
        if (result != BCM2835_V3D_QPU_END ||
            (qpu.tlb_color_all_write_mask & expected_mask) !=
            expected_mask) {
            g_free(code);
            return false;
        }
        for (unsigned lane = 0; lane < batch; lane++) {
            s->tile_color[pixels[first + lane]] = qpu.io[46][lane];
        }
    }
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
                                             const uint32_t *screen_xy)
{
    uint32_t tile_index = tile_y * s->bin_tiles_x + tile_x;
    uint8_t commands[23];
    uint32_t list_address;
    uint32_t used;
    uint32_t list_capacity;
    uint64_t pool_start;
    size_t offset = 0;

    if (tile_x >= s->bin_tiles_x || tile_y >= s->bin_tiles_y ||
        tile_index >= V3D_MAX_RASTER_TILES) {
        return false;
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

    if (sizeof(commands) - 1 > list_capacity - used) {
        uint32_t next_block;
        uint8_t branch[5] = { V3D_PACKET_BRANCH };
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
    if (sizeof(commands) - 1 > list_capacity - used ||
        (uint64_t)list_address + used + sizeof(commands) - 1 >
            UINT32_MAX + 1ULL) {
        return false;
    }

    commands[offset++] = V3D_PACKET_PRIMITIVE_FORMAT;
    commands[offset++] = (3 << 4) | 2; /* 32-bit XY triangles */
    commands[offset++] = V3D_PACKET_GL_SHADER_STATE;
    stl_le_p(commands + offset, s->shader_state);
    offset += sizeof(uint32_t);
    commands[offset++] = V3D_PACKET_COMPRESSED;
    commands[offset++] = 129; /* absolute triangle coordinates */
    for (unsigned i = 0; i < 3; i++) {
        stl_le_p(commands + offset, screen_xy[i]);
        offset += sizeof(uint32_t);
    }
    commands[offset++] = 128; /* compressed-list escape */
    commands[offset++] = V3D_PACKET_RETURN;
    g_assert(offset == sizeof(commands));

    if (dma_memory_write(&s->dma_as, list_address + used - 1, commands,
                         sizeof(commands),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return false;
    }
    s->bin_tile_list_used[tile_index] = used + sizeof(commands) - 1;
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
        scale_y <= 0.0f) {
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
    vertex->word[6] = bcm2835_v3d_float_bits(1.0f / w);
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
    const BCM2835V3DVertex *a, const BCM2835V3DVertex *b, float t)
{
    BCM2835V3DVertex result;

    for (unsigned i = 0; i < 4; i++) {
        float av = bcm2835_v3d_bits_float(a->word[i]);
        float bv = bcm2835_v3d_bits_float(b->word[i]);

        result.word[i] = bcm2835_v3d_float_bits(av + (bv - av) * t);
    }
    result.word[4] = 0;
    for (unsigned i = 5; i < 7; i++) {
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
    unsigned current_count = 3;
    float scale_x = bcm2835_v3d_bits_float(s->clipper_xy_scale[0]);
    float scale_y = bcm2835_v3d_bits_float(s->clipper_xy_scale[1]);
    float z_min = bcm2835_v3d_bits_float(s->clipper_z_min);
    float z_max = bcm2835_v3d_bits_float(s->clipper_z_max);

    if (!s->clipper_xy_scale_valid || !s->clipper_z_clip_valid ||
        !isfinite(scale_x) || !isfinite(scale_y) || scale_x <= 0.0f ||
        scale_y <= 0.0f || !isfinite(z_min) || !isfinite(z_max) ||
        z_min > z_max) {
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

    for (unsigned plane = 0; plane < 6 && current_count; plane++) {
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
                    &previous, &vertex, t);
                if (!bcm2835_v3d_project_clipped_vertex(s, &intersection)) {
                    return false;
                }
                next[next_count++] = intersection;
            }
            if (inside) {
                if (next_count == V3D_MAX_CLIPPED_VERTICES) {
                    return false;
                }
                next[next_count++] = vertex;
            }
            previous = vertex;
            previous_distance = distance;
            previous_inside = inside;
        }
        memcpy(current, next, sizeof(BCM2835V3DVertex) * next_count);
        current_count = next_count;
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
                                                 s->render_width / 2;
    int32_t center_y = s->viewport_offset_valid ? s->viewport_center_y :
                                                 s->render_height / 2;
    int32_t x[3];
    int32_t y[3];
    int32_t min_x, min_y, max_x, max_y;
    int32_t pixel_min_x, pixel_min_y, pixel_max_x, pixel_max_y;
    int64_t area;
    uint32_t screen_xy[3];

    for (unsigned i = 0; i < 3; i++) {
        screen_xy[i] = triangle[i].word[4];
        x[i] = center_x * 16 + (int16_t)screen_xy[i];
        y[i] = center_y * 16 -
               (int16_t)(screen_xy[i] >> 16);
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
    pixel_max_x = MIN((int32_t)s->render_width - 1,
                      bcm2835_v3d_floor_12_4(max_x));
    pixel_max_y = MIN((int32_t)s->render_height - 1,
                      bcm2835_v3d_floor_12_4(max_y));
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
                                                  screen_xy)) {
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
    unsigned format = (s->render_config >> 2) & 3;
    unsigned memory_format = (s->render_config >> 6) & 3;
    uint64_t binned_tiles = 0;
    uint32_t append_index = 0;

    if (!s->bin_draw_resume) {
        s->bin_draw_append_count = 0;
    }

    if (!s->render_config_valid || !s->bin_config_valid ||
        !s->binning_started || !s->render_width || !s->render_height ||
        (format != 1 && format != 2) || memory_format > 2 ||
        DIV_ROUND_UP(s->render_width, V3D_TILE_SIZE) > s->bin_tiles_x ||
        DIV_ROUND_UP(s->render_height, V3D_TILE_SIZE) > s->bin_tiles_y) {
        return false;
    }

    for (unsigned vertex = 0; vertex < count; vertex += 3) {
        if (s->shader_record[0] & V3D_SHADER_STATE_CLIP) {
            BCM2835V3DVertex polygon[V3D_MAX_CLIPPED_VERTICES];
            unsigned polygon_count;

            if (!bcm2835_v3d_clip_triangle(s, vertices + vertex, polygon,
                                           &polygon_count)) {
                return false;
            }
            /* shortcut: XY fans until CT1 handles clipped varyings. */
            for (unsigned i = 1; i + 1 < polygon_count; i++) {
                BCM2835V3DVertex triangle[3] = {
                    polygon[0], polygon[i], polygon[i + 1],
                };

                if (!bcm2835_v3d_bin_screen_triangle(s, triangle,
                                                     &binned_tiles,
                                                     &append_index)) {
                    return false;
                }
            }
        } else if (!bcm2835_v3d_bin_screen_triangle(s, vertices + vertex,
                                                    &binned_tiles,
                                                    &append_index)) {
            return false;
        }
    }
    s->bin_draw_append_count = 0;
    s->bin_draw_resume = false;
    return true;
}

static bool bcm2835_v3d_rasterize_tile_triangle(BCM2835V3DState *s,
                                                 const uint32_t *screen_xy,
                                                 uint64_t *work)
{
    int32_t center_x = s->viewport_offset_valid ? s->viewport_center_x :
                                                 s->render_width / 2;
    int32_t center_y = s->viewport_offset_valid ? s->viewport_center_y :
                                                 s->render_height / 2;
    int32_t x[3];
    int32_t y[3];
    int32_t x0 = (int32_t)s->tile_x * V3D_TILE_SIZE;
    int32_t y0 = (int32_t)s->tile_y * V3D_TILE_SIZE;
    int32_t x1 = MIN(x0 + V3D_TILE_SIZE, s->render_width);
    int32_t y1 = MIN(y0 + V3D_TILE_SIZE, s->render_height);
    int32_t min_x, min_y, max_x, max_y;
    int64_t area;
    uint32_t color = s->clear_color_valid ? s->clear_color[0] : UINT32_MAX;
    uint16_t covered[64 * 64];
    unsigned covered_count = 0;

    if (!s->tile_color_valid) {
        for (unsigned i = 0; i < G_N_ELEMENTS(s->tile_color); i++) {
            s->tile_color[i] = color;
        }
        s->tile_color_valid = true;
    }
    for (unsigned i = 0; i < 3; i++) {
        uint32_t packed = screen_xy[i];

        x[i] = center_x * 16 + (int16_t)packed;
        y[i] = center_y * 16 - (int16_t)(packed >> 16);
    }
    area = bcm2835_v3d_edge(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (!area) {
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
        return true;
    }
    *work += (uint64_t)(max_x - min_x + 1) * (max_y - min_y + 1);
    if (*work > V3D_MAX_RASTER_WORK) {
        return false;
    }

    for (int32_t py = min_y; py <= max_y; py++) {
        for (int32_t px = min_x; px <= max_x; px++) {
            int32_t px_fixed = px * 16 + 8;
            int32_t py_fixed = py * 16 + 8;
            int64_t e0 = bcm2835_v3d_edge(
                x[0], y[0], x[1], y[1], px_fixed, py_fixed);
            int64_t e1 = bcm2835_v3d_edge(
                x[1], y[1], x[2], y[2], px_fixed, py_fixed);
            int64_t e2 = bcm2835_v3d_edge(
                x[2], y[2], x[0], y[0], px_fixed, py_fixed);

            if ((area > 0 && e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (area < 0 && e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                unsigned tile_pixel = (py - y0) * V3D_TILE_SIZE + px - x0;

                if (covered_count >= G_N_ELEMENTS(covered)) {
                    return false;
                }
                covered[covered_count++] = tile_pixel;
            }
        }
    }
    if (covered_count && ldl_le_p(s->shader_record + 4)) {
        return bcm2835_v3d_run_fragment_shader(s, covered, covered_count);
    }
    for (unsigned i = 0; i < covered_count; i++) {
        s->tile_color[covered[i]] = color;
    }
    return true;
}

static bool bcm2835_v3d_execute_compressed_xy(BCM2835V3DState *s,
                                               uint32_t *ca)
{
    uint32_t triangles = 0;
    uint64_t work = 0;

    if (*ca == UINT32_MAX || !s->tile_primitive_format_valid ||
        s->tile_primitive_format != ((3 << 4) | 2) ||
        !s->shader_state_valid || s->shader_record[3]) {
        return false;
    }

    *ca += 1; /* compressed-list packet id */
    while (triangles < V3D_MAX_RASTER_WORK / (V3D_TILE_SIZE *
                                              V3D_TILE_SIZE)) {
        uint8_t code;
        uint8_t coord_bytes[sizeof(uint32_t) * 3];
        uint32_t coords[3];

        if (!bcm2835_v3d_dma_read(s, *ca, &code, sizeof(code))) {
            return false;
        }
        if (code == 128) {
            *ca += 1;
            return true;
        }
        if (code != 129 || (uint64_t)*ca + 1 + sizeof(coord_bytes) >
                           UINT32_MAX + 1ULL ||
            !bcm2835_v3d_dma_read(s, *ca + 1, coord_bytes,
                                   sizeof(coord_bytes))) {
            return false;
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(coords); i++) {
            coords[i] = ldl_le_p(coord_bytes + i * sizeof(uint32_t));
        }
        if (!bcm2835_v3d_rasterize_tile_triangle(s, coords, &work)) {
            return false;
        }
        *ca += 1 + sizeof(coord_bytes);
        triangles++;
    }
    return false;
}

static bool bcm2835_v3d_execute_coordinate_shader(BCM2835V3DState *s,
                                                   const uint8_t *packet,
                                                   uint8_t opcode)
{
    uint32_t count = ldl_le_p(packet + 2);
    uint32_t *indices;
    BCM2835V3DVertex *vertices;
    uint32_t (*vertex_vpm)[BCM2835_V3D_QPU_VPM_ROWS]
                          [BCM2835_V3D_QPU_LANES] = NULL;
    bool success;

    if (!count) {
        return true;
    }
    if (count > V3D_MAX_SHADER_VERTICES) {
        return false;
    }
    if ((packet[1] & 0xf) != 4 || count % 3) {
        return false;
    }
    if (s->shader_record[3]) {
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
        success = bcm2835_v3d_bin_triangles(s, vertices, count);
    }
    g_free(vertices);
    return success;
}

static void bcm2835_v3d_run_thread(BCM2835V3DState *s, unsigned thread)
{
    uint32_t *ca = &s->ct_ca[thread];
    uint32_t ea = s->ct_ea[thread];

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
                bcm2835_v3d_run_thread(s, target);
            }
            break;
        }
        case V3D_PACKET_WAIT_SEMA:
            if (!s->ct_semaphore[thread]) {
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
            if (thread != 1) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "primitive format outside tile rendering");
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
            s->render_base = ldl_le_p(packet + 1);
            s->render_width = lduw_le_p(packet + 5);
            s->render_height = lduw_le_p(packet + 7);
            s->render_config = lduw_le_p(packet + 9);
            s->render_config_valid = true;
            s->tile_color_valid = false;
            break;
        case V3D_PACKET_CLEAR_COLORS:
            s->clear_color[0] = ldl_le_p(packet + 1);
            s->clear_color[1] = ldl_le_p(packet + 5);
            s->clear_color_valid = true;
            break;
        case V3D_PACKET_TILE_COORDS:
            s->tile_x = packet[1];
            s->tile_y = packet[2];
            s->tile_color_valid = false;
            if (s->pending_tile_load) {
                if (!bcm2835_v3d_copy_tile(
                        s, s->pending_tile_load_base,
                        s->pending_tile_load_memory_format,
                        s->pending_tile_load_color_format, false)) {
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
        case V3D_PACKET_LOAD_TILE:
            if (thread != 1 ||
                !bcm2835_v3d_tile_buffer_packet(
                    s, packet, opcode == V3D_PACKET_STORE_TILE)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported tile buffer operation or DMA failure");
                return;
            }
            break;
        case V3D_PACKET_STORE_MS:
        case V3D_PACKET_STORE_MS_EOF:
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

    s->ct_cs[thread] &= ~(V3D_CS_CTRUN | V3D_CS_CTSUBS);
    s->ct_waiting[thread] = false;
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
        return 0xc0101432;
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
        bcm2835_v3d_run_thread(s, thread);
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
        bcm2835_v3d_run_thread(s, thread);
    }
}

static void bcm2835_v3d_write_thread_ca(BCM2835V3DState *s,
                                         unsigned thread, uint32_t value)
{
    if (s->ct_cs[thread] & V3D_CS_CTRUN) {
        return;
    }
    s->ct_ca[thread] = value;
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

static void bcm2835_v3d_realize(DeviceState *dev, Error **errp)
{
    BCM2835V3DState *s = BCM2835_V3D(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);
    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_BCM2835_V3D "-memory");
    bcm2835_v3d_reset(dev);
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

    dc->realize = bcm2835_v3d_realize;
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
