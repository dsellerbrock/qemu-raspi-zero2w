/*
 * QTest for the Raspberry Pi VideoCore IV V3D register block.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest-single.h"
#include "hw/misc/bcm2835_v3d_qpu.h"

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
static void append_u16(uint8_t *buffer, size_t *offset, uint16_t value);
static uint32_t read_u32(const uint8_t *buffer);

typedef struct QPUTestUniform {
    uint32_t expected_address;
    uint32_t value;
    unsigned reads;
} QPUTestUniform;

static bool qpu_test_read_uniform(void *opaque, uint32_t address,
                                  uint32_t *value)
{
    QPUTestUniform *uniform = opaque;

    if (address != uniform->expected_address) {
        return false;
    }
    *value = uniform->value;
    uniform->reads++;
    return true;
}

static uint64_t qpu_test_add_imm(unsigned raddr_a, unsigned waddr_add,
                                unsigned imm)
{
    return (13ULL << 60) | (1ULL << 49) | (uint64_t)waddr_add << 38 |
           (39ULL << 32) | (12ULL << 24) | (uint64_t)raddr_a << 18 |
           (uint64_t)imm << 12 | (6ULL << 9) | (7ULL << 6) | 7ULL;
}

static uint64_t qpu_test_load_imm(unsigned waddr_add, uint32_t value)
{
    return (14ULL << 60) | (1ULL << 49) | (uint64_t)waddr_add << 38 |
           (39ULL << 32) | value;
}

static uint64_t qpu_test_branch(unsigned cond, bool relative, int32_t offset)
{
    return (15ULL << 60) | (uint64_t)cond << 56 |
           (uint64_t)relative << 55 | (39ULL << 42) | (39ULL << 36) |
           (uint32_t)offset;
}

static void qpu_test_store_code(uint8_t code[16], uint64_t instruction)
{
    stq_le_p(code, instruction);
    stq_le_p(code + 8, 3ULL << 60); /* Program End. */
}

static void test_v3d_qpu_uniform_read(void)
{
    uint8_t code[16];
    BCM2835V3DQPUState qpu;
    QPUTestUniform uniform = {
        .expected_address = 0x1000,
        .value = 0x12345678,
    };
    uint32_t executed;

    qpu_test_store_code(code, qpu_test_add_imm(32, 0, 1));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.uniform_address = uniform.expected_address;
    qpu.read_uniform = qpu_test_read_uniform;
    qpu.uniform_opaque = &uniform;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(executed, ==, 2);
    g_assert_cmpuint(uniform.reads, ==, 1);
    g_assert_cmphex(qpu.uniform_address, ==, 0x1004);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][0][lane], ==, uniform.value + 1);
    }
}

static void test_v3d_qpu_vpm_vector_io(void)
{
    uint8_t code[16];
    BCM2835V3DQPUState qpu;
    uint32_t executed;

    qpu_test_store_code(code, qpu_test_add_imm(48, 0, 0));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.vpm_read_setup = true;
    qpu.vpm_read_address = 2;
    qpu.vpm_read_stride = 1;
    qpu.vpm_read_remaining = 1;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.vpm[2][lane] = lane + 0x100;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][0][lane], ==, lane + 0x100);
    }
    g_assert_cmpuint(qpu.vpm_read_remaining, ==, 0);

    qpu_test_store_code(code, qpu_test_add_imm(0, 48, 0));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.vpm_write_setup = true;
    qpu.vpm_write_address = 3;
    qpu.vpm_write_stride = 1;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = lane * 17;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(qpu.vpm_write_address, ==, 4);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.vpm[3][lane], ==, lane * 17);
    }
}

