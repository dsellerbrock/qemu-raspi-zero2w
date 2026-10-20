/*
 * Raspberry Pi BCM2835 PixelValve.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/display/bcm2835_pixelvalve.h"
#include "hw/core/irq.h"
#include "qemu/timer.h"
#include "qemu/module.h"

#define PV_CONTROL          0x00
#define PV_CONTROL_EN       BIT(0)
#define PV_V_CONTROL        0x04
#define PV_VCONTROL_VIDEN   BIT(0)
#define PV_INTEN            0x24
#define PV_INTSTAT          0x28
#define PV_INT_VFP_START    BIT(7)

/* shortcut: drive a 60 Hz vblank; use the pixel clock from CPRMAN when it is
 * connected to this device.
 */
#define PV_FRAME_NS         (NANOSECONDS_PER_SECOND / 60)

static bool bcm2835_pixelvalve_enabled(BCM2835PixelValveState *s)
{
    return (s->regs[PV_CONTROL / 4] & PV_CONTROL_EN) &&
           (s->regs[PV_V_CONTROL / 4] & PV_VCONTROL_VIDEN);
}

static void bcm2835_pixelvalve_update_irq(BCM2835PixelValveState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[PV_INTSTAT / 4] &
                            s->regs[PV_INTEN / 4]));
}

static void bcm2835_pixelvalve_update_timer(BCM2835PixelValveState *s)
{
    if (bcm2835_pixelvalve_enabled(s)) {
        if (!timer_pending(s->frame_timer)) {
            timer_mod_ns(s->frame_timer,
                         qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PV_FRAME_NS);
        }
    } else {
        timer_del(s->frame_timer);
        s->regs[PV_INTSTAT / 4] &= ~PV_INT_VFP_START;
        bcm2835_pixelvalve_update_irq(s);
    }
}

static void bcm2835_pixelvalve_frame(void *opaque)
{
    BCM2835PixelValveState *s = opaque;

    s->regs[PV_INTSTAT / 4] |= PV_INT_VFP_START;
    bcm2835_pixelvalve_update_irq(s);
    bcm2835_pixelvalve_update_timer(s);
}

static uint64_t bcm2835_pixelvalve_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    BCM2835PixelValveState *s = opaque;

    return s->regs[offset / sizeof(uint32_t)];
}

static void bcm2835_pixelvalve_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    BCM2835PixelValveState *s = opaque;
    unsigned reg = offset / sizeof(uint32_t);

    switch (offset) {
    case PV_INTSTAT:
        s->regs[reg] &= ~value;
        bcm2835_pixelvalve_update_irq(s);
        break;
    case PV_CONTROL:
        s->regs[reg] = value & ~BIT(1); /* FIFO clear is self-clearing. */
        bcm2835_pixelvalve_update_timer(s);
        break;
    case PV_V_CONTROL:
        s->regs[reg] = value;
        bcm2835_pixelvalve_update_timer(s);
        break;
    case PV_INTEN:
        s->regs[reg] = value;
        bcm2835_pixelvalve_update_irq(s);
        break;
    default:
        s->regs[reg] = value;
        break;
    }
}

static const MemoryRegionOps bcm2835_pixelvalve_ops = {
    .read = bcm2835_pixelvalve_read,
    .write = bcm2835_pixelvalve_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bcm2835_pixelvalve_reset(DeviceState *dev)
{
    BCM2835PixelValveState *s = BCM2835_PIXELVALVE(dev);

    timer_del(s->frame_timer);
    memset(s->regs, 0, sizeof(s->regs));
    bcm2835_pixelvalve_update_irq(s);
}

static void bcm2835_pixelvalve_finalize(Object *obj)
{
    BCM2835PixelValveState *s = BCM2835_PIXELVALVE(obj);

    timer_free(s->frame_timer);
}

static void bcm2835_pixelvalve_init(Object *obj)
{
    BCM2835PixelValveState *s = BCM2835_PIXELVALVE(obj);

    memory_region_init_io(&s->iomem, obj, &bcm2835_pixelvalve_ops, s,
                          TYPE_BCM2835_PIXELVALVE, sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  bcm2835_pixelvalve_frame, s);
}

static void bcm2835_pixelvalve_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, bcm2835_pixelvalve_reset);
}

static const TypeInfo bcm2835_pixelvalve_info = {
    .name = TYPE_BCM2835_PIXELVALVE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2835PixelValveState),
    .instance_init = bcm2835_pixelvalve_init,
    .instance_finalize = bcm2835_pixelvalve_finalize,
    .class_init = bcm2835_pixelvalve_class_init,
};

static void bcm2835_pixelvalve_register_types(void)
{
    type_register_static(&bcm2835_pixelvalve_info);
}

type_init(bcm2835_pixelvalve_register_types)
