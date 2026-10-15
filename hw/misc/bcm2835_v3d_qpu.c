/*
 * Raspberry Pi VideoCore IV QPU execution core.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "bcm2835_v3d_qpu.h"
#include <math.h>

#define QPU_FIELD(v, shift, mask) (((v) >> (shift)) & (mask))

#define QPU_SIG_SHIFT       60
#define QPU_UNPACK_SHIFT    57
#define QPU_PM_SHIFT        56
#define QPU_PACK_SHIFT      52
#define QPU_COND_ADD_SHIFT  49
#define QPU_COND_MUL_SHIFT  46
#define QPU_SF              (1ULL << 45)
#define QPU_WS              (1ULL << 44)
#define QPU_WADDR_ADD_SHIFT 38
#define QPU_WADDR_MUL_SHIFT 32
#define QPU_OP_MUL_SHIFT    29
#define QPU_OP_ADD_SHIFT    24
#define QPU_RADDR_A_SHIFT   18
#define QPU_RADDR_B_SHIFT   12
#define QPU_SMALL_IMM_SHIFT 12
#define QPU_ADD_A_SHIFT     9
#define QPU_ADD_B_SHIFT     6
#define QPU_MUL_A_SHIFT     3
#define QPU_MUL_B_SHIFT     0
#define QPU_BRANCH_COND_SHIFT 56
#define QPU_BRANCH_REL_SHIFT 55
#define QPU_BRANCH_REG_SHIFT 54
#define QPU_BRANCH_RADDR_A_SHIFT 49
#define QPU_BRANCH_WS_SHIFT 48
#define QPU_BRANCH_WADDR_ADD_SHIFT 42
#define QPU_BRANCH_WADDR_MUL_SHIFT 36

#define QPU_SIG_NONE        1
#define QPU_SIG_THREAD_SWITCH 2
#define QPU_SIG_PROG_END    3
#define QPU_SIG_WAIT_FOR_SCOREBOARD 4
#define QPU_SIG_SCOREBOARD_UNLOCK 5
#define QPU_SIG_LAST_THREAD_SWITCH 6
#define QPU_SIG_COLOR_LOAD  8
#define QPU_SIG_COLOR_LOAD_END 9
#define QPU_SIG_LOAD_TMU0   10
#define QPU_SIG_LOAD_TMU1   11
#define QPU_SIG_SMALL_IMM   13
#define QPU_SIG_LOAD_IMM    14

#define QPU_T_MEMORY_TILE_SIZE 32

#define QPU_A_NOP           0
#define QPU_A_FADD          1
#define QPU_A_FSUB          2
#define QPU_A_FMIN          3
#define QPU_A_FMAX          4
#define QPU_A_FMINABS       5
#define QPU_A_FMAXABS       6
#define QPU_A_FTOI          7
#define QPU_A_ITOF          8
#define QPU_A_ADD           12
#define QPU_A_SUB           13
#define QPU_A_SHR           14
#define QPU_A_ASR           15
#define QPU_A_ROR           16
#define QPU_A_SHL           17
#define QPU_A_MIN           18
#define QPU_A_MAX           19
#define QPU_A_AND           20
#define QPU_A_OR            21
#define QPU_A_XOR           22
#define QPU_A_NOT           23
#define QPU_A_CLZ           24
#define QPU_A_V8ADDS        30
#define QPU_A_V8SUBS        31

#define QPU_M_NOP           0
#define QPU_M_FMUL          1
#define QPU_M_MUL24         2
#define QPU_M_V8MULD        3
#define QPU_M_V8MIN         4
#define QPU_M_V8MAX         5
#define QPU_M_V8ADDS        6
#define QPU_M_V8SUBS        7

#define QPU_MUX_R0          0
#define QPU_MUX_R5          5
#define QPU_MUX_A           6
#define QPU_MUX_B           7

#define QPU_COND_NEVER      0
#define QPU_COND_ALWAYS     1
#define QPU_COND_ZS         2
#define QPU_COND_ZC         3
#define QPU_COND_NS         4
#define QPU_COND_NC         5
#define QPU_COND_CS         6
#define QPU_COND_CC         7

#define QPU_W_ACC0          32
#define QPU_W_NOP           39
#define QPU_R_NOP           39
#define QPU_R_UNIFORM       32
#define QPU_R_VARYING       35
#define QPU_R_VPM           48
#define QPU_W_VPM           48
#define QPU_W_VPM_SETUP     49
#define QPU_W_SFU_RECIP      52
#define QPU_W_SFU_RECIPSQRT  53
#define QPU_W_SFU_EXP        54
#define QPU_W_SFU_LOG        55
#define QPU_W_TMU0_S         56
#define QPU_W_TMU0_T         57
#define QPU_W_TMU0_R         58
#define QPU_W_TMU0_B         59
#define QPU_W_TMU1_S         60
#define QPU_W_TMU1_T         61
#define QPU_W_TMU1_R         62
#define QPU_W_TMU1_B         63

static uint32_t qpu_float_bits(float value)
{
    uint32_t bits;

    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float qpu_bits_float(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool qpu_condition(BCM2835V3DQPUState *qpu, unsigned cond,
                          unsigned lane)
{
    uint16_t bit = 1U << lane;

    switch (cond) {
    case QPU_COND_NEVER:
        return false;
    case QPU_COND_ALWAYS:
        return true;
    case QPU_COND_ZS:
        return qpu->z & bit;
    case QPU_COND_ZC:
        return !(qpu->z & bit);
    case QPU_COND_NS:
        return qpu->n & bit;
    case QPU_COND_NC:
        return !(qpu->n & bit);
    case QPU_COND_CS:
        return qpu->c & bit;
    case QPU_COND_CC:
        return !(qpu->c & bit);
    default:
        return false;
    }
}

static bool qpu_branch_condition(BCM2835V3DQPUState *qpu, unsigned cond)
{
    switch (cond) {
    case 0:
        return qpu->z == UINT16_MAX;
    case 1:
        return qpu->z == 0;
    case 2:
        return qpu->z != 0;
    case 3:
        return qpu->z != UINT16_MAX;
    case 4:
        return qpu->n == UINT16_MAX;
    case 5:
        return qpu->n == 0;
    case 6:
        return qpu->n != 0;
    case 7:
        return qpu->n != UINT16_MAX;
    case 8:
        return qpu->c == UINT16_MAX;
    case 9:
        return qpu->c == 0;
    case 10:
        return qpu->c != 0;
    case 11:
        return qpu->c != UINT16_MAX;
    case 15:
        return true;
    default:
        return false;
    }
}

static uint32_t qpu_small_immediate(unsigned imm)
{
    if (imm < 16) {
        return imm;
    }
    if (imm < 32) {
        return (uint32_t)((int32_t)imm - 32);
    }
    if (imm < 40) {
        return qpu_float_bits((float)(1U << (imm - 32)));
    }
    return qpu_float_bits(1.0f / (float)(1U << (48 - imm)));
}

static uint32_t qpu_v8_saturating(uint32_t a, uint32_t b, bool subtract)
{
    uint32_t result = 0;

    for (unsigned shift = 0; shift < 32; shift += 8) {
        unsigned av = (a >> shift) & 0xff;
        unsigned bv = (b >> shift) & 0xff;
        unsigned value = subtract ? (av > bv ? av - bv : 0) :
                                    MIN(av + bv, 0xff);

        result |= value << shift;
    }
    return result;
}

static uint32_t qpu_v8_minmax(uint32_t a, uint32_t b, bool maximum)
{
    uint32_t result = 0;

    for (unsigned shift = 0; shift < 32; shift += 8) {
        unsigned av = (a >> shift) & 0xff;
        unsigned bv = (b >> shift) & 0xff;

        result |= (maximum ? MAX(av, bv) : MIN(av, bv)) << shift;
    }
    return result;
}

static uint32_t qpu_half_to_float_bits(uint16_t value)
{
    uint32_t sign = (uint32_t)(value & 0x8000) << 16;
    uint32_t exponent = (value >> 10) & 0x1f;
    uint32_t fraction = value & 0x3ff;

    if (exponent == 0) {
        int32_t exp = -14;

        if (!fraction) {
            return sign;
        }
        while (!(fraction & 0x400)) {
            fraction <<= 1;
            exp--;
        }
        fraction &= 0x3ff;
        return sign | ((uint32_t)(exp + 127) << 23) | (fraction << 13);
    }
    if (exponent == 0x1f) {
        return sign | 0x7f800000 | (fraction << 13) |
               (fraction ? 0x00400000 : 0);
    }
    return sign | ((exponent + 112) << 23) | (fraction << 13);
}

static uint32_t qpu_unpack_r4(uint32_t value, unsigned unpack)
{
    unsigned shift;

    if (unpack == 0) {
        return value;
    }
    if (unpack == 1 || unpack == 2) {
        shift = (unpack - 1) * 16;
        return qpu_half_to_float_bits((uint16_t)(value >> shift));
    }
    if (unpack == 3) {
        value = (value >> 24) & 0xff;
        return value * UINT32_C(0x01010101);
    }
    shift = (unpack - 4) * 8;
    return qpu_float_bits(((value >> shift) & 0xff) / 255.0f);
}

static uint32_t qpu_pack_mul_color(uint32_t value, unsigned pack,
                                  uint32_t previous)
{
    float channel = qpu_bits_float(value);
    uint32_t packed;

    if (isnan(channel) || channel <= 0.0f) {
        packed = 0;
    } else if (channel >= 1.0f) {
        packed = 0xff;
    } else {
        packed = floorf(channel * 255.0f + 0.5f);
    }
    if (pack == 3) {
        return packed * UINT32_C(0x01010101);
    }
    unsigned shift = (pack - 4) * 8;

    return (previous & ~(UINT32_C(0xff) << shift)) | (packed << shift);
}

static uint32_t qpu_float_to_half_bits(uint32_t value)
{
    uint32_t sign = (value >> 16) & 0x8000;
    uint32_t exponent = (value >> 23) & 0xff;
    uint32_t fraction = value & 0x7fffff;
    int half_exponent = (int)exponent - 127 + 15;

    if (exponent == 0xff) {
        return sign | 0x7c00 | (fraction ? (fraction >> 13) | 0x0200 : 0);
    }
    if (exponent == 0) {
        return sign;
    }
    if (half_exponent >= 31) {
        return sign | 0x7c00;
    }
    if (half_exponent <= 0) {
        uint32_t significand = fraction | 0x800000;
        unsigned shift = 14 - half_exponent;
        uint32_t rounded;
        uint32_t remainder;
        uint32_t halfway;

        if (shift > 24) {
            return sign;
        }
        rounded = significand >> shift;
        remainder = significand & ((1U << shift) - 1);
        halfway = 1U << (shift - 1);
        if (remainder > halfway ||
            (remainder == halfway && (rounded & 1))) {
            rounded++;
        }
        return sign | rounded;
    }

    uint32_t half_fraction = fraction >> 13;
    uint32_t remainder = fraction & 0x1fff;

    if (remainder > 0x1000 ||
        (remainder == 0x1000 && (half_fraction & 1))) {
        half_fraction++;
        if (half_fraction == 0x400) {
            half_fraction = 0;
            half_exponent++;
            if (half_exponent == 31) {
                return sign | 0x7c00;
            }
        }
    }
    return sign | (half_exponent << 10) | half_fraction;
}

static bool qpu_pack_a(uint32_t value, uint32_t previous, unsigned pack,
                       bool float_result, bool overflow,
                       bool overflow_negative, uint32_t *packed)
{
    uint32_t component;
    unsigned shift;

    switch (pack) {
    case 1:
    case 2:
        component = float_result ? qpu_float_to_half_bits(value) :
                                   value & 0xffff;
        shift = (pack - 1) * 16;
        *packed = (previous & ~(UINT32_C(0xffff) << shift)) |
                  (component << shift);
        return true;
    case 3:
        component = value & 0xff;
        *packed = component * UINT32_C(0x01010101);
        return true;
    case 4:
    case 5:
    case 6:
    case 7:
        shift = (pack - 4) * 8;
        component = value & 0xff;
        *packed = (previous & ~(UINT32_C(0xff) << shift)) |
                  (component << shift);
        return true;
    case 8:
        if (float_result) {
            return false;
        }
        *packed = overflow ? (overflow_negative ? INT32_MIN : INT32_MAX) :
                             value;
        return true;
    case 9:
    case 10:
        if (float_result) {
            component = qpu_float_to_half_bits(value);
        } else {
            int32_t integer = (int32_t)value;

            integer = CLAMP(integer, INT16_MIN, INT16_MAX);
            component = (uint16_t)integer;
        }
        shift = (pack - 9) * 16;
        *packed = (previous & ~(UINT32_C(0xffff) << shift)) |
                  (component << shift);
        return true;
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
        if ((int32_t)value < 0) {
            component = 0;
        } else {
            component = MIN(value, 0xff);
        }
        if (pack == 11) {
            *packed = component * UINT32_C(0x01010101);
        } else {
            shift = (pack - 12) * 8;
            *packed = (previous & ~(UINT32_C(0xff) << shift)) |
                      (component << shift);
        }
        return true;
    default:
        return false;
    }
}

static bool qpu_add_result_is_float(unsigned op)
{
    return (op >= QPU_A_FADD && op <= QPU_A_FMAXABS) ||
           op == QPU_A_ITOF;
}

static bool qpu_tmu_normalize(float coordinate, unsigned wrap,
                              float *normalized)
{
    if (!isfinite(coordinate)) {
        return false;
    }

    switch (wrap) {
    case 0: /* repeat */
        coordinate -= floorf(coordinate);
        break;
    case 1: /* clamp */
        coordinate = MAX(0.0f, MIN(coordinate, 1.0f));
        break;
    case 2: /* mirror */
        coordinate = fmodf(coordinate, 2.0f);
        if (coordinate < 0.0f) {
            coordinate += 2.0f;
        }
        if (coordinate > 1.0f) {
            coordinate = 2.0f - coordinate;
        }
        break;
    default: /* border color */
        return false;
    }

    *normalized = coordinate;
    return true;
}

