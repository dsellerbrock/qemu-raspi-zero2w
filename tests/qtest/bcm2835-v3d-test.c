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
#define V3D_PCS     0x130
#define V3D_RFC     0x138
#define V3D_CT0EA   0x108
#define V3D_CT0CA   0x110
#define V3D_BFC     0x134
#define V3D_BPCA    0x300
#define V3D_BPCS    0x304
#define V3D_BPOA    0x308
#define V3D_BPOS    0x30c
#define V3D_CT1CS   0x104
#define V3D_CT1EA   0x10c
#define V3D_CT1CA   0x114

#define V3D_FRDONE  (1U << 0)
#define V3D_FLDONE  (1U << 1)
#define V3D_INT_SPILLUSE (1U << 3)
#define V3D_INT_OUTOMEM (1U << 2)
#define V3D_CTERR   (1U << 3)
#define V3D_CTRUN   (1U << 5)
#define V3D_CTSUBS  (1U << 4)
#define V3D_BMOOM   (1U << 8)
#define CL_ADDR     0x10000
#define FB_ADDR     0x20000
#define FB_SIZE     (64 * 64 * sizeof(uint32_t))

#define V3D_FORMAT_RGBA8888 1
#define V3D_FORMAT_BGR565   2

#define V3D_EXPECTED_IDENT0 0x02443356
#define V3D_EXPECTED_IDENT1 0xc0101431

static void append_u32(uint8_t *buffer, size_t *offset, uint32_t value);
static void append_u16(uint8_t *buffer, size_t *offset, uint16_t value);
static uint32_t read_u32(const uint8_t *buffer);

typedef struct QPUTestUniform {
    uint32_t expected_address;
    uint32_t value;
    unsigned reads;
} QPUTestUniform;

typedef struct QPUTestTexture {
    uint32_t uniform_address;
    uint32_t config[2];
    uint32_t memory_base;
    uint32_t texels[4];
    unsigned memory_format;
    unsigned uniform_reads;
    unsigned memory_reads;
} QPUTestTexture;

typedef struct QPUTestVarying {
    uint32_t partial[BCM2835_V3D_QPU_LANES];
    uint32_t c[BCM2835_V3D_QPU_LANES];
    unsigned reads;
} QPUTestVarying;

typedef struct QPUTestCapturedShader {
    uint32_t uniforms[8];
    uint32_t varying[2][BCM2835_V3D_QPU_LANES];
    uint32_t texels[4];
    uint32_t texture_base;
    unsigned uniform_reads;
    unsigned texture_reads;
} QPUTestCapturedShader;

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

static bool qpu_test_read_texture_uniform(void *opaque, uint32_t address,
                                          uint32_t *value)
{
    QPUTestTexture *texture = opaque;
    uint32_t offset;

    if (address < texture->uniform_address) {
        return false;
    }
    offset = address - texture->uniform_address;
    if (offset >= sizeof(texture->config) ||
        offset % sizeof(uint32_t)) {
        return false;
    }
    *value = texture->config[offset / sizeof(uint32_t)];
    texture->uniform_reads++;
    return true;
}

static bool qpu_test_read_texture_memory(void *opaque, uint32_t address,
                                         uint32_t *value)
{
    QPUTestTexture *texture = opaque;
    uint32_t offset;

    if (address < texture->memory_base) {
        return false;
    }
    offset = address - texture->memory_base;
    if (offset >= sizeof(texture->texels) || offset % sizeof(uint32_t)) {
        return false;
    }
    *value = texture->texels[offset / sizeof(uint32_t)];
    texture->memory_reads++;
    return true;
}

static bool qpu_test_read_texture(void *opaque, uint32_t base,
                                  unsigned memory_format,
                                  unsigned width, unsigned height,
                                  unsigned x, unsigned y, uint32_t *value)
{
    QPUTestTexture *texture = opaque;

    if (base != texture->memory_base || width != height ||
        (width != 2 && width != 24 && width != 32) ||
        x >= width || y >= height ||
        memory_format > 2 ||
        (memory_format && memory_format != (width <= 16 ? 2 : 1))) {
        return false;
    }
    texture->memory_format = memory_format;
    *value = texture->texels[(y >= height / 2 ? 2 : 0) +
                             (x >= width / 2 ? 1 : 0)];
    texture->memory_reads++;
    return true;
}

static uint32_t qpu_test_float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool qpu_test_read_varying(void *opaque, unsigned index,
                                  uint32_t partial[BCM2835_V3D_QPU_LANES],
                                  uint32_t c[BCM2835_V3D_QPU_LANES])
{
    QPUTestVarying *varying = opaque;

    if (index) {
        return false;
    }
    memcpy(partial, varying->partial, sizeof(varying->partial));
    memcpy(c, varying->c, sizeof(varying->c));
    varying->reads++;
    return true;
}

static bool qpu_test_read_captured_uniform(void *opaque, uint32_t address,
                                           uint32_t *value)
{
    QPUTestCapturedShader *shader = opaque;
    uint32_t offset;

    if (address < 0x1000) {
        return false;
    }
    offset = address - 0x1000;
    if (offset >= sizeof(shader->uniforms) || offset % sizeof(uint32_t)) {
        return false;
    }
    *value = shader->uniforms[offset / sizeof(uint32_t)];
    shader->uniform_reads++;
    return true;
}

static bool qpu_test_read_captured_texture(void *opaque, uint32_t base,
                                           unsigned memory_format,
                                           unsigned width, unsigned height,
                                           unsigned x, unsigned y,
                                           uint32_t *value)
{
    QPUTestCapturedShader *shader = opaque;

    if (base != shader->texture_base || memory_format != 2 ||
        width != 2 || height != 2 || x >= width || y >= height) {
        return false;
    }
    *value = shader->texels[y * width + x];
    shader->texture_reads++;
    return true;
}

static bool qpu_test_read_captured_varying(
    void *opaque, unsigned index,
    uint32_t partial[BCM2835_V3D_QPU_LANES],
    uint32_t c[BCM2835_V3D_QPU_LANES])
{
    QPUTestCapturedShader *shader = opaque;

    if (index >= G_N_ELEMENTS(shader->varying)) {
        return false;
    }
    memcpy(partial, shader->varying[index], sizeof(shader->varying[index]));
    memset(c, 0, BCM2835_V3D_QPU_LANES * sizeof(*c));
    return true;
}

static const char captured_labwc_tmu0_opaque[] =
    "3e303e20e04900107e313e21e148021040739e01a708026080749e15671e0210c0769e15271e021000709e00e70900a000799e04e70802191b799e842248421b00799e846248521d09709e80e24960113f009e80e24970d180749e15a70b021000709e00e7090030";

static void append_tile_buffer_packet(uint8_t *buffer, size_t *offset,
                                      uint8_t opcode, uint32_t address,
                                      unsigned memory_format,
                                      unsigned color_format);

static void append_ct1_tmu0_blend_triangle(uint8_t *cl, size_t *cl_size)
{
    cl[(*cl_size)++] = 129;
    append_u32(cl, cl_size, 0xfe00fe00);
    append_u32(cl, cl_size, 0xfe000200);
    append_u32(cl, cl_size, 0x0200fe00);
    for (unsigned i = 0; i < 6; i++) {
        append_u32(cl, cl_size, qpu_test_float_bits(0.25f));
    }
    for (unsigned i = 0; i < 3; i++) {
        append_u32(cl, cl_size, qpu_test_float_bits(1.0f));
    }
}

static void test_v3d_ct1_wgpu_batches_tmu0_blend_shader(void)
{
    enum {
        TILES_X = 20,
        TILES_Y = 13,
        WIDTH = TILES_X * 64,
        HEIGHT = TILES_Y * 64,
        RENDER_BASE = 0x01000000,
    };
    static const uint64_t program[] = {
        UINT64_C(0x100049e0203e303e), UINT64_C(0x100248e1213e317e),
        UINT64_C(0x600208a7019e7340), UINT64_C(0x10021e67159e7480),
        UINT64_C(0x10021e27159e76c0), UINT64_C(0xa00009e7009e7000),
        UINT64_C(0x1d020867049e7900), UINT64_C(0x1b6258002482790e),
        UINT64_C(0x195258c024827906), UINT64_C(0x114059c02082701e),
        UINT64_C(0x1f0049e220827026), UINT64_C(0x113049e3809e7012),
        UINT64_C(0xd17059c0809e003f), UINT64_C(0x10020827179e76c0),
        UINT64_C(0x800009e7009e7000), UINT64_C(0x100049e1609e7020),
        UINT64_C(0x100059eec0027031), UINT64_C(0x300009e7009e7000),
    };
    uint8_t *cl = g_new(uint8_t, 128 + TILES_X * TILES_Y * 128);
    uint8_t record[44] = { 0 };
    uint8_t code[sizeof(program)];
    uint32_t uniforms[] = {
        0x4000, (2U << 20) | (2U << 8) | (1U << 7) |
                (1U << 4) | (1U << 2) | 1U,
        0x3f800000, 0x3f800000, 0x00000000, 0x3f800000,
    };
    uint32_t texels[] = {
        0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f000,
    };
    size_t cl_size = 0;

    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        stq_le_p(code + i * sizeof(uint64_t), program[i]);
    }
    record[3] = 2;
    stl_le_p(record + 4, 0x5000);
    stl_le_p(record + 8, 0x6000);
    memwrite(0x34000, record, sizeof(record));
    memwrite(0x5000, code, sizeof(code));
    memwrite(0x6000, uniforms, sizeof(uniforms));
    memwrite(0x4000, texels, sizeof(texels));

    cl[cl_size++] = 113;
    append_u32(cl, &cl_size, RENDER_BASE);
    append_u16(cl, &cl_size, WIDTH);
    append_u16(cl, &cl_size, HEIGHT);
    append_u16(cl, &cl_size, V3D_FORMAT_RGBA8888 << 2);
    cl[cl_size++] = 114;
    append_u32(cl, &cl_size, 0xff000000);
    append_u32(cl, &cl_size, 0);
    append_u32(cl, &cl_size, 0);
    cl[cl_size++] = 0;
    cl[cl_size++] = 64;
    append_u32(cl, &cl_size, 0x34001);
    for (unsigned tile = 0; tile < TILES_X * TILES_Y; tile++) {
        cl[cl_size++] = 115;
        cl[cl_size++] = tile % TILES_X;
        cl[cl_size++] = tile / TILES_X;
        cl[cl_size++] = 103;
        append_u16(cl, &cl_size, (tile % TILES_X * 64 + 32) * 16);
        append_u16(cl, &cl_size, (tile / TILES_X * 64 + 32) * 16);
        cl[cl_size++] = 56;
        cl[cl_size++] = 0x32;
        cl[cl_size++] = 48;
        append_ct1_tmu0_blend_triangle(cl, &cl_size);
        if (tile == 0) {
            /* Force an ordered overlap to exercise the batch flush boundary. */
            append_ct1_tmu0_blend_triangle(cl, &cl_size);
        }
        cl[cl_size++] = 128;
        append_tile_buffer_packet(cl, &cl_size, 28, RENDER_BASE, 0, 0);
        if (tile == 2) {
            append_tile_buffer_packet(cl, &cl_size, 29, RENDER_BASE, 0, 0);
            cl[cl_size++] = 115;
            cl[cl_size++] = 0;
            cl[cl_size++] = 0;
            cl[cl_size++] = 103;
            append_u16(cl, &cl_size, 32 * 16);
            append_u16(cl, &cl_size, 32 * 16);
            cl[cl_size++] = 56;
            cl[cl_size++] = 0x32;
            cl[cl_size++] = 48;
            append_ct1_tmu0_blend_triangle(cl, &cl_size);
            cl[cl_size++] = 128;
            append_tile_buffer_packet(cl, &cl_size, 28, RENDER_BASE, 0, 0);
        }
    }
    cl[cl_size++] = 0;
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    for (unsigned retries = 0;
         retries < 5000 &&
         (readl(RASPI3_V3D_BASE + V3D_CT1CS) & V3D_CTRUN);
         retries++) {
        g_usleep(1000);
    }
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) &
                    (V3D_CTRUN | V3D_CTERR), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CA), ==,
                    CL_ADDR + cl_size);

    size_t frame_size = (size_t)WIDTH * HEIGHT * sizeof(uint32_t);
    g_autofree uint8_t *frame = g_malloc(frame_size);
    g_autofree gchar *digest = NULL;

    memread(RENDER_BASE, frame, frame_size);
    digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, frame, frame_size);
    g_test_message("TMU0 blend frame SHA-256: %s", digest);
    g_assert_cmpstr(digest, ==,
                    "89283327c0894aca74c2fd7403f8b5dfc7fc91ee3f8369f8f13c7c4dc94efdf5");
    g_free(cl);
}