static void test_v3d_qpu_vpm_setup_and_write(void)
{
    uint8_t code[9 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t setup = (1U << 12) | (1U << 11) | (2U << 8) | 4;

    stq_le_p(code, qpu_test_load_imm(49, setup) | (1ULL << 44));
    for (unsigned i = 0; i < 7; i++) {
        stq_le_p(code + (i + 1) * 8,
                 qpu_test_add_imm(0, 48, 0) | (1ULL << 44));
    }
    stq_le_p(code + 8 * 8, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = 0x100 + lane;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 16, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(qpu.vpm_write_address, ==, 11);
    g_assert_cmpuint(qpu.vpm_write_history_count, ==,
                     BCM2835_V3D_QPU_VPM_WRITE_HISTORY);
    g_assert_cmpuint(qpu.vpm_write_total, ==,
                     BCM2835_V3D_QPU_VPM_WRITE_HISTORY);
    for (unsigned row = 0; row < 7; row++) {
        g_assert_cmpuint(qpu.vpm_write_history[row], ==, row + 4);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            g_assert_cmphex(qpu.vpm[row + 4][lane], ==, 0x100 + lane);
        }
    }
}

static void test_v3d_qpu_branch_delay_slots(void)
{
    uint8_t code[7 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;

    stq_le_p(code, qpu_test_branch(15, true, 8));
    stq_le_p(code + 8, qpu_test_load_imm(0, 0x11));
    stq_le_p(code + 16, qpu_test_load_imm(1, 0x22));
    stq_le_p(code + 24, qpu_test_load_imm(2, 0x33));
    stq_le_p(code + 32, qpu_test_load_imm(3, 0x44));
    stq_le_p(code + 40, qpu_test_load_imm(4, 0x55));
    stq_le_p(code + 48, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 16, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(executed, ==, 6);
    g_assert_cmphex(qpu.rf[0][0][0], ==, 0x11);
    g_assert_cmphex(qpu.rf[0][1][0], ==, 0x22);
    g_assert_cmphex(qpu.rf[0][2][0], ==, 0x33);
    g_assert_cmphex(qpu.rf[0][3][0], ==, 0);
    g_assert_cmphex(qpu.rf[0][4][0], ==, 0x55);
}

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

static void test_v3d_ct0_shader_state_dma(void)
{
    uint8_t cl[64];
    uint8_t record[44] = { 0 };

    record[3] = 2; /* Fragment shader varying count. */
    record[14] = 1; /* Vertex shader attribute array select. */
    record[15] = 12; /* Vertex shader total attribute size. */
    record[16] = 0x00;
    record[17] = 0x60; /* Vertex shader code: 0x6000. */
    record[20] = 0x00;
    record[21] = 0x61; /* Vertex shader uniforms: 0x6100. */
    record[28] = 0x00;
    record[29] = 0x62; /* Coordinate shader code: 0x6200. */
    record[36] = 0x00;
    record[37] = 0x70; /* Attribute array 0 base: 0x7000. */
    record[40] = 11; /* Attribute array byte count minus one. */
    record[41] = 12; /* Attribute array stride. */
    memwrite(0x34000, record, sizeof(record));

    for (unsigned invalid = 0; invalid < 2; invalid++) {
        size_t cl_size = 0;
        uint32_t pointer = invalid ? 0x7f000001 : 0x34001;

        cl[cl_size++] = 112;
        append_u32(cl, &cl_size, 0x30000);
        append_u32(cl, &cl_size, 0x2000);
        append_u32(cl, &cl_size, 0x32000);
        cl[cl_size++] = 1;
        cl[cl_size++] = 1;
        cl[cl_size++] = 0x04;
        cl[cl_size++] = 6;
        cl[cl_size++] = 64;
        append_u32(cl, &cl_size, pointer);

        writel(RASPI3_V3D_BASE + V3D_CT0CS, 1U << 15);
        memwrite(CL_ADDR, cl, cl_size);
        writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
        writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + cl_size);

        g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) & V3D_CTERR,
                        ==, invalid ? V3D_CTERR : 0);
    }
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
                        ==, 0);
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