static unsigned qpu_tmu_wrap_index(int index, unsigned size, unsigned wrap)
{
    if (wrap == 1) {
        return CLAMP(index, 0, (int)size - 1);
    }
    if (wrap == 2) {
        int period = size * 2;

        index %= period;
        if (index < 0) {
            index += period;
        }
        return index < (int)size ? index : period - 1 - index;
    }
    index %= (int)size;
    return index < 0 ? index + size : index;
}

static bool qpu_tmu_read_texel(BCM2835V3DQPUState *qpu, uint32_t config0,
                               unsigned type, unsigned memory_format,
                               uint32_t base, unsigned width,
                               unsigned height, unsigned x, unsigned y,
                               uint32_t *value)
{
    uint64_t address;

    if (config0 & (1U << 8)) {
        y = height - 1 - y;
    }
    if (type == 16) {
        address = (uint64_t)base + ((uint64_t)y * width + x) * 4;
        return qpu->read_memory && address <= UINT32_MAX &&
               qpu->read_memory(qpu->memory_opaque, address, value);
    }
    if (!qpu->read_texture ||
        !qpu->read_texture(qpu->texture_opaque, base, memory_format,
                           width, height, x, y, value)) {
        return false;
    }
    if (type == 1) {
        *value |= UINT32_C(0xff000000);
    }
    return true;
}

