/*
 * Optional wgpu-native backend for translated VC4 fragment shaders.
 *
 * This work is licensed under the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include <gmodule.h>
#include "bcm2835_v3d_wgpu.h"
#include "third-party/wgpu-native/ffi/webgpu-headers/webgpu.h"

#define WGPU_COLORS 16
#define WGPU_INPUT_OFFSET 5
#define WGPU_SAMPLE_WORDS 6
#define WGPU_OUTPUT_OFFSET (WGPU_INPUT_OFFSET + WGPU_COLORS * WGPU_SAMPLE_WORDS)
#define WGPU_WORDS (WGPU_OUTPUT_OFFSET + WGPU_COLORS)
#define WGPU_BYTES (WGPU_WORDS * sizeof(uint32_t))

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
    "@compute @workgroup_size(16)\n"
    "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "  var color = 0u;\n"
    "  if (data[0] == 0u) {\n"
    "    for (var channel = 0u; channel < 4u; channel += 1u) {\n"
    "      color |= byte_from_float(data[1u + channel]) << (channel * 8u);\n"
    "    }\n"
    "  } else if (data[0] == 1u) {\n"
    "    let alpha = byte_from_float(data[4]);\n"
    "    let inverse_alpha = 255u - alpha;\n"
    "    let destination = data[5u + id.x];\n"
    "    for (var channel = 0u; channel < 4u; channel += 1u) {\n"
    "      let shift = channel * 8u;\n"
    "      let source = byte_from_float(data[1u + channel]);\n"
    "      let dest = (destination >> shift) & 255u;\n"
    "      let src_term = (source * alpha) >> 8u;\n"
    "      let dst_term = (dest * inverse_alpha) >> 8u;\n"
    "      color |= min(255u, src_term + dst_term) << shift;\n"
    "    }\n"
    "  } else {\n"
    "    let input = 5u + id.x * 6u;\n"
    "    let c00 = data[input];\n"
    "    let c10 = data[input + 1u];\n"
    "    let c01 = data[input + 2u];\n"
    "    let c11 = data[input + 3u];\n"
    "    let fx = bitcast<f32>(data[input + 4u]);\n"
    "    let fy = bitcast<f32>(data[input + 5u]);\n"
    "    for (var channel = 0u; channel < 4u; channel += 1u) {\n"
    "      let shift = channel * 8u;\n"
    "      let top = f32((c00 >> shift) & 255u) * (1.0 - fx) +\n"
    "                f32((c10 >> shift) & 255u) * fx;\n"
    "      let bottom = f32((c01 >> shift) & 255u) * (1.0 - fx) +\n"
    "                   f32((c11 >> shift) & 255u) * fx;\n"
    "      let value = floor(top * (1.0 - fy) + bottom * fy + 0.5);\n"
    "      color |= u32(min(value, 255.0)) << shift;\n"
    "    }\n"
    "  }\n"
    "  data[101u + id.x] = color;\n"
    "}\n";

typedef enum BCM2835V3DWGPUShader {
    BCM2835_V3D_WGPU_SHADER_UNSUPPORTED,
    BCM2835_V3D_WGPU_SHADER_UNIFORM_COLOR,
    BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND,
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
    uint64_t word[10];

    if (!code || (size != 6 * sizeof(word[0]) &&
                  size != G_N_ELEMENTS(blend_program) * sizeof(word[0]))) {
        return BCM2835_V3D_WGPU_SHADER_UNSUPPORTED;
    }
    for (unsigned i = 0; i < size / sizeof(word[0]); i++) {
        word[i] = ldq_le_p(code + i * sizeof(uint64_t));
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
    return shader == BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND ?
           "translated alpha-blend QPU shader" :
           "translated uniform-color QPU shader";
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

static bool dispatch_compute(BCM2835V3DWGPU *wgpu,
                             const uint32_t data[WGPU_WORDS], unsigned count,
                             const uint32_t software_colors[WGPU_COLORS],
                             uint32_t colors[WGPU_COLORS],
                             const char *label, uint8_t report_bit)
{
    BCM2835V3DWGPURequest request = { 0 };
    WGPUCommandEncoder encoder;
    WGPUComputePassEncoder pass;
    WGPUCommandBuffer command;
    uint32_t *mapped;

    if (!wgpu || wgpu->failed || !colors || !count || count > WGPU_COLORS) {
        return false;
    }
    wgpu->api.queue_write_buffer(wgpu->queue, wgpu->storage, 0,
                                 data, WGPU_BYTES);
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
    wgpu->api.compute_pass_dispatch(pass, 1, 1, 1);
    wgpu->api.compute_pass_end(pass);
    wgpu->api.compute_pass_release(pass);
    wgpu->api.command_encoder_copy_buffer(encoder, wgpu->storage, 0,
                                           wgpu->readback, 0, WGPU_BYTES);
    command = wgpu->api.command_encoder_finish(encoder, NULL);
    wgpu->api.command_encoder_release(encoder);
    if (!command) {
        return report_failure(wgpu, "command encoding failed");
    }
    wgpu->api.queue_submit(wgpu->queue, 1, &command);
    wgpu->api.command_buffer_release(command);
    wgpu->api.buffer_map_async(
        wgpu->readback, WGPUMapMode_Read, 0, WGPU_BYTES,
        (WGPUBufferMapCallbackInfo){
            .mode = WGPUCallbackMode_AllowSpontaneous,
            .callback = map_callback,
            .userdata1 = &request,
        });
    /* wgpuInstanceWaitAny is unimplemented in this pinned native release. */
    if (!wgpu->api.device_poll(wgpu->device, true, NULL) ||
        request.map_status != WGPUMapAsyncStatus_Success) {
        return report_failure(wgpu, "readback failed");
    }
    mapped = wgpu->api.buffer_get_mapped_range(wgpu->readback, 0,
                                                WGPU_BYTES);
    if (!mapped) {
        return report_failure(wgpu, "mapped readback buffer was null");
    }
    memcpy(colors, mapped + WGPU_OUTPUT_OFFSET, count * sizeof(uint32_t));
    wgpu->api.buffer_unmap(wgpu->readback);
    if (software_colors &&
        memcmp(colors, software_colors, count * sizeof(uint32_t))) {
        if (!wgpu->reported_mismatch) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "bcm2835_v3d: wgpu/software mismatch for %s; keeping software result\n",
                          label);
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
                          const uint32_t uniforms[4], unsigned count,
                          const uint32_t input_colors[16],
                          const uint32_t software_colors[16],
                          uint32_t colors[16])
{
    BCM2835V3DWGPUShader shader = translate_shader(code, code_size);
    uint32_t data[WGPU_WORDS] = { 0 };

    if (!wgpu || wgpu->failed || !uniforms || !colors || !count ||
        count > WGPU_COLORS ||
        shader == BCM2835_V3D_WGPU_SHADER_UNSUPPORTED ||
        (shader == BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND && !input_colors)) {
        return false;
    }
    data[0] = shader == BCM2835_V3D_WGPU_SHADER_ALPHA_BLEND;
    memcpy(data + 1, uniforms, 4 * sizeof(uint32_t));
    if (input_colors) {
        memcpy(data + WGPU_INPUT_OFFSET, input_colors,
               count * sizeof(uint32_t));
    }
    return dispatch_compute(wgpu, data, count, software_colors, colors,
                            shader_name(shader),
                            1U << shader);
}

