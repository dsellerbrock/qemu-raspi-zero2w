/*
 * Optional wgpu-native backend for translated VC4 fragment shaders.
 *
 * This work is licensed under the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "trace.h"
#include <gmodule.h>
#include "bcm2835_v3d_wgpu.h"
#include "third-party/wgpu-native/ffi/webgpu-headers/webgpu.h"

#define WGPU_COLORS BCM2835_V3D_WGPU_MAX_BATCH_PIXELS
#define WGPU_INPUT_OFFSET 9
#define WGPU_COUNT_OFFSET 8
#define WGPU_OUTPUT_OFFSET (WGPU_INPUT_OFFSET + 2 * WGPU_COLORS)
#define WGPU_WORDS (WGPU_OUTPUT_OFFSET + WGPU_COLORS)
#define WGPU_BYTES (WGPU_WORDS * sizeof(uint32_t))
#define WGPU_WORKGROUP_SIZE 16
#define WGPU_MAX_REPORTED_SHADERS 32
#define WGPU_MAX_SHADER_DUMP_BYTES 512

G_STATIC_ASSERT(DIV_ROUND_UP(BCM2835_V3D_WGPU_MAX_BATCH_PIXELS,
                             WGPU_WORKGROUP_SIZE) <=
                65535);

#define BCM2835_V3D_WGPU_PROCS(_) \
    _(WGPUProcCreateInstance, create_instance, wgpuCreateInstance) \
    _(WGPUProcInstanceRequestAdapter, instance_request_adapter, wgpuInstanceRequestAdapter) \
    _(WGPUProcInstanceRelease, instance_release, wgpuInstanceRelease) \
    _(WGPUProcAdapterGetInfo, adapter_get_info, wgpuAdapterGetInfo) \
    _(WGPUProcAdapterInfoFreeMembers, adapter_info_free_members, wgpuAdapterInfoFreeMembers) \
    _(WGPUProcAdapterRequestDevice, adapter_request_device, wgpuAdapterRequestDevice) \
    _(WGPUProcAdapterRelease, adapter_release, wgpuAdapterRelease) \
    _(WGPUProcDeviceCreateShaderModule, device_create_shader_module, wgpuDeviceCreateShaderModule) \
    _(WGPUProcDeviceCreateBuffer, device_create_buffer, wgpuDeviceCreateBuffer) \
    _(WGPUProcDeviceCreateComputePipeline, device_create_compute_pipeline, wgpuDeviceCreateComputePipeline) \
    _(WGPUProcComputePipelineGetBindGroupLayout, compute_pipeline_get_bind_group_layout, wgpuComputePipelineGetBindGroupLayout) \
    _(WGPUProcDeviceCreateBindGroup, device_create_bind_group, wgpuDeviceCreateBindGroup) \
    _(WGPUProcDeviceCreateCommandEncoder, device_create_command_encoder, wgpuDeviceCreateCommandEncoder) \
    _(WGPUProcDeviceGetQueue, device_get_queue, wgpuDeviceGetQueue) \
    _(WGPUProcShaderModuleRelease, shader_module_release, wgpuShaderModuleRelease) \
    _(WGPUProcComputePipelineRelease, compute_pipeline_release, wgpuComputePipelineRelease) \
    _(WGPUProcBindGroupLayoutRelease, bind_group_layout_release, wgpuBindGroupLayoutRelease) \
    _(WGPUProcBindGroupRelease, bind_group_release, wgpuBindGroupRelease) \
    _(WGPUProcBufferRelease, buffer_release, wgpuBufferRelease) \
    _(WGPUProcBufferMapAsync, buffer_map_async, wgpuBufferMapAsync) \
    _(WGPUProcBufferGetMappedRange, buffer_get_mapped_range, wgpuBufferGetMappedRange) \
    _(WGPUProcBufferUnmap, buffer_unmap, wgpuBufferUnmap) \
    _(WGPUProcCommandEncoderBeginComputePass, command_encoder_begin_compute_pass, wgpuCommandEncoderBeginComputePass) \
    _(WGPUProcComputePassEncoderSetPipeline, compute_pass_set_pipeline, wgpuComputePassEncoderSetPipeline) \
    _(WGPUProcComputePassEncoderSetBindGroup, compute_pass_set_bind_group, wgpuComputePassEncoderSetBindGroup) \
    _(WGPUProcComputePassEncoderDispatchWorkgroups, compute_pass_dispatch, wgpuComputePassEncoderDispatchWorkgroups) \
    _(WGPUProcComputePassEncoderEnd, compute_pass_end, wgpuComputePassEncoderEnd) \
    _(WGPUProcComputePassEncoderRelease, compute_pass_release, wgpuComputePassEncoderRelease) \
    _(WGPUProcCommandEncoderCopyBufferToBuffer, command_encoder_copy_buffer, wgpuCommandEncoderCopyBufferToBuffer) \
    _(WGPUProcCommandEncoderFinish, command_encoder_finish, wgpuCommandEncoderFinish) \
    _(WGPUProcCommandEncoderRelease, command_encoder_release, wgpuCommandEncoderRelease) \
    _(WGPUProcQueueWriteBuffer, queue_write_buffer, wgpuQueueWriteBuffer) \
    _(WGPUProcQueueSubmit, queue_submit, wgpuQueueSubmit) \
    _(WGPUProcQueueRelease, queue_release, wgpuQueueRelease) \
    _(WGPUProcCommandBufferRelease, command_buffer_release, wgpuCommandBufferRelease) \
    _(WGPUProcDeviceRelease, device_release, wgpuDeviceRelease)

typedef struct BCM2835V3DWGPUApi {
    /* wgpu-native extension from ffi/wgpu.h; WGPUSubmissionIndex is uint64_t. */
    WGPUBool (*device_poll)(WGPUDevice device, WGPUBool wait,
                            const uint64_t *submission_index);