static uint32_t qpu_tmu_blend(uint32_t c00, uint32_t c10,
                              uint32_t c01, uint32_t c11,
                              float fx, float fy)
{
    uint32_t result = 0;

    for (unsigned shift = 0; shift < 32; shift += 8) {
        float top = ((c00 >> shift) & 0xff) * (1.0f - fx) +
                    ((c10 >> shift) & 0xff) * fx;
        float bottom = ((c01 >> shift) & 0xff) * (1.0f - fx) +
                       ((c11 >> shift) & 0xff) * fx;
        unsigned channel = floorf(top * (1.0f - fy) + bottom * fy + 0.5f);

        result |= MIN(channel, 0xffU) << shift;
    }
    return result;
}

static bool qpu_tmu_sample(BCM2835V3DQPUState *qpu, uint32_t config0,
                           unsigned type, unsigned memory_format,
                           uint32_t base, unsigned width, unsigned height,
                           unsigned wrap_s, unsigned wrap_t,
                           bool linear_filter, float s, float t,
                           uint32_t *value)
{
    float normalized_s, normalized_t;
    float x, y;
    int x0, y0;
    unsigned x1, y1;
    uint32_t c00, c10, c01, c11;

    if (!qpu_tmu_normalize(s, wrap_s, &normalized_s) ||
        !qpu_tmu_normalize(t, wrap_t, &normalized_t)) {
        return false;
    }
    if (!linear_filter) {
        unsigned ix = MIN((unsigned)(normalized_s * width), width - 1);
        unsigned iy = MIN((unsigned)(normalized_t * height), height - 1);

        return qpu_tmu_read_texel(qpu, config0, type, memory_format, base,
                                  width, height, ix, iy, value);
    }

    x = normalized_s * width - 0.5f;
    y = normalized_t * height - 0.5f;
    x0 = floorf(x);
    y0 = floorf(y);
    x1 = x0 + 1;
    y1 = y0 + 1;
    if (!qpu_tmu_read_texel(qpu, config0, type, memory_format, base, width,
                            height, qpu_tmu_wrap_index(x0, width, wrap_s),
                            qpu_tmu_wrap_index(y0, height, wrap_t), &c00) ||
        !qpu_tmu_read_texel(qpu, config0, type, memory_format, base, width,
                            height, qpu_tmu_wrap_index(x1, width, wrap_s),
                            qpu_tmu_wrap_index(y0, height, wrap_t), &c10) ||
        !qpu_tmu_read_texel(qpu, config0, type, memory_format, base, width,
                            height, qpu_tmu_wrap_index(x0, width, wrap_s),
                            qpu_tmu_wrap_index(y1, height, wrap_t), &c01) ||
        !qpu_tmu_read_texel(qpu, config0, type, memory_format, base, width,
                            height, qpu_tmu_wrap_index(x1, width, wrap_s),
                            qpu_tmu_wrap_index(y1, height, wrap_t), &c11)) {
        return false;
    }
    *value = qpu_tmu_blend(c00, c10, c01, c11, x - x0, y - y0);
    return true;
}

static BCM2835V3DQPUResult qpu_tmu_fetch(BCM2835V3DQPUState *qpu,
                                         unsigned unit, uint16_t lanes)
{
    BCM2835V3DQPUTMU *tmu = &qpu->tmu[unit];
    unsigned s_addr = QPU_W_TMU0_S + unit * 4;
    uint32_t config1 = 0;

    if (tmu->pending) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "bcm2835_v3d: QPU TMU%u fetch while previous result pending at pc=%u\n",
                      unit, qpu->pc);
        return BCM2835_V3D_QPU_UNSUPPORTED;
    }

    if (tmu->has_t) {
        unsigned type;
        unsigned memory_format;
        unsigned width;
        unsigned height;
        unsigned wrap_s;
        unsigned wrap_t;
        unsigned min_filter;
        unsigned mag_filter;
        bool linear_filter;
        uint32_t base;

        if (!qpu->read_uniform ||
            qpu->uniform_address > UINT32_MAX - sizeof(uint32_t) ||
            !qpu->read_uniform(qpu->uniform_opaque,
                               qpu->uniform_address, &config1)) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        qpu->uniform_address += sizeof(uint32_t);
        type = ((tmu->config0 >> 4) & 0xf) |
               ((config1 >> 27) & 0x10);
        width = (config1 >> 8) & 0x7ff;
        height = (config1 >> 20) & 0x7ff;
        wrap_s = config1 & 3;
        wrap_t = (config1 >> 2) & 3;
        min_filter = (config1 >> 4) & 7;
        mag_filter = (config1 >> 7) & 1;
        linear_filter = mag_filter == 0;
        base = tmu->config0 & 0xfffff000;

        /* shortcut: one mip level only; add LOD selection for multiple levels. */
        if ((type != 0 && type != 1 && type != 16) ||
            ((tmu->config0 >> 10) & 3) ||
            (tmu->config0 & 0x20f) ||
            (config1 & (1U << 19)) ||
            min_filter > 5 ||
            (linear_filter != (min_filter == 0 || min_filter >= 4)) ||
            wrap_s == 3 || wrap_t == 3) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: unsupported TMU%u type=%u size=%ux%u wrap=%u/%u filter=%u/%u cfg=%08x/%08x\n",
                          unit, type, width, height, wrap_s, wrap_t, min_filter,
                          mag_filter, tmu->config0, config1);
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        width = width ? width : 2048;
        height = height ? height : 2048;
        memory_format = type == 16 ? 0 :
                        (width < QPU_T_MEMORY_TILE_SIZE ||
                         height < QPU_T_MEMORY_TILE_SIZE) ? 2 : 1;
        if ((type == 16 && !qpu->read_memory) ||
            (type != 16 && !qpu->read_texture)) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }

        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            if (!(lanes & (1U << lane))) {
                tmu->result[lane] = 0;
                continue;
            }
            if (!(tmu->t_mask & (1U << lane))) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
            if (!qpu_tmu_sample(
                    qpu, tmu->config0, type, memory_format, base, width, height,
                    wrap_s, wrap_t, linear_filter,
                    qpu_bits_float(qpu->io[s_addr][lane]),
                    qpu_bits_float(tmu->t[lane]),
                    &tmu->result[lane])) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
        }
        tmu->has_t = false;
        tmu->t_mask = 0;
    } else {
        if (!qpu->read_memory) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            uint32_t address;

            if (!(lanes & (1U << lane))) {
                tmu->result[lane] = 0;
                continue;
            }
            address = qpu->io[s_addr][lane] & ~3U;
            if (!qpu->read_memory(qpu->memory_opaque, address,
                                  &tmu->result[lane])) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
        }
    }

    tmu->pending = true;
    return BCM2835_V3D_QPU_END;
}

