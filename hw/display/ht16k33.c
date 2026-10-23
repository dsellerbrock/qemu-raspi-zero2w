/*
 * Holtek HT16K33 LED matrix driver (as on Adafruit 7-segment backpacks)
 *
 * Models the command set a display driver uses: display data address
 * pointer + RAM writes/reads, system setup (oscillator), display setup
 * (on, blink) and dimming. Key scan is not modelled. The state is
 * read-only QOM: "ram" (16 bytes as 32 hex digits), "oscillator",
 * "display-on" (0/1), "blink" (0..3) and "brightness" (0..15), so a host
 * program can draw the display.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_HT16K33 "ht16k33"
OBJECT_DECLARE_SIMPLE_TYPE(HT16K33State, HT16K33)

struct HT16K33State {
    I2CSlave parent_obj;

    uint8_t ram[16];
    uint8_t ptr;
    uint8_t oscillator;  /* 0/1; uint8 so QOM can expose it by pointer */
    uint8_t display_on;
    uint8_t blink;
    uint8_t brightness;
    bool first;         /* next byte written is a command */
    bool ram_write;     /* the command was an address pointer */
};

static int ht16k33_event(I2CSlave *i2c, enum i2c_event event)
{
    HT16K33State *s = HT16K33(i2c);

    if (event == I2C_START_SEND) {
        s->first = true;
        s->ram_write = false;
    }
    return 0;
}

static int ht16k33_send(I2CSlave *i2c, uint8_t data)
{
    HT16K33State *s = HT16K33(i2c);

    if (!s->first) {
        if (s->ram_write) {
            s->ram[s->ptr] = data;
            s->ptr = (s->ptr + 1) & 15;
        }
        return 0;
    }
    s->first = false;
    switch (data >> 4) {
    case 0x0:   /* display data address pointer */
        s->ptr = data & 15;
        s->ram_write = true;
        break;
    case 0x2:   /* system setup */
        s->oscillator = data & 1;
        break;
    case 0x8:   /* display setup */
        s->display_on = data & 1;
        s->blink = (data >> 1) & 3;
        break;
    case 0xe:   /* dimming */
        s->brightness = data & 15;
        break;
    default:    /* ROW/INT set, key data: not modelled */
        break;
    }
    return 0;
}

static uint8_t ht16k33_recv(I2CSlave *i2c)
{
    HT16K33State *s = HT16K33(i2c);
    uint8_t v = s->ram[s->ptr];

    s->ptr = (s->ptr + 1) & 15;
    return v;
}

static char *ht16k33_get_ram(Object *obj, Error **errp)
{
    HT16K33State *s = HT16K33(obj);
    GString *hex = g_string_sized_new(33);

    for (int i = 0; i < ARRAY_SIZE(s->ram); i++) {
        g_string_append_printf(hex, "%02x", s->ram[i]);
    }
    return g_string_free(hex, false);
}

static void ht16k33_reset(DeviceState *dev)
{
    HT16K33State *s = HT16K33(dev);

    memset(s->ram, 0, sizeof(s->ram));
    s->ptr = 0;
    s->oscillator = 0;
    s->display_on = 0;
    s->blink = 0;
    s->brightness = 15;
}

static void ht16k33_init(Object *obj)
{
    HT16K33State *s = HT16K33(obj);

    object_property_add_str(obj, "ram", ht16k33_get_ram, NULL);
    object_property_add_uint8_ptr(obj, "oscillator", &s->oscillator,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "display-on", &s->display_on,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "blink", &s->blink,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "brightness", &s->brightness,
                                  OBJ_PROP_FLAG_READ);
}

static const VMStateDescription vmstate_ht16k33 = {
    .name = TYPE_HT16K33,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, HT16K33State),
        VMSTATE_UINT8_ARRAY(ram, HT16K33State, 16),
        VMSTATE_UINT8(ptr, HT16K33State),
        VMSTATE_UINT8(oscillator, HT16K33State),
        VMSTATE_UINT8(display_on, HT16K33State),
        VMSTATE_UINT8(blink, HT16K33State),
        VMSTATE_UINT8(brightness, HT16K33State),
        VMSTATE_BOOL(first, HT16K33State),
        VMSTATE_BOOL(ram_write, HT16K33State),
        VMSTATE_END_OF_LIST()
    }
};

static void ht16k33_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = ht16k33_event;
    k->send = ht16k33_send;
    k->recv = ht16k33_recv;
    dc->vmsd = &vmstate_ht16k33;
    dc->desc = "Holtek HT16K33 LED driver (7-segment backpack)";
    device_class_set_legacy_reset(dc, ht16k33_reset);
}

static const TypeInfo ht16k33_info = {
    .name = TYPE_HT16K33,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(HT16K33State),
    .instance_init = ht16k33_init,
    .class_init = ht16k33_class_init,
};

static void ht16k33_register_types(void)
{
    type_register_static(&ht16k33_info);
}

type_init(ht16k33_register_types)
