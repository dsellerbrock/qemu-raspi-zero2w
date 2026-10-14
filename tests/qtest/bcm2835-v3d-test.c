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
#define V3D_CT0CS   0x100
#define V3D_RFC     0x138
#define V3D_CT0EA   0x108
#define V3D_CT0CA   0x110
#define V3D_BFC     0x134
#define V3D_CT1CS   0x104
#define V3D_CT1EA   0x10c
#define V3D_CT1CA   0x114

#define V3D_FRDONE  (1U << 0)
#define V3D_FLDONE  (1U << 1)
#define V3D_CTERR   (1U << 3)
#define V3D_CTRUN   (1U << 5)
#define CL_ADDR     0x10000
#define FB_ADDR     0x20000
#define FB_SIZE     (64 * 64 * sizeof(uint32_t))

#define V3D_FORMAT_RGBA8888 1
#define V3D_FORMAT_BGR565   2

#define V3D_EXPECTED_IDENT0 0x02443356
#define V3D_EXPECTED_IDENT1 0xc0101432

static void append_u32(uint8_t *buffer, size_t *offset, uint32_t value);

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

static void test_v3d_ct0_binning_setup(void)
{
    uint8_t cl[32];
    uint8_t state[4096];
    size_t cl_size = 0;

    memset(state, 0xa5, sizeof(state));
    memwrite(0x32000, state, sizeof(state));

    /* TILE_BINNING_MODE_CONFIG, START_TILE_BINNING, and FLUSH. */
    cl[cl_size++] = 112;
    append_u32(cl, &cl_size, 0x30000);
    append_u32(cl, &cl_size, 0x2000);
    append_u32(cl, &cl_size, 0x32000);
    cl[cl_size++] = 2;
    cl[cl_size++] = 2;
    cl[cl_size++] = 0x04; /* Auto-initialize tile state data. */
    cl[cl_size++] = 6;
    cl[cl_size++] = 4;

    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FLDONE);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + cl_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) &
                    (V3D_CTRUN | V3D_CTERR), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BFC), ==, 1);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FLDONE,
                    ==, V3D_FLDONE);

    memread(0x32000, state, sizeof(state));
    for (unsigned i = 0; i < 4 * 48; i++) {
        g_assert_cmphex(state[i], ==, 0);
    }
    for (unsigned i = 4 * 48; i < sizeof(state); i++) {
        g_assert_cmphex(state[i], ==, 0xa5);
    }
}

static void test_v3d_ct0_flush_count(void)
{
    uint8_t cl[32];
    size_t cl_size = 0;

    cl[cl_size++] = 112;
    append_u32(cl, &cl_size, 0x30000);
    append_u32(cl, &cl_size, 0x2000);
    append_u32(cl, &cl_size, 0x32000);
    cl[cl_size++] = 2;
    cl[cl_size++] = 2;
    cl[cl_size++] = 0x04;
    cl[cl_size++] = 6;

    writel(RASPI3_V3D_BASE + V3D_BFC, 1);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FLDONE);
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + cl_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BFC), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FLDONE,
                    ==, 0);

    cl[cl_size++] = 4;
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FLDONE);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + cl_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BFC), ==, 1);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FLDONE,
                    ==, V3D_FLDONE);
}

