/*
 * Microchip MCP3008 8-channel 10-bit SPI ADC
 *
 * The analog inputs are QOM properties ch0..ch7 (0..1023) that can be
 * changed at run time with qom-set, so a host program plays the sensors.
 *
 * Protocol, MSB first, one frame per chip-select: leading zeros, a start
 * bit, SGL/DIFF, D2..D0, one sample clock, a null bit, then B9..B0.
 * The LSB-first repeat that follows on real silicon is not modelled
 * (zeros are shifted out instead).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qom/object.h"

#define TYPE_MCP3008 "mcp3008"
OBJECT_DECLARE_SIMPLE_TYPE(MCP3008State, MCP3008)

#define MCP3008_MAX 1023

struct MCP3008State {
    SSIPeripheral parent_obj;

    uint16_t ch[8];
    uint8_t cmd;        /* start bit and the command bits shifted in so far */
    bool done;          /* command complete, result being (or was) sent */
    uint16_t out;       /* result bits still to shift out */
    uint8_t nout;
};

static uint16_t mcp3008_convert(MCP3008State *s, unsigned cmd)
{
    unsigned d = cmd & 7;

    if (cmd & 8) {
        return s->ch[d];
    }
    /* Pseudo-differential: IN+ is CH(d), IN- its pair; clamps at 0. */
    return s->ch[d] > s->ch[d ^ 1] ? s->ch[d] - s->ch[d ^ 1] : 0;
}

static uint32_t mcp3008_transfer(SSIPeripheral *dev, uint32_t val)
{
    MCP3008State *s = MCP3008(dev);
    uint32_t rx = 0;

    for (int i = 7; i >= 0; i--) {
        bool in = (val >> i) & 1;

        rx <<= 1;
        if (s->nout) {
            rx |= (s->out >> --s->nout) & 1;
        } else if (!s->done && (s->cmd || in)) {
            s->cmd = (s->cmd << 1) | in;
            if (s->cmd & 0x10) {
                /* sample clock and null bit (both 0), then B9..B0 */
                s->out = mcp3008_convert(s, s->cmd & 0xf);
                s->nout = 12;
                s->done = true;
            }
        }
    }
    return rx;
}

static void mcp3008_frame_reset(MCP3008State *s)
{
    s->cmd = 0;
    s->done = false;
    s->nout = 0;
}

static int mcp3008_set_cs(SSIPeripheral *dev, bool level)
{
    mcp3008_frame_reset(MCP3008(dev));
    return 0;
}

static void mcp3008_reset(DeviceState *dev)
{
    mcp3008_frame_reset(MCP3008(dev));
}

static void mcp3008_realize(SSIPeripheral *dev, Error **errp)
{
}

static void mcp3008_get(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    visit_type_uint16(v, name, opaque, errp);
}

static void mcp3008_set(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }
    if (value > MCP3008_MAX) {
        error_setg(errp, "%s must be 0..%d", name, MCP3008_MAX);
        return;
    }
    *(uint16_t *)opaque = value;
}

static void mcp3008_init(Object *obj)
{
    MCP3008State *s = MCP3008(obj);

    for (int i = 0; i < ARRAY_SIZE(s->ch); i++) {
        g_autofree char *name = g_strdup_printf("ch%d", i);

        object_property_add(obj, name, "uint16", mcp3008_get, mcp3008_set,
                            NULL, &s->ch[i]);
    }
}

static const VMStateDescription vmstate_mcp3008 = {
    .name = TYPE_MCP3008,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(parent_obj, MCP3008State),
        VMSTATE_UINT16_ARRAY(ch, MCP3008State, 8),
        VMSTATE_UINT8(cmd, MCP3008State),
        VMSTATE_BOOL(done, MCP3008State),
        VMSTATE_UINT16(out, MCP3008State),
        VMSTATE_UINT8(nout, MCP3008State),
        VMSTATE_END_OF_LIST()
    }
};

static void mcp3008_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = mcp3008_realize;
    k->transfer = mcp3008_transfer;
    k->set_cs = mcp3008_set_cs;
    k->cs_polarity = SSI_CS_LOW;
    dc->vmsd = &vmstate_mcp3008;
    dc->desc = "Microchip MCP3008 8-channel 10-bit ADC";
    device_class_set_legacy_reset(dc, mcp3008_reset);
}

static const TypeInfo mcp3008_info = {
    .name = TYPE_MCP3008,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(MCP3008State),
    .instance_init = mcp3008_init,
    .class_init = mcp3008_class_init,
};

static void mcp3008_register_types(void)
{
    type_register_static(&mcp3008_info);
}

type_init(mcp3008_register_types)