static void test_v3d_qpu_captured_labwc_shaders(void)
{
    static const char *programs[] = {
        captured_labwc_tmu0_opaque,
        "3e303e20e04900107e313e21e148021040739e01a708026080749e15671e0210c0769e15271e021000709e00e70900a000799e046708021d0e7982242148621b06798224e14852191e708220e14940113f009e80e14970d140729e15a70b021000709e00e7090030",
        "3e303e20e04900107e313e21e148021040739e01a708026080749e15671e0210c0769e15271e021000709e00e70900a000799e046708021d0e7982240058621b06798224c05852191e708220c059401126708220e249001f12709e80e34930113f009e80c05970d1c0769e172708021000709e00e709008020709e60e1490010317002c0ee59001000709e00e7090030",
    };
    static const unsigned expected_uniform_reads[] = { 2, 5, 6 };
    static const uint32_t expected_colors[] = {
        0xff203040, 0xff203000, 0xff5b8977,
    };
    QPUTestCapturedShader shader = {
        .uniforms = {
            0x4000, (2U << 20) | (2U << 8) | (1U << 7) |
                    (1U << 4) | (1U << 2) | 1U,
            0x3f800000, 0x3f800000, 0x00000000, 0x3f800000,
        },
        .texels = { 0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f000 },
        .texture_base = 0x4000,
    };

    for (unsigned program = 0; program < G_N_ELEMENTS(programs); program++) {
        uint8_t code[18 * sizeof(uint64_t)];
        BCM2835V3DQPUState qpu;
        uint32_t executed;
        BCM2835V3DQPUResult result;
        size_t code_size = strlen(programs[program]) / 2;

        g_assert_cmpuint(code_size, <=, sizeof(code));
        for (unsigned i = 0; i < code_size; i++) {
            unsigned byte;

            g_assert_cmpint(sscanf(programs[program] + i * 2, "%2x",
                                   &byte), ==, 1);
            code[i] = byte;
        }
        shader.uniform_reads = 0;
        shader.texture_reads = 0;
        bcm2835_v3d_qpu_init(&qpu);
        qpu.fragment_shader = true;
        qpu.uniform_address = 0x1000;
        qpu.read_uniform = qpu_test_read_captured_uniform;
        qpu.uniform_opaque = &shader;
        qpu.read_texture = qpu_test_read_captured_texture;
        qpu.texture_opaque = &shader;
        qpu.read_varying = qpu_test_read_captured_varying;
        qpu.varying_opaque = &shader;
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            float s = (lane & 1) ? 0.75f : 0.25f;
            float t = (lane & 2) ? 0.75f : 0.25f;

            qpu.fragment_w[lane] = qpu_test_float_bits(1.0f);
            shader.varying[0][lane] = qpu_test_float_bits(s);
            shader.varying[1][lane] = qpu_test_float_bits(t);
            qpu.io[45][lane] = 0x20406080;
        }
        result = bcm2835_v3d_qpu_run_until_tmu0(code, code_size, 16,
                                                &qpu, &executed);
        g_assert_cmpint(result, ==, BCM2835_V3D_QPU_TMU0_LOADED);
        g_assert_cmpuint(qpu.pc, ==, 6);
        g_assert_cmpuint(shader.uniform_reads, ==, 2);
        g_assert_cmpuint(shader.texture_reads, ==,
                         BCM2835_V3D_QPU_LANES);
        g_assert_cmphex(qpu.acc[4][0], ==, shader.texels[0]);

        if (program < 2) {
            BCM2835V3DQPUState sampler;
            uint32_t samples[BCM2835_V3D_QPU_LANES];
            uint32_t varying_partial[2 * BCM2835_V3D_QPU_LANES];

            sampler.read_texture = qpu_test_read_captured_texture;
            sampler.texture_opaque = &shader;
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                varying_partial[lane * 2] = shader.varying[0][lane];
                varying_partial[lane * 2 + 1] = shader.varying[1][lane];
            }
            shader.texture_reads = 0;
            g_assert_true(bcm2835_v3d_qpu_sample_tmu0_batch(
                &sampler, shader.uniforms[0], shader.uniforms[1],
                varying_partial, qpu.fragment_w, 2,
                BCM2835_V3D_QPU_LANES, samples));
            g_assert_cmpuint(shader.texture_reads, ==,
                             BCM2835_V3D_QPU_LANES);
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                g_assert_cmphex(samples[lane], ==, qpu.acc[4][lane]);
            }

            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                varying_partial[lane * 2] =
                    qpu_test_float_bits(0.25f);
                varying_partial[lane * 2 + 1] =
                    qpu_test_float_bits(0.25f);
            }
            shader.texture_reads = 0;
            g_assert_true(bcm2835_v3d_qpu_sample_tmu0_batch(
                &sampler, shader.uniforms[0], shader.uniforms[1],
                varying_partial, qpu.fragment_w, 2,
                BCM2835_V3D_QPU_LANES, samples));
            g_assert_cmpuint(shader.texture_reads, ==, 1);
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                g_assert_cmphex(samples[lane], ==, qpu.acc[4][0]);
            }
        }

        shader.uniform_reads = 0;
        shader.texture_reads = 0;
        bcm2835_v3d_qpu_init(&qpu);
        qpu.fragment_shader = true;
        qpu.uniform_address = 0x1000;
        qpu.read_uniform = qpu_test_read_captured_uniform;
        qpu.uniform_opaque = &shader;
        qpu.read_texture = qpu_test_read_captured_texture;
        qpu.texture_opaque = &shader;
        qpu.read_varying = qpu_test_read_captured_varying;
        qpu.varying_opaque = &shader;
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            float s = (lane & 1) ? 0.75f : 0.25f;
            float t = (lane & 2) ? 0.75f : 0.25f;

            qpu.fragment_w[lane] = qpu_test_float_bits(1.0f);
            shader.varying[0][lane] = qpu_test_float_bits(s);
            shader.varying[1][lane] = qpu_test_float_bits(t);
            qpu.io[45][lane] = 0x20406080;
        }
        result = bcm2835_v3d_qpu_run(code, code_size, 64, &qpu, &executed);
        g_assert_cmpint(result, ==, BCM2835_V3D_QPU_END);
        g_assert_cmpuint(executed, ==, code_size / sizeof(uint64_t));
        g_assert_cmpuint(shader.uniform_reads, ==,
                         expected_uniform_reads[program]);
        g_assert_cmpuint(shader.texture_reads, ==,
                         BCM2835_V3D_QPU_LANES);
        g_assert_cmphex(qpu.io[46][0], ==,
                        expected_colors[program]);
    }
}

typedef struct QPUTestMemory {
    uint32_t base;
    uint8_t data[256];
} QPUTestMemory;

static bool qpu_test_read_vpm_dma(void *opaque, uint32_t address,
                                  uint32_t *value)
{
    QPUTestMemory *memory = opaque;

    if (address < memory->base || address - memory->base >
        sizeof(memory->data) - sizeof(*value)) {
        return false;
    }
    *value = ldl_le_p(memory->data + address - memory->base);
    return true;
}

static bool qpu_test_write_vpm_dma(void *opaque, uint32_t address,
                                   uint32_t value)
{
    QPUTestMemory *memory = opaque;

    if (address < memory->base || address - memory->base >
        sizeof(memory->data) - sizeof(value)) {
        return false;
    }
    stl_le_p(memory->data + address - memory->base, value);
    return true;
}

static uint64_t qpu_test_add_imm(unsigned raddr_a, unsigned waddr_add,
                                unsigned imm)
{
    return (13ULL << 60) | (1ULL << 49) | (uint64_t)waddr_add << 38 |
           (39ULL << 32) | (12ULL << 24) | (uint64_t)raddr_a << 18 |
           (uint64_t)imm << 12 | (6ULL << 9) | (7ULL << 6) | 7ULL;
}

static uint64_t qpu_test_add(unsigned raddr_a, unsigned raddr_b,
                            unsigned waddr_add, bool ws)
{
    return (1ULL << 60) | (1ULL << 49) | (uint64_t)waddr_add << 38 |
           (uint64_t)ws << 44 | (39ULL << 32) | (12ULL << 24) |
           (uint64_t)raddr_a << 18 |
           (uint64_t)raddr_b << 12 | (6ULL << 9) | (7ULL << 6) |
           (7ULL << 3) | 7ULL;
}

static uint64_t qpu_test_ftoi(unsigned raddr_a, unsigned waddr_add)
{
    return (1ULL << 60) | (1ULL << 49) | (uint64_t)waddr_add << 38 |
           (39ULL << 32) | (7ULL << 24) | (uint64_t)raddr_a << 18 |
           (39ULL << 12) | (6ULL << 9) | (7ULL << 6) |
           (7ULL << 3) | 7ULL;
}

static uint64_t qpu_test_itof_rfa_unpack(unsigned raddr_a,
                                         unsigned waddr_add)
{
    return (1ULL << 60) | (1ULL << 57) | (1ULL << 49) |
           (uint64_t)waddr_add << 38 | (39ULL << 32) | (8ULL << 24) |
           (uint64_t)raddr_a << 18 | (39ULL << 12) | (6ULL << 9) |
           (6ULL << 6);
}

static uint64_t qpu_test_rfa_unpack(unsigned unpack, unsigned op_add,
                                    unsigned waddr_add)
{
    return (13ULL << 60) | (uint64_t)unpack << 57 | (1ULL << 49) |
           (uint64_t)waddr_add << 38 | (39ULL << 32) |
           (uint64_t)op_add << 24 | (6ULL << 9) | (7ULL << 6) |
           (7ULL << 3) | 7ULL;
}

static uint64_t qpu_test_write_tmu(unsigned address, bool regfile_b)
{
    return (1ULL << 60) | (1ULL << 49) |
           (uint64_t)address << 38 | (39ULL << 32) | (12ULL << 24) |
           (uint64_t)(regfile_b ? 39 : 0) << 18 |
           (uint64_t)(regfile_b ? 0 : 39) << 12 |
           (uint64_t)(regfile_b ? 5 : 6) << 9 |
           (uint64_t)(regfile_b ? 7 : 5) << 6 | (7ULL << 3) | 7ULL;
}

static uint64_t qpu_test_fmul(unsigned raddr_a, unsigned raddr_b,
                              unsigned waddr_mul)
{
    return (1ULL << 60) | (1ULL << 49) | (1ULL << 46) |
           (39ULL << 38) | (uint64_t)waddr_mul << 32 | (1ULL << 29) |
           (uint64_t)raddr_a << 18 | (uint64_t)raddr_b << 12 |
           (6ULL << 9) | (7ULL << 6) | (6ULL << 3) | 7ULL;
}

static uint64_t qpu_test_v8_alu(unsigned op_add, unsigned op_mul,
                                unsigned destination)
{
    return (1ULL << 60) | (1ULL << 49) | (1ULL << 46) |
           (uint64_t)destination << 38 | (uint64_t)destination << 32 |
           (uint64_t)op_mul << 29 | (uint64_t)op_add << 24 |
           (6ULL << 9) | (7ULL << 6) | (6ULL << 3) | 7ULL;
}

static uint64_t qpu_test_read_r4(unsigned destination)
{
    return (1ULL << 60) | (1ULL << 49) | (uint64_t)destination << 38 |
           (39ULL << 32) | (12ULL << 24) | (1ULL << 18) | (39ULL << 12) |
           (4ULL << 9) | (6ULL << 6) | (7ULL << 3) | 7ULL;
}

static uint64_t qpu_test_pack_color(unsigned pack, unsigned destination)
{
    return (13ULL << 60) | (1ULL << 56) | (uint64_t)pack << 52 |
           (1ULL << 46) | (39ULL << 38) |
           (uint64_t)destination << 32 | (1ULL << 29) |
           (7ULL << 6) | 7ULL | (32ULL << 12);
}

static uint64_t qpu_test_unpack_color(unsigned unpack, unsigned destination)
{
    return (13ULL << 60) | (uint64_t)unpack << 57 | (1ULL << 56) |
           (1ULL << 49) | (uint64_t)destination << 38 |
           (39ULL << 32) | (1ULL << 24) | (4ULL << 9) |
           (7ULL << 6) | (7ULL << 3) | 7ULL;
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

static void test_v3d_qpu_tmu0_rgba_texture_sampling(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    static const unsigned texture_types[] = { 16, 0, 0, 0, 1, 0 };
    static const unsigned dimensions[] = { 2, 2, 24, 32, 2, 2 };
    static const unsigned min_filters[] = { 1, 1, 1, 1, 1, 2 };
    QPUTestTexture texture = {
        .uniform_address = 0x1000,
        .memory_base = 0x4000,
        .texels = {
            0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00,
        },
    };
    uint32_t executed;

    stq_le_p(code, qpu_test_write_tmu(57, false));
    stq_le_p(code + sizeof(uint64_t), qpu_test_write_tmu(56, true));
    stq_le_p(code + 2 * sizeof(uint64_t), 10ULL << 60); /* Load TMU0. */
    stq_le_p(code + 3 * sizeof(uint64_t), 3ULL << 60); /* Program End. */
    for (unsigned test = 0; test < G_N_ELEMENTS(texture_types); test++) {
        unsigned type = texture_types[test];
        unsigned size = dimensions[test];
        unsigned memory_format = type == 16 ? 0 : size <= 16 ? 2 : 1;

        texture.config[0] = texture.memory_base | ((type & 0xf) << 4);
        texture.config[1] = (type == 16 ? 1U << 31 : 0) |
                            (size << 20) | (size << 8) |
                            (1U << 7) | (min_filters[test] << 4) |
                            (1U << 2) | 1U;
        texture.uniform_reads = 0;
        texture.memory_reads = 0;
        bcm2835_v3d_qpu_init(&qpu);
        qpu.uniform_address = texture.uniform_address;
        qpu.read_uniform = qpu_test_read_texture_uniform;
        qpu.uniform_opaque = &texture;
        qpu.read_memory = qpu_test_read_texture_memory;
        qpu.memory_opaque = &texture;
        qpu.read_texture = qpu_test_read_texture;
        qpu.texture_opaque = &texture;
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            qpu.rf[0][0][lane] = qpu_test_float_bits(
                lane & 1 ? 0.75f : 0.25f);
            qpu.rf[1][0][lane] = qpu_test_float_bits(
                lane & 2 ? 0.75f : 0.25f);
        }

        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_END);
        g_assert_cmpuint(texture.uniform_reads, ==, 2);
        g_assert_cmpuint(texture.memory_reads, ==,
                         BCM2835_V3D_QPU_LANES);
        g_assert_cmpuint(texture.memory_format, ==, memory_format);
        g_assert_cmphex(qpu.uniform_address, ==, 0x1008);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            unsigned x = lane & 2 ? size - 1 : 0;
            unsigned y = lane & 1 ? size - 1 : 0;
            uint32_t expected = texture.texels[
                (y >= size / 2 ? 2 : 0) + (x >= size / 2 ? 1 : 0)];

            if (type == 1) {
                expected |= UINT32_C(0xff000000);
            }
            g_assert_cmphex(qpu.acc[4][lane], ==, expected);
        }
    }

    texture.config[0] = texture.memory_base;
    texture.config[1] = (2U << 20) | (2U << 8) | (1U << 2) | 1U;
    texture.uniform_reads = 0;
    texture.memory_reads = 0;
    bcm2835_v3d_qpu_init(&qpu);
    qpu.uniform_address = texture.uniform_address;
    qpu.read_uniform = qpu_test_read_texture_uniform;
    qpu.uniform_opaque = &texture;
    qpu.read_memory = qpu_test_read_texture_memory;
    qpu.memory_opaque = &texture;
    qpu.read_texture = qpu_test_read_texture;
    qpu.texture_opaque = &texture;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = qpu_test_float_bits(0.5f);
        qpu.rf[1][0][lane] = qpu_test_float_bits(0.5f);
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(texture.memory_reads, ==,
                     4 * BCM2835_V3D_QPU_LANES);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.acc[4][lane], ==, 0x77889966);
    }
}