static void test_v3d_ct0_primitive_packet_lengths(void)
{
    static const struct {
        uint8_t opcode;
        size_t packet_size;
    } packets[] = {
        { 32, 14 }, /* GL_INDEXED_PRIMITIVE */
        { 33, 10 }, /* GL_ARRAY_PRIMITIVE */
    };

    for (unsigned i = 0; i < G_N_ELEMENTS(packets); i++) {
        uint8_t cl[64];
        size_t cl_size = 0;

        cl[cl_size++] = 112;
        append_u32(cl, &cl_size, 0x30000);
        append_u32(cl, &cl_size, 0x2000);
        append_u32(cl, &cl_size, 0x32000);
        cl[cl_size++] = 2;
        cl[cl_size++] = 2;
        cl[cl_size++] = 0x04;
        cl[cl_size++] = 6;
        cl[cl_size++] = packets[i].opcode;
        memset(cl + cl_size, 0, packets[i].packet_size - 1);
        cl_size += packets[i].packet_size - 1;

        writel(RASPI3_V3D_BASE + V3D_CT0CS, 1U << 15);
        memwrite(CL_ADDR, cl, cl_size);
        writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
        writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + cl_size);

        g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) & V3D_CTERR,
                        ==, V3D_CTERR);
        g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CA),
                        ==, CL_ADDR + cl_size);
    }
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

static void append_u48(uint8_t *buffer, size_t *offset, uint64_t value)
{
    for (unsigned i = 0; i < 6; i++) {
        buffer[(*offset)++] = value >> (i * 8);
    }
}

static uint32_t read_u32(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
           ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

static void append_tile_buffer_packet(uint8_t *buffer, size_t *offset,
                                      uint8_t opcode, uint32_t address,
                                      unsigned memory_format,
                                      unsigned color_format)
{
    buffer[(*offset)++] = opcode;
    append_u48(buffer, offset,
               ((uint64_t)(address >> 4) << 20) |
               ((uint64_t)color_format << 8) |
               ((uint64_t)memory_format << 4) | 1);
}

static uint16_t read_u16(const uint8_t *buffer)
{
    return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

static uint16_t pack_bgr565(uint32_t rgba)
{
    uint8_t red = rgba;
    uint8_t green = rgba >> 8;
    uint8_t blue = rgba >> 16;

    return ((uint16_t)(blue >> 3) << 11) |
           ((uint16_t)(green >> 2) << 5) | (red >> 3);
}

static size_t make_clear_list(uint8_t *buffer, uint32_t target,
                              uint16_t width, uint16_t height,
                              unsigned memory_format, unsigned color_format,
                              uint8_t tile_x,
                              uint8_t tile_y, uint32_t color)
{
    size_t offset = 0;

    /* TILE_RENDERING_MODE_CONFIG: RGBA8888 plus the requested memory format. */
    buffer[offset++] = 113;
    append_u32(buffer, &offset, target);
    append_u16(buffer, &offset, width);
    append_u16(buffer, &offset, height);
    append_u16(buffer, &offset, (memory_format << 6) | (color_format << 2));

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

static void test_v3d_ct1_frame_done_requires_eof(void)
{
    uint8_t cl[64];
    uint8_t fb[32 * 32 * sizeof(uint32_t)];
    size_t cl_size = make_clear_list(cl, FB_ADDR, 32, 32, 0,
                                    V3D_FORMAT_RGBA8888, 0, 0,
                                    0xff123456);

    writel(RASPI3_V3D_BASE + V3D_RFC, 1);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FRDONE);
    cl[cl_size - 1] = 24; /* Store the tile without ending the frame. */
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_RFC), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FRDONE,
                    ==, 0);

    cl[cl_size - 1] = 25;
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FRDONE);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_RFC), ==, 1);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FRDONE,
                    ==, V3D_FRDONE);
    memread(FB_ADDR, fb, sizeof(fb));
    g_assert_cmphex(read_u32(fb), ==, 0xff123456);
}

static void submit_clear(unsigned memory_format, uint8_t tile_x,
                         uint8_t tile_y, uint32_t color)
{
    uint8_t cl[64];
    size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, memory_format,
                                     V3D_FORMAT_RGBA8888,
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
    size_t cl_size = make_clear_list(cl, FB_ADDR, 32, 32, 0,
                                     V3D_FORMAT_RGBA8888, 0, 0, color);

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

static void test_v3d_ct1_clear_linear_bgr565(void)
{
    uint8_t cl[64];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0x11223344;
    uint16_t expected = pack_bgr565(color);
    size_t cl_size = make_clear_list(cl, FB_ADDR, 32, 32, 0,
                                    V3D_FORMAT_BGR565, 0, 0, color);

    g_assert_cmphex(expected, ==, 0x2188);
    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);

    memread(FB_ADDR, fb, 32 * 32 * sizeof(uint16_t));
    for (size_t i = 0; i < 32 * 32; i++) {
        g_assert_cmphex(read_u16(fb + i * sizeof(uint16_t)), ==, expected);
    }
}

