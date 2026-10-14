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
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/dma.h"

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
#define V3D_PACKET_ARRAY_PRIM  33
#define V3D_PACKET_INDEX_PRIM  32
#define V3D_PACKET_COMPRESSED  48
#define V3D_PACKET_CLIPPED     49
#define V3D_PACKET_BIN_CONFIG  112
#define V3D_PACKET_RENDER_CONFIG 113
#define V3D_PACKET_CLEAR_COLORS 114
#define V3D_PACKET_TILE_COORDS 115

#define V3D_MAX_CL_COMMANDS    (1U << 16)
#define V3D_TILE_SIZE          32

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
    case V3D_PACKET_INDEX_PRIM:
    case 64: /* GL Shader State */
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
    case 56: /* Primitive List Format */
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

static void bcm2835_v3d_thread_error(BCM2835V3DState *s, unsigned thread,
                                     uint32_t addr, const char *reason)
{
    qemu_log_mask(LOG_GUEST_ERROR,
                  "bcm2835_v3d: CT%u stopped at 0x%08x: %s\n",
                  thread, addr, reason);
    s->ct_cs[thread] &= ~V3D_CS_CTRUN;
    s->ct_cs[thread] |= V3D_CS_CTSUBS | V3D_CS_CTERR;
    s->ct_waiting[thread] = false;
}

static bool bcm2835_v3d_clear_tile(BCM2835V3DState *s)
{
    uint8_t row[V3D_TILE_SIZE * sizeof(uint32_t)];
    uint32_t x0 = s->tile_x * V3D_TILE_SIZE;
    uint32_t y0 = s->tile_y * V3D_TILE_SIZE;
    uint32_t width = MIN(V3D_TILE_SIZE, s->render_width -
                         MIN(x0, s->render_width));
    uint32_t height = MIN(V3D_TILE_SIZE, s->render_height -
                          MIN(y0, s->render_height));
    uint32_t color = s->clear_color[0];
    unsigned format = (s->render_config >> 2) & 3;
    unsigned memory_format = (s->render_config >> 6) & 3;
    unsigned y;

    if (!s->render_config_valid || !s->clear_color_valid || !width || !height) {
        return true;
    }

    /* shortcut: only linear RGBA8888 clears render; add T/LT and 565 support
     * when implementing tiled surfaces.
     */
    if (memory_format != 0 || format != 1) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: clear skipped for render format %u/%u\n",
                      memory_format, format);
        return false;
    }

    for (unsigned x = 0; x < width; x++) {
        stl_le_p(row + x * sizeof(uint32_t), color);
    }

    for (y = 0; y < height; y++) {
        uint64_t addr = (uint64_t)s->render_base +
                        ((uint64_t)(y0 + y) * s->render_width + x0) *
                        sizeof(uint32_t);

        if (addr + width * sizeof(uint32_t) > UINT32_MAX + 1ULL) {
            return false;
        }
        if (dma_memory_write(&s->dma_as, addr, row,
                            width * sizeof(uint32_t),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            return false;
        }
    }
    return true;
}

static void bcm2835_v3d_run_thread(BCM2835V3DState *s, unsigned thread);

static void bcm2835_v3d_run_thread(BCM2835V3DState *s, unsigned thread)
{
    uint32_t *ca = &s->ct_ca[thread];
    uint32_t ea = s->ct_ea[thread];

    for (uint32_t commands = 0; *ca != ea; commands++) {
        uint8_t packet[16];
        uint8_t opcode;
        unsigned packet_size;
        uint32_t next;

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
        case V3D_PACKET_FLUSH:
        case V3D_PACKET_FLUSH_ALL:
            if (thread == 0) {
                bcm2835_v3d_raise_irq(s, V3D_INT_FLDONE);
            }
            break;
        case V3D_PACKET_RENDER_CONFIG:
            s->render_base = ldl_le_p(packet + 1);
            s->render_width = lduw_le_p(packet + 5);
            s->render_height = lduw_le_p(packet + 7);
            s->render_config = lduw_le_p(packet + 9);
            s->render_config_valid = true;
            break;
        case V3D_PACKET_CLEAR_COLORS:
            s->clear_color[0] = ldl_le_p(packet + 1);
            s->clear_color[1] = ldl_le_p(packet + 5);
            s->clear_color_valid = true;
            break;
        case V3D_PACKET_TILE_COORDS:
            s->tile_x = packet[1];
            s->tile_y = packet[2];
            break;
        case V3D_PACKET_STORE_MS:
        case V3D_PACKET_STORE_MS_EOF:
            if (!bcm2835_v3d_clear_tile(s)) {
                bcm2835_v3d_thread_error(s, thread, *ca,
                                         "unsupported render target or DMA write failed");
                return;
            }
            break;
        case V3D_PACKET_INDEX_PRIM:
        case V3D_PACKET_ARRAY_PRIM:
        case V3D_PACKET_COMPRESSED:
        case V3D_PACKET_CLIPPED:
            bcm2835_v3d_thread_error(s, thread, *ca,
                                     "triangle and shader execution is not implemented");
            return;
        default:
            break;
        }
    }

    s->ct_cs[thread] &= ~(V3D_CS_CTRUN | V3D_CS_CTSUBS);
    s->ct_waiting[thread] = false;
    if (thread == 0) {
        s->regs[V3D_BFC >> 2]++;
        bcm2835_v3d_raise_irq(s, V3D_INT_FLDONE);
    } else {
        s->regs[V3D_RFC >> 2]++;
        bcm2835_v3d_raise_irq(s, V3D_INT_FRDONE);
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
    s->render_base = 0;
    s->render_width = 0;
    s->render_height = 0;
    s->render_config = 0;
    s->tile_x = 0;
    s->tile_y = 0;
    s->render_config_valid = false;
    s->clear_color_valid = false;
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

    bcm2835_v3d_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_bcm2835_v3d = {
    .name = TYPE_BCM2835_V3D,
    .version_id = 2,
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