#define DECLARE_PROC(type, field, symbol) type field;
    BCM2835_V3D_WGPU_PROCS(DECLARE_PROC)
#undef DECLARE_PROC
} BCM2835V3DWGPUApi;

struct BCM2835V3DWGPU {
    GModule *module;
    BCM2835V3DWGPUApi api;
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUShaderModule shader;
    WGPUComputePipeline pipeline;
    WGPUBindGroupLayout bind_group_layout;
    WGPUBindGroup bind_group;
    WGPUBuffer storage;
    WGPUBuffer readback;
    uint8_t reported_match;
    uint8_t reported_offload;
    bool reported_mismatch;
    bool reported_failure;
    uint32_t reported_shader_address[WGPU_MAX_REPORTED_SHADERS];
    unsigned reported_shader_count;
    bool failed;
};

static const char fragment_shader[] =
    "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
    "fn byte_from_float(bits: u32) -> u32 {\n"
    "  let component = bitcast<f32>(bits);\n"
    "  if (component != component || component <= 0.0) { return 0u; }\n"
    "  if (component >= 1.0) { return 255u; }\n"
    "  return u32(floor(component * 255.0 + 0.5));\n"
    "}\n"
    "@compute @workgroup_size(" stringify(WGPU_WORKGROUP_SIZE) ")\n"
    "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "  let count = data[8u];\n"
    "  if (id.x >= count) { return; }\n"
    "  var color = 0u;\n"
    "  if (data[0] == 1u) {\n"
    "    for (var channel = 0u; channel < 4u; channel += 1u) {\n"
    "      color |= byte_from_float(data[1u + channel]) << (channel * 8u);\n"
    "    }\n"
    "  } else if (data[0] == 2u) {\n"
    "    let alpha = byte_from_float(data[4]);\n"
    "    let inverse_alpha = 255u - alpha;\n"
    "    let destination = data[9u + id.x];\n"
    "    for (var channel = 0u; channel < 4u; channel += 1u) {\n"
    "      let shift = channel * 8u;\n"
    "      let source = byte_from_float(data[1u + channel]);\n"
    "      let dest = (destination >> shift) & 255u;\n"
    "      let src_term = (source * alpha) >> 8u;\n"
    "      let dst_term = (dest * inverse_alpha) >> 8u;\n"
    "      color |= min(255u, src_term + dst_term) << shift;\n"
    "    }\n"
    "  } else if (data[0] == 3u || data[0] == 5u) {\n"
    "    let texel = data[9u + count + id.x];\n"
    "    for (var channel = 0u; channel < 3u; channel += 1u) {\n"
    "      let shift = channel * 8u;\n"
    "      let texel_channel = (texel >> shift) & 255u;\n"
    "      let multiplier = bitcast<f32>(data[5u - channel]);\n"
    "      let value = (f32(texel_channel) / 255.0) * multiplier;\n"
    "      color |= byte_from_float(bitcast<u32>(value)) << shift;\n"
    "    }\n"
    "    color |= 255u << 24u;\n"
    "    if (data[0] == 5u) {\n"
    "      let alpha = (texel >> 24u) & 255u;\n"
    "      let alpha_multiplier = bitcast<f32>(data[6u]);\n"
    "      let scaled_alpha = f32(alpha) / 255.0 * alpha_multiplier;\n"
    "      let inverse_alpha = 255u -\n"
    "          byte_from_float(bitcast<u32>(scaled_alpha));\n"
    "      let destination = data[9u + id.x];\n"
    "      var blended = 0u;\n"
    "      for (var channel = 0u; channel < 4u; channel += 1u) {\n"
    "        let shift = channel * 8u;\n"
    "        let source = (color >> shift) & 255u;\n"
    "        let dest = (destination >> shift) & 255u;\n"
    "        let dest_term = (dest * inverse_alpha) >> 8u;\n"
    "        blended |= min(255u, source + dest_term) << shift;\n"
    "      }\n"
    "      color = blended;\n"
    "    }\n"
    "  } else if (data[0] == 4u) {\n"
    "    let texel = data[9u + count + id.x];\n"
    "    color = texel | (255u << 24u);\n"
    "  }\n"
    "  data[data[7u] + id.x] = color;\n"
    "}\n";

