/*
 * QTest for the MCP3008 ADC on raspi3ap SPI0 CE0
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "hw/ssi/bcm2835_spi.h"

#define SPI0_BASE   0x3f204000
#define GPIO_BASE   0x3f200000
#define GPFSEL0     0x00
#define GPSET0      0x1c
#define GPCLR0      0x28
#define CE0_PIN     8

static void set_ch(QTestState *qts, int ch, int value)
{
    g_autofree char *prop = g_strdup_printf("ch%d", ch);

    qtest_qmp_assert_success(qts, "{'execute': 'qom-set', 'arguments': "
                             "{'path': '/machine/mcp3008', 'property': %s, "
                             "'value': %d}}", prop, value);
}

static void cs(QTestState *qts, bool select)
{
    qtest_writel(qts, GPIO_BASE + (select ? GPCLR0 : GPSET0), 1u << CE0_PIN);
}

/* One SPI frame: tx[3] out, rx[3] back, CE0 low around it as Linux does. */
static void xfer(QTestState *qts, bool select, const uint8_t *tx, uint8_t *rx)
{
    if (select) {
        cs(qts, true);
    }
    qtest_writel(qts, SPI0_BASE + BCM2835_SPI_CS, BCM2835_SPI_CS_TA |
                 BCM2835_SPI_CLEAR_TX | BCM2835_SPI_CLEAR_RX);
    for (int i = 0; i < 3; i++) {
        qtest_writel(qts, SPI0_BASE + BCM2835_SPI_FIFO, tx[i]);
    }
    for (int i = 0; i < 3; i++) {
        rx[i] = qtest_readl(qts, SPI0_BASE + BCM2835_SPI_FIFO);
    }
    qtest_writel(qts, SPI0_BASE + BCM2835_SPI_CS, 0);
    cs(qts, false);
}

/* gpiozero's MCP3008 frame: 00000001 MCCCxxxx xxxxxxxx */
static int read_gpiozero(QTestState *qts, bool single, int ch)
{
    uint8_t tx[3] = { 0x01, (single << 7) | (ch << 4), 0 }, rx[3];

    xfer(qts, true, tx, rx);
    g_assert_cmpint(rx[1] & 0x04, ==, 0);   /* null bit */
    return ((rx[1] & 3) << 8) | rx[2];
}

static void test_mcp3008(void)
{
    QTestState *qts = qtest_init("-machine raspi3ap");
    uint8_t rx[3];
    QDict *rsp;

    /* GPIO8 is an output driven high (deselected), as Linux leaves CE0 */
    qtest_writel(qts, GPIO_BASE + GPFSEL0, 1u << (3 * CE0_PIN));
    cs(qts, false);

    set_ch(qts, 3, 777);
    set_ch(qts, 7, 1023);
    g_assert_cmpint(read_gpiozero(qts, true, 3), ==, 777);
    g_assert_cmpint(read_gpiozero(qts, true, 7), ==, 1023);
    g_assert_cmpint(read_gpiozero(qts, true, 0), ==, 0);

    /* a new value is seen by the next conversion */
    set_ch(qts, 3, 512);
    g_assert_cmpint(read_gpiozero(qts, true, 3), ==, 512);

    /* start bit at byte 0 bit 6: 0 1 1 011 | sample, null | B9..B0 */
    xfer(qts, true, (const uint8_t[]) { 0x6c, 0, 0 }, rx);
    g_assert_cmpint((rx[1] << 2) | (rx[2] >> 6), ==, 512);

    /* pseudo-differential: CH0+/CH1- and CH0-/CH1+ (clamped at 0) */
    set_ch(qts, 0, 300);
    set_ch(qts, 1, 100);
    g_assert_cmpint(read_gpiozero(qts, false, 0), ==, 200);
    g_assert_cmpint(read_gpiozero(qts, false, 1), ==, 0);

    /* CE0 high: the chip ignores the bus */
    xfer(qts, false, (const uint8_t[]) { 0x01, 0xf0, 0 }, rx);
    g_assert_cmpint(rx[1] | rx[2], ==, 0);

    /* qom-get reads back; out-of-range values are refused */
    rsp = qtest_qmp(qts, "{'execute': 'qom-get', 'arguments': "
                    "{'path': '/machine/mcp3008', 'property': 'ch7'}}");
    g_assert_cmpint(qdict_get_int(rsp, "return"), ==, 1023);
    qobject_unref(rsp);
    rsp = qtest_qmp(qts, "{'execute': 'qom-set', 'arguments': "
                    "{'path': '/machine/mcp3008', 'property': 'ch0', "
                    "'value': 1024}}");
    g_assert(qdict_haskey(rsp, "error"));
    qobject_unref(rsp);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/raspi/mcp3008", test_mcp3008);
    return g_test_run();
}
