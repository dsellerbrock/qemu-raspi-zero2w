/*
 * QTests for the Raspberry Pi HDMI display connection.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define GPIO_BASE       0x3f200000
#define GPIO_LEV0       (GPIO_BASE + 0x34)
#define HDMI_HPD_GPIO   (1U << 28)

#define BSC2_BASE       0x3f805000
#define BSC_C           (BSC2_BASE + 0x00)
#define BSC_S           (BSC2_BASE + 0x04)
#define BSC_DLEN        (BSC2_BASE + 0x08)
#define BSC_A           (BSC2_BASE + 0x0c)
#define BSC_FIFO        (BSC2_BASE + 0x10)
#define BSC_C_I2CEN     (1U << 15)
#define BSC_C_INTD      (1U << 8)
#define BSC_C_INTT      (1U << 9)
#define BSC_C_INTR      (1U << 10)
#define BSC_C_ST        (1U << 7)
#define BSC_C_READ      (1U << 0)
#define BSC_S_ERR       (1U << 8)
#define BSC_S_TA        (1U << 0)
#define BSC_S_DONE      (1U << 1)

static void test_hdmi_hpd_and_edid(void)
{
    uint8_t edid[128];
    static const uint8_t header[] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
    };
    unsigned checksum = 0;

    /* The Zero 2 W device tree wires HDMI HPD as GPIO_ACTIVE_LOW. */
    g_assert_cmphex(readl(GPIO_LEV0) & HDMI_HPD_GPIO, ==, 0);

    writel(BSC_S, BSC_S_DONE | BSC_S_ERR);
    writel(BSC_DLEN, 1);
    writel(BSC_A, 0x50);
    /* Linux sets INTD only on the last message, to request a repeated start. */
    writel(BSC_C, BSC_C_I2CEN | BSC_C_ST | BSC_C_INTT);
    writel(BSC_FIFO, 0);
    g_assert_cmphex(readl(BSC_S) & BSC_S_ERR, ==, 0);
    g_assert_cmphex(readl(BSC_S) & BSC_S_DONE, ==, 0);
    g_assert_cmphex(readl(BSC_S) & BSC_S_TA, !=, 0);

    writel(BSC_DLEN, sizeof(edid));
    writel(BSC_C, BSC_C_I2CEN | BSC_C_ST | BSC_C_READ |
                 BSC_C_INTR | BSC_C_INTD);
    for (size_t i = 0; i < sizeof(edid); i++) {
        edid[i] = readl(BSC_FIFO);
        checksum += edid[i];
    }

    g_assert_cmphex(readl(BSC_S) & BSC_S_ERR, ==, 0);
    g_assert_cmphex(readl(BSC_S) & BSC_S_DONE, !=, 0);
    g_assert_cmpmem(edid, sizeof(header), header, sizeof(header));
    g_assert_cmphex(checksum & 0xff, ==, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/display/hpd_and_edid", test_hdmi_hpd_and_edid);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();

    return ret;
}
