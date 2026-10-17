/*
 * Optional host-GPU execution for translated VideoCore IV fragment programs.
 *
 * This work is licensed under the GNU GPL, version 2 or later.
 */

#ifndef HW_MISC_BCM2835_V3D_WGPU_H
#define HW_MISC_BCM2835_V3D_WGPU_H

#include <stdbool.h>
#include <stdint.h>
#include "bcm2835_v3d_qpu.h"

typedef struct BCM2835V3DWGPU BCM2835V3DWGPU;

BCM2835V3DWGPU *bcm2835_v3d_wgpu_new(const char *library);
void bcm2835_v3d_wgpu_free(BCM2835V3DWGPU *wgpu);
bool bcm2835_v3d_wgpu_supports(const uint8_t *code, uint32_t code_size);
bool bcm2835_v3d_wgpu_run(BCM2835V3DWGPU *wgpu,
                          const uint8_t *code, uint32_t code_size,
                          const uint32_t uniforms[4], unsigned count,
                          const uint32_t input_colors[16],
                          const uint32_t software_colors[16],
                          uint32_t colors[16]);
bool bcm2835_v3d_wgpu_filter_texture(
    BCM2835V3DWGPU *wgpu,
    const BCM2835V3DQPUTMUSample samples[BCM2835_V3D_QPU_LANES],
    uint16_t lanes,
    const uint32_t software_result[BCM2835_V3D_QPU_LANES],
    uint32_t result[BCM2835_V3D_QPU_LANES]);

#endif /* HW_MISC_BCM2835_V3D_WGPU_H */