typedef enum BCM2835V3DWGPUShader {
    BCM2835_V3D_WGPU_SHADER_UNSUPPORTED,
    BCM2835_V3D_WGPU_SHADER_UNIFORM_COLOR,
    BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND,
    BCM2835_V3D_WGPU_SHADER_TMU0_COLOR,
    BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE,
    BCM2835_V3D_WGPU_SHADER_TMU0_BLEND,
} BCM2835V3DWGPUShader;

typedef struct BCM2835V3DWGPURequest {
    WGPURequestAdapterStatus adapter_status;
    WGPURequestDeviceStatus device_status;
    WGPUMapAsyncStatus map_status;
    WGPUAdapter adapter;
    WGPUDevice device;
} BCM2835V3DWGPURequest;

static void adapter_callback(WGPURequestAdapterStatus status,
                             WGPUAdapter adapter, WGPUStringView message,
                             void *userdata1, void *userdata2)
{
    BCM2835V3DWGPURequest *request = userdata1;

    (void)message;
    (void)userdata2;
    request->adapter_status = status;
    request->adapter = adapter;
}

static void device_callback(WGPURequestDeviceStatus status, WGPUDevice device,
                            WGPUStringView message,
                            void *userdata1, void *userdata2)
{
    BCM2835V3DWGPURequest *request = userdata1;

    (void)message;
    (void)userdata2;
    request->device_status = status;
    request->device = device;
}

static void map_callback(WGPUMapAsyncStatus status, WGPUStringView message,
                         void *userdata1, void *userdata2)
{
    BCM2835V3DWGPURequest *request = userdata1;

    (void)message;
    (void)userdata2;
    request->map_status = status;
}

static const char *backend_name(WGPUBackendType backend)
{
    switch (backend) {
    case WGPUBackendType_Metal:
        return "Metal";
    case WGPUBackendType_Vulkan:
        return "Vulkan";
    case WGPUBackendType_D3D12:
        return "D3D12";
    case WGPUBackendType_OpenGL:
        return "OpenGL";
    case WGPUBackendType_OpenGLES:
        return "OpenGL ES";
    default:
        return "other";
    }
}

