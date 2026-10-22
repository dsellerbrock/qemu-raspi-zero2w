/*
 * QTests for the Raspberry Pi virtio-mmio transport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_mmio.h"

#define RASPI3AP_VIRTIO_MMIO_BASE 0x3ff00000
#define VIRTIO_MMIO_MAGIC         0x74726976

static void test_virtio_gpu(void)
{
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE +
                          VIRTIO_MMIO_MAGIC_VALUE), ==, VIRTIO_MMIO_MAGIC);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE +
                          VIRTIO_MMIO_VERSION), ==, 2);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE +
                          VIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_GPU);

}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/raspi/virtio-mmio/gpu", test_virtio_gpu);
    qtest_start("-machine raspi3ap -smp 4 -snapshot -device virtio-gpu-device");
    ret = g_test_run();
    qtest_end();
    return ret;
}