static void test_v3d_qpu_tmu1_rgba_texture_sampling(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    QPUTestTexture texture = {
        .uniform_address = 0x1000,
        .memory_base = 0x4000,
        .texels = {
            0x11223344, 0x55667788, 0x99aabbcc, 0xddeeff00,
        },
        .config = {
            0x4000,
            (2U << 20) | (2U << 8) | (1U << 7) | (1U << 4) |
            (1U << 2) | 1U,
        },
    };
    uint32_t executed;

    stq_le_p(code, qpu_test_write_tmu(61, false));
    stq_le_p(code + sizeof(uint64_t), qpu_test_write_tmu(60, true));
    stq_le_p(code + 2 * sizeof(uint64_t), 11ULL << 60); /* Load TMU1. */
    stq_le_p(code + 3 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.uniform_address = texture.uniform_address;
    qpu.read_uniform = qpu_test_read_texture_uniform;
    qpu.uniform_opaque = &texture;
    qpu.read_texture = qpu_test_read_texture;
    qpu.texture_opaque = &texture;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = qpu_test_float_bits(
            lane & 1 ? 0.75f : 0.25f);
        qpu.rf[1][0][lane] = qpu_test_float_bits(
            lane & 2 ? 0.75f : 0.25f);
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(texture.uniform_reads, ==, 2);
    g_assert_cmpuint(texture.memory_reads, ==,
                     BCM2835_V3D_QPU_LANES);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        unsigned x = lane & 2 ? 1 : 0;
        unsigned y = lane & 1 ? 1 : 0;

        g_assert_cmphex(qpu.acc[4][lane], ==,
                        texture.texels[y * 2 + x]);
    }
}

static void test_v3d_qpu_fragment_w_register(void)
{
    uint8_t code[2 * sizeof(uint64_t)];

    qpu_test_store_code(code, qpu_test_add_imm(15, 0, 0));
    for (unsigned fragment_shader = 0; fragment_shader < 2;
         fragment_shader++) {
        BCM2835V3DQPUState qpu;
        uint32_t executed;

        bcm2835_v3d_qpu_init(&qpu);
        qpu.fragment_shader = fragment_shader;
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            qpu.rf[0][15][lane] = 0x100 + lane;
            qpu.fragment_w[lane] = 0x200 + lane;
        }
        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_END);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            g_assert_cmphex(qpu.rf[0][0][lane], ==,
                            (fragment_shader ? 0x200 : 0x100) + lane);
        }
    }
}

static void test_v3d_qpu_tile_color_signals(void)
{
    uint8_t code[3 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint64_t instruction;

    bcm2835_v3d_qpu_init(&qpu);
    qpu.fragment_shader = true;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.io[45][lane] = 0x12340000 | lane;
    }
    instruction = qpu_test_add(0, 0, 39, false);
    instruction = (instruction & ~(0xfULL << 60)) | (8ULL << 60);
    stq_le_p(code, instruction);
    stq_le_p(code + sizeof(uint64_t), qpu_test_read_r4(8));
    stq_le_p(code + 2 * sizeof(uint64_t), 3ULL << 60);
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.acc[4][lane], ==, 0x12340000 | lane);
        g_assert_cmphex(qpu.rf[0][8][lane], ==, 0x12340000 | lane);
    }

    bcm2835_v3d_qpu_init(&qpu);
    qpu.fragment_shader = true;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.io[45][lane] = 0x56780000 | lane;
    }
    instruction = qpu_test_add(0, 0, 39, false);
    instruction = (instruction & ~(0xfULL << 60)) | (9ULL << 60);
    stq_le_p(code, instruction);
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(uint64_t), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(executed, ==, 1);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.acc[4][lane], ==, 0x56780000 | lane);
    }

    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][2][lane] = 0x9abc0000 | lane;
    }
    stq_le_p(code, qpu_test_add(2, 3, 45, false));
    stq_le_p(code + sizeof(uint64_t), 3ULL << 60);
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, 2 * sizeof(uint64_t), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmphex(qpu.tlb_color_write_mask, ==, UINT16_MAX);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.io[45][lane], ==, 0x9abc0000 | lane);
        g_assert_cmphex(qpu.io[46][lane], ==, 0x9abc0000 | lane);
    }
}

static void test_v3d_qpu_tile_buffer_signals(void)
{
    uint8_t code[2 * sizeof(uint64_t)];
    static const unsigned signals[] = { 4, 5 };

    for (unsigned i = 0; i < G_N_ELEMENTS(signals); i++) {
        BCM2835V3DQPUState qpu;
        uint32_t executed;
        uint64_t instruction = qpu_test_add(0, 0, 39, false);

        instruction = (instruction & ~(0xfULL << 60)) |
                      ((uint64_t)signals[i] << 60);
        stq_le_p(code, instruction);
        stq_le_p(code + sizeof(uint64_t), 3ULL << 60);
        bcm2835_v3d_qpu_init(&qpu);
        qpu.fragment_shader = true;
        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_END);

        bcm2835_v3d_qpu_init(&qpu);
        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_UNSUPPORTED);
    }
}

static void test_v3d_qpu_varying_read(void)
{
    uint8_t code[3 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    QPUTestVarying varying;
    uint32_t executed;
    uint64_t varying_add = (1ULL << 60) | (1ULL << 49) |
                           (39ULL << 32) | (12ULL << 24) |
                           (35ULL << 18) | (6ULL << 9) | (5ULL << 6) |
                           (7ULL << 3) | 7ULL;
    uint64_t read_r5 = (1ULL << 60) | (1ULL << 49) |
                       (1ULL << 38) | (39ULL << 32) | (12ULL << 24) |
                       (5ULL << 9) | (7ULL << 3) | 7ULL;

    stq_le_p(code, varying_add);
    stq_le_p(code + 8, read_r5);
    stq_le_p(code + 16, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    memset(&varying, 0, sizeof(varying));
    qpu.read_varying = qpu_test_read_varying;
    qpu.varying_opaque = &varying;
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.acc[5][lane] = 0x100 + lane;
        varying.partial[lane] = 10 + lane;
        varying.c[lane] = 0x1000 + lane;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmpuint(varying.reads, ==, 1);
    g_assert_cmpuint(qpu.varying_index, ==, 1);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][0][lane], ==,
                        varying.partial[lane] + 0x100 + lane);
        g_assert_cmphex(qpu.rf[0][1][lane], ==, varying.c[lane]);
        g_assert_cmphex(qpu.acc[5][lane], ==, varying.c[lane]);
    }
}

static void test_v3d_qpu_packed_byte_alu(void)
{
    uint8_t code[5 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    const uint32_t a = 0x7f00faff;
    const uint32_t b = 0x810a14ff;

    stq_le_p(code, qpu_test_v8_alu(30, 4, 2));
    stq_le_p(code + 8, qpu_test_v8_alu(31, 5, 3));
    stq_le_p(code + 16, qpu_test_v8_alu(0, 6, 4));
    stq_le_p(code + 24, qpu_test_v8_alu(0, 7, 5));
    stq_le_p(code + 32, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = a;
        qpu.rf[1][0][lane] = b;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][2][lane], ==, 0xff0affff);
        g_assert_cmphex(qpu.rf[1][2][lane], ==, 0x7f0014ff);
        g_assert_cmphex(qpu.rf[0][3][lane], ==, 0x0000e600);
        g_assert_cmphex(qpu.rf[1][3][lane], ==, 0x810afaff);
        g_assert_cmphex(qpu.rf[1][4][lane], ==, 0xff0affff);
        g_assert_cmphex(qpu.rf[1][5][lane], ==, 0x0000e600);
    }
}

static void test_v3d_qpu_v8muld(void)
{
    uint8_t code[2 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;

    qpu_test_store_code(code, qpu_test_v8_alu(0, 3, 2));
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = 0xff804020;
        qpu.rf[1][0][lane] = 0x7f808080;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[1][2][lane], ==, 0x7e3f1f0f);
    }
}

static void test_v3d_qpu_ftoi_edge_values(void)
{
    uint8_t code[2 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    static const uint32_t input[] = {
        0xbfe00000, 0x3f400000, 0x7fc00000, 0x7f800000,
        0xff800000, 0x4f000000, 0xcf000000, 0x00000000,
    };
    static const uint32_t expected[] = {
        UINT32_MAX, 0, 0, INT32_MAX, INT32_MIN, INT32_MAX, INT32_MIN, 0,
    };

    qpu_test_store_code(code, qpu_test_ftoi(0, 33));
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = input[lane % G_N_ELEMENTS(input)];
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.acc[1][lane], ==,
                        expected[lane % G_N_ELEMENTS(expected)]);
    }
}

static void test_v3d_qpu_rfa_unpack_itof(void)
{
    uint8_t code[2 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    static const uint32_t input[] = {
        0xfeedffff, 0x12340001, 0xabe88000, 0x80007fff,
    };
    static const int16_t expected[] = { -1, 1, INT16_MIN, INT16_MAX };

    qpu_test_store_code(code, qpu_test_itof_rfa_unpack(0, 1));
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = input[lane % G_N_ELEMENTS(input)];
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][1][lane], ==,
                        qpu_test_float_bits((float)expected[
                            lane % G_N_ELEMENTS(expected)]));
    }
}

static void test_v3d_qpu_rfa_unpack_modes(void)
{
    static const uint32_t half_input[] = {
        0x3c00bc00, 0x00013c00, 0x00000000, 0x7bfffc00,
    };
    static const uint32_t byte_input[] = {
        0x12345678, 0x99aabbcc, 0x000000ff, 0x80000000,
    };
    static const uint32_t half_float[2][4] = {
        { 0xbf800000, 0x3f800000, 0x00000000, 0xff800000 },
        { 0x3f800000, 0x33800000, 0x00000000, 0x477fe000 },
    };
    static const uint32_t half_integer[2][4] = {
        { 0xffffbc00, 0x00003c00, 0x00000000, 0xfffffc00 },
        { 0x00003c00, 0x00000001, 0x00000000, 0x00007bff },
    };

    for (unsigned unpack = 1; unpack <= 7; unpack++) {
        for (unsigned floating = 0; floating < 2; floating++) {
            uint8_t code[2 * sizeof(uint64_t)];
            BCM2835V3DQPUState qpu;
            uint32_t executed;

            if (unpack == 3 && floating) {
                continue;
            }
            qpu_test_store_code(code,
                qpu_test_rfa_unpack(unpack, floating ? 1 : 12, 4));
            bcm2835_v3d_qpu_init(&qpu);
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                unsigned index = lane % G_N_ELEMENTS(half_input);

                qpu.rf[0][0][lane] = unpack <= 2 ? half_input[index] :
                                                        byte_input[index];
            }
            g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8,
                                                 &qpu, &executed),
                            ==, BCM2835_V3D_QPU_END);
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                unsigned index = lane % G_N_ELEMENTS(half_input);
                uint32_t expected;

                if (unpack <= 2) {
                    expected = floating ? half_float[unpack - 1][index] :
                                          half_integer[unpack - 1][index];
                } else {
                    uint32_t byte = unpack == 3 ? byte_input[index] >> 24 :
                        (byte_input[index] >> ((unpack - 4) * 8)) & 0xff;

                    if (unpack == 3) {
                        expected = (byte & 0xff) * UINT32_C(0x01010101);
                    } else if (floating) {
                        expected = qpu_test_float_bits((byte & 0xff) / 255.0f);
                    } else {
                        expected = byte & 0xff;
                    }
                }
                g_assert_cmphex(qpu.rf[0][4][lane], ==, expected);
            }
        }
    }
}