static bool load_api(BCM2835V3DWGPU *wgpu)
{
#define LOAD_PROC(type, field, name) \
    do { \
        gpointer symbol; \
        if (!g_module_symbol(wgpu->module, #name, &symbol) || \
            sizeof(symbol) != sizeof(wgpu->api.field)) { \
            return false; \
        } \
        memcpy(&wgpu->api.field, &symbol, sizeof(symbol)); \
    } while (0);
    BCM2835_V3D_WGPU_PROCS(LOAD_PROC)
#undef LOAD_PROC

    gpointer symbol;

    if (!g_module_symbol(wgpu->module, "wgpuDevicePoll", &symbol) ||
        sizeof(symbol) != sizeof(wgpu->api.device_poll)) {
        return false;
    }
    memcpy(&wgpu->api.device_poll, &symbol, sizeof(symbol));
    return true;
}

static bool create_pipeline(BCM2835V3DWGPU *wgpu)
{
    WGPUInstanceDescriptor instance_desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    WGPUShaderModuleDescriptor shader_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    WGPUComputePipelineDescriptor pipeline_desc = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    BCM2835V3DWGPURequest request = { 0 };

    wgpu->instance = wgpu->api.create_instance(&instance_desc);
    if (!wgpu->instance) {
        return false;
    }

    /* wgpu-native v29 completes adapter and device requests synchronously. */
    wgpu->api.instance_request_adapter(
        wgpu->instance, NULL,
        (WGPURequestAdapterCallbackInfo){
            .mode = WGPUCallbackMode_AllowSpontaneous,
            .callback = adapter_callback,
            .userdata1 = &request,
        });
    if (request.adapter_status != WGPURequestAdapterStatus_Success ||
        !request.adapter) {
        return false;
    }
    wgpu->adapter = request.adapter;

    WGPUAdapterInfo adapter_info = WGPU_ADAPTER_INFO_INIT;

    if (wgpu->api.adapter_get_info(wgpu->adapter, &adapter_info) ==
        WGPUStatus_Success) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: wgpu-native backend=%s adapterType=%u\n",
                      backend_name(adapter_info.backendType),
                      adapter_info.adapterType);
        wgpu->api.adapter_info_free_members(adapter_info);
    }

    wgpu->api.adapter_request_device(
        wgpu->adapter, NULL,
        (WGPURequestDeviceCallbackInfo){
            .mode = WGPUCallbackMode_AllowSpontaneous,
            .callback = device_callback,
            .userdata1 = &request,
        });
    if (request.device_status != WGPURequestDeviceStatus_Success ||
        !request.device) {
        return false;
    }
    wgpu->device = request.device;
    wgpu->queue = wgpu->api.device_get_queue(wgpu->device);
    if (!wgpu->queue) {
        return false;
    }

    wgsl.code = (WGPUStringView){
        .data = fragment_shader,
        .length = sizeof(fragment_shader) - 1,
    };
    shader_desc.nextInChain = &wgsl.chain;
    wgpu->shader = wgpu->api.device_create_shader_module(
        wgpu->device, &shader_desc);
    if (!wgpu->shader) {
        return false;
    }
    pipeline_desc.compute.module = wgpu->shader;
    pipeline_desc.compute.entryPoint = (WGPUStringView){
        .data = "main",
        .length = WGPU_STRLEN,
    };
    wgpu->pipeline = wgpu->api.device_create_compute_pipeline(
        wgpu->device, &pipeline_desc);
    if (!wgpu->pipeline) {
        return false;
    }
    wgpu->bind_group_layout = wgpu->api.compute_pipeline_get_bind_group_layout(
        wgpu->pipeline, 0);
    if (!wgpu->bind_group_layout) {
        return false;
    }

    WGPUBufferDescriptor storage_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBufferDescriptor readback_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    WGPUBindGroupEntry entry = { 0 };
    WGPUBindGroupDescriptor bind_group_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;

    storage_desc.usage = WGPUBufferUsage_Storage |
                         WGPUBufferUsage_CopyDst |
                         WGPUBufferUsage_CopySrc;
    storage_desc.size = WGPU_BYTES;
    readback_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    readback_desc.size = WGPU_BYTES;
    wgpu->storage = wgpu->api.device_create_buffer(wgpu->device,
                                                   &storage_desc);
    wgpu->readback = wgpu->api.device_create_buffer(wgpu->device,
                                                     &readback_desc);
    if (!wgpu->storage || !wgpu->readback) {
        return false;
    }

    entry.binding = 0;
    entry.buffer = wgpu->storage;
    entry.size = WGPU_BYTES;
    bind_group_desc.layout = wgpu->bind_group_layout;
    bind_group_desc.entryCount = 1;
    bind_group_desc.entries = &entry;
    wgpu->bind_group = wgpu->api.device_create_bind_group(
        wgpu->device, &bind_group_desc);
    return wgpu->bind_group != NULL;
}

BCM2835V3DWGPU *bcm2835_v3d_wgpu_new(const char *library)
{
    BCM2835V3DWGPU *wgpu = g_new0(BCM2835V3DWGPU, 1);

    wgpu->module = g_module_open(library, G_MODULE_BIND_LAZY);
    if (!wgpu->module || !load_api(wgpu) || !create_pipeline(wgpu)) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: wgpu-native unavailable; using software renderer (%s)\n",
                      wgpu->module ? "API or device initialization failed" :
                                     g_module_error());
        bcm2835_v3d_wgpu_free(wgpu);
        return NULL;
    }
    qemu_log_mask(LOG_UNIMP,
                  "bcm2835_v3d: wgpu-native compute backend ready\n");
    return wgpu;
}

void bcm2835_v3d_wgpu_free(BCM2835V3DWGPU *wgpu)
{
    if (!wgpu) {
        return;
    }
#define RELEASE(field, method) \
    do { \
        if (wgpu->field && wgpu->api.method) { \
            wgpu->api.method(wgpu->field); \
        } \
    } while (0)
    RELEASE(bind_group, bind_group_release);
    RELEASE(bind_group_layout, bind_group_layout_release);
    RELEASE(pipeline, compute_pipeline_release);
    RELEASE(shader, shader_module_release);
    RELEASE(storage, buffer_release);
    RELEASE(readback, buffer_release);
    RELEASE(queue, queue_release);
    RELEASE(device, device_release);
    RELEASE(adapter, adapter_release);
    RELEASE(instance, instance_release);
#undef RELEASE
    if (wgpu->module) {
        g_module_close(wgpu->module);
    }
    g_free(wgpu);
}

