/*
 * Optional host-GPU execution for translated VideoCore IV fragment programs.
 *
 * This work is licensed under the GNU GPL, version 2 or later.
 */

#ifndef HW_MISC_BCM2835_V3D_WGPU_H
#define HW_MISC_BCM2835_V3D_WGPU_H

#include <stdbool.h>
#include <stdint.h>

typedef struct BCM2835V3DWGPU BCM2835V3DWGPU;

#define BCM2835_V3D_WGPU_MAX_TILE_PIXELS (64 * 64)
/* A 1280x800 frame spans 260 tiles; keep the pixel buffer under 1 Mi pixels. */
#define BCM2835_V3D_WGPU_MAX_BATCH_TILES 1023
#define BCM2835_V3D_WGPU_MAX_BATCH_PIXELS \
    (BCM2835_V3D_WGPU_MAX_TILE_PIXELS * 255)
/* Small exact shader batches avoid Metal's synchronous dispatch/readback cost. */
#define BCM2835_V3D_WGPU_MIN_BATCH_PIXELS 4096

BCM2835V3DWGPU *bcm2835_v3d_wgpu_new(const char *library);
void bcm2835_v3d_wgpu_free(BCM2835V3DWGPU *wgpu);
bool bcm2835_v3d_wgpu_supports(const uint8_t *code, uint32_t code_size);
bool bcm2835_v3d_wgpu_needs_tmu0(const uint8_t *code, uint32_t code_size);
bool bcm2835_v3d_wgpu_needs_color_load(const uint8_t *code,
                                      uint32_t code_size);
bool bcm2835_v3d_wgpu_can_batch(const uint8_t *code, uint32_t code_size);
bool bcm2835_v3d_wgpu_run_software(
    const uint8_t *code, uint32_t code_size, const uint32_t uniforms[6],
    unsigned count,
    const uint32_t input_colors[BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
    const uint32_t tmu0_colors[BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
    uint32_t colors[BCM2835_V3D_WGPU_MAX_BATCH_PIXELS]);
void bcm2835_v3d_wgpu_report_unsupported(BCM2835V3DWGPU *wgpu,
                                         uint32_t code_address,
                                         const uint8_t *code,
                                         uint32_t code_size);
bool bcm2835_v3d_wgpu_run(BCM2835V3DWGPU *wgpu,
                          const uint8_t *code, uint32_t code_size,
                          const uint32_t uniforms[6], unsigned count,
                          const uint32_t input_colors[
                              BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
                          const uint32_t tmu0_colors[
                              BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
                          const uint32_t software_colors[
                              BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
                          uint32_t colors[
                              BCM2835_V3D_WGPU_MAX_BATCH_PIXELS]);

#endif /* HW_MISC_BCM2835_V3D_WGPU_H */