static void test_v3d_qpu_color_pack_unpack(void)
{
    uint8_t pack_code[6 * sizeof(uint64_t)];
    uint8_t unpack_code[5 * sizeof(uint64_t)];
    uint8_t half_unpack_code[3 * sizeof(uint64_t)];
    uint8_t pack_a_code[3 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    static const uint32_t input[4] = {
        0x3f000000, 0x3f800000, 0xbf800000, 0x40000000,
    };
    static const uint8_t output[4] = { 128, 255, 0, 255 };
    static const uint32_t half_input[] = {
        0xc0003c00, 0x3c000001, 0x00000000, 0xfc007c00,
    };
    static const uint32_t half_low[] = {
        0x3f800000, 0x33800000, 0x00000000, 0x7f800000,
    };
    static const uint32_t half_high[] = {
        0xc0000000, 0x3f800000, 0x00000000, 0xff800000,
    };

    for (unsigned pack = 3; pack < 8; pack++) {
        stq_le_p(pack_code + (pack - 3) * sizeof(uint64_t),
                 qpu_test_pack_color(pack, pack - 3));
    }
    stq_le_p(pack_code + 5 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.acc[0][lane] = input[lane % G_N_ELEMENTS(input)];
        for (unsigned pack = 3; pack < 8; pack++) {
            qpu.rf[1][pack - 3][lane] = 0x11223344;
        }
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(pack_code, sizeof(pack_code), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        unsigned component = lane % G_N_ELEMENTS(input);

        for (unsigned pack = 3; pack < 8; pack++) {
            uint32_t expected = pack == 3 ?
                (uint32_t)output[component] * UINT32_C(0x01010101) :
                (0x11223344 & ~(UINT32_C(0xff) << ((pack - 4) * 8))) |
                (uint32_t)output[component] << ((pack - 4) * 8);

            g_assert_cmphex(qpu.rf[1][pack - 3][lane], ==, expected);
        }
    }

    for (unsigned unpack = 4; unpack < 8; unpack++) {
        stq_le_p(unpack_code + (unpack - 4) * sizeof(uint64_t),
                 qpu_test_unpack_color(unpack, unpack - 4));
    }
    stq_le_p(unpack_code + 4 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.acc[4][lane] = 0x11223344;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(unpack_code, sizeof(unpack_code), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned unpack = 4; unpack < 8; unpack++) {
        uint32_t byte = (0x11223344 >> ((unpack - 4) * 8)) & 0xff;
        uint32_t expected;
        float value = byte / 255.0f;

        memcpy(&expected, &value, sizeof(expected));
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            g_assert_cmphex(qpu.rf[0][unpack - 4][lane], ==, expected);
        }
    }

    stq_le_p(half_unpack_code, qpu_test_unpack_color(1, 4));
    stq_le_p(half_unpack_code + sizeof(uint64_t),
             qpu_test_unpack_color(2, 5));
    stq_le_p(half_unpack_code + 2 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.acc[4][lane] = half_input[lane % G_N_ELEMENTS(half_input)];
    }
    g_assert_cmpint(bcm2835_v3d_qpu_run(half_unpack_code,
                                         sizeof(half_unpack_code), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        unsigned input_index = lane % G_N_ELEMENTS(half_input);

        g_assert_cmphex(qpu.rf[0][4][lane], ==, half_low[input_index]);
        g_assert_cmphex(qpu.rf[0][5][lane], ==, half_high[input_index]);
    }

    stq_le_p(pack_a_code, qpu_test_add(0, 0, 2, false) | (1ULL << 52));
    stq_le_p(pack_a_code + sizeof(uint64_t),
             qpu_test_add(0, 0, 2, false) | (2ULL << 52));
    stq_le_p(pack_a_code + 2 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = 0x1000;
        qpu.rf[1][0][lane] = 0x234;
        qpu.rf[0][2][lane] = 0xabcd0000;
    }
    g_assert_cmpint(bcm2835_v3d_qpu_run(pack_a_code, sizeof(pack_a_code), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][2][lane], ==, 0x12341234);
    }
}

static void test_v3d_qpu_rfa_pack_modes(void)
{
    uint8_t code[15 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    static const unsigned byte_packs[] = {
        3, 4, 5, 6, 7, 11, 12, 13, 14, 15,
    };
    static const uint32_t float_input[] = {
        0x3fc00000, 0xc0000000, 0x38800000, 0x33800000,
        0x33000000, 0x33c00000, 0x7f800000, 0xff800000,
    };
    static const uint16_t float_half[] = {
        0x3e00, 0xc000, 0x0400, 0x0001, 0x0000, 0x0002, 0x7c00, 0xfc00,
    };
    size_t instruction = 0;

    for (unsigned i = 0; i < G_N_ELEMENTS(byte_packs); i++) {
        unsigned pack = byte_packs[i];
        unsigned source = pack == 12 ? 20 : 0;

        stq_le_p(code + instruction++ * sizeof(uint64_t),
                 qpu_test_add(source, 0, 2 + i, false) |
                 (uint64_t)pack << 52);
    }
    stq_le_p(code + instruction++ * sizeof(uint64_t),
             qpu_test_add(18, 0, 12, false) | (9ULL << 52));
    stq_le_p(code + instruction++ * sizeof(uint64_t),
             qpu_test_add(19, 0, 13, false) | (10ULL << 52));
    for (unsigned pack = 1; pack <= 2; pack++) {
        uint64_t float_add = qpu_test_add(17, 0, 14 + pack, false);

        float_add &= ~(UINT64_C(0x1f) << 24);
        float_add |= (UINT64_C(1) << 24) | (uint64_t)pack << 52;
        stq_le_p(code + instruction++ * sizeof(uint64_t), float_add);
    }
    stq_le_p(code + instruction++ * sizeof(uint64_t), 3ULL << 60);

    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = 0x123;
        qpu.rf[0][18][lane] = 70000;
        qpu.rf[0][19][lane] = (uint32_t)-70000;
        qpu.rf[0][20][lane] = UINT32_MAX;
        qpu.rf[0][17][lane] = float_input[lane % G_N_ELEMENTS(float_input)];
        qpu.rf[0][12][lane] = 0xa1b2c3d4;
        qpu.rf[0][13][lane] = 0xa1b2c3d4;
        qpu.rf[0][15][lane] = 0xa1b2c3d4;
        qpu.rf[0][16][lane] = 0xa1b2c3d4;
        for (unsigned i = 0; i < G_N_ELEMENTS(byte_packs); i++) {
            qpu.rf[0][2 + i][lane] = 0xa1b2c3d4;
        }
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), instruction,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        for (unsigned i = 0; i < G_N_ELEMENTS(byte_packs); i++) {
            unsigned pack = byte_packs[i];
            uint32_t byte = pack < 8 ? 0x23 : pack == 12 ? 0 : 0xff;
            uint32_t expected;

            if (pack == 3 || pack == 11) {
                expected = byte * UINT32_C(0x01010101);
            } else {
                unsigned shift = (pack < 8 ? pack - 4 : pack - 12) * 8;

                expected = (0xa1b2c3d4 & ~(UINT32_C(0xff) << shift)) |
                           (byte << shift);
            }
            g_assert_cmphex(qpu.rf[0][2 + i][lane], ==, expected);
        }
        g_assert_cmphex(qpu.rf[0][12][lane], ==, 0xa1b27fff);
        g_assert_cmphex(qpu.rf[0][13][lane], ==, 0x8000c3d4);
        g_assert_cmphex(qpu.rf[0][15][lane], ==,
                        0xa1b20000 | float_half[lane %
                                                G_N_ELEMENTS(float_half)]);
        g_assert_cmphex(qpu.rf[0][16][lane], ==,
                        ((uint32_t)float_half[lane %
                                              G_N_ELEMENTS(float_half)] << 16) |
                        0xc3d4);
    }
}

static void test_v3d_qpu_rfa_pack_32_sat(void)
{
    static const uint32_t operand_a[] = {
        0x7fffffff, 0x80000000, 0x7fffffff, 0x80000000, 100, (uint32_t)-100,
    };
    static const uint32_t operand_b[] = {
        1, UINT32_MAX, UINT32_MAX, 1, (uint32_t)-23, 23,
    };
    static const uint32_t sub_operand_a[] = {
        0x80000000, 0x7fffffff, 0x7fffffff, 0x80000000, 100, (uint32_t)-100,
    };
    static const uint32_t sub_operand_b[] = {
        1, UINT32_MAX, 1, UINT32_MAX, (uint32_t)-23, 23,
    };
    static const uint32_t add_expected[] = {
        0x7fffffff, 0x80000000, 0x7ffffffe, 0x80000001, 77, (uint32_t)-77,
    };
    static const uint32_t sub_expected[] = {
        0x80000000, 0x7fffffff, 0x7ffffffe, 0x80000001, 123, (uint32_t)-123,
    };
    static const uint32_t shift_a[] = {
        0x40000000, 0x80000000, 0x10000000, 0xf0000000,
    };
    static const uint32_t shift_expected[] = {
        0x7fffffff, 0x80000000, 0x20000000, 0xe0000000,
    };
    static const uint32_t mul_a[] = {
        0x007fffff, 0x00800000, 123, 0x00ffff85,
    };
    static const uint32_t mul_b[] = {
        0x007fffff, 0x007fffff, 2, 2,
    };
    static const uint32_t mul_expected[] = {
        0x7fffffff, 0x80000000, 246, (uint32_t)-246,
    };
    uint8_t code[5 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint64_t sub = qpu_test_add(1, 1, 3, false);
    uint64_t shift = qpu_test_add(4, 4, 4, false);
    uint64_t mul = qpu_test_add(5, 5, 39, true);
    uint64_t float_add = qpu_test_add(0, 0, 2, false);

    sub = (sub & ~(UINT64_C(0x1f) << 24)) | (13ULL << 24) | (8ULL << 52);
    shift = (shift & ~(UINT64_C(0x1f) << 24)) | (17ULL << 24) |
            (8ULL << 52);
    mul = (mul & ~((UINT64_C(0x1f) << 24) | (UINT64_C(7) << 29) |
                   (UINT64_C(0x3f) << 32))) |
          (UINT64_C(2) << 29) | (UINT64_C(5) << 32) | (8ULL << 52) |
          (UINT64_C(1) << 46);
    mul = (mul & ~(UINT64_C(7) << 3)) | (UINT64_C(6) << 3);
    float_add = (float_add & ~(UINT64_C(0x1f) << 24)) |
                (1ULL << 24) | (8ULL << 52);
    stq_le_p(code, qpu_test_add(0, 0, 2, false) | (8ULL << 52));
    stq_le_p(code + sizeof(uint64_t), sub);
    stq_le_p(code + 2 * sizeof(uint64_t), shift);
    stq_le_p(code + 3 * sizeof(uint64_t), mul);
    stq_le_p(code + 4 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        unsigned test = lane % G_N_ELEMENTS(operand_a);

        qpu.rf[0][0][lane] = operand_a[test];
        qpu.rf[1][0][lane] = operand_b[test];
        qpu.rf[0][1][lane] = sub_operand_a[test];
        qpu.rf[1][1][lane] = sub_operand_b[test];
        test = lane % G_N_ELEMENTS(shift_a);
        qpu.rf[0][4][lane] = shift_a[test];
        qpu.rf[1][4][lane] = 1;
        qpu.rf[0][5][lane] = mul_a[test];
        qpu.rf[1][5][lane] = mul_b[test];
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 6, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        unsigned test = lane % G_N_ELEMENTS(operand_a);
        unsigned shift_test = lane % G_N_ELEMENTS(shift_a);

        g_assert_cmphex(qpu.rf[0][2][lane], ==, add_expected[test]);
        g_assert_cmphex(qpu.rf[0][3][lane], ==, sub_expected[test]);
        g_assert_cmphex(qpu.rf[0][4][lane], ==, shift_expected[shift_test]);
        g_assert_cmphex(qpu.rf[0][5][lane], ==, mul_expected[shift_test]);
    }

    bcm2835_v3d_qpu_init(&qpu);
    stq_le_p(code, float_add);
    stq_le_p(code + sizeof(uint64_t), 3ULL << 60);
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, 2 * sizeof(uint64_t), 2,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_UNSUPPORTED);
    g_assert_cmpuint(executed, ==, 1);
}

static void test_v3d_qpu_cross_mux_register_reads(void)
{
    uint8_t code[2 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint64_t instruction = (1ULL << 60) | (1ULL << 49) |
                           (4ULL << 38) | (39ULL << 32) |
                           (12ULL << 24) | (2ULL << 18) | (3ULL << 12) |
                           (7ULL << 9) | (6ULL << 6);

    stq_le_p(code, instruction);
    stq_le_p(code + sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][2][lane] = 3;
        qpu.rf[1][3][lane] = 5;
    }
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][4][lane], ==, 8);
    }
}

static void test_v3d_qpu_thread_switch_signals(void)
{
    uint8_t code[2 * sizeof(uint64_t)];
    static const unsigned signals[] = { 2, 6 };

    for (unsigned i = 0; i < G_N_ELEMENTS(signals); i++) {
        BCM2835V3DQPUState qpu;
        uint32_t executed;
        uint64_t instruction = qpu_test_add(2, 3, 4, false);

        instruction = (instruction & ~(0xfULL << 60)) |
                      ((uint64_t)signals[i] << 60);
        stq_le_p(code, instruction);
        stq_le_p(code + sizeof(uint64_t), 3ULL << 60);
        bcm2835_v3d_qpu_init(&qpu);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            qpu.rf[0][2][lane] = 3;
            qpu.rf[1][3][lane] = 5;
        }

        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_END);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            g_assert_cmphex(qpu.rf[0][4][lane], ==, 8);
        }
    }
}

static void test_v3d_qpu_vector_rotation(void)
{
    static const unsigned rotations[] = { 1, 15, 3 };
    static const unsigned encodings[] = { 49, 63, 48 };
    uint8_t code[4 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;

    for (unsigned i = 0; i < G_N_ELEMENTS(encodings); i++) {
        uint64_t instruction = (13ULL << 60) | (1ULL << 46) |
                               (39ULL << 38) |
                               (uint64_t)(34 + i) << 32 | (1ULL << 29) |
                               (uint64_t)encodings[i] << 12 | 1ULL;

        stq_le_p(code + i * sizeof(uint64_t), instruction);
    }
    stq_le_p(code + 3 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.acc[0][lane] = qpu_test_float_bits((float)lane);
        qpu.acc[1][lane] = qpu_test_float_bits(1.0f);
    }
    qpu.acc[5][0] = rotations[2];

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned rotation = 0; rotation < G_N_ELEMENTS(rotations);
         rotation++) {
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            unsigned source = (lane + BCM2835_V3D_QPU_LANES -
                               rotations[rotation]) %
                              BCM2835_V3D_QPU_LANES;

            g_assert_cmphex(qpu.acc[2 + rotation][lane], ==,
                            qpu_test_float_bits((float)source));
        }
    }

    stq_le_p(code, (ldq_le_p(code) & ~7ULL) | 7ULL);
    stq_le_p(code + sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, 2 * sizeof(uint64_t), 8,
                                         &qpu, &executed),
                    ==, BCM2835_V3D_QPU_UNSUPPORTED);
}

static void test_v3d_qpu_sfu(void)
{
    uint8_t code[5 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    static const unsigned sfu[] = { 52, 53, 54, 55 };
    static const float input[4][4] = {
        { 0.5f, 1.0f, 2.0f, 4.0f },
        { 1.0f, 1.0f, 4.0f, 4.0f },
        { -2.0f, -1.0f, 0.0f, 1.0f },
        { 0.5f, 1.0f, 2.0f, 4.0f },
    };
    static const float expected[4][4] = {
        { 2.0f, 1.0f, 0.5f, 0.25f },
        { 1.0f, 1.0f, 0.5f, 0.5f },
        { 0.25f, 0.5f, 1.0f, 2.0f },
        { -1.0f, 0.0f, 1.0f, 2.0f },
    };

    for (unsigned op = 0; op < G_N_ELEMENTS(sfu); op++) {
        bcm2835_v3d_qpu_init(&qpu);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            float value = input[op][lane % 4];

            memcpy(&qpu.rf[0][0][lane], &value, sizeof(value));
            qpu.rf[0][1][lane] = 0;
            qpu.rf[1][0][lane] = 0;
        }
        stq_le_p(code, qpu_test_add(0, 0, sfu[op], false));
        stq_le_p(code + sizeof(uint64_t), qpu_test_v8_alu(0, 0, 39));
        stq_le_p(code + 2 * sizeof(uint64_t), qpu_test_v8_alu(0, 0, 39));
        stq_le_p(code + 3 * sizeof(uint64_t), qpu_test_read_r4(8));
        stq_le_p(code + 4 * sizeof(uint64_t), 3ULL << 60);

        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 2, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_BUDGET);
        g_assert_true(qpu.sfu_pending[0]);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            g_assert_cmphex(qpu.acc[4][lane], ==, 0);
        }

        g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                             &executed),
                        ==, BCM2835_V3D_QPU_END);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            uint32_t bits;
            float result = expected[op][lane % 4];

            memcpy(&bits, &result, sizeof(bits));
            g_assert_cmphex(qpu.rf[0][8][lane], ==, bits);
        }
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
    qpu.vpm_read_size = 2;
    qpu.vpm_read_horizontal = true;
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
    qpu.vpm_write_size = 2;
    qpu.vpm_write_horizontal = true;
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

