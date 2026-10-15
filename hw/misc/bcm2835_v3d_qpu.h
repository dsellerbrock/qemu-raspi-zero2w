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
#define BCM2835_V3D_QPU_VPM_WRITE_HISTORY BCM2835_V3D_QPU_VPM_ROWS
#define BCM2835_V3D_QPU_TMUS 2

typedef bool (*BCM2835V3DQPUReadUniform)(void *opaque, uint32_t address,
                                         uint32_t *value);
typedef bool (*BCM2835V3DQPUReadMemory)(void *opaque, uint32_t address,
                                        uint32_t *value);
typedef bool (*BCM2835V3DQPUReadTexture)(void *opaque, uint32_t base,
                                         unsigned memory_format,
                                         unsigned width, unsigned height,
                                         unsigned x, unsigned y,
                                         uint32_t *value);
typedef bool (*BCM2835V3DQPUReadVarying)(
    void *opaque, unsigned index,
    uint32_t partial[BCM2835_V3D_QPU_LANES],
    uint32_t c[BCM2835_V3D_QPU_LANES]);

typedef struct BCM2835V3DQPUTMU {
    uint32_t t[BCM2835_V3D_QPU_LANES];
    uint32_t result[BCM2835_V3D_QPU_LANES];
    uint32_t config0;
    uint16_t t_mask;
    uint16_t instruction_write_mask[4];
    bool has_t;
    bool pending;
} BCM2835V3DQPUTMU;

typedef struct BCM2835V3DQPUState {
    uint32_t rf[2][32][BCM2835_V3D_QPU_LANES];
    uint32_t acc[6][BCM2835_V3D_QPU_LANES];
    uint32_t fragment_coord[2][BCM2835_V3D_QPU_LANES];
    uint32_t fragment_w[BCM2835_V3D_QPU_LANES];
    bool fragment_shader;
    uint32_t varying_index;
    uint32_t varying_c_pending[BCM2835_V3D_QPU_LANES];
    uint32_t vpm[BCM2835_V3D_QPU_VPM_ROWS][BCM2835_V3D_QPU_LANES];
    uint32_t io[64][BCM2835_V3D_QPU_LANES];
    uint32_t sfu_result[3][BCM2835_V3D_QPU_LANES];
    uint64_t sfu_ready_at[3];
    bool sfu_pending[3];
    BCM2835V3DQPUTMU tmu[BCM2835_V3D_QPU_TMUS];
    uint16_t z;
    uint16_t n;
    uint16_t c;
    uint16_t tlb_color_write_mask;
    uint32_t pc;
    uint64_t cycles;
    uint32_t uniform_address;
    uint32_t vpm_read_address;
    uint32_t vpm_read_stride;
    uint32_t vpm_read_remaining;
    bool vpm_read_horizontal;
    uint32_t vpm_write_address;
    uint32_t vpm_write_stride;
    bool vpm_write_horizontal;
    uint32_t vpm_write_pending[BCM2835_V3D_QPU_LANES];
    uint8_t vpm_write_history[BCM2835_V3D_QPU_VPM_WRITE_HISTORY];
    uint8_t vpm_write_history_count;
    uint8_t vpm_write_total;
    bool vpm_read_setup;
    bool vpm_write_setup;
    bool varying_c_pending_valid;
    bool io_fault;
    BCM2835V3DQPUReadUniform read_uniform;
    void *uniform_opaque;
    BCM2835V3DQPUReadMemory read_memory;
    void *memory_opaque;
    BCM2835V3DQPUReadTexture read_texture;
    void *texture_opaque;
    BCM2835V3DQPUReadVarying read_varying;
    void *varying_opaque;
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