static void test_v3d_ct0_rasterizes_coordinate_triangle(void)
{
    static const uint32_t screen_xy[] = {
        (352U << 16) | (uint16_t)-352,
        (352U << 16) | 352,
        ((uint16_t)-352 << 16),
    };
    uint8_t render_cl[32];
    uint8_t bin_cl[64];
    uint8_t draw_cl[32];
    uint8_t tile_head[6];
    uint8_t tile_list[23];
    uint8_t record[44] = { 0 };
    uint8_t code[13 * sizeof(uint64_t)];
    uint8_t vertices[sizeof(screen_xy)];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    size_t render_size = 0;
    size_t bin_size = 0;
    size_t draw_size = 0;
    uint32_t draw_color = 0xff00ff00;
    uint32_t setup_write = (1U << 12) | (1U << 11) | (2U << 8) | 8;
    uint32_t setup_read = (1U << 20) | (1U << 11) | (2U << 8);
    uint64_t instructions[] = {
        qpu_test_load_imm(49, setup_write) | (1ULL << 44),
        qpu_test_load_imm(0, 0),
        qpu_test_load_imm(1, 0x3f800000),
        qpu_test_load_imm(49, setup_read),
        qpu_test_add_imm(48, 2, 0),
        qpu_test_add_imm(0, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(0, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(0, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(1, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(2, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(0, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(1, 48, 0) | (1ULL << 44),
        3ULL << 60,
    };

    memset(initial, 0x5a, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    for (unsigned i = 0; i < G_N_ELEMENTS(screen_xy); i++) {
        stl_le_p(vertices + i * sizeof(uint32_t), screen_xy[i]);
    }
    stl_le_p(record + 28, 0x6200);
    record[26] = 1;
    stl_le_p(record + 36, 0x7000);
    record[40] = sizeof(uint32_t) - 1;
    record[41] = sizeof(uint32_t);
    memwrite(0x34000, record, sizeof(record));
    memwrite(0x7000, vertices, sizeof(vertices));
    for (unsigned i = 0; i < G_N_ELEMENTS(instructions); i++) {
        stq_le_p(code + i * sizeof(uint64_t), instructions[i]);
    }
    memwrite(0x6200, code, sizeof(code));

    render_cl[render_size++] = 113;
    append_u32(render_cl, &render_size, FB_ADDR);
    append_u16(render_cl, &render_size, 64);
    append_u16(render_cl, &render_size, 64);
    append_u16(render_cl, &render_size, 1U << 2);
    render_cl[render_size++] = 114;
    append_u32(render_cl, &render_size, draw_color);
    append_u32(render_cl, &render_size, 0);
    append_u32(render_cl, &render_size, 0);
    render_cl[render_size++] = 0;
    memwrite(CL_ADDR, render_cl, render_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + render_size);

    bin_cl[bin_size++] = 112;
    append_u32(bin_cl, &bin_size, 0x30000);
    append_u32(bin_cl, &bin_size, 0x2000);
    append_u32(bin_cl, &bin_size, 0x32000);
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 0x04;
    bin_cl[bin_size++] = 6;
    bin_cl[bin_size++] = 64;
    append_u32(bin_cl, &bin_size, 0x34001);
    bin_cl[bin_size++] = 33;
    bin_cl[bin_size++] = 4;
    append_u32(bin_cl, &bin_size, 3);
    append_u32(bin_cl, &bin_size, 0);
    bin_cl[bin_size++] = 4;
    memwrite(CL_ADDR, bin_cl, bin_size);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + bin_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) & V3D_CTERR, ==, 0);
    memread(FB_ADDR, fb, sizeof(fb));
    g_assert_cmphex(read_u32(fb + (30 * 64 + 32) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);
    g_assert_cmphex(read_u32(fb + (1 * 64 + 1) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);
    memread(0x30000, tile_head, sizeof(tile_head));
    memread(0x30024, tile_list, sizeof(tile_list));
    g_assert_cmphex(tile_head[0], ==, 17);
    g_assert_cmphex(read_u32(tile_head + 1), ==, 0x30024);
    g_assert_cmphex(tile_list[0], ==, 56);
    g_assert_cmphex(tile_list[1], ==, 0x32);
    g_assert_cmphex(tile_list[7], ==, 48);
    g_assert_cmphex(tile_list[8], ==, 129);
    for (unsigned i = 0; i < G_N_ELEMENTS(screen_xy); i++) {
        g_assert_cmphex(read_u32(tile_list + 9 + i * sizeof(uint32_t)),
                        ==, screen_xy[i]);
    }
    g_assert_cmphex(tile_list[21], ==, 128);
    g_assert_cmphex(tile_list[22], ==, 18);

    /* CT1 loads the tile, follows the binner's tile sublist, and stores it. */
    draw_cl[draw_size++] = 29;
    append_u48(draw_cl, &draw_size,
               ((uint64_t)(FB_ADDR >> 4) << 20) | 1);
    draw_cl[draw_size++] = 115;
    draw_cl[draw_size++] = 0;
    draw_cl[draw_size++] = 0;
    draw_cl[draw_size++] = 17;
    append_u32(draw_cl, &draw_size, 0x30000);
    draw_cl[draw_size++] = 28;
    append_u48(draw_cl, &draw_size,
               ((uint64_t)(FB_ADDR >> 4) << 20) | 1 | (1ULL << 19));
    memwrite(CL_ADDR, draw_cl, draw_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + draw_size);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) & V3D_CTERR, ==, 0);

    memread(FB_ADDR, fb, sizeof(fb));
    g_assert_cmphex(read_u32(fb + (30 * 64 + 32) * sizeof(uint32_t)),
                    ==, draw_color);
    g_assert_cmphex(read_u32(fb + (1 * 64 + 1) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);
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

static void test_v3d_ct1_clear_single_sample_64x64_tile(void)
{
    uint8_t cl[64];
    uint8_t initial[64 * 64 * sizeof(uint32_t)];
    uint8_t fb[sizeof(initial)];
    uint32_t color = 0xff123456;
    size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, 0,
                                    V3D_FORMAT_RGBA8888, 0, 0, color);

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    memread(FB_ADDR, fb, sizeof(fb));

    for (size_t i = 0; i < 64 * 64; i++) {
        g_assert_cmphex(read_u32(fb + i * sizeof(uint32_t)), ==, color);
    }
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
    for (size_t i = 0; i < 64 * 64 * sizeof(uint32_t);
         i += sizeof(uint32_t)) {
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

    memread(FB_ADDR, fb, sizeof(fb));
    assert_color_block(fb, 0, colors[0]);
}

static void test_v3d_ct1_clear_lt_tiled(void)
{
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0x01020304;

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FRDONE);
    writel(RASPI3_V3D_BASE + V3D_INTENA, V3D_FRDONE);

    submit_clear(2, 0, 0, color);

    memread(FB_ADDR, fb, sizeof(fb));
    assert_color_block(fb, 0, color);
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
    for (size_t i = 0; i < 64 * 64 * sizeof(uint16_t);
         i += sizeof(uint16_t)) {
        g_assert_cmphex(read_u16(fb + offset + i), ==, color);
    }
}

static size_t t_tiled_pixel_offset(uint32_t width, unsigned x, unsigned y,
                                   unsigned bytes_per_pixel)
{
    static const uint8_t even_stile_map[4] = { 0, 3, 1, 2 };
    static const uint8_t odd_stile_map[4] = { 2, 1, 3, 0 };
    uint32_t tiles_x = (width + 31) / 32;
    uint32_t tile_x = x / 32;
    uint32_t tile_y = y / 32;
    uint32_t local_x = x % 32;
    uint32_t local_y = y % 32;
    uint32_t stile = ((local_y / 16) << 1) | (local_x / 16);
    uint32_t utile = ((local_y / 4) & 3) * 4 + ((local_x / 4) & 3);
    const uint8_t *stile_map = (tile_y & 1) ? odd_stile_map :
                                               even_stile_map;

    if (tile_y & 1) {
        tile_x = tiles_x - tile_x - 1;
    }
    return ((size_t)tile_y * tiles_x + tile_x) * 32 * 32 *
           bytes_per_pixel + stile_map[stile] * 16 * 16 * bytes_per_pixel +
           utile * 16 * bytes_per_pixel +
           ((local_y & 3) * 4 + (local_x & 3)) * bytes_per_pixel;
}

static size_t lt_tiled_pixel_offset(uint32_t width, unsigned x, unsigned y,
                                    unsigned bytes_per_pixel)
{
    uint32_t utiles_x = (width + 3) / 4;

    return ((size_t)(y / 4) * utiles_x + x / 4) * 16 * bytes_per_pixel +
           ((y & 3) * 4 + (x & 3)) * bytes_per_pixel;
}

static void test_v3d_ct1_clear_t_tiled_bgr565(void)
{
    uint8_t cl[64];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0x01020304;

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));

    size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, 1,
                                     V3D_FORMAT_BGR565, 0, 0, color);
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);

    memread(FB_ADDR, fb, sizeof(fb));
    assert_bgr565_block(fb, 0, pack_bgr565(color));
}

static void test_v3d_ct1_clear_lt_tiled_bgr565(void)
{
    uint8_t cl[64];
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0x11223344;
    uint16_t expected = pack_bgr565(color);
    size_t cl_size = make_clear_list(cl, FB_ADDR, 64, 64, 2,
                                    V3D_FORMAT_BGR565, 0, 0, color);

    memset(initial, 0xa5, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    memread(FB_ADDR, fb, sizeof(fb));

    assert_bgr565_block(fb, 0, expected);
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

            /* Copy one complete 64x64 single-sample render tile. */
            cl[offset++] = 113;
            append_u32(cl, &offset, src);
            append_u16(cl, &offset, 64);
            append_u16(cl, &offset, 64);
            append_u16(cl, &offset, 0);
            append_tile_buffer_packet(cl, &offset, 29, src,
                                      memory_format, color_format);
            cl[offset++] = 115;
            cl[offset++] = 0;
            cl[offset++] = 0;
            append_tile_buffer_packet(cl, &offset, 28, dst,
                                      memory_format, color_format);

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
            append_tile_buffer_packet(cl, &offset, 29, src,
                                      memory_format, color_format);
            cl[offset++] = 115;
            cl[offset++] = 0;
            cl[offset++] = 0;
            cl[offset++] = 25;
            memwrite(CL_ADDR, cl, offset);
            writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
            writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + offset);
            memread(dst, result, surface_size);
            g_assert_cmpmem(result, surface_size, source, surface_size);
        }
    }
}

