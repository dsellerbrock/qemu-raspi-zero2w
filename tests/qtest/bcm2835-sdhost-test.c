/*
 * QTests for the BCM2835 SD Host controller.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define GPIO_BASE       0x3f200000
#define GPIO_FSEL4      (GPIO_BASE + 0x10)
#define GPIO_FSEL5      (GPIO_BASE + 0x14)
#define SDHOST_BASE     0x3f202000
#define SDCMD           (SDHOST_BASE + 0x00)
#define SDARG           (SDHOST_BASE + 0x04)
#define SDRSP0          (SDHOST_BASE + 0x10)
#define SDHSTS          (SDHOST_BASE + 0x20)
#define SDHBCT          (SDHOST_BASE + 0x3c)
#define SDDATA          (SDHOST_BASE + 0x40)
#define SDHBLC          (SDHOST_BASE + 0x50)
#define SDHCFG          (SDHOST_BASE + 0x38)

#define SDCMD_NEW       (1U << 15)
#define SDCMD_LONG      (1U << 9)
#define SDCMD_NO_RESP   (1U << 10)
#define SDCMD_BUSYWAIT  (1U << 11)
#define SDCMD_READ      (1U << 6)
#define SDCMD_WRITE     (1U << 7)
#define SDCMD_TIMEOUT   (1U << 6)
#define SDHCFG_DATA_IRPT_EN (1U << 4)
#define SDHCFG_BLOCK_IRPT_EN (1U << 8)
#define SDHCFG_BUSY_IRPT_EN (1U << 10)
#define SDHSTS_SDIO_IRPT (1U << 8)
#define SDHSTS_BLOCK_IRPT (1U << 9)
#define SDHSTS_BUSY_IRPT (1U << 10)
#define SDHSTS_DATA_FLAG (1U << 0)

static char *sd_image;

static void send_command(unsigned command, uint32_t argument, uint32_t flags)
{
    writel(SDHSTS, UINT32_MAX);
    writel(SDARG, argument);
    writel(SDCMD, SDCMD_NEW | flags | command);
    if (!(flags & SDCMD_NO_RESP)) {
        g_assert_cmphex(readl(SDHSTS) & SDCMD_TIMEOUT, ==, 0);
    }
}

static void test_cmd18_bulk_fifo_read(void)
{
    uint32_t rca;
    uint32_t actual;

    qtest_irq_intercept_out_named(global_qtest,
                                  "/machine/soc/peripherals/sdhost",
                                  "sysbus-irq");

    /* Route GPIO 48-53 to SDHOST rather than the default SDHCI bus. */
    writel(GPIO_FSEL4, (4U << 24) | (4U << 27));
    writel(GPIO_FSEL5, (4U << 0) | (4U << 3) | (4U << 6) | (4U << 9));

    send_command(0, 0, SDCMD_NO_RESP);
    send_command(8, 0x1aa, 0);
    send_command(55, 0, 0);
    send_command(41, 0x00ff8000, 0);
    send_command(2, 0, SDCMD_LONG);
    send_command(3, 0, 0);
    rca = readl(SDRSP0) & 0xffff0000;
    g_assert_cmphex(rca, !=, 0);
    send_command(7, rca, 0);
    send_command(16, 512, 0);

    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    send_command(18, 0, SDCMD_READ);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(SDHCFG, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_SDIO_IRPT, !=, 0);
    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    g_assert_true(qtest_get_irq(global_qtest, 0));

    for (unsigned word = 0; word < 1024 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        actual = readl(SDDATA);
        g_assert_cmphex(actual, ==, expected);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* Stop the multi-block read before starting another command. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    send_command(12, 0, SDCMD_BUSYWAIT);
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BUSY_IRPT, !=, 0);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(SDHCFG, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BUSY_IRPT, !=, 0);
    writel(SDHSTS, SDHSTS_BUSY_IRPT);

    writel(SDHCFG, SDHCFG_BLOCK_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    send_command(25, 0, SDCMD_WRITE);
    g_assert_cmphex(readl(SDHSTS) & (SDHSTS_BLOCK_IRPT | SDHSTS_DATA_FLAG), ==, 0);
    for (unsigned word = 0; word < 256; word++) {
        writel(SDDATA, 0x5a5aa5a5);
        if (word == 126) {
            g_assert_cmphex(readl(SDHSTS) & SDHSTS_BLOCK_IRPT, ==, 0);
        } else if (word == 127) {
            g_assert_cmphex(readl(SDHSTS) & SDHSTS_BLOCK_IRPT, !=, 0);
            g_assert_true(qtest_get_irq(global_qtest, 0));
            writel(SDHSTS, SDHSTS_BLOCK_IRPT);
            g_assert_false(qtest_get_irq(global_qtest, 0));
        }
    }
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BLOCK_IRPT, !=, 0);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(SDHCFG, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BLOCK_IRPT, !=, 0);
}

static void create_sd_image(void)
{
    uint8_t data[1024];
    GError *error = NULL;
    int fd;

    sd_image = g_build_filename(g_get_tmp_dir(), "qtest-sdhost-XXXXXX", NULL);
    fd = g_mkstemp(sd_image);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    for (unsigned i = 0; i < sizeof(data); i++) {
        data[i] = (i * 37 + 11) & 0xff;
    }
    g_assert_true(g_file_set_contents(sd_image, (char *)data,
                                      sizeof(data), &error));
    g_assert_no_error(error);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    create_sd_image();
    qtest_add_func("/bcm2835/sdhost/cmd18_bulk_fifo_read",
                   test_cmd18_bulk_fifo_read);
    g_autofree char *args = g_strdup_printf(
        "-machine raspi3ap -smp 4 -drive if=sd,file=%s,format=raw,snapshot=on",
        sd_image);
    qtest_start(args);
    ret = g_test_run();
    qtest_end();
    unlink(sd_image);
    g_free(sd_image);
    return ret;
}