static void assert_bgr565_block(const uint8_t *fb, size_t offset,
                                uint16_t color)
{
    for (size_t i = 0; i < 32 * 32 * sizeof(uint16_t); i += sizeof(uint16_t)) {
        g_assert_cmphex(read_u16(fb + offset + i), ==, color);
    }
}

static void test_v3d_ct1_clear_t_tiled_bgr565(void)
{
    static const uint32_t colors[4] = {
        0x01020304, 0x11223344, 0x55667788, 0x99aabbcc,
    };
    uint8_t cl[64];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));

    for (unsigned tile = 0; tile < G_N_ELEMENTS(colors); tile++) {
        size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, 1,
                                         V3D_FORMAT_BGR565,
                                         tile & 1, tile >> 1, colors[tile]);

        memwrite(CL_ADDR, cl, cl_size);
        writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
        writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    }

    memread(FB_ADDR, fb, sizeof(fb));
    assert_bgr565_block(fb, 0, pack_bgr565(colors[0]));
    assert_bgr565_block(fb, 2048, pack_bgr565(colors[1]));
    /* T-tile rows alternate direction in memory. */
    assert_bgr565_block(fb, 6144, pack_bgr565(colors[2]));
    assert_bgr565_block(fb, 4096, pack_bgr565(colors[3]));
}

static void test_v3d_ct1_clear_lt_tiled_bgr565(void)
{
    uint8_t cl[64];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0x11223344;
    uint16_t expected = pack_bgr565(color);
    size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, 2,
                                    V3D_FORMAT_BGR565, 1, 1, color);

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    memread(FB_ADDR, fb, sizeof(fb));

    for (unsigned utile_y = 0; utile_y < 8; utile_y++) {
        for (unsigned utile_x = 0; utile_x < 8; utile_x++) {
            size_t offset = (32 + utile_y * 4) * 128 +
                            (32 + utile_x * 4) * 8;

            for (unsigned pixel = 0; pixel < 16; pixel++) {
                g_assert_cmphex(read_u16(fb + offset + pixel * 2),
                                ==, expected);
            }
        }
    }
}