static bool qpu_read_register(BCM2835V3DQPUState *qpu, bool regfile_b,
                              unsigned address,
                              uint32_t value[BCM2835_V3D_QPU_LANES])
{
    if (address == 15 && !regfile_b && qpu->fragment_shader) {
        memcpy(value, qpu->fragment_w, sizeof(qpu->fragment_w));
        return true;
    }

    if (address < 32) {
        memcpy(value, qpu->rf[regfile_b][address], sizeof(qpu->rf[0][0]));
        return true;
    }

    if (address == QPU_R_UNIFORM) {
        uint32_t uniform;
        uint32_t uniform_address = qpu->uniform_address;

        if (!qpu->read_uniform ||
            !qpu->read_uniform(qpu->uniform_opaque,
                               uniform_address, &uniform) ||
            qpu->uniform_address > UINT32_MAX - sizeof(uint32_t)) {
            qpu->io_fault = true;
            return false;
        }
        qpu->uniform_address += sizeof(uint32_t);
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            value[lane] = uniform;
        }
        return true;
    }
    if (address == QPU_R_VARYING) {
        if (!qpu->read_varying || qpu->varying_c_pending_valid ||
            !qpu->read_varying(qpu->varying_opaque, qpu->varying_index,
                               value, qpu->varying_c_pending)) {
            qpu->io_fault = true;
            return false;
        }
        qpu->varying_index++;
        qpu->varying_c_pending_valid = true;
        return true;
    }
    if (address == 40) {
        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            value[lane] = qpu->uniform_address;
        }
        return true;
    }

    if (address == 41) {
        memcpy(value, qpu->fragment_coord[regfile_b],
               sizeof(qpu->fragment_coord[0]));
        return true;
    }

    if (address == QPU_R_VPM) {
        if (!qpu->vpm_read_setup || !qpu->vpm_read_remaining) {
            qpu->io_fault = true;
            return false;
        }
        memcpy(value, qpu->vpm[qpu->vpm_read_address],
               sizeof(qpu->vpm[0]));
        qpu->vpm_read_address = (qpu->vpm_read_address +
                                 qpu->vpm_read_stride) &
                                (BCM2835_V3D_QPU_VPM_ROWS - 1);
        qpu->vpm_read_remaining--;
        return true;
    }

    if (address == QPU_R_NOP) {
        memset(value, 0, sizeof(qpu->vpm[0]));
        return true;
    }

    if (address < G_N_ELEMENTS(qpu->io)) {
        memcpy(value, qpu->io[address], sizeof(qpu->io[0]));
        return true;
    }

    qpu->io_fault = true;
    return false;
}

static uint32_t qpu_read_mux(BCM2835V3DQPUState *qpu, unsigned mux,
                             unsigned lane, const uint32_t reg_a[16],
                             const uint32_t reg_b[16], bool small_imm,
                             unsigned imm, bool pm, unsigned unpack)
{
    if (mux <= QPU_MUX_R5) {
        if (pm && mux == 4) {
            return qpu_unpack_r4(qpu->acc[mux][lane], unpack);
        }
        return qpu->acc[mux][lane];
    }
    if (mux == QPU_MUX_A) {
        return reg_a[lane];
    }
    if (mux == QPU_MUX_B) {
        if (small_imm) {
            return qpu_small_immediate(imm);
        }
        return reg_b[lane];
    }
    return 0;
}

static bool qpu_mux_supported(unsigned mux, unsigned raddr, bool small_imm,
                              bool regfile_b)
{
    if (mux <= QPU_MUX_R5) {
        return true;
    }
    if (mux == QPU_MUX_A) {
        return raddr < 64;
    }
    if (mux == QPU_MUX_B) {
        return (small_imm && regfile_b) || raddr < 64;
    }
    return false;
}

static bool qpu_execute_add(unsigned op, uint32_t a, uint32_t b,
                            uint32_t *result, bool *carry, bool *overflow,
                            bool *overflow_negative)
{
    uint32_t shift = b & 31;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    float fa = qpu_bits_float(a);
    float fb = qpu_bits_float(b);

    *carry = false;
    *overflow = false;
    *overflow_negative = false;
    switch (op) {
    case QPU_A_NOP:
        *result = 0;
        break;
    case QPU_A_FADD:
        *result = qpu_float_bits(fa + fb);
        break;
    case QPU_A_FSUB:
        *result = qpu_float_bits(fa - fb);
        break;
    case QPU_A_FMIN:
        *result = qpu_float_bits(fa < fb ? fa : fb);
        break;
    case QPU_A_FMAX:
        *result = qpu_float_bits(fa > fb ? fa : fb);
        break;
    case QPU_A_FMINABS:
        *result = qpu_float_bits(fabsf(fa) < fabsf(fb) ? fa : fb);
        break;
    case QPU_A_FMAXABS:
        *result = qpu_float_bits(fabsf(fa) > fabsf(fb) ? fa : fb);
        break;
    case QPU_A_FTOI:
        /* shortcut: VC4 leaves NaN/overflow bits unspecified; use silicon
         * captures if exact edge behavior matters.
         */
        *result = isnan(fa) ? 0 : fa >= 2147483648.0f ? INT32_MAX :
                  fa <= -2147483648.0f ? INT32_MIN : (uint32_t)(int32_t)fa;
        break;
    case QPU_A_ITOF:
        *result = qpu_float_bits((float)sa);
        break;
    case QPU_A_ADD:
        *result = a + b;
        *carry = *result < a;
        *overflow = (~(a ^ b) & (a ^ *result)) >> 31;
        *overflow_negative = sa < 0;
        break;
    case QPU_A_SUB:
        *result = a - b;
        *carry = a >= b;
        *overflow = ((a ^ b) & (a ^ *result)) >> 31;
        *overflow_negative = sa < 0;
        break;
    case QPU_A_SHR:
        *result = a >> shift;
        break;
    case QPU_A_ASR:
        *result = (uint32_t)(sa >> shift);
        break;
    case QPU_A_ROR:
        *result = shift ? (a >> shift) | (a << (32 - shift)) : a;
        break;
    case QPU_A_SHL: {
        int64_t shifted = (int64_t)sa * (INT64_C(1) << shift);

        *result = a << shift;
        *overflow = shifted < INT32_MIN || shifted > INT32_MAX;
        *overflow_negative = shifted < 0;
        break;
    }
    case QPU_A_MIN:
        *result = sa < sb ? a : b;
        break;
    case QPU_A_MAX:
        *result = sa > sb ? a : b;
        break;
    case QPU_A_AND:
        *result = a & b;
        break;
    case QPU_A_OR:
        *result = a | b;
        break;
    case QPU_A_XOR:
        *result = a ^ b;
        break;
    case QPU_A_NOT:
        *result = ~a;
        break;
    case QPU_A_CLZ:
        *result = clz32(a);
        break;
    case QPU_A_V8ADDS:
        *result = qpu_v8_saturating(a, b, false);
        break;
    case QPU_A_V8SUBS:
        *result = qpu_v8_saturating(a, b, true);
        break;
    default:
        return false;
    }

    return true;
}