static void test_v3d_ct1_load_store_layout_conversion(void)
{
    const uint32_t src = FB_ADDR;
    const uint32_t dst = FB_ADDR + FB_SIZE;
    uint8_t cl[64];
    uint8_t source[64 * 64 * sizeof(uint32_t)];
    uint8_t destination[sizeof(source)];
    uint8_t result[sizeof(source)];

    for (unsigned y = 0; y < 64; y++) {
        for (unsigned x = 0; x < 64; x++) {
            uint32_t color = 0xff000000 | (y * 64 + x);
            size_t offset = ((size_t)y * 64 + x) * sizeof(color);

            source[offset] = color;
            source[offset + 1] = color >> 8;
            source[offset + 2] = color >> 16;
            source[offset + 3] = color >> 24;
        }
    }
    memwrite(src, source, sizeof(source));

    for (unsigned memory_format = 1; memory_format <= 2; memory_format++) {
        size_t offset = 0;

        memset(destination, 0xa5, sizeof(destination));
        memwrite(dst, destination, sizeof(destination));
        cl[offset++] = 113;
        append_u32(cl, &offset, dst);
        append_u16(cl, &offset, 64);
        append_u16(cl, &offset, 64);
        append_u16(cl, &offset, (memory_format << 6) |
                   (V3D_FORMAT_RGBA8888 << 2));
        append_tile_buffer_packet(cl, &offset, 29, src, 0, 0);
        cl[offset++] = 115;
        cl[offset++] = 0;
        cl[offset++] = 0;
        append_tile_buffer_packet(cl, &offset, 28, dst, memory_format, 0);

        writel(RASPI3_V3D_BASE + V3D_CT1CS, 1U << 15);
        memwrite(CL_ADDR, cl, offset);
        writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
        writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + offset);
        memread(dst, result, sizeof(result));

        for (unsigned y = 0; y < 64; y++) {
            for (unsigned x = 0; x < 64; x++) {
                size_t source_offset = ((size_t)y * 64 + x) *
                                       sizeof(uint32_t);
                size_t dest_offset = memory_format == 1 ?
                    t_tiled_pixel_offset(64, x, y, sizeof(uint32_t)) :
                    lt_tiled_pixel_offset(64, x, y, sizeof(uint32_t));

                g_assert_cmphex(read_u32(result + dest_offset), ==,
                                read_u32(source + source_offset));
            }
        }
    }
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/bcm2835/v3d/qpu_uniform_read",
                   test_v3d_qpu_uniform_read);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_vector_io",
                   test_v3d_qpu_vpm_vector_io);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_setup_and_write",
                   test_v3d_qpu_vpm_setup_and_write);
    qtest_add_func("/bcm2835/v3d/qpu_branch_delay_slots",
                   test_v3d_qpu_branch_delay_slots);
    qtest_add_func("/bcm2835/v3d/identity_and_scratch",
                   test_v3d_identity_and_scratch);
    qtest_add_func("/bcm2835/v3d/irq_masks", test_v3d_irq_masks);
    qtest_add_func("/bcm2835/v3d/ct0_binning_setup",
                   test_v3d_ct0_binning_setup);
    qtest_add_func("/bcm2835/v3d/ct0_flush_count",
                   test_v3d_ct0_flush_count);
    qtest_add_func("/bcm2835/v3d/ct0_shader_state_dma",
                   test_v3d_ct0_shader_state_dma);
    qtest_add_func("/bcm2835/v3d/ct0_rasterizes_coordinate_triangle",
                   test_v3d_ct0_rasterizes_coordinate_triangle);
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
    qtest_add_func("/bcm2835/v3d/ct1_load_store_layout_conversion",
                   test_v3d_ct1_load_store_layout_conversion);
    qtest_add_func("/bcm2835/v3d/ct1_clear_single_sample_64x64_tile",
                   test_v3d_ct1_clear_single_sample_64x64_tile);
    qtest_add_func("/bcm2835/v3d/ct1_frame_done_requires_eof",
                   test_v3d_ct1_frame_done_requires_eof);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();
    return ret;
}