static void test_v3d_qpu_vpm_vertical_32bit(void)
{
    uint8_t code[3 * sizeof(uint64_t)];
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t address = (2U << 4) | 3U;
    uint32_t setup_read = (1U << 20) | (1U << 12) | (2U << 8) | address;
    uint32_t setup_write = (1U << 12) | (2U << 8) | address;

    stq_le_p(code, qpu_test_load_imm(49, setup_read));
    stq_le_p(code + 8, qpu_test_add_imm(48, 0, 0));
    stq_le_p(code + 16, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.vpm[32 + lane][3] = 0x100 + lane;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.rf[0][0][lane], ==, 0x100 + lane);
    }

    stq_le_p(code, qpu_test_load_imm(49, setup_write) | (1ULL << 44));
    stq_le_p(code + 8,
             qpu_test_add_imm(0, 48, 0) | (1ULL << 44));
    stq_le_p(code + 16, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        qpu.rf[0][0][lane] = lane * 17;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                         &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
        g_assert_cmphex(qpu.vpm[32 + lane][3], ==, lane * 17);
    }
}

static void test_v3d_qpu_vpm_subword_io(void)
{
    uint8_t code[3 * sizeof(uint64_t)];

    for (unsigned size = 0; size < 2; size++) {
        unsigned sub_bits = 2 - size;
        unsigned sub_count = 1U << sub_bits;
        unsigned vector_words = BCM2835_V3D_QPU_LANES / sub_count;
        unsigned value_bits = 8U << size;
        uint32_t value_mask = (1U << value_bits) - 1;

        for (unsigned horizontal = 0; horizontal < 2; horizontal++) {
            for (unsigned laned = 0; laned < 2; laned++) {
                BCM2835V3DQPUState qpu;
                unsigned sub_index = sub_count - 1;
                unsigned base = horizontal ? 23 : (2U << 4) | 5U;
                uint32_t address = (base << sub_bits) | sub_index;
                uint32_t setup = (1U << 20) | (1U << 12) |
                                 (horizontal << 11) | (laned << 10) |
                                 (size << 8) | address;
                uint32_t expected_vpm[BCM2835_V3D_QPU_VPM_ROWS]
                                     [BCM2835_V3D_QPU_LANES];
                uint32_t executed;

                stq_le_p(code, qpu_test_load_imm(49, setup));
                stq_le_p(code + 8, qpu_test_add_imm(48, 0, 0));
                stq_le_p(code + 16, 3ULL << 60);
                bcm2835_v3d_qpu_init(&qpu);
                for (unsigned row = 0; row < BCM2835_V3D_QPU_VPM_ROWS;
                     row++) {
                    for (unsigned column = 0;
                         column < BCM2835_V3D_QPU_LANES; column++) {
                        qpu.vpm[row][column] =
                            0x10203040 + (row * 16 + column) * 0x01010101;
                    }
                }

                g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8,
                                                     &qpu, &executed),
                                ==, BCM2835_V3D_QPU_END);
                g_assert_cmpuint(qpu.vpm_read_address, ==,
                                 (address + 1) & (0xffU >> size));
                for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES;
                     lane++) {
                    unsigned word = laned ? lane :
                                    sub_index * vector_words + lane / sub_count;
                    unsigned row = horizontal ? base :
                                   ((base >> 4) * 16 + word);
                    unsigned column = horizontal ? word : base & 0xf;
                    unsigned shift = (laned ? sub_index : lane % sub_count) *
                                     value_bits;
                    uint32_t expected = (qpu.vpm[row][column] >> shift) &
                                        value_mask;

                    g_assert_cmphex(qpu.rf[0][0][lane], ==, expected);
                }

                stq_le_p(code, qpu_test_load_imm(49, setup) | (1ULL << 44));
                stq_le_p(code + 8, qpu_test_add_imm(0, 48, 0) |
                         (1ULL << 44));
                stq_le_p(code + 16, 3ULL << 60);
                bcm2835_v3d_qpu_init(&qpu);
                for (unsigned row = 0; row < BCM2835_V3D_QPU_VPM_ROWS;
                     row++) {
                    for (unsigned column = 0;
                         column < BCM2835_V3D_QPU_LANES; column++) {
                        qpu.vpm[row][column] = 0xa1b2c3d4 ^ row ^ column;
                        expected_vpm[row][column] = qpu.vpm[row][column];
                    }
                }
                for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES;
                     lane++) {
                    unsigned word = laned ? lane :
                                    sub_index * vector_words + lane / sub_count;
                    unsigned row = horizontal ? base :
                                   ((base >> 4) * 16 + word);
                    unsigned column = horizontal ? word : base & 0xf;
                    unsigned shift = (laned ? sub_index : lane % sub_count) *
                                     value_bits;
                    uint32_t field_mask = value_mask << shift;
                    uint32_t value = value_mask - lane;

                    qpu.rf[0][0][lane] = value;
                    expected_vpm[row][column] =
                        (expected_vpm[row][column] & ~field_mask) |
                        (value << shift);
                }

                g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8,
                                                     &qpu, &executed),
                                ==, BCM2835_V3D_QPU_END);
                g_assert_cmpuint(qpu.vpm_write_address, ==,
                                 (address + 1) & (0xffU >> size));
                for (unsigned row = 0; row < BCM2835_V3D_QPU_VPM_ROWS;
                     row++) {
                    for (unsigned column = 0;
                         column < BCM2835_V3D_QPU_LANES; column++) {
                        g_assert_cmphex(qpu.vpm[row][column], ==,
                                        expected_vpm[row][column]);
                    }
                }
            }
        }
    }
}

static void test_v3d_qpu_vpm_dma_single_row(void)
{
    uint8_t code[5 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t load_setup = (1U << 31) | (4U << 20) | (1U << 16) |
                          (1U << 12) | (5U << 4) | 3U;
    uint32_t store_setup = (2U << 30) | (1U << 23) | (4U << 16) |
                           (1U << 14) | (((5U << 4) | 3U) << 3);

    for (unsigned i = 0; i < 4; i++) {
        stl_le_p(memory.data + i * sizeof(uint32_t), 0x10203040 + i);
    }
    stq_le_p(code, qpu_test_load_imm(49, load_setup));
    stq_le_p(code + sizeof(uint64_t), qpu_test_load_imm(50, memory.base));
    stq_le_p(code + 2 * sizeof(uint64_t),
             qpu_test_load_imm(49, store_setup) | (1ULL << 44));
    stq_le_p(code + 3 * sizeof(uint64_t),
             qpu_test_load_imm(50, memory.base + 0x40) | (1ULL << 44));
    stq_le_p(code + 4 * sizeof(uint64_t), 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qpu.vpm[5][3 + i], ==, 0x10203040 + i);
        g_assert_cmphex(ldl_le_p(memory.data + 0x40 + i * sizeof(uint32_t)),
                        ==, 0x10203040 + i);
    }
}

static void test_v3d_qpu_vpm_dma_2d(void)
{
    uint8_t code[6 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t load_setup = (1U << 31) | (1U << 24) | (3U << 20) |
                          (2U << 16) | (1U << 12) | (5U << 4) | 3U;
    uint32_t store_setup = (2U << 30) | (2U << 23) | (3U << 16) |
                           (1U << 14) | (((5U << 4) | 3U) << 3);
    uint32_t store_stride = (3U << 30) | 4U;

    for (unsigned i = 0; i < 3; i++) {
        stl_le_p(memory.data + i * sizeof(uint32_t), 0x10203040 + i);
        stl_le_p(memory.data + 16 + i * sizeof(uint32_t), 0x50607080 + i);
    }
    stq_le_p(code, qpu_test_load_imm(49, load_setup));
    stq_le_p(code + 8, qpu_test_load_imm(50, memory.base));
    stq_le_p(code + 16, qpu_test_load_imm(49, store_setup) | (1ULL << 44));
    stq_le_p(code + 24, qpu_test_load_imm(49, store_stride) | (1ULL << 44));
    stq_le_p(code + 32,
             qpu_test_load_imm(50, memory.base + 0x80) | (1ULL << 44));
    stq_le_p(code + 40, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qpu.vpm[5][3 + i], ==, 0x10203040 + i);
        g_assert_cmphex(qpu.vpm[6][3 + i], ==, 0x50607080 + i);
        g_assert_cmphex(ldl_le_p(memory.data + 0x80 + i * 4), ==,
                        0x10203040 + i);
        g_assert_cmphex(ldl_le_p(memory.data + 0x90 + i * 4), ==,
                        0x50607080 + i);
    }

    uint8_t extended_code[4 * sizeof(uint64_t)];
    uint32_t extended_pitch_setup = (9U << 28) | 16U;
    uint32_t extended_load_setup = (1U << 31) | (3U << 20) | (2U << 16) |
                                   (1U << 12) | (5U << 4) | 3U;

    stq_le_p(extended_code, qpu_test_load_imm(49, extended_pitch_setup));
    stq_le_p(extended_code + 8,
             qpu_test_load_imm(49, extended_load_setup));
    stq_le_p(extended_code + 16, qpu_test_load_imm(50, memory.base));
    stq_le_p(extended_code + 24, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(extended_code, sizeof(extended_code),
                                        8, &qpu, &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(qpu.vpm[5][3 + i], ==, 0x10203040 + i);
        g_assert_cmphex(qpu.vpm[6][3 + i], ==, 0x50607080 + i);
    }
}

static void test_v3d_qpu_vpm_dma_store_blockmode(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t store_stride = (3U << 30) | (1U << 16) | 4U;
    uint32_t store_setup = (2U << 30) | (2U << 23) | (3U << 16) |
                           (1U << 14) | (((5U << 4) | 14U) << 3);

    stq_le_p(code, qpu_test_load_imm(49, store_stride) | (1ULL << 44));
    stq_le_p(code + 8, qpu_test_load_imm(49, store_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x40) | (1ULL << 44));
    stq_le_p(code + 24, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned i = 0; i < 6; i++) {
        unsigned index = 5 * BCM2835_V3D_QPU_LANES + 14 + i;

        qpu.vpm[index / BCM2835_V3D_QPU_LANES]
               [index % BCM2835_V3D_QPU_LANES] = 0x10203040 + i;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(ldl_le_p(memory.data + 0x40 + i * 4), ==,
                        0x10203040 + i);
        g_assert_cmphex(ldl_le_p(memory.data + 0x50 + i * 4), ==,
                        0x10203040 + 3 + i);
    }
}

static void test_v3d_qpu_vpm_dma_vertical_32bit(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t load_setup = (1U << 31) | (3U << 24) | (2U << 16) |
                          (1U << 11) | 15U;

    for (unsigned i = 0; i < 32; i++) {
        stl_le_p(memory.data + i * sizeof(uint32_t), 0x10203040 + i);
    }
    stq_le_p(code, qpu_test_load_imm(49, load_setup));
    stq_le_p(code + 8, qpu_test_load_imm(50, memory.base));
    stq_le_p(code + 16, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(qpu.vpm[i][15], ==, 0x10203040 + i);
        g_assert_cmphex(qpu.vpm[16 + i][15], ==, 0x10203040 + 16 + i);
    }
}

static void test_v3d_qpu_vpm_dma_vertical_store_32bit(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t store_stride = (3U << 30) | 4U;
    uint32_t store_setup = (2U << 30) | (2U << 23) | (3U << 16) |
                           (5U << 7) | (3U << 3);

    stq_le_p(code, qpu_test_load_imm(49, store_stride) | (1ULL << 44));
    stq_le_p(code + 8, qpu_test_load_imm(49, store_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x40) | (1ULL << 44));
    stq_le_p(code + 24, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 3; x++) {
            qpu.vpm[5 + x][3 + y] = 0x10203040 + y * 3 + x;
        }
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned x = 0; x < 3; x++) {
            g_assert_cmphex(ldl_le_p(memory.data + 0x40 + y * 0x10 + x * 4),
                            ==, 0x10203040 + y * 3 + x);
        }
    }
}

static void test_v3d_qpu_vpm_dma_vertical_store_blockmode(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t store_stride = (3U << 30) | (1U << 16) | 4U;
    uint32_t store_setup = (2U << 30) | (2U << 23) | (3U << 16) |
                           (62U << 7) | (3U << 3);

    stq_le_p(code, qpu_test_load_imm(49, store_stride) | (1ULL << 44));
    stq_le_p(code + 8, qpu_test_load_imm(49, store_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x40) | (1ULL << 44));
    stq_le_p(code + 24, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned i = 0; i < 6; i++) {
        unsigned index = 3 * BCM2835_V3D_QPU_VPM_ROWS + 62 + i;

        qpu.vpm[index % BCM2835_V3D_QPU_VPM_ROWS]
               [index / BCM2835_V3D_QPU_VPM_ROWS] = 0x10203040 + i;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 3; i++) {
        g_assert_cmphex(ldl_le_p(memory.data + 0x40 + i * 4), ==,
                        0x10203040 + i);
        g_assert_cmphex(ldl_le_p(memory.data + 0x50 + i * 4), ==,
                        0x10203040 + 3 + i);
    }
}

static void test_v3d_qpu_vpm_dma_packed_load(void)
{
    uint8_t code[3 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t load_setup = (1U << 31) | (6U << 28) | (1U << 24) |
                          (2U << 16) | (1U << 12) | (5U << 4);

    for (unsigned i = 0; i < 32; i++) {
        memory.data[i] = i < 16 ? i + 1 : i + 0x21;
    }
    stq_le_p(code, qpu_test_load_imm(49, load_setup));
    stq_le_p(code + 8, qpu_test_load_imm(50, memory.base));
    stq_le_p(code + 16, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 4; i++) {
        uint32_t first = memory.data[i * 4];
        uint32_t second = memory.data[16 + i * 4];
        uint32_t expected_first = first | memory.data[i * 4 + 1] << 8 |
                                  memory.data[i * 4 + 2] << 16 |
                                  memory.data[i * 4 + 3] << 24;
        uint32_t expected_second = second |
                                   memory.data[17 + i * 4] << 8 |
                                   memory.data[18 + i * 4] << 16 |
                                   memory.data[19 + i * 4] << 24;

        g_assert_cmphex(qpu.vpm[5][8 + i], ==, expected_first);
        g_assert_cmphex(qpu.vpm[6][8 + i], ==, expected_second);
    }

    uint32_t horizontal16_setup = (1U << 31) | (3U << 28) | (2U << 24) |
                                  (1U << 16) | (1U << 12) | (7U << 4);

    for (unsigned i = 0; i < 16; i++) {
        unsigned value = 0x100 + i;

        memory.data[i * 2] = value;
        memory.data[i * 2 + 1] = value >> 8;
    }
    stq_le_p(code, qpu_test_load_imm(49, horizontal16_setup));
    stq_le_p(code + 8, qpu_test_load_imm(50, memory.base));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qpu.vpm[7][8 + i], ==,
                        (0x100 + i * 2) | ((0x101 + i * 2) << 16));
    }

    uint32_t vertical_setup = (1U << 31) | (3U << 28) | (2U << 24) |
                              (2U << 16) | (1U << 11) | (16U << 4) | 3U;

    for (unsigned i = 0; i < 16; i++) {
        unsigned first = 0x100 + i;
        unsigned second = 0x200 + i;

        memory.data[i * 2] = first;
        memory.data[i * 2 + 1] = first >> 8;
        memory.data[32 + i * 2] = second;
        memory.data[33 + i * 2] = second >> 8;
    }
    stq_le_p(code, qpu_test_load_imm(49, vertical_setup));
    stq_le_p(code + 8, qpu_test_load_imm(50, memory.base));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 8; i++) {
        g_assert_cmphex(qpu.vpm[24 + i][3], ==,
                        (0x100 + i * 2) | ((0x101 + i * 2) << 16));
        g_assert_cmphex(qpu.vpm[40 + i][3], ==,
                        (0x200 + i * 2) | ((0x201 + i * 2) << 16));
    }

    uint32_t vertical8_setup = (1U << 31) | (7U << 28) | (1U << 24) |
                               (1U << 16) | (1U << 11) | (32U << 4) | 4U;

    for (unsigned i = 0; i < 16; i++) {
        memory.data[i] = i + 0x31;
    }
    stq_le_p(code, qpu_test_load_imm(49, vertical8_setup));
    stq_le_p(code + 8, qpu_test_load_imm(50, memory.base));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qpu.vpm[44 + i][4], ==,
                        0x34333231 + i * 0x04040404);
    }
}