static bool qpu_execute_mul(unsigned op, uint32_t a, uint32_t b,
                            uint32_t *result, bool *overflow,
                            bool *overflow_negative)
{
    *overflow = false;
    *overflow_negative = false;
    switch (op) {
    case QPU_M_NOP:
        *result = 0;
        return true;
    case QPU_M_FMUL:
        *result = qpu_float_bits(qpu_bits_float(a) * qpu_bits_float(b));
        return true;
    case QPU_M_MUL24: {
        int32_t sa = (int32_t)(a << 8) >> 8;
        int32_t sb = (int32_t)(b << 8) >> 8;
        int64_t product = (int64_t)sa * sb;

        *result = (uint32_t)product;
        *overflow = product < INT32_MIN || product > INT32_MAX;
        *overflow_negative = product < 0;
        return true;
    }
    case QPU_M_V8MULD:
        *result = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) {
            *result |= ((((a >> shift) & 0xff) * (b >> 24)) >> 8) << shift;
        }
        return true;
    case QPU_M_V8MIN:
        *result = qpu_v8_minmax(a, b, false);
        return true;
    case QPU_M_V8MAX:
        *result = qpu_v8_minmax(a, b, true);
        return true;
    case QPU_M_V8ADDS:
        *result = qpu_v8_saturating(a, b, false);
        return true;
    case QPU_M_V8SUBS:
        *result = qpu_v8_saturating(a, b, true);
        return true;
    default:
        return false;
    }
}

static uint32_t qpu_execute_sfu(unsigned address, uint32_t input)
{
    float value = qpu_bits_float(input);
    float result;

    /* shortcut: libm is not bit-exact; use VC4 tables if rendered pixels require it. */
    switch (address) {
    case QPU_W_SFU_RECIP:
        result = 1.0f / value;
        break;
    case QPU_W_SFU_RECIPSQRT:
        result = 1.0f / sqrtf(value);
        break;
    case QPU_W_SFU_EXP:
        result = exp2f(value);
        break;
    case QPU_W_SFU_LOG:
        result = log2f(value);
        break;
    default:
        g_assert_not_reached();
    }
    return qpu_float_bits(result);
}

static bool qpu_write(BCM2835V3DQPUState *qpu, unsigned waddr, bool regfile,
                      bool ws, unsigned lane, uint32_t value)
{
    if (waddr < 32) {
        qpu->rf[regfile ^ ws][waddr][lane] = value;
        return true;
    }
    if (waddr >= QPU_W_ACC0 && waddr < QPU_W_ACC0 + 6) {
        qpu->acc[waddr - QPU_W_ACC0][lane] = value;
        return true;
    }
    if (waddr == QPU_W_VPM) {
        qpu->vpm_write_pending[lane] = value;
        return true;
    }
    if (waddr == QPU_W_VPM_SETUP) {
        unsigned size;
        unsigned stride;
        unsigned address;

        if (lane != 0) {
            return true;
        }
        if ((value >> 30) != 0 || !(value & (1U << 11))) {
            qpu->io_fault = true;
            return false;
        }
        size = (value >> 8) & 3;
        stride = (value >> 12) & 0x3f;
        address = value & 0x3f;
        if (size != 2) {
            qpu->io_fault = true;
            return false;
        }
        stride = stride ? stride : 64;
        if (regfile ^ ws) {
            qpu->vpm_write_address = address;
            qpu->vpm_write_stride = stride;
            qpu->vpm_write_setup = true;
            qpu->vpm_write_history_count = 0;
            qpu->vpm_write_total = 0;
        } else {
            qpu->vpm_read_address = address;
            qpu->vpm_read_stride = stride;
            qpu->vpm_read_remaining = (value >> 20) & 0xf;
            if (!qpu->vpm_read_remaining) {
                qpu->vpm_read_remaining = BCM2835_V3D_QPU_LANES;
            }
            qpu->vpm_read_setup = true;
        }
        return true;
    }
    if (waddr == QPU_W_NOP) {
        return true;
    }
    if (waddr == 40) {
        if (lane == 0) {
            qpu->uniform_address = value;
        }
        return true;
    }
    if (waddr >= 38 && waddr < 64) {
        qpu->io[waddr][lane] = value;
        if (waddr >= QPU_W_TMU0_S) {
            unsigned unit = (waddr - QPU_W_TMU0_S) / 4;
            unsigned reg = (waddr - QPU_W_TMU0_S) % 4;

            qpu->tmu[unit].instruction_write_mask[reg] |= 1U << lane;
        }
        if (waddr == 45 || waddr == 46) {
            /* shortcut: alias both color ports until MSAA is implemented. */
            qpu->io[45][lane] = value;
            qpu->io[46][lane] = value;
            qpu->tlb_color_write_mask |= 1U << lane;
        }
        return true;
    }
    return false;
}

void bcm2835_v3d_qpu_init(BCM2835V3DQPUState *qpu)
{
    memset(qpu, 0, sizeof(*qpu));
}

/*
 * This bounded slice covers part of the QPU ALUs, branches, uniforms,
 * horizontal 32-bit VPM access, MUL vector rotation, TMU0/TMU1 RGBA texture
 * sampling, and most register-file A pack/unpack modes. shortcut: pack mode 8
 * saturation, register-file timing, vertical VPM access, different
 * min/mag spatial filters or multiple mip levels; add those as guest shaders
 * require.
 */
