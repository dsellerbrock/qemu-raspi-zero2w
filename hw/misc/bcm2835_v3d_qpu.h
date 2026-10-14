/*
 * Raspberry Pi VideoCore IV QPU execution core.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#ifndef HW_MISC_BCM2835_V3D_QPU_H
#define HW_MISC_BCM2835_V3D_QPU_H

#include <stdbool.h>
#include <stdint.h>

#define BCM2835_V3D_QPU_LANES 16
#define BCM2835_V3D_QPU_VPM_ROWS 64

typedef bool (*BCM2835V3DQPUReadUniform)(void *opaque, uint32_t address,
                                         uint32_t *value);

typedef struct BCM2835V3DQPUState {
    uint32_t rf[2][32][BCM2835_V3D_QPU_LANES];
    uint32_t acc[6][BCM2835_V3D_QPU_LANES];
    uint32_t vpm[BCM2835_V3D_QPU_VPM_ROWS][BCM2835_V3D_QPU_LANES];
    uint32_t io[64][BCM2835_V3D_QPU_LANES];
    uint16_t z;
    uint16_t n;
    uint16_t c;
    uint32_t pc;
    uint32_t uniform_address;
    uint32_t vpm_read_address;
    uint32_t vpm_read_stride;
    uint32_t vpm_read_remaining;
    uint32_t vpm_write_address;
    uint32_t vpm_write_stride;
    uint32_t vpm_write_pending[BCM2835_V3D_QPU_LANES];
    bool vpm_read_setup;
    bool vpm_write_setup;
    bool io_fault;
    BCM2835V3DQPUReadUniform read_uniform;
    void *uniform_opaque;
} BCM2835V3DQPUState;

typedef enum BCM2835V3DQPUResult {
    BCM2835_V3D_QPU_END,
    BCM2835_V3D_QPU_UNSUPPORTED,
    BCM2835_V3D_QPU_BUDGET,
    BCM2835_V3D_QPU_CODE_FAULT,
} BCM2835V3DQPUResult;

void bcm2835_v3d_qpu_init(BCM2835V3DQPUState *qpu);
BCM2835V3DQPUResult bcm2835_v3d_qpu_run(const uint8_t *code,
                                         uint32_t code_size,
                                         uint32_t instruction_budget,
                                         BCM2835V3DQPUState *qpu,
                                         uint32_t *executed);

#endif /* HW_MISC_BCM2835_V3D_QPU_H */