static void test_v3d_qpu_vpm_dma_packed_store(void)
{
    uint8_t code[4 * sizeof(uint64_t)];
    QPUTestMemory memory = { .base = 0x1000 };
    BCM2835V3DQPUState qpu;
    uint32_t executed;
    uint32_t store_stride = 3U << 30;
    uint32_t store_setup = (2U << 30) | (1U << 23) | (7U << 16) |
                           (1U << 14) | (5U << 7) | 6U;

    memset(memory.data, 0xcc, sizeof(memory.data));
    stq_le_p(code, qpu_test_load_imm(49, store_stride) | (1ULL << 44));
    stq_le_p(code + 8, qpu_test_load_imm(49, store_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x80) | (1ULL << 44));
    stq_le_p(code + 24, 3ULL << 60);
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned i = 0; i < 4; i++) {
        qpu.vpm[5][8 + i] = 0x04030201 + i * 0x04040404;
    }

    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmphex(memory.data[0x7f], ==, 0xcc);
    g_assert_cmphex(memory.data[0x87], ==, 0xcc);
    for (unsigned i = 0; i < 7; i++) {
        g_assert_cmphex(memory.data[0x80 + i], ==, i + 1);
    }

    uint32_t horizontal16_setup = (2U << 30) | (1U << 23) | (16U << 16) |
                                  (1U << 14) | (7U << 7) | 3U;

    memset(memory.data, 0xcc, sizeof(memory.data));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned i = 0; i < 8; i++) {
        qpu.vpm[7][8 + i] = (0x0102 + i * 2) | ((0x0304 + i * 2) << 16);
    }
    stq_le_p(code + 8,
             qpu_test_load_imm(49, horizontal16_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x80) | (1ULL << 44));
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = i & 1 ? 0x0304 + i - 1 : 0x0102 + i;

        g_assert_cmphex(memory.data[0x80 + i * 2], ==, value & 0xff);
        g_assert_cmphex(memory.data[0x81 + i * 2], ==, value >> 8);
    }

    uint32_t vertical_stride = 3U << 30;
    uint32_t vertical_setup = (2U << 30) | (2U << 23) | (16U << 16) |
                              (16U << 7) | (3U << 3) | 3U;

    memset(memory.data, 0xcc, sizeof(memory.data));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned i = 0; i < 8; i++) {
            unsigned first = 1 + y * 0x20 + i * 4;

            qpu.vpm[24 + i][3 + y] = first | (first + 1) << 8 |
                                     (first + 2) << 16 |
                                     (first + 3) << 24;
        }
    }
    stq_le_p(code, qpu_test_load_imm(49, vertical_stride) | (1ULL << 44));
    stq_le_p(code + 8,
             qpu_test_load_imm(49, vertical_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x80) | (1ULL << 44));
    stq_le_p(code + 24, 3ULL << 60);
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    g_assert_cmphex(memory.data[0x7f], ==, 0xcc);
    g_assert_cmphex(memory.data[0xc0], ==, 0xcc);
    for (unsigned y = 0; y < 2; y++) {
        for (unsigned i = 0; i < 16; i++) {
            g_assert_cmphex(memory.data[0x80 + y * 32 + i], ==,
                            1 + y * 0x20 + i);
        }
    }

    uint32_t vertical8_setup = (2U << 30) | (1U << 23) | (16U << 16) |
                               (32U << 7) | (4U << 3) | 7U;

    memset(memory.data, 0xcc, sizeof(memory.data));
    bcm2835_v3d_qpu_init(&qpu);
    qpu.write_memory = qpu_test_write_vpm_dma;
    qpu.read_memory = qpu_test_read_vpm_dma;
    qpu.memory_opaque = &memory;
    for (unsigned i = 0; i < 4; i++) {
        qpu.vpm[44 + i][4] = 0x34333231 + i * 0x04040404;
    }
    stq_le_p(code + 8,
             qpu_test_load_imm(49, vertical8_setup) | (1ULL << 44));
    stq_le_p(code + 16,
             qpu_test_load_imm(50, memory.base + 0x80) | (1ULL << 44));
    g_assert_cmpint(bcm2835_v3d_qpu_run(code, sizeof(code), 8, &qpu,
                                        &executed),
                    ==, BCM2835_V3D_QPU_END);
    for (unsigned i = 0; i < 16; i++) {
        g_assert_cmphex(memory.data[0x80 + i], ==, 0x31 + i);
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
                     7);
    g_assert_cmpuint(qpu.vpm_write_total, ==,
                     7);
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

static void test_v3d_ct0_primitive_list_format(void)
{
    uint8_t cl[48];
    size_t cl_size = 0;

    cl[cl_size++] = 112;
    append_u32(cl, &cl_size, 0x30000);
    append_u32(cl, &cl_size, 0x2000);
    append_u32(cl, &cl_size, 0x32000);
    cl[cl_size++] = 2;
    cl[cl_size++] = 2;
    cl[cl_size++] = 0x04;
    cl[cl_size++] = 6;
    cl[cl_size++] = 56; /* PRIMITIVE_LIST_FORMAT, legal in CT0 bin CL. */
    cl[cl_size++] = 0x12; /* 16-bit index triangles, as emitted by Mesa. */
    cl[cl_size++] = 4;

    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_INTCTL, V3D_FLDONE);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + cl_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) &
                    (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CA), ==,
                    CL_ADDR + cl_size);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_FLDONE,
                    ==, V3D_FLDONE);
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

