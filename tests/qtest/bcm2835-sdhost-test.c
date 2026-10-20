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
#define DMA_BASE        0x3f007000
#define DMA_CHAN_BASE   (DMA_BASE + 4 * 0x100)
#define DMA_CS          0x00
#define DMA_ADDR        0x04
#define DMA_NEXTCB      0x1c
#define DMA_INT_STATUS  0xfe0
#define DMA_ACTIVE      (1U << 0)
#define DMA_END         (1U << 1)
#define DMA_INT         (1U << 2)
#define DMA_ISHELD      (1U << 5)
#define DMA_ERROR       (1U << 8)
#define DMA_ABORT       (1U << 30)
#define DMA_RESET       (1U << 31)
#define DMA_TI_INT_EN   (1U << 0)
#define DMA_TI_WAIT_RESP (1U << 3)
#define DMA_TI_D_INC    (1U << 4)
#define DMA_TI_D_DREQ   (1U << 6)
#define DMA_TI_S_INC    (1U << 8)
#define DMA_TI_S_DREQ   (1U << 10)
#define DMA_TI_PERMAP_SDHOST (13U << 16)
#define DMA_CB_ADDR     0x1000
#define DMA_CB_BUS_ADDR (0xc0000000U + DMA_CB_ADDR)
#define DMA_CB_NEXT     (DMA_CB_ADDR + 32)
#define DMA_CB_NEXT_BUS_ADDR (DMA_CB_BUS_ADDR + 32)
#define DMA_DEST_ADDR   0x2000
#define DMA_DEST_BUS_ADDR (0xc0000000U + DMA_DEST_ADDR)
#define DMA_SDDATA      0x7e202040
#define SDCMD           (SDHOST_BASE + 0x00)
#define SDARG           (SDHOST_BASE + 0x04)
#define SDRSP0          (SDHOST_BASE + 0x10)
#define SDHSTS          (SDHOST_BASE + 0x20)
#define SDHBCT          (SDHOST_BASE + 0x3c)
#define SDDATA          (SDHOST_BASE + 0x40)
#define SDEDM           (SDHOST_BASE + 0x34)
#define SDHBLC          (SDHOST_BASE + 0x50)
#define SDHCFG          (SDHOST_BASE + 0x38)

#define SDCMD_NEW       (1U << 15)
#define SDCMD_FAIL      (1U << 14)
#define SDCMD_LONG      (1U << 9)
#define SDCMD_NO_RESP   (1U << 10)
#define SDCMD_BUSYWAIT  (1U << 11)
#define SDCMD_READ      (1U << 6)
#define SDCMD_WRITE     (1U << 7)
#define SDCMD_TIMEOUT   (1U << 6)
#define SDHCFG_DATA_IRPT_EN (1U << 4)
#define SDHCFG_BLOCK_IRPT_EN (1U << 8)
#define SDHCFG_BUSY_IRPT_EN (1U << 10)
#define SDHCFG_SLOW_CARD (1U << 3)
#define SDHCFG_WIDE_INT_BUS (1U << 1)
#define SDHCFG_WIDE_EXT_BUS (1U << 2)
#define SDHSTS_SDIO_IRPT (1U << 8)
#define SDHSTS_BLOCK_IRPT (1U << 9)
#define SDHSTS_BUSY_IRPT (1U << 10)
#define SDHSTS_DATA_FLAG (1U << 0)

static char *sd_image;
static bool sd_image_temporary;

static void send_command(unsigned command, uint32_t argument, uint32_t flags)
{
    writel(SDHSTS, UINT32_MAX);
    writel(SDARG, argument);
    writel(SDCMD, SDCMD_NEW | flags | command);
    if (!(flags & SDCMD_NO_RESP)) {
        uint32_t status = readl(SDHSTS);

        if (status & SDCMD_TIMEOUT) {
            g_error("CMD%u arg=0x%08x timed out (HSTS=0x%08x)",
                    command, argument, status);
        }
    }
}

