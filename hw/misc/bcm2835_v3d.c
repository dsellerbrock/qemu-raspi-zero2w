/*
 * Raspberry Pi VideoCore IV V3D register block.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/misc/bcm2835_v3d.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define V3D_IDENT0 0x000
#define V3D_IDENT1 0x004
#define V3D_IDENT2 0x008

#define V3D_IDENT0_VALUE 0x02443356
/* VC4: 12 KiB VPM, 16 semaphores, 1 TMU/slice, 4 QPUs/slice, 3 slices. */
#define V3D_IDENT1_VALUE 0xc0101432

static void bcm2835_v3d_update_irq(BCM2835V3DState *s)
{
    qemu_set_irq(s->irq, (s->irq_status & s->irq_enable) != 0);
}

static uint64_t bcm2835_v3d_read(void *opaque, hwaddr offset, unsigned size)
{
    BCM2835V3DState *s = opaque;

    assert(size == 4);
    switch (offset) {
    case V3D_IDENT0:
        return V3D_IDENT0_VALUE;
    case V3D_IDENT1:
        return V3D_IDENT1_VALUE;
    case V3D_IDENT2:
        return 0;
    case BCM2835_V3D_INTCTL:
        return s->irq_status;
    case BCM2835_V3D_INTENA:
        return s->irq_enable;
    case BCM2835_V3D_INTDIS:
        return 0;
    default:
        return s->regs[offset >> 2];
    }
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
        s->irq_enable |= value32;
        bcm2835_v3d_update_irq(s);
        break;
    case BCM2835_V3D_INTDIS:
        s->irq_enable &= ~value32;
        bcm2835_v3d_update_irq(s);
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
    s->irq_status = 0;
    s->irq_enable = 0;
    bcm2835_v3d_update_irq(s);
}

static int bcm2835_v3d_post_load(void *opaque, int version_id)
{
    BCM2835V3DState *s = opaque;

    bcm2835_v3d_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_bcm2835_v3d = {
    .name = TYPE_BCM2835_V3D,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = bcm2835_v3d_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, BCM2835V3DState,
                             BCM2835_V3D_REG_SIZE / sizeof(uint32_t)),
        VMSTATE_UINT32(irq_status, BCM2835V3DState),
        VMSTATE_UINT32(irq_enable, BCM2835V3DState),
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