bool bcm2835_v3d_wgpu_filter_texture(
    BCM2835V3DWGPU *wgpu,
    const BCM2835V3DQPUTMUSample samples[BCM2835_V3D_QPU_LANES],
    uint16_t lanes,
    const uint32_t software_result[BCM2835_V3D_QPU_LANES],
    uint32_t result[BCM2835_V3D_QPU_LANES])
{
    uint32_t data[WGPU_WORDS] = { 0 };
    uint32_t reference[WGPU_COLORS];
    uint32_t filtered[WGPU_COLORS];
    uint8_t lane_index[WGPU_COLORS];
    unsigned count = 0;

    if (!wgpu || wgpu->failed || !samples || !lanes || !result) {
        return false;
    }
    data[0] = 2;
    for (unsigned lane = 0; lane < WGPU_COLORS; lane++) {
        unsigned offset;

        if (!(lanes & (1U << lane))) {
            continue;
        }
        offset = WGPU_INPUT_OFFSET + count * WGPU_SAMPLE_WORDS;
        memcpy(data + offset, samples[lane].texel,
               sizeof(samples[lane].texel));
        data[offset + 4] = samples[lane].fraction_s;
        data[offset + 5] = samples[lane].fraction_t;
        lane_index[count] = lane;
        if (software_result) {
            reference[count] = software_result[lane];
        }
        count++;
    }
    if (!dispatch_compute(wgpu, data, count,
                          software_result ? reference : NULL, filtered,
                          "linear TMU filter", 1U)) {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        result[lane_index[i]] = filtered[i];
    }
    return true;
}
