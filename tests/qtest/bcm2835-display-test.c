/*
 * QTests for the Raspberry Pi HDMI display connection.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "hw/arm/raspi_platform.h"

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

#define HVS_BASE         0x3f400000
#define HVS_DLIST_START  0x2000
#define HVS_GLOBAL_EN    (1U << 31)
#define HVS_CHANNEL_EN   (1U << 31)

#define PIXELVALVE2_BASE  0x3f807000
#define PV_CONTROL        (PIXELVALVE2_BASE + 0x00)
#define PV_V_CONTROL      (PIXELVALVE2_BASE + 0x04)
#define PV_INTEN          (PIXELVALVE2_BASE + 0x24)
#define PV_INTSTAT        (PIXELVALVE2_BASE + 0x28)
#define PV_INT_VFP_START  (1U << 7)
#define IRQ_BASE          0x3f00b200
#define IRQ_PENDING_2     (IRQ_BASE + 0x08)
#define IRQ_ENABLE_2      (IRQ_BASE + 0x14)
#define PV2_IRQ_BIT       (1U << (INTERRUPT_PIXELVALVE2 - 32))

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

static void test_hvs_display_list_ram(void)
{
    const uint32_t word = 0x12345678;

    writel(HVS_BASE + HVS_DLIST_START, word);
    g_assert_cmphex(readl(HVS_BASE + HVS_DLIST_START), ==, word);
}

static void test_hvs_primary_plane(void)
{
    uint32_t pixels[4] = { 0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff };
    uint32_t dlist[8] = {
        (1U << 30) | (7U << 24) | (1U << 4) | 7,
        0,
        (2U << 16) | 2,
        0xc0c0c0c0,
        0xc0100000,
        0xc0c0c0c0,
        8,
        1U << 31,
    };
    unsigned i;

    qtest_memwrite(global_qtest, 0x00100000, pixels, sizeof(pixels));
    for (i = 0; i < ARRAY_SIZE(dlist); i++) {
        writel(HVS_BASE + HVS_DLIST_START + (32 + i) * sizeof(uint32_t),
               dlist[i]);
    }
    writel(HVS_BASE, HVS_GLOBAL_EN);
    writel(HVS_BASE + 0x28, 32);
    writel(HVS_BASE + 0x60, HVS_CHANNEL_EN | (2U << 12) | 2);

    g_assert_cmphex(readl(HVS_BASE + 0x38), ==, 32);
    g_assert_cmphex(readl(HVS_BASE + 0x68) >> 30, ==, 2);
}

static void test_pixelvalve_vblank_irq(void)
{
    writel(IRQ_ENABLE_2, PV2_IRQ_BIT);
    writel(PV_INTEN, PV_INT_VFP_START);
    writel(PV_CONTROL, 1);
    writel(PV_V_CONTROL, 1);

    clock_step(20 * 1000 * 1000);
    g_assert_cmphex(readl(PV_INTSTAT) & PV_INT_VFP_START, !=, 0);
    g_assert_cmphex(readl(IRQ_PENDING_2) & PV2_IRQ_BIT, !=, 0);

    writel(PV_INTSTAT, PV_INT_VFP_START);
    g_assert_cmphex(readl(PV_INTSTAT) & PV_INT_VFP_START, ==, 0);
    g_assert_cmphex(readl(IRQ_PENDING_2) & PV2_IRQ_BIT, ==, 0);

    clock_step(20 * 1000 * 1000);
    g_assert_cmphex(readl(PV_INTSTAT) & PV_INT_VFP_START, !=, 0);
    g_assert_cmphex(readl(IRQ_PENDING_2) & PV2_IRQ_BIT, !=, 0);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/display/hpd_and_edid", test_hdmi_hpd_and_edid);
    qtest_add_func("/bcm2835/display/hvs_display_list_ram",
                   test_hvs_display_list_ram);
    qtest_add_func("/bcm2835/display/hvs_primary_plane",
                   test_hvs_primary_plane);
    qtest_add_func("/bcm2835/display/pixelvalve_vblank_irq",
                   test_pixelvalve_vblank_irq);
    if (g_strcmp0(g_getenv("QEMU_TEST_V3D_FRAGMENT_UNLOCK"), "1") == 0) {
        qtest_start("-machine raspi3ap -smp 4 "
                    "-global bcm2835_v3d.fragment-workers=4 "
                    "-global bcm2835_v3d.fragment-unlock=on");
    } else if (g_strcmp0(g_getenv("QEMU_TEST_V3D_FRAGMENT_WORKERS"), "4") == 0) {
        qtest_start("-machine raspi3ap -smp 4 "
                    "-global bcm2835_v3d.fragment-workers=4");
    } else {
        qtest_start("-machine raspi3ap -smp 4");
    }
    ret = g_test_run();
    qtest_end();

    return ret;
}
