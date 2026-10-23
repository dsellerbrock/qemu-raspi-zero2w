/*
 * 16x2 HD44780 character LCD behind a PCF8574 I2C backpack ("I2C LCD1602")
 *
 * Each byte written is the PCF8574 port: P0 RS, P1 RW, P2 E, P3 backlight,
 * P4..P7 D4..D7. The HD44780 latches on the falling edge of E, starts in
 * 8-bit mode and is switched to 4-bit mode by the usual init sequence.
 * Modelled: clear, home, entry mode (increment/decrement), display on/off,
 * cursor and blink, cursor shift, function set, CGRAM/DDRAM addressing
 * and data writes in 2-line mode. Display shift and reads of the busy
 * flag/address are not (reads return the port with D4..D7 low, "not busy").
 *
 * Read-only QOM state for a host program to draw the panel: "text"
 * (the 2x16 visible characters as 64 hex digits, HD44780 ROM codes),
 * "cursor" (DDRAM address), "display-on", "cursor-on", "blink" and
 * "backlight" (0/1).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_LCD1602_I2C "lcd1602-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(LCD1602State, LCD1602_I2C)

#define P_RS    0x01
#define P_RW    0x02
#define P_E     0x04
#define P_BL    0x08

struct LCD1602State {
    I2CSlave parent_obj;

    uint8_t port;
    uint8_t ddram[0x80];
    uint8_t cgram[64];
    uint8_t addr;
    uint8_t cg;         /* addr points into CGRAM */
    uint8_t dl8;        /* 8-bit interface (power-on state) */
    uint8_t half;       /* 4-bit mode: high nibble latched, low pending */
    uint8_t hi;
    uint8_t inc;
    /* 0/1 flags; uint8 so QOM can expose them by pointer */
    uint8_t display_on;
    uint8_t cursor_on;
    uint8_t blink;
    uint8_t backlight;
};

static void lcd_step(LCD1602State *s, bool up)
{
    if (s->cg) {
        s->addr = (s->addr + (up ? 1 : -1)) & 63;
    } else if (up) {
        s->addr = s->addr == 0x27 ? 0x40 : s->addr == 0x67 ? 0 : s->addr + 1;
    } else {
        s->addr = s->addr == 0x40 ? 0x27 : s->addr == 0 ? 0x67 : s->addr - 1;
    }
}

static void lcd_clear(LCD1602State *s)
{
    memset(s->ddram, ' ', sizeof(s->ddram));
    s->addr = 0;
    s->cg = 0;
    s->inc = 1;
}

static void lcd_exec(LCD1602State *s, bool rs, uint8_t v)
{
    if (rs) {
        if (s->cg) {
            s->cgram[s->addr & 63] = v;
        } else {
            s->ddram[s->addr & 0x7f] = v;
        }
        lcd_step(s, s->inc);
    } else if (v & 0x80) {          /* set DDRAM address */
        s->addr = v & 0x7f;
        s->cg = 0;
    } else if (v & 0x40) {          /* set CGRAM address */
        s->addr = v & 0x3f;
        s->cg = 1;
    } else if (v & 0x20) {          /* function set */
        s->dl8 = !!(v & 0x10);
        s->half = 0;
    } else if (v & 0x10) {          /* cursor/display shift */
        if (!(v & 0x08)) {
            s->cg = 0;
            lcd_step(s, v & 0x04);
        }
    } else if (v & 0x08) {          /* display on/off control */
        s->display_on = !!(v & 0x04);
        s->cursor_on = !!(v & 0x02);
        s->blink = v & 0x01;
    } else if (v & 0x04) {          /* entry mode set */
        s->inc = !!(v & 0x02);
    } else if (v & 0x02) {          /* return home */
        s->addr = 0;
        s->cg = 0;
    } else if (v & 0x01) {          /* clear display */
        lcd_clear(s);
    }
}

static int lcd1602_send(I2CSlave *i2c, uint8_t data)
{
    LCD1602State *s = LCD1602_I2C(i2c);
    uint8_t old = s->port;

    s->port = data;
    s->backlight = !!(data & P_BL);
    if ((old & P_E) && !(data & P_E) && !(old & P_RW)) {
        uint8_t nib = old >> 4;

        if (s->dl8) {
            lcd_exec(s, old & P_RS, nib << 4);
        } else if (!s->half) {
            s->hi = nib;
            s->half = 1;
        } else {
            s->half = 0;
            lcd_exec(s, old & P_RS, (s->hi << 4) | nib);
        }
    }
    return 0;
}

static uint8_t lcd1602_recv(I2CSlave *i2c)
{
    return LCD1602_I2C(i2c)->port & 0x0f;
}

static char *lcd1602_get_text(Object *obj, Error **errp)
{
    LCD1602State *s = LCD1602_I2C(obj);
    GString *hex = g_string_sized_new(65);

    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 16; col++) {
            g_string_append_printf(hex, "%02x", s->ddram[row * 0x40 + col]);
        }
    }
    return g_string_free(hex, false);
}

static void lcd1602_reset(DeviceState *dev)
{
    LCD1602State *s = LCD1602_I2C(dev);

    lcd_clear(s);
    memset(s->cgram, 0, sizeof(s->cgram));
    s->port = 0;
    s->dl8 = 1;
    s->half = 0;
    s->display_on = 0;
    s->cursor_on = 0;
    s->blink = 0;
    s->backlight = 0;
}

static void lcd1602_init(Object *obj)
{
    LCD1602State *s = LCD1602_I2C(obj);

    object_property_add_str(obj, "text", lcd1602_get_text, NULL);
    object_property_add_uint8_ptr(obj, "cursor", &s->addr, OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "display-on", &s->display_on,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "cursor-on", &s->cursor_on,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "blink", &s->blink, OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "backlight", &s->backlight,
                                  OBJ_PROP_FLAG_READ);
}

static const VMStateDescription vmstate_lcd1602 = {
    .name = TYPE_LCD1602_I2C,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, LCD1602State),
        VMSTATE_UINT8(port, LCD1602State),
        VMSTATE_UINT8_ARRAY(ddram, LCD1602State, 0x80),
        VMSTATE_UINT8_ARRAY(cgram, LCD1602State, 64),
        VMSTATE_UINT8(addr, LCD1602State),
        VMSTATE_UINT8(cg, LCD1602State),
        VMSTATE_UINT8(dl8, LCD1602State),
        VMSTATE_UINT8(half, LCD1602State),
        VMSTATE_UINT8(hi, LCD1602State),
        VMSTATE_UINT8(inc, LCD1602State),
        VMSTATE_UINT8(display_on, LCD1602State),
        VMSTATE_UINT8(cursor_on, LCD1602State),
        VMSTATE_UINT8(blink, LCD1602State),
        VMSTATE_UINT8(backlight, LCD1602State),
        VMSTATE_END_OF_LIST()
    }
};

static void lcd1602_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->send = lcd1602_send;
    k->recv = lcd1602_recv;
    dc->vmsd = &vmstate_lcd1602;
    dc->desc = "16x2 HD44780 LCD on a PCF8574 I2C backpack";
    device_class_set_legacy_reset(dc, lcd1602_reset);
}

static const TypeInfo lcd1602_info = {
    .name = TYPE_LCD1602_I2C,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(LCD1602State),
    .instance_init = lcd1602_init,
    .class_init = lcd1602_class_init,
};

static void lcd1602_register_types(void)
{
    type_register_static(&lcd1602_info);
}

type_init(lcd1602_register_types)