static BCM2835V3DWGPUShader translate_shader(const uint8_t *code,
                                              uint32_t size)
{
    /* shortcut: match exact Mesa probe shaders; add translations for others. */
    static const uint64_t blend_program[] = {
        UINT64_C(0x114049e080827036), UINT64_C(0x115049e080827036),
        UINT64_C(0x116049e080827036), UINT64_C(0x113049e180827036),
        UINT64_C(0x117248a097827276), UINT64_C(0x100049e3609e7001),
        UINT64_C(0x800009e7009e7000), UINT64_C(0x100049e0609e7022),
        UINT64_C(0x100059eec09e7018), UINT64_C(0x300009e7009e7000),
    };
    static const uint64_t tmu0_opaque_program[] = {
        UINT64_C(0x100049e0203e303e), UINT64_C(0x100248e1213e317e),
        UINT64_C(0x600208a7019e7340), UINT64_C(0x10021e67159e7480),
        UINT64_C(0x10021e27159e76c0), UINT64_C(0xa00009e7009e7000),
        UINT64_C(0x190208e7049e7900), UINT64_C(0x1b424822849e791b),
        UINT64_C(0x1d524862849e7900), UINT64_C(0x116049e2809e7009),
        UINT64_C(0xd17049e2809e003f), UINT64_C(0x10020ba7159e7480),
        UINT64_C(0x300009e7009e7000),
    };
    static const uint64_t tmu0_color_program[] = {
        UINT64_C(0x100049e0203e303e), UINT64_C(0x100248e1213e317e),
        UINT64_C(0x600208a7019e7340), UINT64_C(0x10021e67159e7480),
        UINT64_C(0x10021e27159e76c0), UINT64_C(0xa00009e7009e7000),
        UINT64_C(0x1d020867049e7900), UINT64_C(0x1b6248212482790e),
        UINT64_C(0x195248e124827906), UINT64_C(0x114049e12082701e),
        UINT64_C(0xd17049e1809e003f), UINT64_C(0x10020ba7159e7240),
        UINT64_C(0x300009e7009e7000),
    };
    static const uint64_t tmu0_blend_program[] = {
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
    uint64_t word[18];

    if (!code || (size != 6 * sizeof(word[0]) &&
                  size != G_N_ELEMENTS(blend_program) * sizeof(word[0]) &&
                  size != G_N_ELEMENTS(tmu0_opaque_program) * sizeof(word[0]) &&
                  size != G_N_ELEMENTS(tmu0_blend_program) * sizeof(word[0]))) {
        return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
    }
    for (unsigned i = 0; i < size / sizeof(word[0]); i++) {
        word[i] = ldq_le_p(code + i * sizeof(uint64_t));
    }
    if (size == G_N_ELEMENTS(tmu0_opaque_program) * sizeof(word[0])) {
        bool matches_opaque = true;

        for (unsigned i = 0; i < G_N_ELEMENTS(tmu0_opaque_program); i++) {
            if (word[i] != tmu0_opaque_program[i]) {
                matches_opaque = false;
                break;
            }
        }
        if (matches_opaque) {
            return BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE;
        }
        for (unsigned i = 0; i < G_N_ELEMENTS(tmu0_color_program); i++) {
            if (word[i] != tmu0_color_program[i]) {
                return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
            }
        }
        return BCM2835_V3D_WGPU_SHADER_TMU0_COLOR;
    }
    if (size == G_N_ELEMENTS(tmu0_blend_program) * sizeof(word[0])) {
        for (unsigned i = 0; i < G_N_ELEMENTS(tmu0_blend_program); i++) {
            if (word[i] != tmu0_blend_program[i]) {
                return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
            }
        }
        return BCM2835_V3D_WGPU_SHADER_TMU0_BLEND;
    }
    if (size == G_N_ELEMENTS(blend_program) * sizeof(word[0])) {
        for (unsigned i = 0; i < G_N_ELEMENTS(blend_program); i++) {
            if (word[i] != blend_program[i]) {
                return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
            }
        }
        return BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND;
    }

    for (unsigned i = 0; i < 4; i++) {
        uint64_t inst = word[i];

        if (((inst >> 60) & 0xf) != 1 || /* no signal */
            ((inst >> 56) & 1) != 1 ||  /* packed MUL result */
            ((inst >> 52) & 0xf) != i + 4 ||
            ((inst >> 49) & 7) != 0 ||  /* ADD never */
            ((inst >> 46) & 7) != 1 ||  /* MUL always */
            ((inst >> 38) & 0x3f) != 39 ||
            ((inst >> 32) & 0x3f) != 32 || /* ACC0 */
            ((inst >> 29) & 7) != 4 ||  /* V8MIN */
            ((inst >> 24) & 0x1f) != 0 ||
            ((inst >> 18) & 0x3f) != 32 || /* uniform */
            ((inst >> 12) & 0x3f) != 39 ||
            ((inst >> 9) & 7) != 0 ||
            ((inst >> 6) & 7) != 0 ||
            ((inst >> 3) & 7) != 6 ||
            (inst & 7) != 6) {
            return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
        }
    }

    if (word[4] != UINT64_C(0x10020ba7159e7000) ||
        word[5] != UINT64_C(0x300009e7009e7000)) {
        return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
    }
    return BCM2835_V3D_WGPU_SHADER_UNIFORM_COLOR;
}

static const char *shader_name(BCM2835V3DWGPUShader shader)
{
    switch (shader) {
    case BCM2835_V3D_WGPU_SHADER_UNIFORM_COLOR:
        return "translated uniform-color QPU shader";
    case BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND:
        return "translated alpha-blend QPU shader";
    case BCM2835_V3D_WGPU_SHADER_TMU0_COLOR:
        return "translated LOAD_TMU0 color QPU shader";
    case BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE:
        return "translated LOAD_TMU0 QPU shader";
    case BCM2835_V3D_WGPU_SHADER_TMU0_BLEND:
        return "translated LOAD_TMU0/COLOR_LOAD QPU shader";
    default:
        return "unsupported QPU shader";
    }
}

static bool report_failure(BCM2835V3DWGPU *wgpu, const char *reason)
{
    if (!wgpu->reported_failure) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: wgpu %s; using software result\n",
                      reason);
        wgpu->reported_failure = true;
    }
    wgpu->failed = true;
    return false;
}

