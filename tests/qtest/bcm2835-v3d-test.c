/*
 * QTest for the Raspberry Pi VideoCore IV V3D register block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

#define RASPI3_V3D_BASE 0x3fc00000

#define V3D_IDENT0  0x000
#define V3D_IDENT1  0x004
#define V3D_IDENT2  0x008
#define V3D_SCRATCH 0x010
#define V3D_INTCTL  0x030
#define V3D_INTENA  0x034
#define V3D_INTDIS  0x038
#define V3D_CT1CS   0x104
#define V3D_CT1EA   0x10c
#define V3D_CT1CA   0x114

#define V3D_FRDONE  (1U << 0)
#define CL_ADDR     0x10000
#define FB_ADDR     0x20000
#define FB_SIZE     (64 * 64 * sizeof(uint32_t))

#define V3D_EXPECTED_IDENT0 0x02443356
#define V3D_EXPECTED_IDENT1 0xc0101432

static void test_v3d_identity_and_scratch(void)
{
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_IDENT0),
                    ==, V3D_EXPECTED_IDENT0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_IDENT1),
                    ==, V3D_EXPECTED_IDENT1);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_IDENT2), ==, 0);

    writel(RASPI3_V3D_BASE + V3D_IDENT0, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_IDENT0),
                    ==, V3D_EXPECTED_IDENT0);

    writel(RASPI3_V3D_BASE + V3D_SCRATCH, 0xa5a55a5a);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_SCRATCH), ==, 0xa5a55a5a);
}

static void test_v3d_irq_masks(void)
{
    writel(RASPI3_V3D_BASE + V3D_INTENA, 0x0f);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTENA), ==, 0x0f);

    writel(RASPI3_V3D_BASE + V3D_INTDIS, 0x05);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTENA), ==, 0x0a);

    writel(RASPI3_V3D_BASE + V3D_INTCTL, UINT32_MAX);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL), ==, 0);
}

static void append_u16(uint8_t *buffer, size_t *offset, uint16_t value)
{
    buffer[(*offset)++] = value;
    buffer[(*offset)++] = value >> 8;
}

static void append_u32(uint8_t *buffer, size_t *offset, uint32_t value)
{
    for (unsigned i = 0; i < sizeof(value); i++) {
        buffer[(*offset)++] = value >> (i * 8);
    }
}

static uint32_t read_u32(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

static size_t make_clear_list(uint8_t *buffer, uint32_t target,
                              uint16_t width, uint16_t height,
                              unsigned memory_format, uint8_t tile_x,
                              uint8_t tile_y, uint32_t color)
{
    size_t offset = 0;

    /* TILE_RENDERING_MODE_CONFIG: RGBA8888 plus the requested memory format. */
    buffer[offset++] = 113;
    append_u32(buffer, &offset, target);
    append_u16(buffer, &offset, width);
    append_u16(buffer, &offset, height);
    append_u16(buffer, &offset, (memory_format << 6) | 4);

    /* CLEAR_COLORS, TILE_COORDS, and STORE_MS_TILE_BUFFER_AND_EOF. */
    buffer[offset++] = 114;
    append_u32(buffer, &offset, color);
    append_u32(buffer, &offset, 0);
    append_u32(buffer, &offset, 0);
    buffer[offset++] = 0;
    buffer[offset++] = 115;
    buffer[offset++] = tile_x;
    buffer[offset++] = tile_y;
    buffer[offset++] = 25;

    return offset;
}

static void submit_clear(unsigned memory_format, uint8_t tile_x,
                         uint8_t tile_y, uint32_t color)
{
    uint8_t cl[64];
    size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, memory_format,
                                     tile_x, tile_y, color);

    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
}

static void assert_color_block(const uint8_t *fb, size_t offset,
                               uint32_t color)
{
    for (size_t i = 0; i < 4096; i += sizeof(uint32_t)) {
        g_assert_cmphex(read_u32(fb + offset + i), ==, color);
    }
}

static void test_v3d_ct1_clear_linear(void)
{
    uint8_t cl[64];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0x11223344;
    size_t cl_size = make_clear_list(cl, FB_ADDR, 32, 32, 0, 0, 0, color);

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FRDONE);
    writel(RASPI3_V3D_BASE + V3D_INTENA, V3D_FRDONE);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);

    memread(FB_ADDR, fb, 32 * 32 * sizeof(uint32_t));
    for (size_t i = 0; i < 32 * 32; i++) {
        g_assert_cmphex(read_u32(fb + i * sizeof(uint32_t)), ==, color);
    }
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FRDONE,
                    ==, V3D_FRDONE);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) & (1U << 5), ==, 0);
}

static void test_v3d_ct1_clear_t_tiled(void)
{
    static const uint32_t colors[4] = {
        0x01020304, 0x11223344, 0x55667788, 0x99aabbcc,
    };
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FRDONE);
    writel(RASPI3_V3D_BASE + V3D_INTENA, V3D_FRDONE);

    submit_clear(1, 0, 0, colors[0]);
    submit_clear(1, 1, 0, colors[1]);
    submit_clear(1, 0, 1, colors[2]);
    submit_clear(1, 1, 1, colors[3]);

    memread(FB_ADDR, fb, sizeof(fb));
    assert_color_block(fb, 0, colors[0]);
    assert_color_block(fb, 4096, colors[1]);
    /* T-tile rows alternate direction in memory. */
    assert_color_block(fb, 8192, colors[3]);
    assert_color_block(fb, 12288, colors[2]);
}

static void test_v3d_ct1_clear_lt_tiled(void)
{
    static const uint32_t colors[4] = {
        0x01020304, 0x11223344, 0x55667788, 0x99aabbcc,
    };
    static const uint8_t tile_x[4] = { 0, 1, 0, 1 };
    static const uint8_t tile_y[4] = { 0, 0, 1, 1 };
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FRDONE);
    writel(RASPI3_V3D_BASE + V3D_INTENA, V3D_FRDONE);

    for (unsigned tile = 0; tile < G_N_ELEMENTS(colors); tile++) {
        submit_clear(2, tile_x[tile], tile_y[tile], colors[tile]);
    }

    memread(FB_ADDR, fb, sizeof(fb));
    for (unsigned tile = 0; tile < G_N_ELEMENTS(colors); tile++) {
        uint32_t x = tile_x[tile] * 32;
        uint32_t y = tile_y[tile] * 32;
        uint32_t stride = 64 * sizeof(uint32_t);
        size_t offset = (size_t)y * stride + (size_t)x * 16;

        g_assert_cmphex(read_u32(fb + offset), ==, colors[tile]);
    }
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/v3d/identity_and_scratch",
                   test_v3d_identity_and_scratch);
    qtest_add_func("/bcm2835/v3d/irq_masks", test_v3d_irq_masks);
    qtest_add_func("/bcm2835/v3d/ct1_clear_linear",
                   test_v3d_ct1_clear_linear);
    qtest_add_func("/bcm2835/v3d/ct1_clear_t_tiled",
                   test_v3d_ct1_clear_t_tiled);
    qtest_add_func("/bcm2835/v3d/ct1_clear_lt_tiled",
                   test_v3d_ct1_clear_lt_tiled);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();
    return ret;
}