BCM2835V3DQPUResult bcm2835_v3d_qpu_run(const uint8_t *code,
                                         uint32_t code_size,
                                         uint32_t instruction_budget,
                                         BCM2835V3DQPUState *qpu,
                                         uint32_t *executed)
{
    if (!qpu || !executed || (!code && code_size)) {
        return BCM2835_V3D_QPU_CODE_FAULT;
    }
    *executed = 0;
    bool branch_pending = false;
    uint32_t branch_target = 0;
    unsigned branch_delay = 0;
    while (*executed < instruction_budget) {
        uint64_t inst;
        unsigned sig;
        unsigned op_add;
        unsigned op_mul;
        unsigned cond_add;
        unsigned cond_mul;
        unsigned waddr_add;
        unsigned waddr_mul;
        unsigned raddr_a;
        unsigned raddr_b;
        unsigned add_a;
        unsigned add_b;
        unsigned mul_a;
        unsigned mul_b;
        bool ws;
        bool set_flags;
        bool small_imm;
        bool pm;
        bool end;
        uint32_t imm;
        unsigned unpack;
        unsigned pack;
        uint32_t add_result[BCM2835_V3D_QPU_LANES];
        uint32_t mul_result[BCM2835_V3D_QPU_LANES];
        bool add_overflow[BCM2835_V3D_QPU_LANES];
        bool add_overflow_negative[BCM2835_V3D_QPU_LANES];
        bool mul_overflow[BCM2835_V3D_QPU_LANES];
        bool mul_overflow_negative[BCM2835_V3D_QPU_LANES];
        uint32_t instruction_uniform_address;
        uint32_t reg_a[BCM2835_V3D_QPU_LANES] = { 0 };
        uint32_t reg_b[BCM2835_V3D_QPU_LANES] = { 0 };
        bool carry[BCM2835_V3D_QPU_LANES];
        bool reads_a;
        bool reads_b;
        bool rfa_float_input;
        bool writes_vpm;
        uint16_t active_add = 0;
        uint16_t active_mul = 0;
        bool add_sfu_write;
        bool mul_sfu_write;

        qpu->varying_c_pending_valid = false;
        for (unsigned i = 0; i < BCM2835_V3D_QPU_TMUS; i++) {
            memset(qpu->tmu[i].instruction_write_mask, 0,
                   sizeof(qpu->tmu[i].instruction_write_mask));
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(qpu->sfu_pending); i++) {
            if (qpu->sfu_pending[i] &&
                qpu->sfu_ready_at[i] <= qpu->cycles) {
                memcpy(qpu->acc[4], qpu->sfu_result[i],
                       sizeof(qpu->acc[4]));
                qpu->sfu_pending[i] = false;
            }
        }

        if ((uint64_t)qpu->pc * sizeof(uint64_t) + sizeof(uint64_t) >
            code_size) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        inst = ldq_le_p(code + qpu->pc * sizeof(uint64_t));
        (*executed)++;
        qpu->cycles++;
        end = QPU_FIELD(inst, QPU_SIG_SHIFT, 0xf) == QPU_SIG_PROG_END ||
              QPU_FIELD(inst, QPU_SIG_SHIFT, 0xf) ==
              QPU_SIG_COLOR_LOAD_END;
        sig = QPU_FIELD(inst, QPU_SIG_SHIFT, 0xf);
        if (sig == 15) {
            unsigned branch_cond = QPU_FIELD(inst,
                                              QPU_BRANCH_COND_SHIFT, 0xf);
            bool relative = inst & (1ULL << QPU_BRANCH_REL_SHIFT);
            bool use_reg = inst & (1ULL << QPU_BRANCH_REG_SHIFT);
            unsigned branch_raddr = QPU_FIELD(
                inst, QPU_BRANCH_RADDR_A_SHIFT, 0x1f);
            bool branch_ws = inst & (1ULL << QPU_BRANCH_WS_SHIFT);
            unsigned branch_waddr_add = QPU_FIELD(
                inst, QPU_BRANCH_WADDR_ADD_SHIFT, 0x3f);
            unsigned branch_waddr_mul = QPU_FIELD(
                inst, QPU_BRANCH_WADDR_MUL_SHIFT, 0x3f);
            int64_t target = (int32_t)(uint32_t)inst;
            uint32_t link = (qpu->pc + 4) * sizeof(uint64_t);
            bool taken;

            if (branch_pending ||
                (branch_cond > 11 && branch_cond != 15) ||
                (branch_waddr_add >= 38 && branch_waddr_add != QPU_W_NOP) ||
                (branch_waddr_mul >= 38 && branch_waddr_mul != QPU_W_NOP) ||
                (branch_waddr_add == branch_waddr_mul &&
                 branch_waddr_add >= QPU_W_ACC0 &&
                 branch_waddr_add < QPU_W_ACC0 + 6)) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
            if (relative) {
                target += (int64_t)(qpu->pc + 4) * sizeof(uint64_t);
            }
            if (use_reg) {
                target += qpu->rf[0][branch_raddr][0];
            }
            taken = qpu_branch_condition(qpu, branch_cond);
            if (taken && (target < 0 || target > UINT32_MAX ||
                          (target & (sizeof(uint64_t) - 1)))) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                if (!qpu_write(qpu, branch_waddr_add, false, branch_ws,
                               lane, link) ||
                    !qpu_write(qpu, branch_waddr_mul, true, branch_ws,
                               lane, link)) {
                    return BCM2835_V3D_QPU_UNSUPPORTED;
                }
            }
            if (qpu->io_fault) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
            qpu->pc++;
            if (taken) {
                branch_pending = true;
                branch_target = target / sizeof(uint64_t);
                branch_delay = 3;
            }
            continue;
        }
        /* shortcut: execute thread switches serially; add dual threads when needed. */
        if (sig != QPU_SIG_NONE && sig != QPU_SIG_PROG_END &&
            sig != QPU_SIG_THREAD_SWITCH &&
            sig != QPU_SIG_WAIT_FOR_SCOREBOARD &&
            sig != QPU_SIG_SCOREBOARD_UNLOCK &&
            sig != QPU_SIG_LAST_THREAD_SWITCH &&
            sig != QPU_SIG_COLOR_LOAD &&
            sig != QPU_SIG_COLOR_LOAD_END &&
            sig != QPU_SIG_LOAD_TMU0 &&
            sig != QPU_SIG_LOAD_TMU1 &&
            sig != QPU_SIG_SMALL_IMM && sig != QPU_SIG_LOAD_IMM) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        if (((sig == QPU_SIG_WAIT_FOR_SCOREBOARD ||
              sig == QPU_SIG_SCOREBOARD_UNLOCK ||
              sig == QPU_SIG_COLOR_LOAD ||
              sig == QPU_SIG_COLOR_LOAD_END) && !qpu->fragment_shader)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        /* shortcut: CT1 is serial; add a scoreboard if fragment QPUs overlap. */
        op_add = QPU_FIELD(inst, QPU_OP_ADD_SHIFT, 0x1f);
        op_mul = QPU_FIELD(inst, QPU_OP_MUL_SHIFT, 7);
        cond_add = QPU_FIELD(inst, QPU_COND_ADD_SHIFT, 7);
        cond_mul = QPU_FIELD(inst, QPU_COND_MUL_SHIFT, 7);
        waddr_add = QPU_FIELD(inst, QPU_WADDR_ADD_SHIFT, 0x3f);
        waddr_mul = QPU_FIELD(inst, QPU_WADDR_MUL_SHIFT, 0x3f);
        raddr_a = QPU_FIELD(inst, QPU_RADDR_A_SHIFT, 0x3f);
        raddr_b = QPU_FIELD(inst, QPU_RADDR_B_SHIFT, 0x3f);
        add_a = QPU_FIELD(inst, QPU_ADD_A_SHIFT, 7);
        add_b = QPU_FIELD(inst, QPU_ADD_B_SHIFT, 7);
        mul_a = QPU_FIELD(inst, QPU_MUL_A_SHIFT, 7);
        mul_b = QPU_FIELD(inst, QPU_MUL_B_SHIFT, 7);
        ws = inst & QPU_WS;
        set_flags = inst & QPU_SF;
        small_imm = sig == QPU_SIG_SMALL_IMM;
        imm = QPU_FIELD(inst, QPU_SMALL_IMM_SHIFT, 0x3f);
        unpack = QPU_FIELD(inst, QPU_UNPACK_SHIFT, 7);
        pm = inst & (1ULL << QPU_PM_SHIFT);
        pack = QPU_FIELD(inst, QPU_PACK_SHIFT, 0xf);
        rfa_float_input =
            ((add_a == QPU_MUX_A || add_b == QPU_MUX_A) &&
             op_add >= QPU_A_FADD && op_add <= QPU_A_FTOI) ||
            ((mul_a == QPU_MUX_A || mul_b == QPU_MUX_A) &&
             op_mul == QPU_M_FMUL);

        if ((pm && (pack == 1 || pack == 2 || unpack > 7 || pack > 7)) ||
            (!pm && unpack > 7)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }

        if (small_imm && imm >= 48 &&
            (add_a == QPU_MUX_B || add_b == QPU_MUX_B ||
             mul_a == QPU_MUX_B || mul_b == QPU_MUX_B)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        reads_a = sig != QPU_SIG_LOAD_IMM &&
                  (add_a == QPU_MUX_A || add_b == QPU_MUX_A ||
                   mul_a == QPU_MUX_A || mul_b == QPU_MUX_A);
        reads_b = sig != QPU_SIG_LOAD_IMM && !small_imm &&
                  (add_a == QPU_MUX_B || add_b == QPU_MUX_B ||
                   mul_a == QPU_MUX_B || mul_b == QPU_MUX_B);
        instruction_uniform_address = qpu->uniform_address;
        if ((reads_a && !qpu_read_register(qpu, false, raddr_a, reg_a)) ||
            (reads_b && !qpu_read_register(qpu, true, raddr_b, reg_b))) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        if (qpu->io_fault) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        if (!pm && unpack) {
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint32_t value = reg_a[lane];

                if (unpack == 1 || unpack == 2) {
                    unsigned shift = (unpack - 1) * 16;
                    uint16_t half = (uint16_t)(value >> shift);

                    reg_a[lane] = rfa_float_input ?
                        qpu_half_to_float_bits(half) :
                        (uint32_t)(int32_t)(int16_t)half;
                } else if (unpack == 3) {
                    uint32_t alpha = (value >> 24) & 0xff;

                    reg_a[lane] = alpha * UINT32_C(0x01010101);
                } else {
                    unsigned shift = (unpack - 4) * 8;
                    uint32_t byte = (value >> shift) & 0xff;

                    reg_a[lane] = rfa_float_input ?
                        qpu_float_bits(byte / 255.0f) : byte;
                }
            }
        }
        if (sig == QPU_SIG_LOAD_IMM) {
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint16_t bit = 1U << lane;

                add_result[lane] = (uint32_t)inst;
                mul_result[lane] = (uint32_t)inst;
                carry[lane] = false;
                add_overflow[lane] = false;
                add_overflow_negative[lane] = false;
                mul_overflow[lane] = false;
                mul_overflow_negative[lane] = false;
                if (qpu_condition(qpu, cond_add, lane)) {
                    active_add |= bit;
                }
                if (qpu_condition(qpu, cond_mul, lane)) {
                    active_mul |= bit;
                }
            }
        } else {
            if (!qpu_mux_supported(add_a, raddr_a, small_imm, false) ||
                !qpu_mux_supported(add_b, raddr_b, small_imm, true) ||
                !qpu_mux_supported(mul_a, raddr_a, small_imm, false) ||
                !qpu_mux_supported(mul_b, raddr_b, small_imm, true)) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint32_t aa = qpu_read_mux(qpu, add_a, lane, reg_a,
                                           reg_b, small_imm, imm, pm, unpack);
                uint32_t ab = qpu_read_mux(qpu, add_b, lane, reg_a,
                                           reg_b, small_imm, imm, pm, unpack);
                uint32_t ma = qpu_read_mux(qpu, mul_a, lane, reg_a,
                                           reg_b, small_imm, imm, pm, unpack);
                uint32_t mb = qpu_read_mux(qpu, mul_b, lane, reg_a,
                                           reg_b, small_imm, imm, pm, unpack);
                uint16_t bit = 1U << lane;

                if (!qpu_execute_add(op_add, aa, ab, &add_result[lane],
                                     &carry[lane], &add_overflow[lane],
                                     &add_overflow_negative[lane]) ||
                    !qpu_execute_mul(op_mul, ma, mb, &mul_result[lane],
                                     &mul_overflow[lane],
                                     &mul_overflow_negative[lane])) {
                    return BCM2835_V3D_QPU_UNSUPPORTED;
                }
                if (qpu_condition(qpu, cond_add, lane)) {
                    active_add |= bit;
                }
                if (qpu_condition(qpu, cond_mul, lane)) {
                    active_mul |= bit;
                }
            }
        }

        /* Small-immediate values 48–63 rotate the MUL output vector. */
        if (small_imm && imm >= 48) {
            unsigned rotation = imm == 48 ? qpu->acc[5][0] & 0xf : imm - 48;
            uint32_t rotated[BCM2835_V3D_QPU_LANES];

            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                rotated[(lane + rotation) % BCM2835_V3D_QPU_LANES] =
                    mul_result[lane];
            }
            memcpy(mul_result, rotated, sizeof(mul_result));
        }

        if (pm && pack) {
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint32_t previous;

                if (waddr_mul < 32) {
                    previous = qpu->rf[1 ^ ws][waddr_mul][lane];
                } else if (waddr_mul >= QPU_W_ACC0 &&
                           waddr_mul < QPU_W_ACC0 + 6) {
                    previous = qpu->acc[waddr_mul - QPU_W_ACC0][lane];
                } else if (waddr_mul == QPU_W_NOP) {
                    previous = 0;
                } else {
                    return BCM2835_V3D_QPU_UNSUPPORTED;
                }
                mul_result[lane] = qpu_pack_mul_color(mul_result[lane],
                                                       pack, previous);
            }
        }
        if (!pm && pack) {
            bool pack_add = !ws;
            unsigned waddr = pack_add ? waddr_add : waddr_mul;
            uint16_t active = pack_add ? active_add : active_mul;
            uint32_t *result = pack_add ? add_result : mul_result;
            bool float_result = pack_add ?
                qpu_add_result_is_float(op_add) : op_mul == QPU_M_FMUL;

            if (waddr >= 32) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                if (active & (1U << lane)) {
                    if (!qpu_pack_a(result[lane], qpu->rf[0][waddr][lane],
                                    pack, float_result,
                                    pack_add ? add_overflow[lane] :
                                               mul_overflow[lane],
                                    pack_add ? add_overflow_negative[lane] :
                                               mul_overflow_negative[lane],
                                    &result[lane])) {
                        return BCM2835_V3D_QPU_UNSUPPORTED;
                    }
                }
            }
        }

        add_sfu_write = waddr_add >= QPU_W_SFU_RECIP &&
                        waddr_add <= QPU_W_SFU_LOG && active_add;
        mul_sfu_write = waddr_mul >= QPU_W_SFU_RECIP &&
                        waddr_mul <= QPU_W_SFU_LOG && active_mul;
        if ((add_sfu_write && mul_sfu_write) ||
            (pm && pack && (add_sfu_write || mul_sfu_write))) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        if (add_sfu_write || mul_sfu_write) {
            const uint32_t *input = add_sfu_write ? add_result : mul_result;
            uint16_t active = add_sfu_write ? active_add : active_mul;
            unsigned address = add_sfu_write ? waddr_add : waddr_mul;
            unsigned slot;

            for (slot = 0; slot < G_N_ELEMENTS(qpu->sfu_pending); slot++) {
                if (!qpu->sfu_pending[slot]) {
                    break;
                }
            }
            if (slot == G_N_ELEMENTS(qpu->sfu_pending)) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
            memcpy(qpu->sfu_result[slot], qpu->acc[4],
                   sizeof(qpu->acc[4]));
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                if (active & (1U << lane)) {
                    qpu->sfu_result[slot][lane] =
                        qpu_execute_sfu(address, input[lane]);
                }
            }
            qpu->sfu_ready_at[slot] = qpu->cycles + 2;
            qpu->sfu_pending[slot] = true;
        }

        if (waddr_add >= QPU_W_ACC0 &&
            waddr_add < QPU_W_ACC0 + 6 && waddr_add == waddr_mul &&
            (active_add & active_mul)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        if (waddr_add == QPU_W_VPM && waddr_mul == QPU_W_VPM &&
            (active_add & active_mul)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }

        writes_vpm = (waddr_add == QPU_W_VPM && active_add) ||
                     (waddr_mul == QPU_W_VPM && active_mul);
        if (writes_vpm) {
            if (!qpu->vpm_write_setup ||
                qpu->vpm_write_address >= BCM2835_V3D_QPU_VPM_ROWS) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
            memcpy(qpu->vpm_write_pending,
                   qpu->vpm[qpu->vpm_write_address],
                   sizeof(qpu->vpm_write_pending));
        }

        for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
            uint16_t bit = 1U << lane;

            if (((active_add & bit) && !add_sfu_write &&
                 !qpu_write(qpu, waddr_add, false, ws, lane,
                            add_result[lane])) ||
                ((active_mul & bit) && !mul_sfu_write &&
                 !qpu_write(qpu, waddr_mul, true, ws, lane,
                            mul_result[lane]))) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
        }
        if (qpu->io_fault) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        if (waddr_add == waddr_mul && waddr_add >= QPU_W_TMU0_S &&
            waddr_add <= QPU_W_TMU1_B && (active_add & active_mul)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        /* shortcut: consume at most one texture setup uniform per issue. */
        for (unsigned unit = 0; unit < BCM2835_V3D_QPU_TMUS; unit++) {
            BCM2835V3DQPUTMU *tmu = &qpu->tmu[unit];
            unsigned t_addr = QPU_W_TMU0_T + unit * 4;
            uint16_t *write_mask = tmu->instruction_write_mask;

            if (write_mask[2] || write_mask[3] ||
                (write_mask[0] && write_mask[1])) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
            if (write_mask[1]) {
                if (tmu->has_t ||
                    qpu->uniform_address != instruction_uniform_address ||
                    !qpu->read_uniform ||
                    qpu->uniform_address > UINT32_MAX - sizeof(uint32_t) ||
                    !qpu->read_uniform(qpu->uniform_opaque,
                                       qpu->uniform_address,
                                       &tmu->config0)) {
                    return tmu->has_t ? BCM2835_V3D_QPU_UNSUPPORTED :
                                        BCM2835_V3D_QPU_CODE_FAULT;
                }
                qpu->uniform_address += sizeof(uint32_t);
                tmu->t_mask = write_mask[1];
                memcpy(tmu->t, qpu->io[t_addr], sizeof(tmu->t));
                tmu->has_t = true;
            }
            if (write_mask[0]) {
                BCM2835V3DQPUResult result;

                if (tmu->has_t &&
                    qpu->uniform_address != instruction_uniform_address) {
                    return BCM2835_V3D_QPU_UNSUPPORTED;
                }
                result = qpu_tmu_fetch(qpu, unit, write_mask[0]);
                if (result != BCM2835_V3D_QPU_END) {
                    return result;
                }
            }
        }
        if (sig == QPU_SIG_LOAD_TMU0 || sig == QPU_SIG_LOAD_TMU1) {
            unsigned unit = sig == QPU_SIG_LOAD_TMU0 ? 0 : 1;
            BCM2835V3DQPUTMU *tmu = &qpu->tmu[unit];

            if (!tmu->pending) {
                return BCM2835_V3D_QPU_CODE_FAULT;
            }
            memcpy(qpu->acc[4], tmu->result, sizeof(tmu->result));
            tmu->pending = false;
        }
        if (sig == QPU_SIG_COLOR_LOAD || sig == QPU_SIG_COLOR_LOAD_END) {
            memcpy(qpu->acc[4], qpu->io[45], sizeof(qpu->acc[4]));
        }
        if (writes_vpm) {
            if (qpu->vpm_write_total < UINT8_MAX) {
                qpu->vpm_write_total++;
            }
            if (qpu->vpm_write_history_count <
                BCM2835_V3D_QPU_VPM_WRITE_HISTORY) {
                qpu->vpm_write_history[qpu->vpm_write_history_count++] =
                    qpu->vpm_write_address;
            } else {
                memmove(qpu->vpm_write_history,
                        qpu->vpm_write_history + 1,
                        (BCM2835_V3D_QPU_VPM_WRITE_HISTORY - 1) *
                        sizeof(qpu->vpm_write_history[0]));
                qpu->vpm_write_history[
                    BCM2835_V3D_QPU_VPM_WRITE_HISTORY - 1] =
                    qpu->vpm_write_address;
            }
            memcpy(qpu->vpm[qpu->vpm_write_address],
                   qpu->vpm_write_pending,
                   sizeof(qpu->vpm_write_pending));
            qpu->vpm_write_address = (qpu->vpm_write_address +
                                      qpu->vpm_write_stride) &
                                     (BCM2835_V3D_QPU_VPM_ROWS - 1);
        }

        if (set_flags) {
            bool use_mul = op_add == QPU_A_NOP || cond_add == QPU_COND_NEVER;

            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint32_t result = use_mul ? mul_result[lane] : add_result[lane];
                uint16_t bit = 1U << lane;

                qpu->z = (qpu->z & ~bit) | (!result ? bit : 0);
                qpu->n = (qpu->n & ~bit) |
                         ((int32_t)result < 0 ? bit : 0);
                qpu->c = (qpu->c & ~bit) |
                         (!use_mul && carry[lane] ? bit : 0);
            }
        }
        if (qpu->varying_c_pending_valid) {
            memcpy(qpu->acc[5], qpu->varying_c_pending,
                   sizeof(qpu->acc[5]));
        }
        qpu->pc++;
        if (branch_pending && !--branch_delay) {
            qpu->pc = branch_target;
            branch_pending = false;
        }
        if (end) {
            return BCM2835_V3D_QPU_END;
        }
    }
    return BCM2835_V3D_QPU_BUDGET;
}