bool bcm2835_v3d_wgpu_supports(const uint8_t *code, uint32_t code_size)
{
    return translate_shader(code, code_size) !=
           BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
}

bool bcm2835_v3d_wgpu_needs_tmu0(const uint8_t *code, uint32_t code_size)
{
    BCM2835V3DWGPUShader shader = translate_shader(code, code_size);

    return shader == BCM2835_V3D_WGPU_SHADER_TMU0_COLOR ||
           shader == BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE ||
           shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND;
}

bool bcm2835_v3d_wgpu_needs_color_load(const uint8_t *code,
                                      uint32_t code_size)
{
    BCM2835V3DWGPUShader shader = translate_shader(code, code_size);

    return shader == BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND ||
           shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND;
}

bool bcm2835_v3d_wgpu_can_batch(const uint8_t *code, uint32_t code_size)
{
    BCM2835V3DWGPUShader shader = translate_shader(code, code_size);

    return shader == BCM2835_V3D_WGPU_SHADER_TMU0_COLOR ||
           shader == BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE ||
           shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND;
}

static float wgpu_float_from_bits(uint32_t bits)
{
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t wgpu_byte_from_float(float value)
{
    if (value != value || value <= 0.0f) {
        return 0;
    }
    if (value >= 1.0f) {
        return 255;
    }
    return floorf(value * 255.0f + 0.5f);
}

bool bcm2835_v3d_wgpu_run_software(
    const uint8_t *code, uint32_t code_size, const uint32_t uniforms[6],
    unsigned count,
    const uint32_t input_colors[BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
    const uint32_t tmu0_colors[BCM2835_V3D_WGPU_MAX_BATCH_PIXELS],
    uint32_t colors[BCM2835_V3D_WGPU_MAX_BATCH_PIXELS])
{
    BCM2835V3DWGPUShader shader = translate_shader(code, code_size);

    if (!uniforms || !count || count > BCM2835_V3D_WGPU_MAX_BATCH_PIXELS ||
        !colors || !tmu0_colors ||
        (shader != BCM2835_V3D_WGPU_SHADER_TMU0_COLOR &&
         shader != BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE &&
         shader != BCM2835_V3D_WGPU_SHADER_TMU0_BLEND) ||
        (shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND && !input_colors)) {
        return false;
    }

    for (unsigned i = 0; i < count; i++) {
        uint32_t texel = tmu0_colors[i];
        uint32_t color = texel | UINT32_C(0xff000000);

        if (shader != BCM2835_V3D_WGPU_SHADER_TMU0_OPAQUE) {
            color = UINT32_C(0xff000000);
            for (unsigned channel = 0; channel < 3; channel++) {
                unsigned shift = channel * 8;
                float multiplier = wgpu_float_from_bits(
                    uniforms[4 - channel]);
                float value = ((texel >> shift) & 0xff) / 255.0f *
                              multiplier;

                color |= wgpu_byte_from_float(value) << shift;
            }
        }
        if (shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND) {
            float alpha = ((texel >> 24) & 0xff) / 255.0f *
                          wgpu_float_from_bits(uniforms[5]);
            uint32_t inverse_alpha = 255 - wgpu_byte_from_float(alpha);
            uint32_t destination = input_colors[i];
            uint32_t blended = 0;

            for (unsigned channel = 0; channel < 4; channel++) {
                unsigned shift = channel * 8;
                uint32_t source = (color >> shift) & 0xff;
                uint32_t dest = (destination >> shift) & 0xff;
                uint32_t dest_term = (dest * inverse_alpha) >> 8;

                blended |= MIN(255, source + dest_term) << shift;
            }
            color = blended;
        }
        colors[i] = color;
    }
    return true;
}

void bcm2835_v3d_wgpu_report_unsupported(BCM2835V3DWGPU *wgpu,
                                         uint32_t code_address,
                                         const uint8_t *code,
                                         uint32_t code_size)
{
    char bytecode[WGPU_MAX_SHADER_DUMP_BYTES * 2 + 1];
    unsigned dump_size;

    if (!wgpu || !code || !code_size || !qemu_loglevel_mask(LOG_UNIMP)) {
        return;
    }
    for (unsigned i = 0; i < wgpu->reported_shader_count; i++) {
        if (wgpu->reported_shader_address[i] == code_address) {
            return;
        }
    }
    /* shortcut: first 32 program addresses, raise the cap if a desktop uses more. */
    if (wgpu->reported_shader_count ==
        G_N_ELEMENTS(wgpu->reported_shader_address)) {
        return;
    }
    wgpu->reported_shader_address[wgpu->reported_shader_count++] =
        code_address;
    dump_size = MIN(code_size, WGPU_MAX_SHADER_DUMP_BYTES);
    for (unsigned i = 0; i < dump_size; i++) {
        g_snprintf(bytecode + i * 2, 3, "%02x", code[i]);
    }
    bytecode[dump_size * 2] = '\0';
    qemu_log_mask(LOG_UNIMP,
                  "bcm2835_v3d: unsupported WGPU fragment shader addr=0x%08x size=%u bytecode%s=%s\n",
                  code_address, code_size,
                  dump_size < code_size ? " (truncated)" : "", bytecode);
}

static bool dispatch_compute(BCM2835V3DWGPU *wgpu,
                             const uint32_t data[WGPU_WORDS], unsigned count,
                             unsigned input_words,
                             const uint32_t software_colors[WGPU_COLORS],
                             uint32_t colors[WGPU_COLORS],
                             const char *label, uint8_t report_bit)
{
    BCM2835V3DWGPURequest request = { 0 };
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer command;
    uint32_t *mapped;
    size_t input_bytes = (WGPU_INPUT_OFFSET + input_words) * sizeof(*data);
    size_t output_bytes = count * sizeof(*colors);
    bool trace_dispatch;
    WGPUBool poll_succeeded;
    int64_t start_us;
    int64_t poll_start_us;

    if (!wgpu || wgpu->failed || !colors || !count || count > WGPU_COLORS ||
        input_words > WGPU_WORDS - WGPU_INPUT_OFFSET) {
        return false;
    }
    trace_dispatch =
        trace_event_get_state(TRACE_BCM2835_V3D_WGPU_DISPATCH);
    start_us = trace_dispatch ? g_get_monotonic_time() : 0;
    wgpu->api.queue_write_buffer(wgpu->queue, wgpu->storage, 0,
                                 data, input_bytes);
    encoder = wgpu->api.device_create_command_encoder(wgpu->device, NULL);
    if (!encoder) {
        return report_failure(wgpu, "command encoder creation failed");
    }
    pass = wgpu->api.command_encoder_begin_compute_pass(encoder, NULL);
    if (!pass) {
        wgpu->api.command_encoder_release(encoder);
        return report_failure(wgpu, "compute pass creation failed");
    }
    wgpu->api.compute_pass_set_pipeline(pass, wgpu->pipeline);
    wgpu->api.compute_pass_set_bind_group(pass, 0, wgpu->bind_group, 0, NULL);
    wgpu->api.compute_pass_dispatch(pass,
                                    DIV_ROUND_UP(count, WGPU_WORKGROUP_SIZE),
                                    1, 1);
    wgpu->api.compute_pass_end(pass);
    wgpu->api.compute_pass_release(pass);
    wgpu->api.command_encoder_copy_buffer(
        encoder, wgpu->storage, WGPU_OUTPUT_OFFSET * sizeof(uint32_t),
        wgpu->readback, 0, output_bytes);
    command = wgpu->api.command_encoder_finish(encoder, NULL);
    wgpu->api.command_encoder_release(encoder);
    if (!command) {
        return report_failure(wgpu, "command encoding failed");
    }
    wgpu->api.queue_submit(wgpu->queue, 1, &command);
    wgpu->api.command_buffer_release(command);
    wgpu->api.buffer_map_async(
        wgpu->readback, WGPUMapMode_Read, 0, output_bytes,
        (WGPUBufferMapCallbackInfo){
            .mode = WGPUCallbackMode_AllowSpontaneous,
            .callback = map_callback,
            .userdata1 = &request,
        });
    /* wgpuInstanceWaitAny is unimplemented in this pinned native release. */
    poll_start_us = trace_dispatch ? g_get_monotonic_time() : 0;
    poll_succeeded = wgpu->api.device_poll(wgpu->device, true, NULL);
    if (trace_dispatch) {
        trace_bcm2835_v3d_wgpu_poll(count,
                                    g_get_monotonic_time() - poll_start_us);
    }
    if (!poll_succeeded ||
        request.map_status != WGPUMapAsyncStatus_Success) {
        return report_failure(wgpu, "readback failed");
    }
    mapped = wgpu->api.buffer_get_mapped_range(wgpu->readback, 0,
                                                output_bytes);
    if (!mapped) {
        return report_failure(wgpu, "mapped readback buffer was null");
    }
    memcpy(colors, mapped, output_bytes);
    wgpu->api.buffer_unmap(wgpu->readback);
    if (trace_dispatch) {
        trace_bcm2835_v3d_wgpu_dispatch(label, count,
                                        g_get_monotonic_time() - start_us);
    }
    if (software_colors &&
        memcmp(colors, software_colors, count * sizeof(uint32_t))) {
        unsigned mismatch = 0;

        while (colors[mismatch] == software_colors[mismatch]) {
            mismatch++;
        }
        if (!wgpu->reported_mismatch) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: wgpu/software mismatch for %s at %u/%u: wgpu=0x%08x software=0x%08x; keeping software result\n",
                          label, mismatch, count, colors[mismatch],
                          software_colors[mismatch]);
            wgpu->reported_mismatch = true;
        }
        wgpu->failed = true;
        return false;
    }
    if (software_colors && !(wgpu->reported_match & report_bit)) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: %s matched software on wgpu-native\n",
                      label);
        wgpu->reported_match |= report_bit;
    } else if (!software_colors &&
               !(wgpu->reported_offload & report_bit)) {
        qemu_log_mask(LOG_UNIMP,
                      "bcm2835_v3d: %s offloaded to wgpu-native\n", label);
        wgpu->reported_offload |= report_bit;
    }
    return true;
}

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
                              BCM2835_V3D_WGPU_MAX_BATCH_PIXELS])
{
    BCM2835V3DWGPUShader shader = translate_shader(code, code_size);
    g_autofree uint32_t *data;

    if (!wgpu || wgpu->failed || !uniforms || !colors || !count ||
        count > WGPU_COLORS ||
        shader == BCM2835_V3D_WGPU_SHADER_UNSUPPORTED ||
        ((shader == BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND ||
          shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND) && !input_colors) ||
        (bcm2835_v3d_wgpu_needs_tmu0(code, code_size) && !tmu0_colors)) {
        return false;
    }
    data = g_new0(uint32_t, WGPU_INPUT_OFFSET + 2 * count);
    data[0] = shader;
    memcpy(data + 1, uniforms, 6 * sizeof(uint32_t));
    data[7] = WGPU_OUTPUT_OFFSET;
    data[WGPU_COUNT_OFFSET] = count;
    unsigned input_words = 0;
    if (input_colors) {
        memcpy(data + WGPU_INPUT_OFFSET, input_colors,
               count * sizeof(uint32_t));
        if (shader == BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND ||
            shader == BCM2835_V3D_WGPU_SHADER_TMU0_BLEND) {
            input_words += count;
        }
    }
    if (tmu0_colors && bcm2835_v3d_wgpu_needs_tmu0(code, code_size)) {
        memcpy(data + WGPU_INPUT_OFFSET + count, tmu0_colors,
               count * sizeof(uint32_t));
        input_words = count * 2;
    }
    return dispatch_compute(wgpu, data, count, input_words,
                            software_colors, colors,
                            shader_name(shader), 1U << shader);
}