static void dma_start_sdhost(unsigned length)
{
    unsigned first_length = (length / 2) & ~3U;

    writel(DMA_CHAN_BASE + DMA_CS, DMA_END | DMA_INT);
    writel(DMA_CB_ADDR + 0, DMA_TI_D_INC | DMA_TI_S_DREQ |
                             DMA_TI_PERMAP_SDHOST);
    writel(DMA_CB_ADDR + 4, DMA_SDDATA);
    writel(DMA_CB_ADDR + 8, DMA_DEST_BUS_ADDR);
    writel(DMA_CB_ADDR + 12, first_length);
    writel(DMA_CB_ADDR + 16, 0);
    writel(DMA_CB_ADDR + 20, DMA_CB_NEXT_BUS_ADDR);
    writel(DMA_CB_NEXT + 0, DMA_TI_INT_EN | DMA_TI_D_INC | DMA_TI_S_DREQ |
                            DMA_TI_PERMAP_SDHOST);
    writel(DMA_CB_NEXT + 4, DMA_SDDATA);
    writel(DMA_CB_NEXT + 8, DMA_DEST_BUS_ADDR + first_length);
    writel(DMA_CB_NEXT + 12, length - first_length);
    writel(DMA_CB_NEXT + 16, 0);
    writel(DMA_CB_NEXT + 20, 0);
    for (unsigned i = 0; i < length / sizeof(uint32_t); i++) {
        writel(DMA_DEST_ADDR + i * sizeof(uint32_t), UINT32_MAX);
    }

    writel(DMA_CHAN_BASE + DMA_ADDR, DMA_CB_BUS_ADDR);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_ACTIVE);
}

static void dma_start_sdhost_single(unsigned length)
{
    writel(DMA_CHAN_BASE + DMA_CS, DMA_END | DMA_INT);
    writel(DMA_CB_ADDR + 0, DMA_TI_INT_EN | DMA_TI_WAIT_RESP |
                             DMA_TI_D_INC | DMA_TI_S_DREQ |
                             DMA_TI_PERMAP_SDHOST);
    writel(DMA_CB_ADDR + 4, DMA_SDDATA);
    writel(DMA_CB_ADDR + 8, DMA_DEST_BUS_ADDR);
    writel(DMA_CB_ADDR + 12, length);
    writel(DMA_CB_ADDR + 16, 0);
    writel(DMA_CB_ADDR + 20, 0);
    for (unsigned i = 0; i < length / sizeof(uint32_t); i++) {
        writel(DMA_DEST_ADDR + i * sizeof(uint32_t), UINT32_MAX);
    }

    writel(DMA_CHAN_BASE + DMA_ADDR, DMA_CB_BUS_ADDR);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_ACTIVE);
}

static void dma_check_complete(void)
{
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_END, !=, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_INT, !=, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ERROR, ==, 0);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), !=, 0);
}

static void dma_ack_sdhost(void)
{
    /* Match the guest DMA driver: acknowledge INT while keeping the channel
     * active. The completed control block must not raise a second IRQ.
     */
    writel(DMA_CHAN_BASE + DMA_CS, DMA_INT | DMA_ACTIVE);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);
}

static void dma_read_sdhost(unsigned length)
{
    dma_start_sdhost(length);
    dma_check_complete();
    dma_ack_sdhost();
}

static void dma_start_sdhost_write(unsigned length)
{
    writel(DMA_CHAN_BASE + DMA_CS, DMA_END | DMA_INT);
    writel(DMA_CB_ADDR + 0, DMA_TI_INT_EN | DMA_TI_S_INC | DMA_TI_D_DREQ |
                             DMA_TI_PERMAP_SDHOST);
    writel(DMA_CB_ADDR + 4, DMA_DEST_BUS_ADDR);
    writel(DMA_CB_ADDR + 8, DMA_SDDATA);
    writel(DMA_CB_ADDR + 12, length);
    writel(DMA_CB_ADDR + 16, 0);
    writel(DMA_CB_ADDR + 20, 0);
    for (unsigned i = 0; i < length / sizeof(uint32_t); i++) {
        uint32_t value = 0;

        for (unsigned byte = 0; byte < sizeof(value); byte++) {
            unsigned offset = i * sizeof(value) + byte;

            value |= ((offset * 53 + 0xa7) & 0xff) << (byte * 8);
        }
        writel(DMA_DEST_ADDR + i * sizeof(value), value);
    }

    writel(DMA_CHAN_BASE + DMA_ADDR, DMA_CB_BUS_ADDR);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_ACTIVE);
}

