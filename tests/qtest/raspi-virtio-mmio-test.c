/*
 * QTests for the Raspberry Pi virtio-mmio transport.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_gpu.h"
#include "standard-headers/linux/virtio_mmio.h"

#define RASPI3AP_VIRTIO_MMIO_BASE 0x3ff00000
#define RASPI3AP_VIRTIO_MMIO_BASE1 0x3ff00200
#define RASPI3AP_VIRTIO_SHM_BASE 0x80000000
#define VIRTIO_MMIO_MAGIC         0x74726976

static void test_virtio_gpu(void)
{
    const uint32_t shmid = VIRTIO_GPU_SHM_ID_HOST_VISIBLE;

    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE +
                          VIRTIO_MMIO_MAGIC_VALUE), ==, VIRTIO_MMIO_MAGIC);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE +
                          VIRTIO_MMIO_VERSION), ==, 2);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE +
                          VIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_GPU);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_CONFIG +
                          offsetof(struct virtio_gpu_config, num_capsets)),
                    ==, 1);

    writel(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_SEL, 0x101);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_LEN_LOW),
                    ==, UINT32_MAX);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_LEN_HIGH),
                    ==, UINT32_MAX);

    writel(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_SEL, shmid);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_SEL),
                    ==, shmid);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_LEN_LOW),
                    ==, 16 * 1024 * 1024);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_LEN_HIGH),
                    ==, 0);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_BASE_LOW),
                    ==, RASPI3AP_VIRTIO_SHM_BASE);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE + VIRTIO_MMIO_SHM_BASE_HIGH),
                    ==, 0);
}

static void test_virtio_blk(void)
{
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE1 +
                          VIRTIO_MMIO_MAGIC_VALUE), ==, VIRTIO_MMIO_MAGIC);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE1 +
                          VIRTIO_MMIO_VERSION), ==, 2);
    g_assert_cmphex(readl(RASPI3AP_VIRTIO_MMIO_BASE1 +
                          VIRTIO_MMIO_DEVICE_ID), ==, VIRTIO_ID_BLOCK);
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/raspi/virtio-mmio/gpu", test_virtio_gpu);
    qtest_add_func("/raspi/virtio-mmio/blk", test_virtio_blk);
    qtest_start("-machine raspi3ap -smp 4 -snapshot "
                "-blockdev driver=null-co,node-name=d0,size=1048576 "
                "-device virtio-blk-device,bus=virtio-mmio-bus.1,drive=d0 "
                "-device virtio-gpu-gl-device,bus=virtio-mmio-bus.0,"
                "venus=on,blob=on,hostmem=16M");
    ret = g_test_run();
    qtest_end();
    return ret;
}
