/*
 * Raspberry Pi VC4 Hardware Video Scaler.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/bcm2835_hvs.h"
#include "qemu/module.h"

#define HVS_DLIST_OFFSET        0x2000
#define HVS_DISPLIST0           0x20
#define HVS_DISPLACT0           0x30
#define HVS_DISPCTRL0           0x40
#define HVS_DISPSTAT0           0x48
#define HVS_DISPCTRL_GLOBAL     0x00
#define HVS_DISPCTRL_GLOBAL_EN  BIT(31)
#define HVS_DISPCTRL_EN         BIT(31)
#define HVS_DISPCTRL_RESET      BIT(30)
#define HVS_DISPCTRL_WIDTH_MASK 0x00fff000
#define HVS_DISPCTRL_HEIGHT_MASK 0x00000fff
#define HVS_DISPSTAT_MODE_INIT  (1U << 30)
#define HVS_DISPSTAT_MODE_RUN   (2U << 30)
#define HVS_CTL0_END            BIT(31)
#define HVS_CTL0_VALID          BIT(30)
#define HVS_CTL0_SIZE_MASK      0x3f000000
#define HVS_CTL0_SIZE_SHIFT     24
#define HVS_CTL0_UNITY          BIT(4)
#define HVS_CTL0_FORMAT_MASK    0x0f
#define HVS_POS2_WIDTH_MASK     0x00000fff
#define HVS_POS2_HEIGHT_MASK    0x0fff0000

static bool bcm2835_hvs_primary_plane(BCM2835HVSState *s, unsigned channel,
                                      BCM2835FBConfig *config)
{
    uint32_t ctrl = s->regs[(HVS_DISPCTRL0 + channel * 0x10) / 4];
    uint32_t start = s->regs[(HVS_DISPLIST0 + channel * 4) / 4];
    uint32_t ctl0, pos0, pos2, fb_addr, pitch, width, height, bpp, format;
    unsigned size, pointer_offset;

    if (!(s->regs[HVS_DISPCTRL_GLOBAL / 4] & HVS_DISPCTRL_GLOBAL_EN) ||
        !(ctrl & HVS_DISPCTRL_EN) || (ctrl & HVS_DISPCTRL_RESET) ||
        start >= BCM2835_HVS_DLIST_WORDS) {
        return false;
    }

    ctl0 = s->dlist[start];
    size = (ctl0 & HVS_CTL0_SIZE_MASK) >> HVS_CTL0_SIZE_SHIFT;
    if ((ctl0 & HVS_CTL0_END) || !(ctl0 & HVS_CTL0_VALID) ||
        !(ctl0 & HVS_CTL0_UNITY) || size < 7 ||
        start + size > BCM2835_HVS_DLIST_WORDS) {
        return false;
    }

    format = ctl0 & HVS_CTL0_FORMAT_MASK;
    if (format == 4) {
        bpp = 16;
    } else if (format == 7) {
        bpp = 32;
    } else {
        return false;
    }

    pos0 = s->dlist[start + 1];
    pos2 = s->dlist[start + 2];
    width = pos2 & HVS_POS2_WIDTH_MASK;
    height = (pos2 & HVS_POS2_HEIGHT_MASK) >> 16;
    pointer_offset = start + 4;
    fb_addr = s->dlist[pointer_offset];
    pitch = s->dlist[pointer_offset + 2];

    /* shortcut: only full-screen, unity-scaled RGB planes; add composition
     * and scaling when the guest's DLIST requires them.
     */
    if ((pos0 & 0x00ffffff) || !width || !height || width > 3840 ||
        height > 2560 || (ctrl & HVS_DISPCTRL_WIDTH_MASK) >> 12 != width ||
        (ctrl & HVS_DISPCTRL_HEIGHT_MASK) != height ||
        pitch < width * (bpp / 8) || pitch % (bpp / 8) ||
        pitch / (bpp / 8) > 3840) {
        return false;
    }

    *config = (BCM2835FBConfig) {
        .xres = width,
        .yres = height,
        .xres_virtual = pitch / (bpp / 8),
        .yres_virtual = height,
        .bpp = bpp,
        .base = fb_addr,
        .pixo = format == 7 ? 0 : 1,
        .alpha = s->fb->config.alpha,
    };
    return true;
}

static void bcm2835_hvs_update(BCM2835HVSState *s)
{
    unsigned channel;

    for (channel = 0; channel < 3; channel++) {
        uint32_t ctrl = s->regs[(HVS_DISPCTRL0 + channel * 0x10) / 4];
        BCM2835FBConfig config;
        bool scanout = s->fb &&
            bcm2835_hvs_primary_plane(s, channel, &config);

        s->channel_status[channel] = !(ctrl & HVS_DISPCTRL_EN) ? 0 :
            (scanout ? HVS_DISPSTAT_MODE_RUN : HVS_DISPSTAT_MODE_INIT);
        s->active_dlist[channel] =
            s->regs[(HVS_DISPLIST0 + channel * 4) / 4];

        if (scanout && memcmp(&s->fb->config, &config, sizeof(config))) {
            bcm2835_fb_reconfigure(s->fb, &config);
        }
    }
}

static uint64_t bcm2835_hvs_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835HVSState *s = opaque;
    unsigned channel;

    if (offset >= HVS_DLIST_OFFSET) {
        return s->dlist[(offset - HVS_DLIST_OFFSET) / 4];
    }
    for (channel = 0; channel < 3; channel++) {
        if (offset == HVS_DISPLACT0 + channel * 4) {
            return s->active_dlist[channel];
        }
        if (offset == HVS_DISPSTAT0 + channel * 0x10) {
            return s->channel_status[channel];
        }
    }
    return s->regs[offset / 4];
}

static void bcm2835_hvs_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    BCM2835HVSState *s = opaque;

    if (offset >= HVS_DLIST_OFFSET) {
        s->dlist[(offset - HVS_DLIST_OFFSET) / 4] = value;
    } else if (offset == 0x04) {
        s->regs[offset / 4] &= ~value;
    } else {
        s->regs[offset / 4] = value;
    }
    bcm2835_hvs_update(s);
}

static const MemoryRegionOps bcm2835_hvs_ops = {
    .read = bcm2835_hvs_read,
    .write = bcm2835_hvs_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2835_hvs_init(Object *obj)
{
    BCM2835HVSState *s = BCM2835_HVS(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_hvs_ops, s,
                          TYPE_BCM2835_HVS, 0x6000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
}

static const TypeInfo bcm2835_hvs_info = {
    .name = TYPE_BCM2835_HVS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835HVSState),
    .instance_init = bcm2835_hvs_init,
};

static void bcm2835_hvs_register_types(void)
{
    type_register_static(&bcm2835_hvs_info);
}

type_init(bcm2835_hvs_register_types)