static uint32_t init_sdhost(void)
{
    uint32_t rca;

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
    return rca;
}

static void drain_pio_read(const char *label, unsigned length)
{
    g_assert_cmpuint(length % sizeof(uint32_t), ==, 0);
    for (unsigned word = 0; word < length / sizeof(uint32_t); word++) {
        if ((word & 7) == 0) {
            uint32_t edm = readl(SDEDM);
            uint32_t words = (edm >> 4) & 0x1f;

            if (words < 8) {
                g_test_message("%s PIO FIFO short at word %u: EDM=0x%08x "
                               "HSTS=0x%08x", label, word, edm,
                               readl(SDHSTS));
            }
            g_assert_cmphex(words, >=, 8);
        }
        readl(SDDATA);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
}

static void test_real_image_mbr_pio(void)
{
    uint8_t expected[4096];
    uint8_t long_expected[128 * 512];
    uint32_t root_start;
    uint32_t rca = init_sdhost();
    int fd = open(sd_image, O_RDONLY);

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, expected, sizeof(expected), 0), ==,
                    sizeof(expected));
    root_start = expected[470] | (uint32_t)expected[471] << 8 |
                 (uint32_t)expected[472] << 16 |
                 (uint32_t)expected[473] << 24;

    writel(SDEDM, (4U << 14) | (4U << 9));
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS |
                   SDHCFG_WIDE_EXT_BUS);
    writel(SDHBCT, 64);
    writel(SDHBLC, 1);
    send_command(55, rca, 0);
    send_command(13, 0, SDCMD_READ);
    drain_pio_read("ACMD13", 64);
    writel(SDHBCT, 64);
    writel(SDHBLC, 1);
    send_command(6, 0x00fffff0, SDCMD_READ);
    g_assert_cmphex(readl(SDCMD) & SDCMD_FAIL, ==, 0);
    drain_pio_read("CMD6 check", 64);
    writel(SDHBCT, 64);
    writel(SDHBLC, 1);
    send_command(6, 0x80fffff1, SDCMD_READ);
    g_assert_cmphex(readl(SDCMD) & SDCMD_FAIL, ==, 0);
    drain_pio_read("CMD6 switch", 64);

    writel(SDHBCT, 512);
    writel(SDHBLC, 1);
    send_command(17, 0, SDCMD_READ);
    for (unsigned word = 0; word < 512 / sizeof(uint32_t); word++) {
        uint32_t actual = readl(SDDATA);

        for (unsigned byte = 0; byte < sizeof(actual); byte++) {
            unsigned offset = word * sizeof(actual) + byte;

            g_assert_cmphex((actual >> (byte * 8)) & 0xff, ==,
                            expected[offset]);
        }
    }
    g_assert_cmphex(expected[510], ==, 0x55);
    g_assert_cmphex(expected[511], ==, 0xaa);
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_DATA_FLAG, ==, 0);

    g_assert_cmpint(pread(fd, expected, 512,
                          (off_t)(root_start + 2) * 512), ==, 512);
    writel(SDHBCT, 512);
    writel(SDHBLC, 1);
    send_command(17, root_start + 2, SDCMD_READ);
    for (unsigned word = 0; word < 512 / sizeof(uint32_t); word++) {
        uint32_t actual = readl(SDDATA);

        for (unsigned byte = 0; byte < sizeof(actual); byte++) {
            unsigned offset = word * sizeof(actual) + byte;

            g_assert_cmphex((actual >> (byte * 8)) & 0xff, ==,
                            expected[offset]);
        }
    }
    g_assert_cmphex(expected[56], ==, 0x53);
    g_assert_cmphex(expected[57], ==, 0xef);

    /* Match the kernel's open-ended eight-sector root-filesystem read. */
    g_assert_cmpint(pread(fd, expected, sizeof(expected),
                          (off_t)root_start * 512), ==, sizeof(expected));
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 8);
    dma_start_sdhost_single(4084);
    send_command(18, root_start, SDCMD_READ);
    dma_check_complete();
    dma_ack_sdhost();
    for (unsigned word = 0; word < 4084 / sizeof(uint32_t); word++) {
        uint32_t actual = readl(DMA_DEST_ADDR + word * sizeof(actual));

        for (unsigned byte = 0; byte < sizeof(actual); byte++) {
            unsigned offset = word * sizeof(actual) + byte;

            g_assert_cmphex((actual >> (byte * 8)) & 0xff, ==,
                            expected[offset]);
        }
    }
    for (unsigned word = 0; word < 12 / sizeof(uint32_t); word++) {
        uint32_t actual = readl(SDDATA);

        for (unsigned byte = 0; byte < sizeof(actual); byte++) {
            unsigned offset = 4084 + word * sizeof(actual) + byte;

            g_assert_cmphex((actual >> (byte * 8)) & 0xff, ==,
                            expected[offset]);
        }
    }
    send_command(12, 0, SDCMD_BUSYWAIT);

    /* Exercise repeated card-block refills over a longer DMA-style drain. */
    g_assert_cmpint(pread(fd, long_expected, sizeof(long_expected),
                          (off_t)(root_start + 2048) * 512), ==,
                    sizeof(long_expected));
    close(fd);
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHBCT, 512);
    writel(SDHBLC, 128);
    dma_start_sdhost_single(sizeof(long_expected) - 12);
    send_command(18, root_start + 2048, SDCMD_READ);
    dma_check_complete();
    dma_ack_sdhost();
    for (unsigned word = 0; word < (sizeof(long_expected) - 12) /
                                 sizeof(uint32_t); word++) {
        uint32_t actual = readl(DMA_DEST_ADDR + word * sizeof(actual));
        uint32_t expected_word = 0;

        for (unsigned byte = 0; byte < sizeof(actual); byte++) {
            expected_word |= (uint32_t)long_expected[word * sizeof(actual) +
                                                     byte] << (byte * 8);
        }
        g_assert_cmphex(actual, ==, expected_word);
    }
    for (unsigned word = 0; word < 12 / sizeof(uint32_t); word++) {
        uint32_t actual = readl(SDDATA);
        uint32_t expected_word = 0;

        for (unsigned byte = 0; byte < sizeof(actual); byte++) {
            expected_word |= (uint32_t)long_expected[sizeof(long_expected) -
                                                     12 + word * sizeof(actual) +
                                                     byte] << (byte * 8);
        }
        g_assert_cmphex(actual, ==, expected_word);
    }
    send_command(12, 0, SDCMD_BUSYWAIT);

    /* The guest's PIO path drains one 512-byte block per data interrupt. */
    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 128);
    send_command(18, root_start + 2048, SDCMD_READ);
    for (unsigned block = 0; block < 128; block++) {
        g_assert_true(qtest_get_irq(global_qtest, 0));
        for (unsigned word = 0; word < 512 / sizeof(uint32_t); word++) {
            unsigned offset = block * 512 + word * sizeof(uint32_t);
            uint32_t actual = readl(SDDATA);
            uint32_t expected_word = 0;

            for (unsigned byte = 0; byte < sizeof(actual); byte++) {
                expected_word |= (uint32_t)long_expected[offset + byte] <<
                                 (byte * 8);
            }
            g_assert_cmphex(actual, ==, expected_word);
        }
        writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
        g_assert_cmpint(qtest_get_irq(global_qtest, 0), ==, block != 127);
    }
    send_command(12, 0, SDCMD_BUSYWAIT);

    /* A full CMD25 DMA write must survive a subsequent CMD18 DMA read. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 128);
    send_command(23, 128, 0);
    dma_start_sdhost_write(sizeof(long_expected));
    send_command(25, root_start + 2048, SDCMD_WRITE);
    g_assert_cmphex(readl(SDCMD) & SDCMD_FAIL, ==, 0);
    dma_check_complete();
    dma_ack_sdhost();

    /* A completed write must not leave DREQ asserted for a new DMA CB. */
    dma_start_sdhost_single(4);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==,
                    DMA_ACTIVE | DMA_ISHELD);
    writel(DMA_CHAN_BASE + DMA_NEXTCB, 0);
    writel(DMA_CHAN_BASE + DMA_CS,
           readl(DMA_CHAN_BASE + DMA_CS) | DMA_ABORT | DMA_ACTIVE);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==, 0);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_RESET);

    send_command(23, 128, 0);
    writel(SDHBLC, 128);
    dma_start_sdhost_single(sizeof(long_expected));
    send_command(18, root_start + 2048, SDCMD_READ);
    g_assert_cmphex(readl(SDCMD) & SDCMD_FAIL, ==, 0);
    dma_check_complete();
    dma_ack_sdhost();
    for (unsigned word = 0; word < sizeof(long_expected) / sizeof(uint32_t);
         word++) {
        uint32_t expected_word = 0;

        for (unsigned byte = 0; byte < sizeof(expected_word); byte++) {
            unsigned offset = word * sizeof(expected_word) + byte;

            expected_word |= ((offset * 53 + 0xa7) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(DMA_DEST_ADDR + word * sizeof(expected_word)),
                        ==, expected_word);
    }
}

