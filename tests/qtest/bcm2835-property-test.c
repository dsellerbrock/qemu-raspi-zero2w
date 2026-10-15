/*
 * QTest for Raspberry Pi firmware property mailbox requests.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest-single.h"
#include "hw/arm/raspberrypi-fw-defs.h"
#include "hw/misc/bcm2835_mbox_defs.h"

#define MBOX_BASE       0x3f00b800
#define MBOX_READ       (MBOX_BASE + 0x80)
#define MBOX_WRITE      (MBOX_BASE + 0xa0)
#define PROPERTY_BUFFER 0x1000
#define CLOCKS_BYTES    (RPI_FIRMWARE_NUM_CLK_ID * 2 * sizeof(uint32_t))
#define PROPERTY_WORDS  ((4 + 4 + 12 + CLOCKS_BYTES + 4) / sizeof(uint32_t))

static void test_get_clock_list(void)
{
    static const uint32_t expected_ids[] = {
        RPI_FIRMWARE_ARM_CLK_ID,
        RPI_FIRMWARE_PIXEL_CLK_ID,
        RPI_FIRMWARE_M2MC_CLK_ID,
    };
    uint32_t request[PROPERTY_WORDS] = { 0 };
    uint32_t response[PROPERTY_WORDS];

    request[0] = cpu_to_le32(sizeof(request));
    request[2] = cpu_to_le32(RPI_FWREQ_GET_CLOCKS);
    request[3] = cpu_to_le32(CLOCKS_BYTES);
    request[5 + 2 * ARRAY_SIZE(expected_ids)] = 0;

    memwrite(PROPERTY_BUFFER, request, sizeof(request));
    writel(MBOX_WRITE, PROPERTY_BUFFER | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(readl(MBOX_READ), ==,
                    PROPERTY_BUFFER | MBOX_CHAN_PROPERTY);
    memread(PROPERTY_BUFFER, response, sizeof(response));

    g_assert_cmphex(le32_to_cpu(response[1]), ==, 1U << 31);
    g_assert_cmphex(le32_to_cpu(response[4]), ==,
                    (1U << 31) | (2 * sizeof(uint32_t) *
                                  ARRAY_SIZE(expected_ids)));
    for (size_t i = 0; i < ARRAY_SIZE(expected_ids); i++) {
        g_assert_cmphex(le32_to_cpu(response[5 + 2 * i]), ==, 0);
        g_assert_cmphex(le32_to_cpu(response[6 + 2 * i]), ==,
                        expected_ids[i]);
    }
    g_assert_cmphex(le32_to_cpu(response[5 + 2 * ARRAY_SIZE(expected_ids)]),
                    ==, 0);
    g_assert_cmphex(le32_to_cpu(response[6 + 2 * ARRAY_SIZE(expected_ids)]),
                    ==, 0);
}

static void test_get_clock_list_small_buffer(void)
{
    uint32_t request[8] = { 0 };
    uint32_t response[8];

    request[0] = cpu_to_le32(sizeof(request));
    request[2] = cpu_to_le32(RPI_FWREQ_GET_CLOCKS);
    request[3] = cpu_to_le32(2 * sizeof(uint32_t));
    request[5] = cpu_to_le32(0xdeadbeef);
    request[6] = cpu_to_le32(0xcafebabe);

    memwrite(PROPERTY_BUFFER, request, sizeof(request));
    writel(MBOX_WRITE, PROPERTY_BUFFER | MBOX_CHAN_PROPERTY);
    g_assert_cmphex(readl(MBOX_READ), ==,
                    PROPERTY_BUFFER | MBOX_CHAN_PROPERTY);
    memread(PROPERTY_BUFFER, response, sizeof(response));

    g_assert_cmphex(le32_to_cpu(response[4]), ==, (1U << 31) | 24);
    g_assert_cmphex(le32_to_cpu(response[5]), ==, 0xdeadbeef);
    g_assert_cmphex(le32_to_cpu(response[6]), ==, 0xcafebabe);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/property/get_clock_list", test_get_clock_list);
    qtest_add_func("/bcm2835/property/get_clock_list_small_buffer",
                   test_get_clock_list_small_buffer);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();

    return ret;
}
