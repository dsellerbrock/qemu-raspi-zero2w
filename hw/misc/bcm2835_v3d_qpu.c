/*
 * Raspberry Pi VideoCore IV QPU execution core.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
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
#define QPU_SIG_PROG_END    3
#define QPU_SIG_SMALL_IMM   13
#define QPU_SIG_LOAD_IMM    14

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

static uint32_t qpu_unpack_r4(uint32_t value, unsigned unpack)
{
    unsigned shift;

    if (unpack == 0) {
        return value;
    }
    if (unpack == 3) {
        value = (value >> 24) & 0xff;
        return value * UINT32_C(0x01010101);
    }
    shift = (unpack - 4) * 8;
    return qpu_float_bits(((value >> shift) & 0xff) / 255.0f);
}

static uint32_t qpu_pack_mul_color(uint32_t value, unsigned pack)
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
    return packed << ((pack - 4) * 8);
}

static bool qpu_read_register(BCM2835V3DQPUState *qpu, bool regfile_b,
                              unsigned address,
                              uint32_t value[BCM2835_V3D_QPU_LANES])
{
    if (address < 32) {
        memcpy(value, qpu->rf[regfile_b][address], sizeof(qpu->rf[0][0]));
        return true;
    }

    if (address == QPU_R_UNIFORM) {
        uint32_t uniform;

        if (!qpu->read_uniform ||
            !qpu->read_uniform(qpu->uniform_opaque,
                               qpu->uniform_address, &uniform) ||
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
                            uint32_t *result, bool *carry)
{
    uint32_t shift = b & 31;
    int32_t sa = (int32_t)a;
    int32_t sb = (int32_t)b;
    float fa = qpu_bits_float(a);
    float fb = qpu_bits_float(b);

    *carry = false;
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
        if (!isfinite(fa) || fa < -2147483648.0f || fa >= 2147483648.0f) {
            return false;
        }
        *result = (uint32_t)(int32_t)fa;
        break;
    case QPU_A_ITOF:
        *result = qpu_float_bits((float)sa);
        break;
    case QPU_A_ADD:
        *result = a + b;
        *carry = *result < a;
        break;
    case QPU_A_SUB:
        *result = a - b;
        *carry = a >= b;
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
    case QPU_A_SHL:
        *result = a << shift;
        break;
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
                            uint32_t *result)
{
    switch (op) {
    case QPU_M_NOP:
        *result = 0;
        return true;
    case QPU_M_FMUL:
        *result = qpu_float_bits(qpu_bits_float(a) * qpu_bits_float(b));
        return true;
    case QPU_M_MUL24:
        *result = (uint32_t)((int32_t)(a << 8) >> 8) *
                  (uint32_t)((int32_t)(b << 8) >> 8);
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
        if (waddr == 46) {
            qpu->tlb_color_all_write_mask |= 1U << lane;
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
 * This bounded slice covers part of the QPU ALUs, branches, uniforms, and
 * horizontal 32-bit VPM access. shortcut: no register-file timing, A-file
 * pack/unpack, r4 half-float unpack, V8MULD, vertical VPM, TMU, or SFU; add
 * each as guest shader coverage requires it.
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
        uint32_t reg_a[BCM2835_V3D_QPU_LANES] = { 0 };
        uint32_t reg_b[BCM2835_V3D_QPU_LANES] = { 0 };
        bool carry[BCM2835_V3D_QPU_LANES];
        bool reads_a;
        bool reads_b;
        bool writes_vpm;
        uint16_t active_add = 0;
        uint16_t active_mul = 0;

        qpu->varying_c_pending_valid = false;

        if ((uint64_t)qpu->pc * sizeof(uint64_t) + sizeof(uint64_t) >
            code_size) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        inst = ldq_le_p(code + qpu->pc * sizeof(uint64_t));
        (*executed)++;
        end = QPU_FIELD(inst, QPU_SIG_SHIFT, 0xf) == QPU_SIG_PROG_END;
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
        if (sig != QPU_SIG_NONE && sig != QPU_SIG_PROG_END &&
            sig != QPU_SIG_SMALL_IMM && sig != QPU_SIG_LOAD_IMM) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
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

        if ((pm && (unpack == 1 || unpack == 2 || pack == 1 || pack == 2 ||
                    unpack > 7 || pack > 7)) ||
            (!pm && (unpack || pack))) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }

        if (sig == QPU_SIG_SMALL_IMM && imm >= 48) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        reads_a = sig != QPU_SIG_LOAD_IMM &&
                  (add_a == QPU_MUX_A || mul_a == QPU_MUX_A);
        reads_b = sig != QPU_SIG_LOAD_IMM && !small_imm &&
                  (add_b == QPU_MUX_B || mul_b == QPU_MUX_B);
        if ((reads_a && !qpu_read_register(qpu, false, raddr_a, reg_a)) ||
            (reads_b && !qpu_read_register(qpu, true, raddr_b, reg_b))) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        if (qpu->io_fault) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        if (sig == QPU_SIG_LOAD_IMM) {
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                uint16_t bit = 1U << lane;

                add_result[lane] = (uint32_t)inst;
                mul_result[lane] = (uint32_t)inst;
                carry[lane] = false;
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
                                     &carry[lane]) ||
                    !qpu_execute_mul(op_mul, ma, mb, &mul_result[lane])) {
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

        if (pm && pack) {
            for (unsigned lane = 0; lane < BCM2835_V3D_QPU_LANES; lane++) {
                mul_result[lane] = qpu_pack_mul_color(mul_result[lane], pack);
            }
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

            if (((active_add & bit) &&
                 !qpu_write(qpu, waddr_add, false, ws, lane,
                            add_result[lane])) ||
                ((active_mul & bit) &&
                 !qpu_write(qpu, waddr_mul, true, ws, lane,
                            mul_result[lane]))) {
                return BCM2835_V3D_QPU_UNSUPPORTED;
            }
        }
        if (qpu->io_fault) {
            return BCM2835_V3D_QPU_CODE_FAULT;
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