static void test_cmd18_dma_fifo_read(void)
{
    uint32_t rca;
    uint32_t actual;

    rca = init_sdhost();

    /* Data arriving before DATA_IRPT_EN must interrupt when enabled later. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_SLOW_CARD |
                   SDHCFG_WIDE_INT_BUS);
    writel(SDHBCT, 64);
    writel(SDHBLC, 1);
    send_command(55, rca, 0);
    send_command(13, 0, SDCMD_READ);
    g_assert_cmphex((readl(SDEDM) >> 4) & 0x1f, ==, 16);
    g_assert_cmphex(readl(SDHSTS) &
                    (SDHSTS_DATA_FLAG | SDHSTS_SDIO_IRPT), ==,
                    SDHSTS_DATA_FLAG);
    g_assert_false(qtest_get_irq(global_qtest, 0));
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS);
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_SDIO_IRPT, !=, 0);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    for (unsigned word = 0; word < 64 / sizeof(uint32_t); word++) {
        readl(SDDATA);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* The guest reads the 8-byte SCR through PIO during card setup. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS);
    writel(SDHBCT, 8);
    writel(SDHBLC, 1);
    send_command(55, rca, 0);
    send_command(51, 0, SDCMD_READ);
    g_assert_cmphex((readl(SDEDM) >> 4) & 0x1f, ==, 2);
    actual = readl(SDDATA);
    g_assert_cmphex(actual & 0xffff, ==, 0x2502);
    g_assert_cmphex(actual & 0xff000000, ==, 0);
    g_assert_cmphex(readl(SDDATA), ==, 0);

    /* A completed short read must not leave a data IRQ for the next request. */
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS);
    writel(SDHBCT, 64);
    writel(SDHBLC, 1);
    send_command(6, 0x00fffff0, SDCMD_READ);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    for (unsigned word = 0; word < 64 / sizeof(uint32_t); word++) {
        readl(SDDATA);
    }
    g_assert_cmphex(readl(SDHSTS) &
                    (SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG), ==, 0);
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* A PIO single-block read must refill the 16-word FIFO through all 512 bytes. */
    /* Match the guest driver's 4-byte FIFO thresholds and SDHCFG. */
    writel(SDEDM, (4U << 14) | (4U << 9));
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS |
                   SDHCFG_WIDE_EXT_BUS);
    writel(SDHBCT, 512);
    writel(SDHBLC, 1);
    send_command(17, 0, SDCMD_READ);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    for (unsigned word = 0; word < 512 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        if ((word & 7) == 0) {
            g_assert_cmphex((readl(SDEDM) >> 4) & 0x1f, >=, 8);
        }
        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(SDDATA), ==, expected);
        if ((word & 15) == 15) {
            writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
            g_assert_cmpint(qtest_get_irq(global_qtest, 0), ==,
                            word != 127);
        }
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHCFG, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* PIO must refill continuously across the eight-block test image. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN |
                   SDHCFG_SLOW_CARD | SDHCFG_WIDE_INT_BUS |
                   SDHCFG_WIDE_EXT_BUS);
    writel(SDHBCT, 512);
    writel(SDHBLC, 8);
    send_command(23, 8, 0);
    send_command(18, 0, SDCMD_READ);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    for (unsigned word = 0; word < 4096 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        if ((word & 7) == 0) {
            g_assert_cmphex((readl(SDEDM) >> 4) & 0x1f, >=, 8);
        }
        for (unsigned byte = 0; byte < sizeof(expected); byte++) {
            unsigned offset = word * sizeof(expected) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(SDDATA), ==, expected);
        if ((word & 15) == 15) {
            writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
            g_assert_cmpint(qtest_get_irq(global_qtest, 0), ==,
                            word != 1023);
        }
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHCFG, 0);

    /* A complete single-block DMA descriptor must drain its whole sector. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 1);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_END | DMA_INT);
    writel(DMA_CB_ADDR + 0, DMA_TI_INT_EN | DMA_TI_D_INC | DMA_TI_S_DREQ |
                             DMA_TI_PERMAP_SDHOST);
    writel(DMA_CB_ADDR + 4, DMA_SDDATA);
    writel(DMA_CB_ADDR + 8, DMA_DEST_BUS_ADDR);
    writel(DMA_CB_ADDR + 12, 512);
    writel(DMA_CB_ADDR + 16, 0);
    writel(DMA_CB_ADDR + 20, 0);
    writel(DMA_CHAN_BASE + DMA_ADDR, DMA_CB_BUS_ADDR);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_ACTIVE);
    send_command(17, 0, SDCMD_READ);
    dma_check_complete();
    dma_ack_sdhost();
    for (unsigned word = 0; word < 512 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(DMA_DEST_ADDR + word * sizeof(expected)), ==,
                        expected);
    }

    writel(SDHCFG, 0);
    writel(SDHBCT, 512);
    writel(SDHBLC, 1);
    dma_start_sdhost(500);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_END, ==, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ACTIVE, !=, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ISHELD, !=, 0);
    send_command(17, 0, SDCMD_READ);
    dma_check_complete();
    dma_ack_sdhost();
    g_assert_false(qtest_get_irq(global_qtest, 0));
    g_assert_cmphex(readl(SDHSTS) & (SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT), ==,
                    0);
    for (unsigned word = 0; word < 500 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(DMA_DEST_ADDR + word * sizeof(uint32_t)), ==,
                        expected);
    }
    for (unsigned word = 0; word < 3; word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = 500 + word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(SDDATA), ==, expected);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);

    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    writel(SDHBLC, 8);
    send_command(23, 8, 0);
    send_command(18, 0, SDCMD_READ);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(SDHCFG, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_SDIO_IRPT, !=, 0);
    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    g_assert_true(qtest_get_irq(global_qtest, 0));

    /* The Linux driver leaves the last 12 bytes of a multi-block read to PIO. */
    dma_start_sdhost_single(4084);
    dma_check_complete();
    dma_ack_sdhost();
    for (unsigned word = 0; word < 4084 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        actual = readl(DMA_DEST_ADDR + word * sizeof(uint32_t));
        g_assert_cmphex(actual, ==, expected);
    }

    for (unsigned word = 0; word < 12 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = 4084 + word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        actual = readl(SDDATA);
        g_assert_cmphex(actual, ==, expected);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* A completed read must not leave a DREQ for a later DMA descriptor. */
    dma_start_sdhost(4);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==,
                    DMA_ACTIVE | DMA_ISHELD);
    writel(DMA_CHAN_BASE + DMA_NEXTCB, 0);
    writel(DMA_CHAN_BASE + DMA_CS,
           readl(DMA_CHAN_BASE + DMA_CS) | DMA_ABORT | DMA_ACTIVE);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==, 0);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_RESET);

    /* A rejected read command must cancel the stale request from an
     * unfinished prior read instead of feeding dummy bytes to DMA.
     */
    writel(SDHCFG, 0);
    writel(SDHBCT, 512);
    writel(SDHBLC, 1);
    send_command(17, 0, SDCMD_READ);
    g_assert_cmphex((readl(SDEDM) >> 4) & 0x1f, ==, 16);
    send_command(13, rca, 0);
    writel(SDHBLC, 8);
    dma_start_sdhost(4084);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ISHELD, !=, 0);
    writel(SDHSTS, UINT32_MAX);
    writel(SDARG, 0);
    writel(SDCMD, SDCMD_NEW | SDCMD_READ | 18);
    g_assert_cmphex(readl(SDCMD) & SDCMD_FAIL, !=, 0);
    g_assert_cmphex(readl(SDHSTS) & SDCMD_TIMEOUT, !=, 0);
    g_assert_cmphex((readl(SDEDM) >> 4) & 0x1f, ==, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==,
                    DMA_ACTIVE | DMA_ISHELD);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);
    send_command(12, 0, SDCMD_BUSYWAIT);
    writel(DMA_CHAN_BASE + DMA_NEXTCB, 0);
    writel(DMA_CHAN_BASE + DMA_CS,
           readl(DMA_CHAN_BASE + DMA_CS) | DMA_ABORT | DMA_ACTIVE);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==, 0);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_RESET);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);

    /* Stopping a read with an oversized DMA CB drops its remaining DREQ so a
     * later command cannot complete the stale DMA request.
     */
    writel(SDHCFG, 0);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    dma_start_sdhost(1536);
    send_command(18, 0, SDCMD_READ);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==,
                    DMA_ACTIVE | DMA_ISHELD);
    g_assert_cmphex(readl(DMA_CHAN_BASE + 0x14), ==, 512);
    send_command(12, 0, SDCMD_BUSYWAIT);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==,
                    DMA_ACTIVE | DMA_ISHELD);
    g_assert_cmphex(readl(SDHSTS) &
                    (SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT |
                     SDHSTS_DATA_FLAG), ==, 0);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);
    /* Match the Linux DMA driver's terminate sequence. */
    writel(DMA_CHAN_BASE + DMA_NEXTCB, 0);
    writel(DMA_CHAN_BASE + DMA_CS,
           readl(DMA_CHAN_BASE + DMA_CS) | DMA_ABORT | DMA_ACTIVE);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) &
                    (DMA_ACTIVE | DMA_ISHELD), ==, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_ADDR), ==, 0);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);
    writel(DMA_CHAN_BASE + DMA_CS,
           readl(DMA_CHAN_BASE + DMA_CS) & ~DMA_ACTIVE);
    writel(DMA_CHAN_BASE + DMA_CS, DMA_RESET);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);

    /* CMD12 must discard pending FIFO data and its data IRQ. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_DATA_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    send_command(18, 0, SDCMD_READ);
    g_assert_cmphex(readl(SDHSTS) &
                    (SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG), ==,
                    SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    send_command(12, 0, SDCMD_BUSYWAIT);
    g_assert_cmphex(readl(SDHSTS) &
                    (SDHSTS_BLOCK_IRPT | SDHSTS_SDIO_IRPT |
                     SDHSTS_DATA_FLAG), ==, 0);
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BUSY_IRPT, !=, 0);
    writel(SDHSTS, SDHSTS_BUSY_IRPT);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* CMD12 must also discard a pending write block interrupt. */
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN | SDHCFG_BLOCK_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    send_command(25, 512, SDCMD_WRITE);
    for (unsigned word = 0; word < 512 / sizeof(uint32_t); word++) {
        writel(SDDATA, 0x5a5aa5a5);
    }
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BLOCK_IRPT, !=, 0);
    send_command(12, 0, SDCMD_BUSYWAIT);
    g_assert_cmphex(readl(SDHSTS) &
                    (SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT |
                     SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG), ==,
                    SDHSTS_BUSY_IRPT);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    writel(SDHSTS, SDHSTS_BUSY_IRPT);
    g_assert_false(qtest_get_irq(global_qtest, 0));

    /* Exercise repeated refills and a short final refill. */
    send_command(16, 100, 0);
    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    writel(SDHBCT, 100);
    writel(SDHBLC, 2);
    send_command(18, 0, SDCMD_READ);
    dma_read_sdhost(200);
    for (unsigned word = 0; word < 200 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(uint32_t); byte++) {
            unsigned offset = word * sizeof(uint32_t) + byte;

            expected |= ((offset * 37 + 11) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(DMA_DEST_ADDR + word * sizeof(uint32_t)), ==,
                        expected);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    send_command(12, 0, SDCMD_BUSYWAIT);
    g_assert_cmphex(readl(SDHSTS) & SDHSTS_BUSY_IRPT, !=, 0);
    writel(SDHSTS, SDHSTS_BUSY_IRPT);

    /* The final FIFO word may contain fewer than four valid data bytes. */
    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    writel(SDHBCT, 7);
    writel(SDHBLC, 2);
    send_command(18, 0, SDCMD_READ);
    dma_read_sdhost(16);
    for (unsigned byte = 0; byte < 16; byte++) {
        uint8_t expected = byte < 14 ? (byte * 37 + 11) & 0xff : 0;
        uint32_t actual = readl(DMA_DEST_ADDR + (byte & ~3U));

        g_assert_cmphex((actual >> ((byte & 3) * 8)) & 0xff, ==, expected);
    }
    writel(SDHSTS, SDHSTS_SDIO_IRPT | SDHSTS_DATA_FLAG);
    writel(SDHCFG, SDHCFG_BUSY_IRPT_EN);
    send_command(12, 0, SDCMD_BUSYWAIT);
    writel(SDHSTS, SDHSTS_BUSY_IRPT);

    send_command(16, 512, 0);
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
    send_command(12, 0, SDCMD_BUSYWAIT);

    /* DMA writes wait until the SDHOST FIFO has room for destination data. */
    writel(SDHSTS, UINT32_MAX);
    writel(SDHCFG, 0);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    dma_start_sdhost_write(1024);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_END, ==, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ACTIVE, !=, 0);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ISHELD, !=, 0);
    send_command(25, 0, SDCMD_WRITE);
    dma_check_complete();
    /* Resetting a completed channel must also drop its pending shared IRQ. */
    writel(DMA_CHAN_BASE + DMA_CS, 1U << 31);
    g_assert_cmphex(readl(DMA_BASE + DMA_INT_STATUS) & (1U << 4), ==, 0);
    send_command(12, 0, SDCMD_BUSYWAIT);

    /* Read the written blocks back through source-DREQ and compare every byte. */
    writel(SDHSTS, UINT32_MAX);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    dma_start_sdhost(1024);
    g_assert_cmphex(readl(DMA_CHAN_BASE + DMA_CS) & DMA_ISHELD, !=, 0);
    send_command(18, 0, SDCMD_READ);
    dma_check_complete();
    dma_ack_sdhost();
    for (unsigned word = 0; word < 1024 / sizeof(uint32_t); word++) {
        uint32_t expected = 0;

        for (unsigned byte = 0; byte < sizeof(expected); byte++) {
            unsigned offset = word * sizeof(expected) + byte;

            expected |= ((offset * 53 + 0xa7) & 0xff) << (byte * 8);
        }
        g_assert_cmphex(readl(DMA_DEST_ADDR + word * sizeof(expected)), ==,
                        expected);
    }
    send_command(12, 0, SDCMD_BUSYWAIT);

    /* Reset must clear a latched SDHOST interrupt before the guest can
     * associate it with a later transfer.
     */
    writel(SDHCFG, SDHCFG_DATA_IRPT_EN);
    writel(SDHBCT, 512);
    writel(SDHBLC, 2);
    send_command(18, 0, SDCMD_READ);
    g_assert_true(qtest_get_irq(global_qtest, 0));
    qtest_system_reset(global_qtest);
    g_assert_cmphex(readl(SDHSTS), ==, 0);
    g_assert_false(qtest_get_irq(global_qtest, 0));
}

static void create_sd_image(void)
{
    uint8_t data[4096];
    GError *error = NULL;
    int fd;

    const char *image = g_getenv("A6_SDHOST_IMAGE");
    if (image) {
        sd_image = g_strdup(image);
        return;
    }

    sd_image = g_build_filename(g_get_tmp_dir(), "qtest-sdhost-XXXXXX", NULL);
    fd = g_mkstemp(sd_image);
    g_assert_cmpint(fd, >=, 0);
    sd_image_temporary = true;
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
    if (g_getenv("A6_SDHOST_IMAGE")) {
        qtest_add_func("/bcm2835/sdhost/real_image_mbr_pio",
                       test_real_image_mbr_pio);
    } else {
        qtest_add_func("/bcm2835/sdhost/cmd18_dma_fifo_read",
                       test_cmd18_dma_fifo_read);
    }
    g_autofree char *args = g_strdup_printf(
        "-machine raspi3ap -smp 4 -drive if=sd,file=%s,format=raw,snapshot=on",
        sd_image);
    qtest_start(args);
    ret = g_test_run();
    qtest_end();
    if (sd_image_temporary) {
        unlink(sd_image);
    }
    g_free(sd_image);
    return ret;
}