static void test_v3d_ct0_clips_and_rasterizes_varying_triangles(void)
{
    enum { test_fb_width = 64, test_fb_height = 65 };
    static const uint32_t vertices[9][8] = {
        { 0xbf000000, 0xbf000000, 0x00000000, 0x3f800000,
          0x0100ff00, 0x3f800000, 0x40000000, 0x3f800000 },
        { 0x40000000, 0xbf000000, 0x00000000, 0x3f800000,
          0x01000400, 0x3f800000, 0x40000000, 0x3f800000 },
        { 0x00000000, 0x3f800000, 0x00000000, 0x40000000,
          0xff000000, 0x3fc00000, 0x40400000, 0x3f000000 },
        { 0x40000000, 0xbf000000, 0x00000000, 0x3f800000,
          0x0100ff00, 0x3f800000, 0x3f800000, 0x3f800000 },
        { 0x40000000, 0x00000000, 0x00000000, 0x3f800000,
          0x01000400, 0x3f800000, 0x3f800000, 0x3f800000 },
        { 0x40000000, 0x3f000000, 0x00000000, 0x3f800000,
          0xff000000, 0x3f800000, 0x3f800000, 0x3f800000 },
        { 0xbf000000, 0xbf000000, 0x00000000, 0x3f800000,
          0x0100ff00, 0x3f800000, 0x40000000, 0x3f800000 },
        { 0x3f000000, 0xbf000000, 0x00000000, 0x3f800000,
          0x01000400, 0x3f800000, 0x40000000, 0x3f800000 },
        { 0x00000000, 0x3f000000, 0x00000000, 0x40000000,
          0xff000000, 0x3fc00000, 0x40400000, 0x3f000000 },
    };
    uint8_t render_cl[32];
    uint8_t bin_cl[128];
    uint8_t draw_cl[32];
    uint8_t tile_head[64];
    uint8_t tile_list[64];
    uint8_t tile_overflow[64];
    uint8_t record[44] = { 0 };
    uint8_t record_no_varying[44];
    uint8_t vertex_code[19 * sizeof(uint64_t)] = { 0 };
    uint8_t code[19 * sizeof(uint64_t)] = { 0 };
    uint8_t fragment_code[4 * sizeof(uint64_t)];
    uint8_t vertex_data[sizeof(vertices)];
    uint8_t initial[test_fb_width * test_fb_height * sizeof(uint32_t)];
    uint8_t fb[sizeof(initial)];
    size_t render_size = 0;
    size_t bin_size = 0;
    size_t draw_size = 0;
    uint32_t draw_color = 0xff00ff00;
    uint32_t bin_count;
    uint32_t setup_write = (1U << 12) | (1U << 11) | (2U << 8);
    uint32_t vertex_setup_read = (1U << 12) | (1U << 11) | (2U << 8);
    uint32_t coordinate_setup_read = vertex_setup_read | 7;
    uint64_t instructions[] = {
        qpu_test_load_imm(49, setup_write) | (1ULL << 44),
        qpu_test_load_imm(49, coordinate_setup_read | (8U << 20)),
        qpu_test_add_imm(48, 0, 0),
        qpu_test_add_imm(48, 1, 0),
        qpu_test_add_imm(48, 2, 0),
        qpu_test_add_imm(48, 3, 0),
        qpu_test_add_imm(48, 4, 0),
        qpu_test_add_imm(48, 5, 0),
        qpu_test_add_imm(48, 6, 0),
        qpu_test_add_imm(48, 7, 0),
        /* Mesa writes viewport XY, ZS, then 1/WC after clip coordinates. */
        qpu_test_add_imm(0, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(1, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(2, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(3, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(4, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(2, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(7, 48, 0) | (1ULL << 44),
        3ULL << 60,
    };
    uint64_t vertex_instructions[] = {
        qpu_test_load_imm(49, setup_write) | (1ULL << 44),
        qpu_test_load_imm(49, vertex_setup_read | (8U << 20)),
        qpu_test_add_imm(48, 0, 0),
        qpu_test_add_imm(48, 1, 0),
        qpu_test_add_imm(48, 2, 0),
        qpu_test_add_imm(48, 3, 0),
        qpu_test_add_imm(48, 4, 0),
        qpu_test_add_imm(48, 5, 0),
        qpu_test_add_imm(48, 6, 0),
        qpu_test_add_imm(48, 7, 0),
        qpu_test_add_imm(4, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(2, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(7, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(5, 48, 0) | (1ULL << 44),
        qpu_test_add_imm(6, 48, 0) | (1ULL << 44),
        3ULL << 60,
    };
    uint64_t fragment_instructions[] = {
        qpu_test_add(35, 39, 0, true),
        qpu_test_add(35, 39, 1, true),
        qpu_test_fmul(15, 1, 46),
        3ULL << 60,
    };

    memset(initial, 0x5a, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    for (unsigned i = 0; i < G_N_ELEMENTS(vertices); i++) {
        for (unsigned word = 0; word < G_N_ELEMENTS(vertices[i]); word++) {
            stl_le_p(vertex_data + (i * G_N_ELEMENTS(vertices[i]) + word) *
                     sizeof(uint32_t), vertices[i][word]);
        }
    }
    stl_le_p(record + 28, 0x6200);
    stl_le_p(record + 4, 0x6400);
    stl_le_p(record + 8, 0x6600);
    record[3] = 2;
    record[14] = 1;
    stl_le_p(record + 16, 0x6000);
    record[26] = 1;
    record[0] = 1U << 2; /* Enable homogeneous clipping. */
    stl_le_p(record + 36, 0x7000);
    record[40] = sizeof(vertices[0]) - 1;
    record[41] = sizeof(vertices[0]);
    record[43] = 7 * sizeof(uint32_t);
    memwrite(0x34000, record, sizeof(record));
    memwrite(0x7000, vertex_data, sizeof(vertex_data));
    for (unsigned i = 0; i < G_N_ELEMENTS(instructions); i++) {
        stq_le_p(code + i * sizeof(uint64_t), instructions[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(vertex_instructions); i++) {
        stq_le_p(vertex_code + i * sizeof(uint64_t), vertex_instructions[i]);
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(fragment_instructions); i++) {
        stq_le_p(fragment_code + i * sizeof(uint64_t),
                 fragment_instructions[i]);
    }
    memwrite(0x6200, code, sizeof(code));
    memwrite(0x6000, vertex_code, sizeof(vertex_code));
    memwrite(0x6400, fragment_code, sizeof(fragment_code));

    bin_cl[bin_size++] = 112;
    append_u32(bin_cl, &bin_size, 0x30000);
    append_u32(bin_cl, &bin_size, 0x140);
    append_u32(bin_cl, &bin_size, 0x32000);
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 0x2c;
    bin_cl[bin_size++] = 6;
    bin_cl[bin_size++] = 64;
    append_u32(bin_cl, &bin_size, 0x34001);
    bin_cl[bin_size++] = 103;
    /* VIEWPORT_OFFSET fields are signed 12.4 pixel coordinates. */
    append_u16(bin_cl, &bin_size, 40 * 16);
    append_u16(bin_cl, &bin_size, 32 * 16);
    bin_cl[bin_size++] = 105;
    append_u32(bin_cl, &bin_size, 0x44000000);
    /* Negative Y scale mirrors Mesa's Y-flipped target; screen rows stay fixed. */
    append_u32(bin_cl, &bin_size, 0xc4000000);
    bin_cl[bin_size++] = 33;
    bin_cl[bin_size++] = 4;
    append_u32(bin_cl, &bin_size, G_N_ELEMENTS(vertices));
    append_u32(bin_cl, &bin_size, 0);
    bin_cl[bin_size++] = 4;
    memwrite(CL_ADDR, bin_cl, bin_size);
    writel(RASPI3_V3D_BASE + V3D_CT0CS, 1U << 15);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + bin_size);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) &
                    (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR),
                    ==, V3D_CTRUN | V3D_CTSUBS);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_PCS) & V3D_BMOOM,
                    ==, V3D_BMOOM);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_INT_OUTOMEM,
                    ==, V3D_INT_OUTOMEM);
    writel(RASPI3_V3D_BASE + V3D_BPOA, 0x35000);
    writel(RASPI3_V3D_BASE + V3D_BPOS, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_PCS) & V3D_BMOOM, ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) &
                    (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR),
                    ==, V3D_CTRUN | V3D_CTSUBS);
    writel(RASPI3_V3D_BASE + V3D_BPOS, 0x80);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) & V3D_CTERR, ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_PCS) & V3D_BMOOM, ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BPOA), ==, 0x35000);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BPOS), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BPCA), ==, 0x30140);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BPCS), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_INTCTL) & V3D_INT_SPILLUSE,
                    ==, V3D_INT_SPILLUSE);

    /* CT0 bins before CT1 supplies the render target configuration. */
    render_cl[render_size++] = 113;
    append_u32(render_cl, &render_size, FB_ADDR);
    append_u16(render_cl, &render_size, test_fb_width);
    append_u16(render_cl, &render_size, test_fb_height);
    append_u16(render_cl, &render_size, 1U << 2);
    render_cl[render_size++] = 114;
    append_u32(render_cl, &render_size, draw_color);
    append_u32(render_cl, &render_size, 0);
    append_u32(render_cl, &render_size, 0);
    render_cl[render_size++] = 103;
    append_u16(render_cl, &render_size, 40 * 16);
    append_u16(render_cl, &render_size, 32 * 16);
    render_cl[render_size++] = 0;
    memwrite(CL_ADDR, render_cl, render_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + render_size);

    memread(FB_ADDR, fb, sizeof(fb));
    g_assert_cmphex(read_u32(fb + (30 * test_fb_width + 32) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);
    g_assert_cmphex(read_u32(fb + (1 * test_fb_width + 1) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);
    memread(0x30000, tile_head, sizeof(tile_head));
    memread(0x30100, tile_list, sizeof(tile_list));
    memread(0x35000, tile_overflow, sizeof(tile_overflow));
    g_assert_cmphex(tile_head[0], ==, 56);
    g_assert_cmphex(tile_head[1], ==, 0x32);
    g_assert_cmphex(tile_head[7], ==, 48);
    g_assert_cmphex(tile_head[8], ==, 129);
    g_assert_cmphex(read_u32(tile_head + 9), ==, 0x0100ff00);
    g_assert_cmphex(read_u32(tile_head + 13), ==, 0x01000200);
    g_assert_cmphex(read_u32(tile_head + 17), ==, 0x00000200);
    g_assert_cmphex(read_u32(tile_head + 33), ==, 0x40000000);
    g_assert_cmphex(read_u32(tile_head + 21), ==, 0x3f800000);
    g_assert_cmphex(tile_head[57], ==, 128);
    g_assert_cmphex(tile_head[58], ==, 16);
    g_assert_cmphex(read_u32(tile_head + 59), ==, 0x30100);
    g_assert_cmphex(tile_list[0], ==, 56);
    g_assert_cmphex(tile_list[1], ==, 0x32);
    g_assert_cmphex(tile_list[7], ==, 48);
    g_assert_cmphex(tile_list[8], ==, 129);
    g_assert_cmphex(read_u32(tile_list + 9), ==, 0x0100ff00);
    g_assert_cmphex(read_u32(tile_list + 13), ==, 0x00000200);
    g_assert_cmphex(read_u32(tile_list + 17), ==, 0xff000000);
    g_assert_cmphex(read_u32(tile_list + 21), ==, 0x3f800000);
    g_assert_cmphex(tile_list[57], ==, 128);
    g_assert_cmphex(tile_list[58], ==, 16);
    g_assert_cmphex(read_u32(tile_list + 59), ==, 0x35000);
    g_assert_cmphex(tile_overflow[0], ==, 56);
    g_assert_cmphex(tile_overflow[1], ==, 0x32);
    g_assert_cmphex(tile_overflow[57], ==, 128);
    g_assert_cmphex(tile_overflow[58], ==, 18);

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
    {
        uint32_t bits = read_u32(fb + (40 * test_fb_width + 50) *
                                 sizeof(uint32_t));
        float value;

        memcpy(&value, &bits, sizeof(value));
        g_assert_cmpfloat_with_epsilon(value, 2.132743f, 0.001f);
    }
    g_assert_cmphex(read_u32(fb + (1 * test_fb_width + 1) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);

    /* Also clip Mesa's four-vertex fan with the negative Y scale it emits. */
    bin_count = readl(RASPI3_V3D_BASE + V3D_BFC);
    memmove(vertex_data, vertex_data + 6 * sizeof(vertices[0]),
            3 * sizeof(vertices[0]));
    memcpy(vertex_data + 3 * sizeof(vertices[0]), vertex_data,
           sizeof(vertices[0]));
    memwrite(0x7000, vertex_data, sizeof(vertex_data));
    record[0] = 1U << 2;
    memwrite(0x34000, record, sizeof(record));
    bin_size = 0;
    bin_cl[bin_size++] = 112;
    append_u32(bin_cl, &bin_size, 0x36000);
    append_u32(bin_cl, &bin_size, 0x1000);
    append_u32(bin_cl, &bin_size, 0x38000);
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 0x2c;
    bin_cl[bin_size++] = 6;
    bin_cl[bin_size++] = 64;
    append_u32(bin_cl, &bin_size, 0x34001);
    bin_cl[bin_size++] = 103;
    append_u16(bin_cl, &bin_size, 40 * 16);
    append_u16(bin_cl, &bin_size, 32 * 16);
    bin_cl[bin_size++] = 104;
    append_u32(bin_cl, &bin_size, 0xbf800000);
    append_u32(bin_cl, &bin_size, 0x3f800000);
    bin_cl[bin_size++] = 105;
    append_u32(bin_cl, &bin_size, 0x44000000);
    append_u32(bin_cl, &bin_size, 0xc4000000);
    bin_cl[bin_size++] = 33;
    bin_cl[bin_size++] = 6;
    append_u32(bin_cl, &bin_size, 4);
    append_u32(bin_cl, &bin_size, 0);
    bin_cl[bin_size++] = 4;
    memwrite(CL_ADDR, bin_cl, bin_size);
    writel(RASPI3_V3D_BASE + V3D_CT0CS, 1U << 15);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + bin_size);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) &
                    (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CA), ==,
                    CL_ADDR + bin_size);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_BFC), ==, bin_count + 1);

    /* Leave room for a 5-byte BRANCH when a tile list block fills. */
    memcpy(record_no_varying, record, sizeof(record_no_varying));
    record_no_varying[3] = 0;
    memwrite(0x34000, record, sizeof(record));
    memwrite(0x34100, record_no_varying, sizeof(record_no_varying));
    bin_size = 0;
    bin_cl[bin_size++] = 112;
    append_u32(bin_cl, &bin_size, 0x3a000);
    append_u32(bin_cl, &bin_size, 0x2000);
    append_u32(bin_cl, &bin_size, 0x3c000);
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 1;
    bin_cl[bin_size++] = 0x54; /* 128-byte initial and spill blocks. */
    bin_cl[bin_size++] = 6;
    bin_cl[bin_size++] = 103;
    append_u16(bin_cl, &bin_size, 40 * 16);
    append_u16(bin_cl, &bin_size, 32 * 16);
    bin_cl[bin_size++] = 105;
    append_u32(bin_cl, &bin_size, 0x44000000);
    append_u32(bin_cl, &bin_size, 0xc4000000);
    for (unsigned primitive = 0; primitive < 5; primitive++) {
        bin_cl[bin_size++] = 64;
        append_u32(bin_cl, &bin_size,
                   primitive ? 0x34101 : 0x34001);
        bin_cl[bin_size++] = 33;
        bin_cl[bin_size++] = 4;
        append_u32(bin_cl, &bin_size, 3);
        append_u32(bin_cl, &bin_size, 0);
    }
    bin_cl[bin_size++] = 4;
    memwrite(CL_ADDR, bin_cl, bin_size);
    writel(RASPI3_V3D_BASE + V3D_CT0CS, 1U << 15);
    writel(RASPI3_V3D_BASE + V3D_CT0CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT0EA, CL_ADDR + bin_size);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CS) &
                    (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT0CA), ==,
                    CL_ADDR + bin_size);
    {
        uint8_t spill_head[128];
        uint8_t spill_block[64];

        memread(0x3a000, spill_head, sizeof(spill_head));
        memread(0x3a100, spill_block, sizeof(spill_block));
        g_assert_cmphex(spill_head[102], ==, 16);
        g_assert_cmphex(read_u32(spill_head + 103), ==, 0x3a100);
        g_assert_cmphex(spill_block[0], ==, 56);
        g_assert_cmphex(spill_block[1], ==, 0x32);
    }
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

static void test_v3d_ct1_resumes_after_compressed_tile(void)
{
    uint8_t cl[128];
    uint8_t record[44] = { 0 };
    uint8_t initial[128 * 64 * sizeof(uint32_t)];
    uint8_t fb[sizeof(initial)];
    uint32_t color = 0xff123456;
    size_t cl_size = 0;

    memset(initial, 0x5a, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(0x34000, record, sizeof(record));

    cl[cl_size++] = 113;
    append_u32(cl, &cl_size, FB_ADDR);
    append_u16(cl, &cl_size, 128);
    append_u16(cl, &cl_size, 64);
    append_u16(cl, &cl_size, V3D_FORMAT_RGBA8888 << 2);
    cl[cl_size++] = 114;
    append_u32(cl, &cl_size, color);
    append_u32(cl, &cl_size, 0);
    append_u32(cl, &cl_size, 0);
    cl[cl_size++] = 0;
    cl[cl_size++] = 64;
    append_u32(cl, &cl_size, 0x34001);

    for (unsigned tile_x = 0; tile_x < 2; tile_x++) {
        cl[cl_size++] = 115;
        cl[cl_size++] = tile_x;
        cl[cl_size++] = 0;
        cl[cl_size++] = 56;
        cl[cl_size++] = 0x32;
        cl[cl_size++] = 48;
        cl[cl_size++] = 129;
        append_u32(cl, &cl_size, 0);
        append_u32(cl, &cl_size, 0);
        append_u32(cl, &cl_size, 0);
        cl[cl_size++] = 128;
        append_tile_buffer_packet(cl, &cl_size, 28, FB_ADDR, 0, 0);
    }
    cl[cl_size++] = 0;

    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    for (unsigned retries = 0;
         retries < 100 && (readl(RASPI3_V3D_BASE + V3D_CT1CS) & V3D_CTRUN);
         retries++) {
        g_usleep(1000);
    }
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) &
                    (V3D_CTRUN | V3D_CTERR), ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CA), ==,
                    CL_ADDR + cl_size);

    memread(FB_ADDR, fb, sizeof(fb));
    g_assert_cmphex(read_u32(fb), ==, color);
    g_assert_cmphex(read_u32(fb + 64 * sizeof(uint32_t)), ==, color);
}

static void test_v3d_ct1_wgpu_batches_opaque_shader(void)
{
    enum {
        TILES_X = 20,
        TILES_Y = 13,
        WIDTH = TILES_X * 64,
        HEIGHT = TILES_Y * 64,
        RENDER_BASE = 0x01000000,
    };
    uint8_t *cl = g_new(uint8_t, 128 + TILES_X * TILES_Y * 80);
    uint8_t record[44] = { 0 };
    uint8_t code[(sizeof(captured_labwc_tmu0_opaque) - 1) / 2];
    uint32_t uniforms[] = {
        0x4000, (2U << 20) | (2U << 8) | (1U << 7) |
                (1U << 4) | (1U << 2) | 1U,
        0x3f800000, 0x3f800000, 0x00000000, 0x3f800000,
    };
    uint32_t texels[] = {
        0x10203040, 0x50607080, 0x90a0b0c0, 0xd0e0f000,
    };
    size_t tile_ends[TILES_X * TILES_Y + 1];
    size_t cl_size = 0;

    for (unsigned i = 0; i < sizeof(code); i++) {
        unsigned byte;

        g_assert_cmpint(sscanf(captured_labwc_tmu0_opaque + i * 2, "%2x",
                               &byte), ==, 1);
        code[i] = byte;
    }
    record[3] = 2;
    stl_le_p(record + 4, 0x5000);
    stl_le_p(record + 8, 0x6000);
    memwrite(0x34000, record, sizeof(record));
    memwrite(0x5000, code, sizeof(code));
    memwrite(0x6000, uniforms, sizeof(uniforms));
    memwrite(0x4000, texels, sizeof(texels));

    cl[cl_size++] = 113;
    append_u32(cl, &cl_size, RENDER_BASE);
    append_u16(cl, &cl_size, WIDTH);
    append_u16(cl, &cl_size, HEIGHT);
    append_u16(cl, &cl_size, V3D_FORMAT_RGBA8888 << 2);
    cl[cl_size++] = 114;
    append_u32(cl, &cl_size, 0xff000000);
    append_u32(cl, &cl_size, 0);
    append_u32(cl, &cl_size, 0);
    cl[cl_size++] = 0;
    cl[cl_size++] = 64;
    append_u32(cl, &cl_size, 0x34001);
    tile_ends[0] = cl_size;

    for (unsigned tile = 0; tile < TILES_X * TILES_Y; tile++) {
        cl[cl_size++] = 115;
        cl[cl_size++] = tile % TILES_X;
        cl[cl_size++] = tile / TILES_X;
        cl[cl_size++] = 103;
        append_u16(cl, &cl_size, (tile % TILES_X * 64 + 32) * 16);
        append_u16(cl, &cl_size, (tile / TILES_X * 64 + 32) * 16);
        cl[cl_size++] = 56;
        cl[cl_size++] = 0x32;
        cl[cl_size++] = 48;
        cl[cl_size++] = 129;
        append_u32(cl, &cl_size, 0xfe00fe00);
        append_u32(cl, &cl_size, 0xfe000200);
        append_u32(cl, &cl_size, 0x0200fe00);
        for (unsigned i = 0; i < 6; i++) {
            append_u32(cl, &cl_size, qpu_test_float_bits(0.25f));
        }
        for (unsigned i = 0; i < 3; i++) {
            append_u32(cl, &cl_size, qpu_test_float_bits(1.0f));
        }
        cl[cl_size++] = 128;
        append_tile_buffer_packet(cl, &cl_size, 28, RENDER_BASE, 0, 0);
        tile_ends[tile + 1] = cl_size;
    }
    cl[cl_size++] = 0;
    g_autofree uint8_t *cl_template = g_memdup2(cl, cl_size);
    static const unsigned tile_counts[] = { 1, 2, 3, 4, 16, 64, 260, 1 };
    static const unsigned samples[] = { 0, 64, 254, 255, 259 };

    for (unsigned pass = 0; pass < G_N_ELEMENTS(tile_counts); pass++) {
        unsigned tile_count = tile_counts[pass];
        size_t active_cl_size = tile_ends[tile_count] + 1;

        memcpy(cl, cl_template, tile_ends[tile_count]);
        cl[active_cl_size - 1] = 0;
        writel(RASPI3_V3D_BASE + V3D_CT1CS, 1U << 15);
        memwrite(CL_ADDR, cl, active_cl_size);
        writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
        writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + active_cl_size);
        for (unsigned retries = 0;
             retries < 5000 &&
             (readl(RASPI3_V3D_BASE + V3D_CT1CS) & V3D_CTRUN);
             retries++) {
            g_usleep(1000);
        }
        g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) &
                        (V3D_CTRUN | V3D_CTERR), ==, 0);
        g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CA), ==,
                        CL_ADDR + active_cl_size);

        for (unsigned i = 0; i < G_N_ELEMENTS(samples); i++) {
            unsigned tile = samples[i];
            size_t pixel;
            uint8_t color[sizeof(uint32_t)];

            if (tile >= tile_count) {
                continue;
            }
            pixel = (size_t)(tile / TILES_X * 64 + 16) * WIDTH +
                    tile % TILES_X * 64 + 16;
            memread(RENDER_BASE + pixel * sizeof(uint32_t), color,
                    sizeof(color));
            g_assert_cmphex(read_u32(color), ==, 0xff203040);
        }
        if (tile_count == TILES_X * TILES_Y) {
            size_t frame_size = (size_t)WIDTH * HEIGHT * sizeof(uint32_t);
            g_autofree uint8_t *frame = g_malloc(frame_size);
            g_autofree gchar *digest = NULL;

            memread(RENDER_BASE, frame, frame_size);
            digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, frame,
                                                 frame_size);
            g_assert_cmpstr(digest, ==,
                            "468cac829f9203d5cd0a16ae9aa1d103430de322f16aeb5d5f8df48a440a8b2f");
        }
    }
    g_free(cl);
}

