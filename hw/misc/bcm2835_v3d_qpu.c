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

#define QPU_M_NOP           0
#define QPU_M_FMUL          1
#define QPU_M_MUL24         2

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

static uint32_t qpu_read_mux(BCM2835V3DQPUState *qpu, unsigned mux,
                             unsigned lane, unsigned raddr_a,
                             unsigned raddr_b, bool small_imm,
                             unsigned imm)
{
    if (mux <= QPU_MUX_R5) {
        return qpu->acc[mux][lane];
    }
    if (mux == QPU_MUX_A && raddr_a < 32) {
        return qpu->rf[0][raddr_a][lane];
    }
    if (mux == QPU_MUX_B) {
        if (small_imm) {
            return qpu_small_immediate(imm);
        }
        if (raddr_b < 32) {
            return qpu->rf[1][raddr_b][lane];
        }
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
        return raddr < 32 || raddr == QPU_R_NOP;
    }
    if (mux == QPU_MUX_B) {
        return (small_imm && regfile_b) || raddr < 32 || raddr == QPU_R_NOP;
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
    return waddr == QPU_W_NOP;
}

void bcm2835_v3d_qpu_init(BCM2835V3DQPUState *qpu)
{
    memset(qpu, 0, sizeof(*qpu));
}

/*
 * This first execution slice covers the scalar/vector ALUs only.
 * shortcut: no RF pipeline delay or special-register I/O, add both before
 * wiring this core into VPM-fed vertex execution.
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
        bool end;
        uint32_t imm;
        uint32_t add_result[BCM2835_V3D_QPU_LANES];
        uint32_t mul_result[BCM2835_V3D_QPU_LANES];
        bool carry[BCM2835_V3D_QPU_LANES];
        uint16_t active_add = 0;
        uint16_t active_mul = 0;

        if ((uint64_t)qpu->pc * sizeof(uint64_t) + sizeof(uint64_t) >
            code_size) {
            return BCM2835_V3D_QPU_CODE_FAULT;
        }
        inst = ldq_le_p(code + qpu->pc * sizeof(uint64_t));
        (*executed)++;
        end = QPU_FIELD(inst, QPU_SIG_SHIFT, 0xf) == QPU_SIG_PROG_END;
        sig = QPU_FIELD(inst, QPU_SIG_SHIFT, 0xf);
        if (sig != QPU_SIG_NONE && sig != QPU_SIG_PROG_END &&
            sig != QPU_SIG_SMALL_IMM && sig != QPU_SIG_LOAD_IMM) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
        }
        if (QPU_FIELD(inst, QPU_UNPACK_SHIFT, 7) ||
            QPU_FIELD(inst, QPU_PM_SHIFT, 1) ||
            QPU_FIELD(inst, QPU_PACK_SHIFT, 0xf)) {
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

        if (sig == QPU_SIG_SMALL_IMM && imm >= 48) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
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
                uint32_t aa = qpu_read_mux(qpu, add_a, lane, raddr_a,
                                           raddr_b, small_imm, imm);
                uint32_t ab = qpu_read_mux(qpu, add_b, lane, raddr_a,
                                           raddr_b, small_imm, imm);
                uint32_t ma = qpu_read_mux(qpu, mul_a, lane, raddr_a,
                                           raddr_b, small_imm, imm);
                uint32_t mb = qpu_read_mux(qpu, mul_b, lane, raddr_a,
                                           raddr_b, small_imm, imm);
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

        if (waddr_add >= QPU_W_ACC0 &&
            waddr_add < QPU_W_ACC0 + 6 && waddr_add == waddr_mul &&
            (active_add & active_mul)) {
            return BCM2835_V3D_QPU_UNSUPPORTED;
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
        qpu->pc++;
        if (end) {
            return BCM2835_V3D_QPU_END;
        }
    }
    return BCM2835_V3D_QPU_BUDGET;
}