static void test_v3d_ct1_load_store_color_tile(void)
{
    const uint32_t src = FB_ADDR;
    const uint32_t dst = FB_ADDR + FB_SIZE;
    uint8_t cl[96];
    uint8_t source[FB_SIZE];
    uint8_t destination[FB_SIZE];
    uint8_t result[FB_SIZE];

    for (unsigned color_format_i = 0; color_format_i < 2; color_format_i++) {
        unsigned color_format = color_format_i == 0 ? 0 : 2;
        unsigned bytes_per_pixel = color_format == 0 ? 4 : 2;
        size_t surface_size = 64 * 64 * bytes_per_pixel;

        for (unsigned i = 0; i < 64 * 64; i++) {
            if (color_format == 0) {
                uint32_t color = 0xff000000 | i;

                source[i * 4] = color;
                source[i * 4 + 1] = color >> 8;
                source[i * 4 + 2] = color >> 16;
                source[i * 4 + 3] = color >> 24;
            } else {
                uint16_t pixel = i * 13;

                source[i * 2] = pixel;
                source[i * 2 + 1] = pixel >> 8;
            }
        }
        memwrite(src, source, surface_size);

        for (unsigned memory_format = 0; memory_format <= 2; memory_format++) {
            size_t offset = 0;

            memset(destination, 0xa5, sizeof(destination));
            memwrite(dst, destination, surface_size);

            /* Copy a complete 64x64 surface as four V3D color tiles. */
            cl[offset++] = 113;
            append_u32(cl, &offset, src);
            append_u16(cl, &offset, 64);
            append_u16(cl, &offset, 64);
            append_u16(cl, &offset, 0);
            for (unsigned tile_y = 0; tile_y < 2; tile_y++) {
                for (unsigned tile_x = 0; tile_x < 2; tile_x++) {
                    cl[offset++] = 115;
                    cl[offset++] = tile_x;
                    cl[offset++] = tile_y;
                    append_tile_buffer_packet(cl, &offset, 29, src,
                                              memory_format, color_format);
                    append_tile_buffer_packet(cl, &offset, 28, dst,
                                              memory_format, color_format);
                }
            }

            memwrite(CL_ADDR, cl, offset);
            writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
            writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + offset);
            memread(dst, result, surface_size);
            g_assert_cmpmem(result, surface_size, source, surface_size);

            /* The resolved multi-sample store consumes the loaded tile too. */
            memset(destination, 0xa5, sizeof(destination));
            memwrite(dst, destination, surface_size);
            offset = 0;
            cl[offset++] = 113;
            append_u32(cl, &offset, dst);
            append_u16(cl, &offset, 64);
            append_u16(cl, &offset, 64);
            append_u16(cl, &offset, (memory_format << 6) |
                       ((color_format == 0 ? 1 : 2) << 2));
            for (unsigned tile_y = 0; tile_y < 2; tile_y++) {
                for (unsigned tile_x = 0; tile_x < 2; tile_x++) {
                    cl[offset++] = 115;
                    cl[offset++] = tile_x;
                    cl[offset++] = tile_y;
                    append_tile_buffer_packet(cl, &offset, 29, src,
                                              memory_format, color_format);
                    cl[offset++] = 25;
                }
            }
            memwrite(CL_ADDR, cl, offset);
            writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
            writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + offset);
            memread(dst, result, surface_size);
            g_assert_cmpmem(result, surface_size, source, surface_size);
        }
    }
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/v3d/identity_and_scratch",
                   test_v3d_identity_and_scratch);
    qtest_add_func("/bcm2835/v3d/irq_masks", test_v3d_irq_masks);
    qtest_add_func("/bcm2835/v3d/ct0_binning_setup",
                   test_v3d_ct0_binning_setup);
    qtest_add_func("/bcm2835/v3d/ct0_flush_count",
                   test_v3d_ct0_flush_count);
    qtest_add_func("/bcm2835/v3d/ct0_primitive_packet_lengths",
                   test_v3d_ct0_primitive_packet_lengths);
    qtest_add_func("/bcm2835/v3d/ct1_clear_linear",
                   test_v3d_ct1_clear_linear);
    qtest_add_func("/bcm2835/v3d/ct1_clear_t_tiled",
                   test_v3d_ct1_clear_t_tiled);
    qtest_add_func("/bcm2835/v3d/ct1_clear_lt_tiled",
                   test_v3d_ct1_clear_lt_tiled);
    qtest_add_func("/bcm2835/v3d/ct1_clear_linear_bgr565",
                   test_v3d_ct1_clear_linear_bgr565);
    qtest_add_func("/bcm2835/v3d/ct1_clear_t_tiled_bgr565",
                   test_v3d_ct1_clear_t_tiled_bgr565);
    qtest_add_func("/bcm2835/v3d/ct1_clear_lt_tiled_bgr565",
                   test_v3d_ct1_clear_lt_tiled_bgr565);
    qtest_add_func("/bcm2835/v3d/ct1_load_store_color_tile",
                   test_v3d_ct1_load_store_color_tile);
    qtest_add_func("/bcm2835/v3d/ct1_frame_done_requires_eof",
                   test_v3d_ct1_frame_done_requires_eof);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();
    return ret;
}