static void test_v3d_ct1_compressed_xy_triangle_encodings(void)
{
    uint8_t cl[96];
    uint8_t record[44] = { 0 };
    uint8_t initial[FB_SIZE];
    uint8_t fb[FB_SIZE];
    uint32_t color = 0xff123456;
    size_t cl_size = 0;
    /* Y mirrored for VC4 screen Y = centre + Y; rows match the old layout. */
    uint64_t relative_triangle = 15 | (48ULL << 4) | (48ULL << 11) |
                                 (80ULL << 25) |
                                 (0xffe8ULL << 32) | (0x0018ULL << 48);

    memset(initial, 0x5a, sizeof(initial));
    memwrite(FB_ADDR, initial, sizeof(initial));
    memwrite(0x34000, record, sizeof(record));

    cl[cl_size++] = 113;
    append_u32(cl, &cl_size, FB_ADDR);
    append_u16(cl, &cl_size, 64);
    append_u16(cl, &cl_size, 64);
    append_u16(cl, &cl_size, V3D_FORMAT_RGBA8888 << 2);
    cl[cl_size++] = 114;
    append_u32(cl, &cl_size, color);
    append_u32(cl, &cl_size, 0);
    append_u32(cl, &cl_size, 0);
    cl[cl_size++] = 0; /* Final byte of the 14-byte clear-colors packet. */
    append_tile_buffer_packet(cl, &cl_size, 29, FB_ADDR, 0, 0);
    cl[cl_size++] = 115;
    cl[cl_size++] = 0;
    cl[cl_size++] = 0;
    cl[cl_size++] = 103;
    append_u16(cl, &cl_size, 32 * 16);
    append_u16(cl, &cl_size, 32 * 16);
    cl[cl_size++] = 56;
    cl[cl_size++] = 0x32; /* 32-bit XY triangle coordinates. */
    cl[cl_size++] = 64;
    append_u32(cl, &cl_size, 0x34001);
    cl[cl_size++] = 48;
    stq_le_p(cl + cl_size, relative_triangle); /* Coding 2: absolute + deltas. */
    cl_size += sizeof(relative_triangle);
    append_u16(cl, &cl_size, 0x6004); /* Coding 0: shared edge + delta. */
    cl[cl_size++] = 11; /* Coding 1: shared edge + wide delta, orientation 2. */
    cl[cl_size++] = 0;
    cl[cl_size++] = 0;
    cl[cl_size++] = 129; /* Coding 3: absolute triangle. */
    append_u32(cl, &cl_size, 0x0018ffe8);
    append_u32(cl, &cl_size, 0x00180018);
    append_u32(cl, &cl_size, 0xffe8ffe8);
    cl[cl_size++] = 128; /* Compressed-list escape. */
    cl[cl_size++] = 25;  /* Store tile and end frame. */

    writel(RASPI3_V3D_BASE + V3D_CT1CS, 1U << 15);
    memwrite(CL_ADDR, cl, cl_size);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + cl_size);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) & V3D_CTERR, ==, 0);
    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CA), ==,
                    CL_ADDR + cl_size);

    memread(FB_ADDR, fb, sizeof(fb));
    unsigned drawn_pixels = 0;
    for (unsigned i = 0; i < FB_SIZE / sizeof(uint32_t); i++) {
        drawn_pixels += read_u32(fb + i * sizeof(uint32_t)) == color;
    }
    g_assert_cmpuint(drawn_pixels, >, 0);
    /* Sample inside the triangle; (32, 32) lies on its exclusive diagonal. */
    g_assert_cmphex(read_u32(fb + (32 * 64 + 31) * sizeof(uint32_t)),
                    ==, color);
    g_assert_cmphex(read_u32(fb + (10 * 64 + 50) * sizeof(uint32_t)),
                    ==, 0x5a5a5a5a);
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

    for (unsigned color_format_i = 0; color_format_i < 3; color_format_i++) {
        unsigned color_format = color_format_i;
        unsigned store_color_format = color_format == 1 ? 2 : color_format;
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
                                      memory_format, store_color_format);

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

static void test_v3d_ct1_tile_load_high_dma_address(void)
{
    const uint32_t src = 0xc0029000;
    const uint32_t dst = 0xc0030000;
    const uint32_t src_phys = src - 0xc0000000;
    const uint32_t dst_phys = dst - 0xc0000000;
    uint8_t cl[64];
    uint8_t source[64 * 64 * sizeof(uint32_t)];
    uint8_t destination[sizeof(source)];
    uint8_t result[sizeof(source)];
    size_t offset = 0;

    for (unsigned i = 0; i < 64 * 64; i++) {
        stl_le_p(source + i * sizeof(uint32_t), 0xff000000 | i);
    }
    memset(destination, 0xa5, sizeof(destination));
    memwrite(src_phys, source, sizeof(source));
    memwrite(dst_phys, destination, sizeof(destination));

    cl[offset++] = 113;
    append_u32(cl, &offset, dst);
    append_u16(cl, &offset, 64);
    append_u16(cl, &offset, 64);
    append_u16(cl, &offset, 0);
    append_tile_buffer_packet(cl, &offset, 29, src, 0, 0);
    cl[offset++] = 115;
    cl[offset++] = 0;
    cl[offset++] = 0;
    append_tile_buffer_packet(cl, &offset, 28, dst, 0, 0);

    writel(RASPI3_V3D_BASE + V3D_CT1CS, 1U << 15);
    memwrite(CL_ADDR, cl, offset);
    writel(RASPI3_V3D_BASE + V3D_CT1CA, CL_ADDR);
    writel(RASPI3_V3D_BASE + V3D_CT1EA, CL_ADDR + offset);

    g_assert_cmphex(readl(RASPI3_V3D_BASE + V3D_CT1CS) & V3D_CTERR, ==, 0);
    memread(dst_phys, result, sizeof(result));
    g_assert_cmpmem(result, sizeof(result), source, sizeof(source));
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
    qtest_add_func("/bcm2835/v3d/qpu_tmu0_rgba_texture_sampling",
                   test_v3d_qpu_tmu0_rgba_texture_sampling);
    qtest_add_func("/bcm2835/v3d/qpu_captured_labwc_shaders",
                   test_v3d_qpu_captured_labwc_shaders);
    qtest_add_func("/bcm2835/v3d/qpu_tmu1_rgba_texture_sampling",
                   test_v3d_qpu_tmu1_rgba_texture_sampling);
    qtest_add_func("/bcm2835/v3d/qpu_fragment_w_register",
                   test_v3d_qpu_fragment_w_register);
    qtest_add_func("/bcm2835/v3d/qpu_tile_color_signals",
                   test_v3d_qpu_tile_color_signals);
    qtest_add_func("/bcm2835/v3d/qpu_tile_buffer_signals",
                   test_v3d_qpu_tile_buffer_signals);
    qtest_add_func("/bcm2835/v3d/qpu_varying_read",
                   test_v3d_qpu_varying_read);
    qtest_add_func("/bcm2835/v3d/qpu_packed_byte_alu",
                   test_v3d_qpu_packed_byte_alu);
    qtest_add_func("/bcm2835/v3d/qpu_v8muld", test_v3d_qpu_v8muld);
    qtest_add_func("/bcm2835/v3d/qpu_ftoi_edge_values",
                   test_v3d_qpu_ftoi_edge_values);
    qtest_add_func("/bcm2835/v3d/qpu_rfa_unpack_itof",
                   test_v3d_qpu_rfa_unpack_itof);
    qtest_add_func("/bcm2835/v3d/qpu_rfa_unpack_modes",
                   test_v3d_qpu_rfa_unpack_modes);
    qtest_add_func("/bcm2835/v3d/qpu_color_pack_unpack",
                   test_v3d_qpu_color_pack_unpack);
    qtest_add_func("/bcm2835/v3d/qpu_rfa_pack_modes",
                   test_v3d_qpu_rfa_pack_modes);
    qtest_add_func("/bcm2835/v3d/qpu_rfa_pack_32_sat",
                   test_v3d_qpu_rfa_pack_32_sat);
    qtest_add_func("/bcm2835/v3d/qpu_cross_mux_register_reads",
                   test_v3d_qpu_cross_mux_register_reads);
    qtest_add_func("/bcm2835/v3d/qpu_thread_switch_signals",
                   test_v3d_qpu_thread_switch_signals);
    qtest_add_func("/bcm2835/v3d/qpu_vector_rotation",
                   test_v3d_qpu_vector_rotation);
    qtest_add_func("/bcm2835/v3d/qpu_sfu", test_v3d_qpu_sfu);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_vector_io",
                   test_v3d_qpu_vpm_vector_io);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_vertical_32bit",
                   test_v3d_qpu_vpm_vertical_32bit);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_subword_io",
                   test_v3d_qpu_vpm_subword_io);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_single_row",
                   test_v3d_qpu_vpm_dma_single_row);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_2d",
                   test_v3d_qpu_vpm_dma_2d);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_store_blockmode",
                   test_v3d_qpu_vpm_dma_store_blockmode);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_vertical_32bit",
                   test_v3d_qpu_vpm_dma_vertical_32bit);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_vertical_store_32bit",
                   test_v3d_qpu_vpm_dma_vertical_store_32bit);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_vertical_store_blockmode",
                   test_v3d_qpu_vpm_dma_vertical_store_blockmode);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_packed_load",
                   test_v3d_qpu_vpm_dma_packed_load);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_dma_packed_store",
                   test_v3d_qpu_vpm_dma_packed_store);
    qtest_add_func("/bcm2835/v3d/qpu_vpm_setup_and_write",
                   test_v3d_qpu_vpm_setup_and_write);
    qtest_add_func("/bcm2835/v3d/qpu_branch_delay_slots",
                   test_v3d_qpu_branch_delay_slots);
    qtest_add_func("/bcm2835/v3d/identity_and_scratch",
                   test_v3d_identity_and_scratch);
    qtest_add_func("/bcm2835/v3d/irq_masks", test_v3d_irq_masks);
    qtest_add_func("/bcm2835/v3d/ct0_binning_setup",
                   test_v3d_ct0_binning_setup);
    qtest_add_func("/bcm2835/v3d/ct0_primitive_list_format",
                   test_v3d_ct0_primitive_list_format);
    qtest_add_func("/bcm2835/v3d/ct0_flush_count",
                   test_v3d_ct0_flush_count);
    qtest_add_func("/bcm2835/v3d/ct0_shader_state_dma",
                   test_v3d_ct0_shader_state_dma);
    qtest_add_func("/bcm2835/v3d/ct0_clips_and_rasterizes_varying",
                   test_v3d_ct0_clips_and_rasterizes_varying_triangles);
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
    qtest_add_func("/bcm2835/v3d/ct1_tile_load_high_dma_address",
                   test_v3d_ct1_tile_load_high_dma_address);
    qtest_add_func("/bcm2835/v3d/ct1_load_store_layout_conversion",
                   test_v3d_ct1_load_store_layout_conversion);
    qtest_add_func("/bcm2835/v3d/ct1_clear_single_sample_64x64_tile",
                   test_v3d_ct1_clear_single_sample_64x64_tile);
    qtest_add_func("/bcm2835/v3d/ct1_resumes_after_compressed_tile",
                   test_v3d_ct1_resumes_after_compressed_tile);
    qtest_add_func("/bcm2835/v3d/ct1_wgpu_batches_opaque_shader",
                   test_v3d_ct1_wgpu_batches_opaque_shader);
    qtest_add_func("/bcm2835/v3d/ct1_wgpu_batches_tmu0_blend_shader",
                   test_v3d_ct1_wgpu_batches_tmu0_blend_shader);
    qtest_add_func("/bcm2835/v3d/ct1_compressed_xy_triangle_encodings",
                   test_v3d_ct1_compressed_xy_triangle_encodings);
    qtest_add_func("/bcm2835/v3d/ct1_frame_done_requires_eof",
                   test_v3d_ct1_frame_done_requires_eof);
    qtest_start("-machine raspi3ap -smp 4");
    ret = g_test_run();
    qtest_end();
    return ret;
}
