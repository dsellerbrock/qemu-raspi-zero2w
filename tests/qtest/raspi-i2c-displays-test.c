/*
 * QTest for the raspi3ap I2C1 displays: HT16K33 at 0x70, LCD1602 at 0x27
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/i2c/bcm2835_i2c.h"

#define BSC1 0x3f804000

static void bsc_start(QTestState *qts, uint8_t addr, int len, bool read)
{
    qtest_writel(qts, BSC1 + BCM2835_I2C_A, addr);
    qtest_writel(qts, BSC1 + BCM2835_I2C_DLEN, len);
    /* INTD as Linux's i2c-bcm2835 sets it: the model completes on it */
    qtest_writel(qts, BSC1 + BCM2835_I2C_C, BCM2835_I2C_C_I2CEN |
                 BCM2835_I2C_C_INTD | BCM2835_I2C_C_ST |
                 BCM2835_I2C_C_CLEAR | read);
}

static void bsc_done(QTestState *qts)
{
    g_assert_true(qtest_readl(qts, BSC1 + BCM2835_I2C_S) & BCM2835_I2C_S_DONE);
    g_assert_false(qtest_readl(qts, BSC1 + BCM2835_I2C_S) &
                   BCM2835_I2C_S_ERR);
    qtest_writel(qts, BSC1 + BCM2835_I2C_S, BCM2835_I2C_S_DONE |
                 BCM2835_I2C_S_ERR | BCM2835_I2C_S_CLKT);
}

/* One I2C write transaction, as smbus write_i2c_block_data/write_byte do */
static void i2c_write(QTestState *qts, uint8_t addr, const uint8_t *b, int n)
{
    bsc_start(qts, addr, n, false);
    for (int i = 0; i < n; i++) {
        qtest_writel(qts, BSC1 + BCM2835_I2C_FIFO, b[i]);
    }
    bsc_done(qts);
}

#define WRITE(qts, addr, ...) do { \
        const uint8_t b_[] = { __VA_ARGS__ }; \
        i2c_write(qts, addr, b_, sizeof(b_)); \
    } while (0)

static QDict *props(QTestState *qts, const char *path)
{
    return qtest_qmp(qts, "{'execute': 'qom-list-get', 'arguments': "
                     "{'paths': [%s]}}", path);
}

static int64_t prop_int(QTestState *qts, const char *path, const char *name)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': %s, 'property': %s}}", path, name);
    int64_t v = qdict_get_int(rsp, "return");

    qobject_unref(rsp);
    return v;
}

static char *prop_str(QTestState *qts, const char *path, const char *name)
{
    QDict *rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                           "{'path': %s, 'property': %s}}", path, name);
    char *v = g_strdup(qdict_get_str(rsp, "return"));

    qobject_unref(rsp);
    return v;
}

static void test_ht16k33(void)
{
    QTestState *qts = qtest_init("-machine raspi3ap");
    const char *p = "/machine/ht16k33";
    g_autofree char *ram = NULL;
    QDict *rsp;

    g_assert_cmpint(prop_int(qts, p, "oscillator"), ==, 0);
    WRITE(qts, 0x70, 0x21);                     /* oscillator on */
    WRITE(qts, 0x70, 0x81);                     /* display on, no blink */
    WRITE(qts, 0x70, 0xe5);                     /* brightness 5 */
    /* Adafruit 4-digit layout "12:34": digits at 0, 2, 6, 8, colon at 4 */
    WRITE(qts, 0x70, 0x00, 0x06, 0x00, 0x5b, 0x00, 0x02, 0x00, 0x4f, 0x00,
          0x66, 0x00);
    ram = prop_str(qts, p, "ram");
    g_assert_cmpstr(ram, ==, "06005b0002004f006600000000000000");
    g_assert_cmpint(prop_int(qts, p, "oscillator"), ==, 1);
    g_assert_cmpint(prop_int(qts, p, "display-on"), ==, 1);
    g_assert_cmpint(prop_int(qts, p, "blink"), ==, 0);
    g_assert_cmpint(prop_int(qts, p, "brightness"), ==, 5);

    WRITE(qts, 0x70, 0x83);                     /* blink 2 Hz */
    g_assert_cmpint(prop_int(qts, p, "blink"), ==, 1);

    /* RAM reads back from the address pointer */
    WRITE(qts, 0x70, 0x02);
    bsc_start(qts, 0x70, 2, true);
    g_assert_cmpint(qtest_readl(qts, BSC1 + BCM2835_I2C_FIFO), ==, 0x5b);
    g_assert_cmpint(qtest_readl(qts, BSC1 + BCM2835_I2C_FIFO), ==, 0x00);
    bsc_done(qts);

    /* all of it in one call, as Zero Lab polls it */
    rsp = props(qts, p);
    g_assert(qdict_haskey(rsp, "return"));
    qobject_unref(rsp);
    qtest_quit(qts);
}

#define BL 0x08
#define EN 0x04

/* One HD44780 nibble through the PCF8574: E high, then E low latches it */
static void lcd_nibble(QTestState *qts, bool rs, uint8_t n)
{
    uint8_t port = (n << 4) | BL | rs;

    WRITE(qts, 0x27, port | EN, port);
}

static void lcd_byte(QTestState *qts, bool rs, uint8_t v)
{
    lcd_nibble(qts, rs, v >> 4);
    lcd_nibble(qts, rs, v & 15);
}

static void lcd_str(QTestState *qts, const char *s)
{
    while (*s) {
        lcd_byte(qts, true, *s++);
    }
}

static void test_lcd1602(void)
{
    QTestState *qts = qtest_init("-machine raspi3ap");
    const char *p = "/machine/lcd1602";
    g_autoptr(GString) want = g_string_new("");
    g_autofree char *text = NULL;
    const char *rows[2] = { "Hello", "Y" };

    /* the usual 4-bit init: 3, 3, 3, 2 as 8-bit nibbles */
    lcd_nibble(qts, false, 3);
    lcd_nibble(qts, false, 3);
    lcd_nibble(qts, false, 3);
    lcd_nibble(qts, false, 2);
    lcd_byte(qts, false, 0x28);     /* 4-bit, 2 lines */
    lcd_byte(qts, false, 0x0c);     /* display on, cursor off */
    lcd_byte(qts, false, 0x06);     /* increment */
    lcd_byte(qts, false, 0x01);     /* clear */
    lcd_str(qts, "Hello");
    lcd_byte(qts, false, 0x40);     /* CGRAM: not shown in the text */
    lcd_byte(qts, true, 0x1f);
    lcd_byte(qts, false, 0xa7);     /* DDRAM 0x27, last of row 0 */
    lcd_str(qts, "XY");             /* X at 0x27, then wraps to 0x40 */

    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 16; c++) {
            g_string_append_printf(want, "%02x",
                                   c < strlen(rows[r]) ? rows[r][c] : ' ');
        }
    }
    text = prop_str(qts, p, "text");
    g_assert_cmpstr(text, ==, want->str);
    g_assert_cmpint(prop_int(qts, p, "cursor"), ==, 0x41);
    g_assert_cmpint(prop_int(qts, p, "display-on"), ==, 1);
    g_assert_cmpint(prop_int(qts, p, "cursor-on"), ==, 0);
    g_assert_cmpint(prop_int(qts, p, "backlight"), ==, 1);

    /* backlight follows P3 on every write */
    WRITE(qts, 0x27, 0x00);
    g_assert_cmpint(prop_int(qts, p, "backlight"), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/raspi/i2c/ht16k33", test_ht16k33);
    qtest_add_func("/raspi/i2c/lcd1602", test_lcd1602);
    return g_test_run();
}
