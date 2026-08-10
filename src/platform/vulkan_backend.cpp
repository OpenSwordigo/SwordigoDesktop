// Vulkan backend for Swordigo Desktop — GLES 1.x fixed-function pipeline emulator
// Implementation file
//
// Uses volk for dynamic Vulkan function loading and VMA for memory allocation.
// Translates GLES 1.x fixed-function calls to Vulkan with:
//   - Software matrix stack (modelview + projection)
//   - Uber-shader with specialization constants for feature toggles
//   - Per-frame staging buffer for client-side vertex data
//   - Pipeline caching by state hash

#define VK_NO_PROTOTYPES
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "vulkan_backend.h"
#include "fbo_scaler.h"
#include "swordfare_gui.h"

// volk — dynamic Vulkan function loader (single compilation unit)
#define VOLK_IMPLEMENTATION
#include "volk.h"

// VMA — Vulkan Memory Allocator (single compilation unit)
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

#include "vulkan_shaders_spirv.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cassert>

// ============================================================================
// Vulkan Diagnostics Helpers
// ============================================================================

static const char* vk_result_name(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        case VK_ERROR_FRAGMENTATION: return "VK_ERROR_FRAGMENTATION";
        case VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS: return "VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_INCOMPATIBLE_DISPLAY_KHR: return "VK_ERROR_INCOMPATIBLE_DISPLAY_KHR";
        case VK_ERROR_VALIDATION_FAILED_EXT: return "VK_ERROR_VALIDATION_FAILED_EXT";
        case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
        case VK_ERROR_NOT_PERMITTED_EXT: return "VK_ERROR_NOT_PERMITTED_EXT";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        default: return "UNKNOWN_VK_ERROR";
    }
}

#define VK_CHECK(expr, label) do { \
    VkResult r = (expr); \
    std::cerr << "[Vulkan] " << label << " ... " << (r == VK_SUCCESS ? "OK" : vk_result_name(r)) << std::endl; \
    if (r != VK_SUCCESS) return false; \
} while(0)

// ============================================================================
// Helper: matrix math (column-major, OpenGL convention)
// ============================================================================

static void mat4_identity(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float out[16], const float a[16], const float b[16]) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            tmp[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, tmp, 16 * sizeof(float));
}

// Computes the transpose of the inverse of the upper 3x3 of a 4x4 matrix
static void calc_normal_matrix(float* out_mat4, const float* in_mat4) {
    // Extract 3x3
    float m00 = in_mat4[0], m01 = in_mat4[4], m02 = in_mat4[8];
    float m10 = in_mat4[1], m11 = in_mat4[5], m12 = in_mat4[9];
    float m20 = in_mat4[2], m21 = in_mat4[6], m22 = in_mat4[10];

    // Compute determinant
    float det = m00 * (m11 * m22 - m12 * m21) -
                m01 * (m10 * m22 - m12 * m20) +
                m02 * (m10 * m21 - m11 * m20);

    float invDet = 1.0f;
    if (det != 0.0f) invDet = 1.0f / det;

    for (int i = 0; i < 16; i++) out_mat4[i] = 0.0f;
    out_mat4[15] = 1.0f;

    out_mat4[0] = (m11 * m22 - m12 * m21) * invDet; // 0,0
    out_mat4[1] = (m12 * m20 - m10 * m22) * invDet; // 1,0 -> 0,1
    out_mat4[2] = (m10 * m21 - m11 * m20) * invDet; // 2,0 -> 0,2

    out_mat4[4] = (m02 * m21 - m01 * m22) * invDet; // 0,1 -> 1,0
    out_mat4[5] = (m00 * m22 - m02 * m20) * invDet; // 1,1
    out_mat4[6] = (m01 * m20 - m00 * m21) * invDet; // 2,1 -> 1,2

    out_mat4[8] = (m01 * m12 - m02 * m11) * invDet; // 0,2 -> 2,0
    out_mat4[9] = (m02 * m10 - m00 * m12) * invDet; // 1,2 -> 2,1
    out_mat4[10]= (m00 * m11 - m01 * m10) * invDet; // 2,2
}

static void mat4_ortho(float m[16], float l, float r, float b, float t, float n, float f) {
    mat4_identity(m);
    m[0]  =  2.0f / (r - l);
    m[5]  =  2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
}

static void mat4_frustum(float m[16], float l, float r, float b, float t, float n, float f) {
    memset(m, 0, 16 * sizeof(float));
    m[0]  = (2.0f * n) / (r - l);
    m[5]  = (2.0f * n) / (t - b);
    m[8]  = (r + l) / (r - l);
    m[9]  = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -(2.0f * f * n) / (f - n);
}

static void mat4_translate(float m[16], float x, float y, float z) {
    float t[16];
    mat4_identity(t);
    t[12] = x; t[13] = y; t[14] = z;
    float tmp[16];
    mat4_multiply(tmp, m, t);
    memcpy(m, tmp, 16 * sizeof(float));
}

static void mat4_scale(float m[16], float x, float y, float z) {
    float s[16];
    mat4_identity(s);
    s[0] = x; s[5] = y; s[10] = z;
    float tmp[16];
    mat4_multiply(tmp, m, s);
    memcpy(m, tmp, 16 * sizeof(float));
}

static void mat4_rotate(float m[16], float angle_deg, float ax, float ay, float az) {
    float rad = angle_deg * 3.14159265358979f / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    float len = sqrtf(ax*ax + ay*ay + az*az);
    if (len < 0.0001f) return;
    ax /= len; ay /= len; az /= len;
    float r[16];
    r[0] = ax*ax*(1-c)+c;     r[4] = ax*ay*(1-c)-az*s;  r[8]  = ax*az*(1-c)+ay*s;  r[12] = 0;
    r[1] = ay*ax*(1-c)+az*s;  r[5] = ay*ay*(1-c)+c;     r[9]  = ay*az*(1-c)-ax*s;  r[13] = 0;
    r[2] = az*ax*(1-c)-ay*s;  r[6] = az*ay*(1-c)+ax*s;  r[10] = az*az*(1-c)+c;     r[14] = 0;
    r[3] = 0;                  r[7] = 0;                  r[11] = 0;                  r[15] = 1;
    float tmp[16];
    mat4_multiply(tmp, m, r);
    memcpy(m, tmp, 16 * sizeof(float));
}

// ============================================================================
// Push constant layout (must match shader, ≤128 bytes)
// ============================================================================

struct PushConstantBlock {
    float mvp[16];          // 64 bytes
    float current_color[4]; // 16 bytes
    float fog_params[4];    // 16 bytes  (start, end, density, mode)
    float fog_color[4];     // 16 bytes
    float alpha_ref;        // 4 bytes
    float pad[3];           // 12 bytes padding
    // Total: 128 bytes exactly
};
static_assert(sizeof(PushConstantBlock) == 128, "Push constants must be exactly 128 bytes");

struct SceneData {
    float modelview[16];
    float normal_matrix[16];
    float light_ambient[4];
    float light_diffuse[4];
    float light_dir[4];
    float mat_ambient[4];
    float mat_diffuse[4];
    float mat_emission[4];
    float tex_env_color[4];
    float alpha_ref;
    float _pad[3];
};
static_assert(sizeof(SceneData) == 256, "SceneData must be exactly 256 bytes");

// ============================================================================
// Staging buffer constants
// ============================================================================

static constexpr size_t STAGING_BUFFER_SIZE = 16 * 1024 * 1024; // 16 MB per frame
// Constants

// ============================================================================
// VulkanBackend::init
// ============================================================================

bool VulkanBackend::init(SDL_Window* window, int gw, int gh) {
    game_w_ = gw;
    game_h_ = gh;

    // --- 1. Initialize volk ---
    VK_CHECK(volkInitialize(), "volk initialize");

    // --- 2. Create VkInstance ---
    {
        VkApplicationInfo app_info = {};
        app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app_info.pApplicationName = "Swordigo Desktop";
        app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.pEngineName = "SwordigoVK";
        app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app_info.apiVersion = VK_API_VERSION_1_0;

        // Get required extensions from SDL
        Uint32 ext_count = 0;
        const char * const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&ext_count);
        std::vector<const char*> extensions(sdl_exts, sdl_exts + ext_count);

        // Enable validation in debug builds
#ifndef NDEBUG
        const char* validation_layer = "VK_LAYER_KHRONOS_validation";
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
        bool has_validation = false;
        for (auto& l : layers) {
            if (strcmp(l.layerName, validation_layer) == 0) {
                has_validation = true;
                break;
            }
        }
#endif

        VkInstanceCreateInfo ci = {};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app_info;
        ci.enabledExtensionCount = (uint32_t)extensions.size();
        ci.ppEnabledExtensionNames = extensions.data();
#ifndef NDEBUG
        if (has_validation) {
            ci.enabledLayerCount = 1;
            ci.ppEnabledLayerNames = &validation_layer;
            std::cout << "[VK] Validation layers enabled" << std::endl;
        }
#endif

        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_), "Instance creation");
        volkLoadInstance(instance_);
    }

    // --- 3. Create surface ---
    if (!SDL_Vulkan_CreateSurface(window, instance_, NULL, &surface_)) {
        std::cerr << "[VK] Failed to create surface: " << SDL_GetError() << std::endl;
        return false;
    }

    // --- 4. Pick physical device ---
    {
        uint32_t dev_count = 0;
        vkEnumeratePhysicalDevices(instance_, &dev_count, nullptr);
        if (dev_count == 0) {
            std::cerr << "[VK] No Vulkan-capable GPUs found" << std::endl;
            return false;
        }
        std::vector<VkPhysicalDevice> devices(dev_count);
        vkEnumeratePhysicalDevices(instance_, &dev_count, devices.data());

        // Prefer discrete GPU, fallback to first
        physical_device_ = devices[0];
        for (auto& d : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(d, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical_device_ = d;
                break;
            }
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device_, &props);
        min_ubo_alignment_ = (uint32_t)props.limits.minUniformBufferOffsetAlignment;
        std::cout << "[VK] Using physical device: " << props.deviceName << " (UBO align: " << min_ubo_alignment_ << ")" << std::endl;
    }

    // --- 5. Find queue families ---
    {
        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, nullptr);
        std::vector<VkQueueFamilyProperties> qf_props(qf_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device_, &qf_count, qf_props.data());

        graphics_family_ = UINT32_MAX;
        present_family_ = UINT32_MAX;
        for (uint32_t i = 0; i < qf_count; i++) {
            if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                graphics_family_ = i;
            VkBool32 present_support = VK_FALSE;
            // CHECK instead of raw call
            VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(physical_device_, i, surface_, &present_support), "Query surface support");
            if (present_support)
                present_family_ = i;
            if (graphics_family_ != UINT32_MAX && present_family_ != UINT32_MAX) break;
        }
        if (graphics_family_ == UINT32_MAX) {
            std::cerr << "[Vulkan] Graphics queue family ... FAILED" << std::endl;
            return false;
        }
        std::cerr << "[Vulkan] Graphics queue family ... " << graphics_family_ << std::endl;
        
        if (present_family_ == UINT32_MAX) {
            std::cerr << "[Vulkan] Present support ... NO" << std::endl;
            return false;
        }
        std::cerr << "[Vulkan] Present support ... YES queue=" << present_family_ << std::endl;
    }

    // --- 6. Create logical device ---
    {
        float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queue_cis;
        std::vector<uint32_t> unique_families = { graphics_family_ };
        if (present_family_ != graphics_family_)
            unique_families.push_back(present_family_);

        for (auto fam : unique_families) {
            VkDeviceQueueCreateInfo qci = {};
            qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qci.queueFamilyIndex = fam;
            qci.queueCount = 1;
            qci.pQueuePriorities = &priority;
            queue_cis.push_back(qci);
        }

        const char* dev_exts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkPhysicalDeviceFeatures features = {};
        features.fillModeNonSolid = VK_TRUE;  // For wireframe debug if needed

        VkDeviceCreateInfo dci = {};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = (uint32_t)queue_cis.size();
        dci.pQueueCreateInfos = queue_cis.data();
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = dev_exts;
        dci.pEnabledFeatures = &features;

        VK_CHECK(vkCreateDevice(physical_device_, &dci, nullptr, &device_), "Logical device");
        volkLoadDevice(device_);
        vkGetDeviceQueue(device_, graphics_family_, 0, &graphics_queue_);
        vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
    }

    // --- 7. Create VMA allocator ---
    {
        VmaVulkanFunctions vma_funcs = {};
        vma_funcs.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vma_funcs.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

        VmaAllocatorCreateInfo aci = {};
        aci.physicalDevice = physical_device_;
        aci.device = device_;
        aci.instance = instance_;
        aci.pVulkanFunctions = &vma_funcs;
        aci.vulkanApiVersion = VK_API_VERSION_1_0;

        VK_CHECK(vmaCreateAllocator(&aci, &allocator_), "VMA allocator");
    }

    // --- 8. Create swapchain ---
    if (!create_swapchain(window)) return false;

    // --- 9. Create render pass ---
    if (!create_render_pass()) return false;

    // --- 10. Create framebuffers ---
    if (!create_framebuffers()) return false;

    // --- 11. Create command pool + buffers ---
    {
        VkCommandPoolCreateInfo cpi = {};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.queueFamilyIndex = graphics_family_;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device_, &cpi, nullptr, &command_pool_), "Command pool");

        command_buffers_.resize(MAX_FRAMES_IN_FLIGHT);
        VkCommandBufferAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = command_pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = MAX_FRAMES_IN_FLIGHT;
        VK_CHECK(vkAllocateCommandBuffers(device_, &ai, command_buffers_.data()), "Command buffers");
    }

    // --- 12. Create sync objects ---
    {
        image_available_.resize(MAX_FRAMES_IN_FLIGHT);
        render_finished_.resize(MAX_FRAMES_IN_FLIGHT);
        in_flight_fences_.resize(MAX_FRAMES_IN_FLIGHT);

        VkSemaphoreCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &image_available_[i]), "Semaphore (image_available)");
            VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &render_finished_[i]), "Semaphore (render_finished)");
            VK_CHECK(vkCreateFence(device_, &fci, nullptr, &in_flight_fences_[i]), "Fence");
        }
    }

    // --- 13. Create staging buffers ---
    {
        staging_buffers_.resize(MAX_FRAMES_IN_FLIGHT);
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo bci = {};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = STAGING_BUFFER_SIZE;
            bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo aci = {};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo alloc_info;
            VK_CHECK(vmaCreateBuffer(allocator_, &bci, &aci, &staging_buffers_[i].buffer,
                           &staging_buffers_[i].allocation, &alloc_info), "Staging buffer");
            staging_buffers_[i].mapped_ptr = alloc_info.pMappedData;
            staging_buffers_[i].capacity = STAGING_BUFFER_SIZE;
            staging_buffers_[i].offset = 0;
        }
        std::cout << "[VK] Staging buffers created (" << STAGING_BUFFER_SIZE / 1024 / 1024 << " MB each)" << std::endl;
    }

    // --- 14. Create descriptor set layouts (sampler + UBO) ---
    {
        VkDescriptorSetLayoutBinding tex_binding = {};
        // set 0, binding 0: combined image sampler (texture)
        tex_binding.binding = 0;
        tex_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tex_binding.descriptorCount = 1;
        tex_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dslci = {};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 1;
        dslci.pBindings = &tex_binding;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &descriptor_set_layout_), "Descriptor set layout (Texture)");

        VkDescriptorSetLayoutBinding ubo_binding = {};
        // set 1, binding 0: UBO for scene/lighting data
        ubo_binding.binding = 0;
        ubo_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ubo_binding.descriptorCount = 1;
        ubo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ubo_dslci = {};
        ubo_dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ubo_dslci.bindingCount = 1;
        ubo_dslci.pBindings = &ubo_binding;
        VK_CHECK(vkCreateDescriptorSetLayout(device_, &ubo_dslci, nullptr, &ubo_set_layout_), "Descriptor set layout (UBO)");
    }

    // --- 15. Create pipeline layout ---
    {
        VkPushConstantRange push_range = {};
        push_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push_range.offset = 0;
        push_range.size = sizeof(PushConstantBlock);

        VkDescriptorSetLayout layouts[] = { descriptor_set_layout_, ubo_set_layout_ };

        VkPipelineLayoutCreateInfo plci = {};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 2;
        plci.pSetLayouts = layouts;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push_range;
        VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_), "Pipeline layout");
    }

    // --- 16. Create pipeline cache ---
    {
        VkPipelineCacheCreateInfo pcci = {};
        pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        VK_CHECK(vkCreatePipelineCache(device_, &pcci, nullptr, &pipeline_cache_), "Pipeline cache");
    }

    // --- 17. Compile shaders ---
    if (!compile_shaders()) {
        std::cerr << "[VK] Shader compilation failed — running without Vulkan" << std::endl;
        return false;
    }

    // --- 18. Create default white texture (1x1) ---
    create_default_texture();

    // --- 19. Create descriptor pool ---
    {
        VkDescriptorPoolSize pool_sizes[2] = {};
        pool_sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_sizes[0].descriptorCount = 256; // Enough for all game textures
        pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        pool_sizes[1].descriptorCount = MAX_FRAMES_IN_FLIGHT;

        VkDescriptorPoolCreateInfo dpci = {};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets = 256 + MAX_FRAMES_IN_FLIGHT;
        dpci.poolSizeCount = 2;
        dpci.pPoolSizes = pool_sizes;
        VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descriptor_pool_), "Descriptor pool");
    }

    // --- 19.5 Allocate and update UBO descriptor sets ---
    {
        VkDescriptorSetLayout ubo_layouts[MAX_FRAMES_IN_FLIGHT];
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            ubo_layouts[i] = ubo_set_layout_;
        }

        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descriptor_pool_;
        dsai.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        dsai.pSetLayouts = ubo_layouts;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, ubo_descriptor_sets_), "Allocate UBO descriptor sets");

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkDescriptorBufferInfo dbi = {};
            dbi.buffer = staging_buffers_[i].buffer;
            dbi.offset = 0;
            dbi.range = sizeof(SceneData);

            VkWriteDescriptorSet wds = {};
            wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds.dstSet = ubo_descriptor_sets_[i];
            wds.dstBinding = 0;
            wds.descriptorCount = 1;
            wds.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            wds.pBufferInfo = &dbi;
            vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
        }
    }

    // --- 20. Allocate descriptor set for default white texture ---
    {
        auto& tex = textures_[0]; // Default white texture created at step 18
        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descriptor_pool_;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &descriptor_set_layout_;
        VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &tex.descriptor_set), "Allocate descriptor set (default)");

        VkDescriptorImageInfo dii = {};
        dii.sampler = tex.sampler;
        dii.imageView = tex.view;
        dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkDescriptorImageInfo dii = {};
            dii.sampler = tex.sampler;
            dii.imageView = tex.view;
            dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet wds = {};
            wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds.dstSet = tex.descriptor_set;
            wds.dstBinding = 0;
            wds.descriptorCount = 1;
            wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds.pImageInfo = &dii;
            vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
        }
    }

    // --- 21. Create offscreen target & PostFX pipeline ---
    if (!create_offscreen_target()) {
        std::cerr << "[VK] Offscreen target creation failed" << std::endl;
        return false;
    }
    if (!create_postfx_pipeline()) {
        std::cerr << "[VK] PostFX pipeline creation failed" << std::endl;
        return false;
    }

    current_state_.reset();
    initialized_ = true;
    std::cout << "[VK] ===== Vulkan backend initialized (with PostFX + Offscreen Pass) =====" << std::endl;
    std::cout << "[VK] Game resolution: " << game_w_ << "x" << game_h_ << std::endl;
    return true;
}

// ============================================================================
// Swapchain creation
// ============================================================================

bool VulkanBackend::create_swapchain(SDL_Window* window) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);

    uint32_t fmt_count;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmt_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &fmt_count, formats.data());

    std::cerr << "[Vulkan] Surface formats:" << std::endl;
    for (auto& f : formats) {
        std::cerr << "         format=" << f.format << " colorSpace=" << f.colorSpace << " ... supported" << std::endl;
    }

    uint32_t pm_count;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &pm_count, present_modes.data());

    std::cerr << "[Vulkan] Present modes:" << std::endl;
    for (auto& pm : present_modes) {
        std::cerr << "         mode=" << pm << " ... supported" << std::endl;
    }

    // Pick format: prefer BGRA8 SRGB
    swapchain_format_ = formats[0];
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            swapchain_format_ = f;
            break;
        }
    }
    // Fallback to BGRA8 UNORM
    if (swapchain_format_.format != VK_FORMAT_B8G8R8A8_SRGB) {
        for (auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM) {
                swapchain_format_ = f;
                break;
            }
        }
    }

    // Pick present mode: prefer MAILBOX (triple buffering), fallback to FIFO (vsync)
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto& pm : present_modes) {
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            present_mode = pm;
            break;
        }
    }

    // Pick extent
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent.width = std::clamp((uint32_t)w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp((uint32_t)h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    swapchain_extent_ = extent;

    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = surface_;
    sci.minImageCount = image_count;
    sci.imageFormat = swapchain_format_.format;
    sci.imageColorSpace = swapchain_format_.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = present_mode;
    sci.clipped = VK_TRUE;

    uint32_t family_indices[] = { graphics_family_, present_family_ };
    if (graphics_family_ != present_family_) {
        sci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices = family_indices;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_), "Swapchain");

    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    swapchain_image_views_.resize(image_count);
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo ivci = {};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = swapchain_images_[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = swapchain_format_.format;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &ivci, nullptr, &swapchain_image_views_[i]);
    }

    std::cout << "[VK] Swapchain created: " << extent.width << "x" << extent.height
              << " (" << image_count << " images)" << std::endl;
    return true;
}

// ============================================================================
// Render pass
// ============================================================================

bool VulkanBackend::create_render_pass() {
    // Color attachment
    VkAttachmentDescription color_att = {};
    color_att.format = swapchain_format_.format;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Depth/stencil capability search
    std::cerr << "[Vulkan] Depth/stencil candidates:" << std::endl;
    VkFormat candidates[] = {
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT
    };
    depth_format_ = VK_FORMAT_UNDEFINED;
    for (VkFormat format : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physical_device_, format, &props);
        bool supported = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
        
        std::cerr << "         ";
        if (format == VK_FORMAT_D24_UNORM_S8_UINT) std::cerr << "D24_UNORM_S8_UINT .............. ";
        else if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) std::cerr << "D32_SFLOAT_S8_UINT ............. ";
        else if (format == VK_FORMAT_D16_UNORM_S8_UINT) std::cerr << "D16_UNORM_S8_UINT .............. ";
        else std::cerr << format << " .............. ";
        
        if (supported) {
            if (depth_format_ == VK_FORMAT_UNDEFINED) {
                std::cerr << "supported -> selected" << std::endl;
                depth_format_ = format;
            } else {
                std::cerr << "supported" << std::endl;
            }
        } else {
            std::cerr << "UNSUPPORTED" << (depth_format_ == VK_FORMAT_UNDEFINED ? " (fallback)" : "") << std::endl;
        }
    }

    if (depth_format_ == VK_FORMAT_UNDEFINED) {
        std::cerr << "[Vulkan] Failed to find a supported depth format!" << std::endl;
        return false;
    }

    // Depth/stencil attachment
    VkAttachmentDescription depth_att = {};
    depth_att.format = depth_format_;
    depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkAttachmentDescription attachments[] = { color_att, depth_att };
    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;

    VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &render_pass_), "Render pass");
    return true;
}

// ============================================================================
// Depth image + Framebuffers
// ============================================================================

bool VulkanBackend::create_framebuffers() {
    // Create depth image
    {
        VkImageCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = depth_format_;
        ici.extent = { swapchain_extent_.width, swapchain_extent_.height, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

        VmaAllocationCreateInfo aci = {};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VK_CHECK(vmaCreateImage(allocator_, &ici, &aci, &depth_image_, &depth_allocation_, nullptr), "Depth image");

        VkImageViewCreateInfo ivci = {};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = depth_image_;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = depth_format_;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &ivci, nullptr, &depth_image_view_), "Depth image view");
    }

    // Create framebuffers
    swapchain_framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
        VkImageView attachments[] = { swapchain_image_views_[i], depth_image_view_ };
        VkFramebufferCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = render_pass_;
        fci.attachmentCount = 2;
        fci.pAttachments = attachments;
        fci.width = swapchain_extent_.width;
        fci.height = swapchain_extent_.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &swapchain_framebuffers_[i]), "Framebuffer");
    }
    return true;
}

// ============================================================================
// Shader compilation (runtime GLSL → SPIR-V via shaderc if available,
// otherwise use pre-compiled SPIR-V from vulkan_shaders.glsl.h)
// ============================================================================

bool VulkanBackend::create_offscreen_target() {
    if (game_w_ <= 0 || game_h_ <= 0) return false;

    // 1. Offscreen Color Image
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { (uint32_t)game_w_, (uint32_t)game_h_, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    VK_CHECK(vmaCreateImage(allocator_, &ici, &aci, &offscreen_color_image_, &offscreen_color_alloc_, nullptr), "Offscreen Color Image");

    VkImageViewCreateInfo ivci = {};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = offscreen_color_image_;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    VK_CHECK(vkCreateImageView(device_, &ivci, nullptr, &offscreen_color_view_), "Offscreen Color View");

    // 2. Offscreen Depth Image
    ici.format = depth_format_;
    ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VK_CHECK(vmaCreateImage(allocator_, &ici, &aci, &offscreen_depth_image_, &offscreen_depth_alloc_, nullptr), "Offscreen Depth Image");

    ivci.image = offscreen_depth_image_;
    ivci.format = depth_format_;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | (depth_format_ >= VK_FORMAT_D16_UNORM_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    VK_CHECK(vkCreateImageView(device_, &ivci, nullptr, &offscreen_depth_view_), "Offscreen Depth View");

    // 3. Offscreen Render Pass
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    attachments[1].format = depth_format_;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    VkSubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 2;
    rpci.pAttachments = attachments;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dependency;

    VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &offscreen_render_pass_), "Offscreen Render Pass");

    // 4. Offscreen Framebuffer
    VkImageView fb_views[2] = { offscreen_color_view_, offscreen_depth_view_ };
    VkFramebufferCreateInfo fci = {};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = offscreen_render_pass_;
    fci.attachmentCount = 2;
    fci.pAttachments = fb_views;
    fci.width = (uint32_t)game_w_;
    fci.height = (uint32_t)game_h_;
    fci.layers = 1;

    VK_CHECK(vkCreateFramebuffer(device_, &fci, nullptr, &offscreen_framebuffer_), "Offscreen Framebuffer");
    return true;
}

bool VulkanBackend::create_postfx_pipeline() {
    postfx_vert_shader_ = create_shader_module(VK_VERT_POSTFX_SPIRV, sizeof(VK_VERT_POSTFX_SPIRV));
    postfx_frag_shader_ = create_shader_module(VK_FRAG_POSTFX_SPIRV, sizeof(VK_FRAG_POSTFX_SPIRV));
    if (!postfx_vert_shader_ || !postfx_frag_shader_) return false;

    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &postfx_sampler_), "PostFX Sampler");

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo dslci = {};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 1;
    dslci.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &postfx_set_layout_), "PostFX Set Layout");

    struct PostFXPushData {
        float color_adjust[4];
        float effects_a[4];
        float effects_b[4];
        float tex_size[2];
        float out_size[2];
    };
    VkPushConstantRange pcr = {};
    pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(PostFXPushData);

    VkPipelineLayoutCreateInfo plci = {};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &postfx_set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &postfx_pipeline_layout_), "PostFX Pipeline Layout");

    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptor_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &postfx_set_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &postfx_descriptor_set_), "PostFX Descriptor Set");

    VkDescriptorImageInfo dii = {};
    dii.sampler = postfx_sampler_;
    dii.imageView = offscreen_color_view_;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds = {};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = postfx_descriptor_set_;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = postfx_vert_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = postfx_frag_shader_;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vps = {};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkGraphicsPipelineCreateInfo gpci = {};
    gpci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vps;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = postfx_pipeline_layout_;
    gpci.renderPass = render_pass_; // Swapchain render pass
    gpci.subpass = 0;

    VK_CHECK(vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &gpci, nullptr, &postfx_pipeline_), "PostFX Pipeline");
    return true;
}

// ============================================================================
// Shader compilation
// ============================================================================

bool VulkanBackend::compile_shaders() {
    vert_shader_ = create_shader_module(VK_VERT_SHADER_SPIRV, sizeof(VK_VERT_SHADER_SPIRV));
    frag_shader_ = create_shader_module(VK_FRAG_SHADER_SPIRV, sizeof(VK_FRAG_SHADER_SPIRV));

    if (vert_shader_ == VK_NULL_HANDLE || frag_shader_ == VK_NULL_HANDLE) {
        std::cerr << "[VK] Failed to create uber shader modules" << std::endl;
        return false;
    }
    std::cout << "[VK] Shaders compiled" << std::endl;
    return true;
}

VkShaderModule VulkanBackend::create_shader_module(const uint32_t* code, size_t size) {
    VkShaderModuleCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = code;

    VkShaderModule module;
    VkResult res = vkCreateShaderModule(device_, &ci, nullptr, &module);
    if (res != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

// ============================================================================
// Default 1x1 white texture
// ============================================================================

void VulkanBackend::create_default_texture() {
    uint8_t white_pixel[] = { 255, 255, 255, 255 };
    VulkanTexture tex = {};
    tex.width = 1;
    tex.height = 1;

    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { 1, 1, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(allocator_, &ici, &aci, &tex.image, &tex.allocation, nullptr);

    VkImageViewCreateInfo ivci = {};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = tex.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &ivci, nullptr, &tex.view);

    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_NEAREST;
    sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(device_, &sci, nullptr, &tex.sampler);

    VkBuffer staging_buf;
    VmaAllocation staging_alloc;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo saci = {};
    saci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    saci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo sinfo;
    vmaCreateBuffer(allocator_, &bci, &saci, &staging_buf, &staging_alloc, &sinfo);
    memcpy(sinfo.pMappedData, white_pixel, 4);

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = command_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { 1, 1, 1 };
    vkCmdCopyBufferToImage(cmd, staging_buf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging_buf, staging_alloc);

    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptor_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &descriptor_set_layout_;
    vkAllocateDescriptorSets(device_, &dsai, &tex.descriptor_set);

    VkDescriptorImageInfo dii = {};
    dii.sampler = tex.sampler;
    dii.imageView = tex.view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds = {};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = tex.descriptor_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

    textures_[0] = tex;
    default_texture_id_ = 0;
    std::cout << "[VK] Default white texture created" << std::endl;
}

// ============================================================================
// Frame begin/end
// ============================================================================

void VulkanBackend::begin_frame() {
    if (!initialized_) return;

    vkWaitForFences(device_, 1, &in_flight_fences_[current_frame_], VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &in_flight_fences_[current_frame_]);

    staging_buffers_[current_frame_].offset = 0;

    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &cbbi);

    // Begin offscreen render pass
    VkClearValue clear_values[2] = {};
    clear_values[0].color = { { current_state_.clear_color[0], current_state_.clear_color[1],
                                 current_state_.clear_color[2], current_state_.clear_color[3] } };
    clear_values[1].depthStencil = { 1.0f, 0 };

    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = offscreen_render_pass_;
    rpbi.framebuffer = offscreen_framebuffer_;
    rpbi.renderArea.extent = { (uint32_t)game_w_, (uint32_t)game_h_ };
    rpbi.clearValueCount = 2;
    rpbi.pClearValues = clear_values;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {};
    vp.width = (float)game_w_;
    vp.height = (float)game_h_;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor = {};
    scissor.extent = { (uint32_t)game_w_, (uint32_t)game_h_ };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    in_render_pass_ = true;
}

void VulkanBackend::end_frame_and_present(const PostFXState* postfx, int scale_mode) {
    if (!initialized_ || !in_render_pass_) return;

    VkCommandBuffer cmd = command_buffers_[current_frame_];
    vkCmdEndRenderPass(cmd);

    // Acquire swapchain image
    VkResult acquire_res = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                  image_available_[current_frame_], VK_NULL_HANDLE,
                                                  &current_image_index_);
    if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR) {
        vkEndCommandBuffer(cmd);
        in_render_pass_ = false;
        return;
    }

    // Begin swapchain render pass
    VkClearValue clear_val = {};
    clear_val.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };

    VkRenderPassBeginInfo rpbi = {};
    rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpbi.renderPass = render_pass_;
    rpbi.framebuffer = swapchain_framebuffers_[current_image_index_];
    rpbi.renderArea.extent = swapchain_extent_;
    rpbi.clearValueCount = 1;
    rpbi.pClearValues = &clear_val;
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {};
    vp.width = (float)swapchain_extent_.width;
    vp.height = (float)swapchain_extent_.height;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor = {};
    scissor.extent = swapchain_extent_;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Render PostFX blit pass
    if (postfx_pipeline_ != VK_NULL_HANDLE) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postfx_pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postfx_pipeline_layout_,
                                0, 1, &postfx_descriptor_set_, 0, nullptr);

        struct PostFXPushData {
            float color_adjust[4];
            float effects_a[4];
            float effects_b[4];
            float tex_size[2];
            float out_size[2];
        } push = {};

        static float s_postfx_time = 0.0f;
        s_postfx_time += 0.016f;

        if (postfx) {
            push.color_adjust[0] = postfx->color_adjust ? postfx->brightness : 0.0f;
            push.color_adjust[1] = postfx->color_adjust ? postfx->contrast : 1.0f;
            push.color_adjust[2] = postfx->color_adjust ? postfx->saturation : 1.0f;
            push.color_adjust[3] = postfx->color_adjust ? postfx->warmth : 0.0f;

            push.effects_a[0] = postfx->vignette ? postfx->vignette_intensity : 0.0f;
            push.effects_a[1] = postfx->film_grain ? postfx->grain_intensity : 0.0f;
            push.effects_a[2] = postfx->chromatic_aberration ? postfx->ca_offset : 0.0f;
            push.effects_a[3] = postfx->sharpen ? postfx->sharpen_strength : 0.0f;

            push.effects_b[0] = s_postfx_time;
            push.effects_b[1] = postfx->enabled ? 1.0f : 0.0f;
            push.effects_b[2] = (float)scale_mode;
            push.effects_b[3] = 0.0f;
        } else {
            push.color_adjust[1] = 1.0f; push.color_adjust[2] = 1.0f;
            push.effects_b[0] = s_postfx_time;
            push.effects_b[1] = 0.0f;
            push.effects_b[2] = (float)scale_mode;
        }
        push.tex_size[0] = (float)game_w_;
        push.tex_size[1] = (float)game_h_;
        push.out_size[0] = (float)swapchain_extent_.width;
        push.out_size[1] = (float)swapchain_extent_.height;

        vkCmdPushConstants(cmd, postfx_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0); // Fullscreen triangle
    }

    // Render Swordfare GUI (if active)
    extern SwordfareGUI g_swordfare_gui;
    if (g_swordfare_gui.is_initialized()) {
        g_swordfare_gui.end_frame();
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &image_available_[current_frame_];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &render_finished_[current_frame_];
    vkQueueSubmit(graphics_queue_, 1, &si, in_flight_fences_[current_frame_]);

    // Present
    VkPresentInfoKHR pi = {};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_finished_[current_frame_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &current_image_index_;
    vkQueuePresentKHR(present_queue_, &pi);

    current_frame_ = (current_frame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    in_render_pass_ = false;
}

// ============================================================================
// Matrix operations (software stack — identical to OpenGL fixed-function)
// ============================================================================

void VulkanBackend::matrix_mode(uint32_t mode) { current_state_.matrix_mode = mode; }
void VulkanBackend::load_identity() { mat4_identity(current_state_.current_matrix()); }
void VulkanBackend::load_matrixf(const float m[16]) { memcpy(current_state_.current_matrix(), m, 16 * sizeof(float)); }

void VulkanBackend::mult_matrixf(const float m[16]) {
    float tmp[16];
    mat4_multiply(tmp, current_state_.current_matrix(), m);
    memcpy(current_state_.current_matrix(), tmp, 16 * sizeof(float));
}

void VulkanBackend::push_matrix() {
    auto& s = current_state_;
    if (s.matrix_mode == gl::MODELVIEW && s.mv_depth < 31) {
        memcpy(s.modelview_stack[s.mv_depth + 1], s.modelview_stack[s.mv_depth], 16 * sizeof(float));
        s.mv_depth++;
    } else if (s.matrix_mode == gl::PROJECTION && s.proj_depth < 3) {
        memcpy(s.projection_stack[s.proj_depth + 1], s.projection_stack[s.proj_depth], 16 * sizeof(float));
        s.proj_depth++;
    }
}

void VulkanBackend::pop_matrix() {
    auto& s = current_state_;
    if (s.matrix_mode == gl::MODELVIEW && s.mv_depth > 0) s.mv_depth--;
    else if (s.matrix_mode == gl::PROJECTION && s.proj_depth > 0) s.proj_depth--;
}

void VulkanBackend::translatef(float x, float y, float z) { mat4_translate(current_state_.current_matrix(), x, y, z); }
void VulkanBackend::rotatef(float angle, float x, float y, float z) { mat4_rotate(current_state_.current_matrix(), angle, x, y, z); }
void VulkanBackend::scalef(float x, float y, float z) { mat4_scale(current_state_.current_matrix(), x, y, z); }

void VulkanBackend::orthof(float l, float r, float b, float t, float n, float f) {
    float o[16];
    mat4_ortho(o, l, r, b, t, n, f);
    float tmp[16];
    mat4_multiply(tmp, current_state_.current_matrix(), o);
    memcpy(current_state_.current_matrix(), tmp, 16 * sizeof(float));
}

void VulkanBackend::frustumf(float l, float r, float b, float t, float n, float f) {
    float fr[16];
    mat4_frustum(fr, l, r, b, t, n, f);
    float tmp[16];
    mat4_multiply(tmp, current_state_.current_matrix(), fr);
    memcpy(current_state_.current_matrix(), tmp, 16 * sizeof(float));
}

// ============================================================================
// State setting (all the glEnable/glDisable/glBlendFunc etc.)
// ============================================================================

void VulkanBackend::enable(uint32_t cap) {
    auto& s = current_state_;
    switch (cap) {
        case gl::TEXTURE_2D:  s.tex_units[s.active_texture].enabled = true; break;
        case gl::BLEND:       s.blend_enabled = true; break;
        case gl::DEPTH_TEST:  s.depth_enabled = true; break;
        case gl::ALPHA_TEST:  s.alpha_test_enabled = true; break;
        case gl::CULL_FACE:   s.cull_enabled = true; break;
        case gl::SCISSOR_TEST: s.scissor_enabled = true; break;
        case gl::STENCIL_TEST: s.stencil_enabled = true; break;
        case gl::LIGHTING:    s.lighting_enabled = true; break;
        case gl::FOG:         s.fog_enabled = true; break;
        case gl::LIGHT0: case gl::LIGHT0+1: case gl::LIGHT0+2: case gl::LIGHT0+3:
        case gl::LIGHT0+4: case gl::LIGHT0+5: case gl::LIGHT0+6: case gl::LIGHT0+7:
            s.lights[cap - gl::LIGHT0].enabled = true; break;
    }
}

void VulkanBackend::disable(uint32_t cap) {
    auto& s = current_state_;
    switch (cap) {
        case gl::TEXTURE_2D:  s.tex_units[s.active_texture].enabled = false; break;
        case gl::BLEND:       s.blend_enabled = false; break;
        case gl::DEPTH_TEST:  s.depth_enabled = false; break;
        case gl::ALPHA_TEST:  s.alpha_test_enabled = false; break;
        case gl::CULL_FACE:   s.cull_enabled = false; break;
        case gl::SCISSOR_TEST: s.scissor_enabled = false; break;
        case gl::STENCIL_TEST: s.stencil_enabled = false; break;
        case gl::LIGHTING:    s.lighting_enabled = false; break;
        case gl::FOG:         s.fog_enabled = false; break;
        case gl::LIGHT0: case gl::LIGHT0+1: case gl::LIGHT0+2: case gl::LIGHT0+3:
        case gl::LIGHT0+4: case gl::LIGHT0+5: case gl::LIGHT0+6: case gl::LIGHT0+7:
            s.lights[cap - gl::LIGHT0].enabled = false; break;
    }
}

void VulkanBackend::blend_func(uint32_t s, uint32_t d) { current_state_.blend_src = s; current_state_.blend_dst = d; }
void VulkanBackend::depth_func(uint32_t f) { current_state_.depth_func = f; }
void VulkanBackend::depth_mask(bool f) { current_state_.depth_mask = f; }
void VulkanBackend::alpha_func(uint32_t f, float ref) { current_state_.alpha_func = f; current_state_.alpha_ref = ref; }
void VulkanBackend::cull_face(uint32_t m) { current_state_.cull_mode = m; }
void VulkanBackend::stencil_func(uint32_t f, int r, uint32_t m) { current_state_.stencil_func = f; current_state_.stencil_ref = r; current_state_.stencil_mask = m; }
void VulkanBackend::stencil_mask(uint32_t m) { current_state_.stencil_write_mask = m; }
void VulkanBackend::stencil_op(uint32_t sf, uint32_t dpf, uint32_t dpp) { current_state_.stencil_sfail = sf; current_state_.stencil_dpfail = dpf; current_state_.stencil_dppass = dpp; }
void VulkanBackend::color_mask(bool r, bool g, bool b, bool a) { current_state_.color_mask_r = r; current_state_.color_mask_g = g; current_state_.color_mask_b = b; current_state_.color_mask_a = a; }
void VulkanBackend::shade_model(uint32_t m) { current_state_.shade_model = m; }
void VulkanBackend::active_texture(uint32_t t) { current_state_.active_texture = (t >= gl::TEXTURE0) ? (t - gl::TEXTURE0) : t; }
void VulkanBackend::color4f(float r, float g, float b, float a) { current_state_.current_color[0] = r; current_state_.current_color[1] = g; current_state_.current_color[2] = b; current_state_.current_color[3] = a; }
void VulkanBackend::color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { color4f(r/255.0f, g/255.0f, b/255.0f, a/255.0f); }

void VulkanBackend::clear_color(float r, float g, float b, float a) {
    current_state_.clear_color[0] = r; current_state_.clear_color[1] = g;
    current_state_.clear_color[2] = b; current_state_.clear_color[3] = a;
}

void VulkanBackend::clear(uint32_t mask) {
    // In Vulkan, clears happen at render pass begin. We store the clear color/depth
    // and they'll be applied at the next begin_frame(). For mid-frame clears,
    // we'd need to end/restart the render pass, which we skip for now.
    (void)mask;
}

void VulkanBackend::viewport(int x, int y, int w, int h) {
    if (!in_render_pass_) return;
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    VkViewport vp = {};
    vp.x = (float)x;
    // Vulkan has Y-down, OpenGL has Y-up. Flip via negative height trick.
    vp.y = (float)(y + h);
    vp.width = (float)w;
    vp.height = -(float)h;
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);
}

void VulkanBackend::scissor(int x, int y, int w, int h) {
    if (!in_render_pass_) return;
    VkCommandBuffer cmd = command_buffers_[current_frame_];
    VkRect2D sc = {};
    sc.offset = { x, (int)swapchain_extent_.height - y - h };
    sc.extent = { (uint32_t)w, (uint32_t)h };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

// ============================================================================
// Vertex array pointers
// ============================================================================

void VulkanBackend::vertex_pointer(int size, uint32_t type, int stride, uint32_t ptr) {
    current_state_.vertex_array = { true, size, type, stride, ptr };
}
void VulkanBackend::texcoord_pointer(int size, uint32_t type, int stride, uint32_t ptr) {
    current_state_.texcoord_array = { true, size, type, stride, ptr };
}
void VulkanBackend::color_pointer(int size, uint32_t type, int stride, uint32_t ptr) {
    current_state_.color_array = { true, size, type, stride, ptr };
}
void VulkanBackend::normal_pointer(uint32_t type, int stride, uint32_t ptr) {
    current_state_.normal_array = { true, 3, type, stride, ptr };
}

void VulkanBackend::enable_client_state(uint32_t cap) {
    switch (cap) {
        case gl::VERTEX_ARRAY:   current_state_.vertex_array.enabled = true; break;
        case gl::TEXTURE_COORD_ARRAY: current_state_.texcoord_array.enabled = true; break;
        case gl::COLOR_ARRAY:    current_state_.color_array.enabled = true; break;
        case gl::NORMAL_ARRAY:   current_state_.normal_array.enabled = true; break;
    }
}
void VulkanBackend::disable_client_state(uint32_t cap) {
    switch (cap) {
        case gl::VERTEX_ARRAY:   current_state_.vertex_array.enabled = false; break;
        case gl::TEXTURE_COORD_ARRAY: current_state_.texcoord_array.enabled = false; break;
        case gl::COLOR_ARRAY:    current_state_.color_array.enabled = false; break;
        case gl::NORMAL_ARRAY:   current_state_.normal_array.enabled = false; break;
    }
}
void VulkanBackend::client_active_texture(uint32_t t) { (void)t; /* We only use unit 0 for texcoords */ }

// ============================================================================
// Texture operations (stubs for now — will be fleshed out)
// ============================================================================

void VulkanBackend::bind_texture(uint32_t target, uint32_t id) {
    (void)target;
    current_state_.tex_units[current_state_.active_texture].bound_texture = id;
}

void VulkanBackend::gen_textures(int n, uint32_t* ids) {
    for (int i = 0; i < n; i++) {
        ids[i] = next_texture_id_++;
        // Don't create the VulkanTexture yet — wait for tex_image_2d
    }
}

void VulkanBackend::delete_textures(int n, const uint32_t* ids) {
    for (int i = 0; i < n; i++) {
        auto it = textures_.find(ids[i]);
        if (it != textures_.end()) {
            // Queue for deletion (don't destroy while in use)
            auto& tex = it->second;
            if (tex.descriptor_set != VK_NULL_HANDLE)
                vkFreeDescriptorSets(device_, descriptor_pool_, 1, &tex.descriptor_set);
            if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(device_, tex.sampler, nullptr);
            if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(device_, tex.view, nullptr);
            if (tex.image != VK_NULL_HANDLE) vmaDestroyImage(allocator_, tex.image, tex.allocation);
            textures_.erase(it);
        }
    }
}

void VulkanBackend::tex_image_2d(uint32_t target, int level, int internal_fmt,
                                  int w, int h, int border, uint32_t fmt,
                                  uint32_t type, const void* data) {
    (void)target; (void)level; (void)internal_fmt; (void)border; (void)type;
    uint32_t id = current_state_.tex_units[current_state_.active_texture].bound_texture;
    if (id == 0) return;
    if (w <= 0 || h <= 0) return;

    // Delete old texture if exists
    auto old_it = textures_.find(id);
    if (old_it != textures_.end()) {
        auto& old = old_it->second;
        if (old.descriptor_set != VK_NULL_HANDLE)
            vkFreeDescriptorSets(device_, descriptor_pool_, 1, &old.descriptor_set);
        if (old.sampler != VK_NULL_HANDLE) vkDestroySampler(device_, old.sampler, nullptr);
        if (old.view != VK_NULL_HANDLE) vkDestroyImageView(device_, old.view, nullptr);
        if (old.image != VK_NULL_HANDLE) vmaDestroyImage(allocator_, old.image, old.allocation);
        textures_.erase(old_it);
    }

    VulkanTexture tex = {};
    tex.width = (uint32_t)w;
    tex.height = (uint32_t)h;
    tex.format = VK_FORMAT_R8G8B8A8_UNORM;

    // Convert source pixel data to RGBA8
    size_t pixel_count = (size_t)w * h;
    std::vector<uint8_t> rgba_data(pixel_count * 4);

    if (data) {
        const uint8_t* src = (const uint8_t*)data;
        if (type == 0x8363 /* UNSIGNED_SHORT_5_6_5 */) {
            const uint16_t* src16 = (const uint16_t*)data;
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t v = src16[i];
                rgba_data[i*4+0] = ((v >> 11) & 0x1F) * 255 / 31;
                rgba_data[i*4+1] = ((v >> 5)  & 0x3F) * 255 / 63;
                rgba_data[i*4+2] = (v         & 0x1F) * 255 / 31;
                rgba_data[i*4+3] = 255;
            }
        } else if (type == 0x8033 /* UNSIGNED_SHORT_4_4_4_4 */) {
            const uint16_t* src16 = (const uint16_t*)data;
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t v = src16[i];
                rgba_data[i*4+0] = ((v >> 12) & 0x0F) * 255 / 15;
                rgba_data[i*4+1] = ((v >> 8)  & 0x0F) * 255 / 15;
                rgba_data[i*4+2] = ((v >> 4)  & 0x0F) * 255 / 15;
                rgba_data[i*4+3] = (v         & 0x0F) * 255 / 15;
            }
        } else if (type == 0x8034 /* UNSIGNED_SHORT_5_5_5_1 */) {
            const uint16_t* src16 = (const uint16_t*)data;
            for (size_t i = 0; i < pixel_count; i++) {
                uint16_t v = src16[i];
                rgba_data[i*4+0] = ((v >> 11) & 0x1F) * 255 / 31;
                rgba_data[i*4+1] = ((v >> 6)  & 0x1F) * 255 / 31;
                rgba_data[i*4+2] = ((v >> 1)  & 0x1F) * 255 / 31;
                rgba_data[i*4+3] = (v & 0x01) ? 255 : 0;
            }
        } else if (fmt == gl::RGBA) {
            memcpy(rgba_data.data(), src, pixel_count * 4);
        } else if (fmt == gl::RGB) {
            for (size_t i = 0; i < pixel_count; i++) {
                rgba_data[i*4+0] = src[i*3+0];
                rgba_data[i*4+1] = src[i*3+1];
                rgba_data[i*4+2] = src[i*3+2];
                rgba_data[i*4+3] = 255;
            }
        } else if (fmt == gl::LUMINANCE) {
            for (size_t i = 0; i < pixel_count; i++) {
                rgba_data[i*4+0] = rgba_data[i*4+1] = rgba_data[i*4+2] = src[i];
                rgba_data[i*4+3] = 255;
            }
        } else if (fmt == gl::LUMINANCE_ALPHA) {
            for (size_t i = 0; i < pixel_count; i++) {
                rgba_data[i*4+0] = rgba_data[i*4+1] = rgba_data[i*4+2] = src[i*2+0];
                rgba_data[i*4+3] = src[i*2+1];
            }
        } else if (fmt == gl::ALPHA) {
            for (size_t i = 0; i < pixel_count; i++) {
                rgba_data[i*4+0] = rgba_data[i*4+1] = rgba_data[i*4+2] = 255;
                rgba_data[i*4+3] = src[i];
            }
        } else {
            // Unknown format — fill white
            memset(rgba_data.data(), 255, pixel_count * 4);
        }
    } else {
        // No data — zero-fill (placeholder)
        memset(rgba_data.data(), 0, pixel_count * 4);
    }

    // Create VkImage
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = { (uint32_t)w, (uint32_t)h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo aci = {};
    aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    vmaCreateImage(allocator_, &ici, &aci, &tex.image, &tex.allocation, nullptr);

    // Create staging buffer and upload
    size_t data_size = pixel_count * 4;
    VkBuffer staging_buf;
    VmaAllocation staging_alloc;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = data_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo saci = {};
    saci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    saci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo sinfo;
    vmaCreateBuffer(allocator_, &bci, &saci, &staging_buf, &staging_alloc, &sinfo);
    memcpy(sinfo.pMappedData, rgba_data.data(), data_size);

    // One-shot command buffer for upload
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = command_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyBufferToImage(cmd, staging_buf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging_buf, staging_alloc);

    // Create image view
    VkImageViewCreateInfo ivci = {};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = tex.image;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &ivci, nullptr, &tex.view);

    // Create sampler
    VkSamplerCreateInfo sci = {};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = (tex.mag_filter == gl::NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.minFilter = (tex.min_filter == gl::NEAREST || tex.min_filter == gl::NEAREST_MIPMAP_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = (tex.wrap_s == gl::CLAMP_TO_EDGE) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = (tex.wrap_t == gl::CLAMP_TO_EDGE) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(device_, &sci, nullptr, &tex.sampler);

    // Create descriptor set
    VkDescriptorSetAllocateInfo dsai = {};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = descriptor_pool_;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &descriptor_set_layout_;
    vkAllocateDescriptorSets(device_, &dsai, &tex.descriptor_set);

    VkDescriptorImageInfo dii = {};
    dii.sampler = tex.sampler;
    dii.imageView = tex.view;
    dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wds = {};
    wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wds.dstSet = tex.descriptor_set;
    wds.dstBinding = 0;
    wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);

    textures_[id] = tex;
}

void VulkanBackend::tex_sub_image_2d(uint32_t target, int level, int xoff, int yoff,
                                      int w, int h, uint32_t fmt, uint32_t type,
                                      const void* pixels) {
    (void)target; (void)level;
    if (!pixels || w <= 0 || h <= 0) return;
    uint32_t bound_id = current_state_.tex_units[current_state_.active_texture].bound_texture;
    auto it = textures_.find(bound_id);
    if (it == textures_.end()) return;
    auto& tex = it->second;

    size_t pixel_count = (size_t)w * h;
    std::vector<uint8_t> rgba_data(pixel_count * 4);

    const uint8_t* src = (const uint8_t*)pixels;
    if (type == 0x8363 /* UNSIGNED_SHORT_5_6_5 */) {
        const uint16_t* src16 = (const uint16_t*)pixels;
        for (size_t i = 0; i < pixel_count; i++) {
            uint16_t v = src16[i];
            rgba_data[i*4+0] = ((v >> 11) & 0x1F) * 255 / 31;
            rgba_data[i*4+1] = ((v >> 5)  & 0x3F) * 255 / 63;
            rgba_data[i*4+2] = (v         & 0x1F) * 255 / 31;
            rgba_data[i*4+3] = 255;
        }
    } else if (fmt == gl::RGBA) {
        memcpy(rgba_data.data(), src, pixel_count * 4);
    } else if (fmt == gl::RGB) {
        for (size_t i = 0; i < pixel_count; i++) {
            rgba_data[i*4+0] = src[i*3+0];
            rgba_data[i*4+1] = src[i*3+1];
            rgba_data[i*4+2] = src[i*3+2];
            rgba_data[i*4+3] = 255;
        }
    } else {
        memcpy(rgba_data.data(), src, std::min(pixel_count * 4, (size_t)(w * h * 4)));
    }

    VkBuffer staging_buf;
    VmaAllocation staging_alloc;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = rgba_data.size();
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo saci = {};
    saci.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    saci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo sinfo;
    vmaCreateBuffer(allocator_, &bci, &saci, &staging_buf, &staging_alloc, &sinfo);
    memcpy(sinfo.pMappedData, rgba_data.data(), rgba_data.size());

    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = command_pool_;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);

    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = tex.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = { xoff, yoff, 0 };
    region.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
    vkCmdCopyBufferToImage(cmd, staging_buf, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(graphics_queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &cmd);
    vmaDestroyBuffer(allocator_, staging_buf, staging_alloc);
}

void VulkanBackend::tex_parameteri(uint32_t target, uint32_t pname, int param) {
    (void)target;
    uint32_t bound_id = current_state_.tex_units[current_state_.active_texture].bound_texture;
    auto it = textures_.find(bound_id);
    if (it == textures_.end()) return;
    auto& tex = it->second;

    bool update = false;
    if (pname == gl::TEXTURE_WRAP_S && tex.wrap_s != (uint32_t)param) {
        tex.wrap_s = (uint32_t)param;
        update = true;
    } else if (pname == gl::TEXTURE_WRAP_T && tex.wrap_t != (uint32_t)param) {
        tex.wrap_t = (uint32_t)param;
        update = true;
    } else if (pname == gl::TEXTURE_MIN_FILTER && tex.min_filter != (uint32_t)param) {
        tex.min_filter = (uint32_t)param;
        update = true;
    } else if (pname == gl::TEXTURE_MAG_FILTER && tex.mag_filter != (uint32_t)param) {
        tex.mag_filter = (uint32_t)param;
        update = true;
    }

    if (update && tex.view != VK_NULL_HANDLE) {
        if (tex.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device_, tex.sampler, nullptr);
        }
        VkSamplerCreateInfo sci = {};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = (tex.mag_filter == gl::NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sci.minFilter = (tex.min_filter == gl::NEAREST || tex.min_filter == gl::NEAREST_MIPMAP_NEAREST) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = (tex.wrap_s == gl::CLAMP_TO_EDGE) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = (tex.wrap_t == gl::CLAMP_TO_EDGE) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCreateSampler(device_, &sci, nullptr, &tex.sampler);

        if (tex.descriptor_set != VK_NULL_HANDLE) {
            VkDescriptorImageInfo dii = {};
            dii.sampler = tex.sampler;
            dii.imageView = tex.view;
            dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet wds = {};
            wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds.dstSet = tex.descriptor_set;
            wds.dstBinding = 0;
            wds.descriptorCount = 1;
            wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds.pImageInfo = &dii;
            vkUpdateDescriptorSets(device_, 1, &wds, 0, nullptr);
        }
    }
}

void VulkanBackend::tex_parameterf(uint32_t target, uint32_t pname, float param) {
    (void)target; (void)pname; (void)param;
}

void VulkanBackend::tex_envi(uint32_t target, uint32_t pname, int param) {
    (void)target; (void)pname;
    current_state_.tex_units[current_state_.active_texture].env_mode = (uint32_t)param;
}

void VulkanBackend::tex_envfv(uint32_t target, uint32_t pname, const float* params) {
    (void)target; (void)pname;
    auto& unit = current_state_.tex_units[current_state_.active_texture];
    memcpy(unit.env_color, params, 4 * sizeof(float));
}

// ============================================================================
// Lighting / Material / Fog
// ============================================================================

void VulkanBackend::lightf(uint32_t light, uint32_t pname, float param) {
    int idx = (light - gl::LIGHT0);
    if (idx < 0 || idx >= 8) return;
    auto& l = current_state_.lights[idx];
    switch (pname) {
        case gl::SPOT_EXPONENT: l.spot_exponent = param; break;
        case gl::SPOT_CUTOFF: l.spot_cutoff = param; break;
        case gl::CONSTANT_ATTENUATION: l.constant_atten = param; break;
        case gl::LINEAR_ATTENUATION: l.linear_atten = param; break;
        case gl::QUADRATIC_ATTENUATION: l.quadratic_atten = param; break;
    }
}

void VulkanBackend::lightfv(uint32_t light, uint32_t pname, const float* params) {
    int idx = (light - gl::LIGHT0);
    if (idx < 0 || idx >= 8) return;
    auto& l = current_state_.lights[idx];
    switch (pname) {
        case gl::POSITION: memcpy(l.position, params, 4 * sizeof(float)); break;
        case gl::AMBIENT:  memcpy(l.ambient, params, 4 * sizeof(float)); break;
        case gl::DIFFUSE:  memcpy(l.diffuse, params, 4 * sizeof(float)); break;
        case gl::SPECULAR: memcpy(l.specular, params, 4 * sizeof(float)); break;
        case gl::SPOT_DIRECTION: memcpy(l.spot_direction, params, 3 * sizeof(float)); break;
    }
}

void VulkanBackend::materialf(uint32_t face, uint32_t pname, float param) {
    (void)face;
    if (pname == gl::SHININESS) current_state_.material.shininess = param;
}

void VulkanBackend::materialfv(uint32_t face, uint32_t pname, const float* params) {
    (void)face;
    auto& m = current_state_.material;
    switch (pname) {
        case gl::AMBIENT: memcpy(m.ambient, params, 4 * sizeof(float)); break;
        case gl::DIFFUSE: memcpy(m.diffuse, params, 4 * sizeof(float)); break;
        case gl::SPECULAR: memcpy(m.specular, params, 4 * sizeof(float)); break;
        case gl::EMISSION: memcpy(m.emission, params, 4 * sizeof(float)); break;
        case gl::AMBIENT_AND_DIFFUSE:
            memcpy(m.ambient, params, 4 * sizeof(float));
            memcpy(m.diffuse, params, 4 * sizeof(float));
            break;
    }
}

void VulkanBackend::light_modelf(uint32_t pname, float param) { (void)pname; (void)param; }
void VulkanBackend::light_modelfv(uint32_t pname, const float* params) { (void)pname; (void)params; }

void VulkanBackend::fogf(uint32_t pname, float param) {
    auto& s = current_state_;
    switch (pname) {
        case gl::FOG_DENSITY: s.fog_density = param; break;
        case gl::FOG_START:   s.fog_start = param; break;
        case gl::FOG_END:     s.fog_end = param; break;
        case gl::FOG_MODE:    s.fog_mode = (uint32_t)param; break;
    }
}

void VulkanBackend::fogfv(uint32_t pname, const float* params) {
    if (pname == gl::FOG_COLOR) memcpy(current_state_.fog_color, params, 4 * sizeof(float));
    else fogf(pname, params[0]);
}

void VulkanBackend::fogi(uint32_t pname, int param) { fogf(pname, (float)param); }

// ============================================================================
// Helper: map GL primitive mode to VkPrimitiveTopology
// ============================================================================

static VkPrimitiveTopology gl_mode_to_vk(uint32_t mode) {
    switch (mode) {
        case 0x0000: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;      // GL_POINTS
        case 0x0001: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;       // GL_LINES
        case 0x0003: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;      // GL_LINE_STRIP
        case 0x0004: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // GL_TRIANGLES
        case 0x0005: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;  // GL_TRIANGLE_STRIP
        case 0x0006: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;    // GL_TRIANGLE_FAN
        default:     return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// ============================================================================
// Pipeline state hashing
// ============================================================================

static uint64_t hash_combine(uint64_t seed, uint64_t v) {
    seed ^= v + 0x9e3779b97f4a7c15ULL + (seed << 12) + (seed >> 4);
    return seed;
}

uint64_t VulkanBackend::compute_pipeline_hash(uint32_t mode) const {
    const auto& s = current_state_;
    uint64_t h = 0;
    h = hash_combine(h, (uint64_t)gl_mode_to_vk(mode));
    h = hash_combine(h, s.tex_units[0].enabled ? 1 : 0);
    h = hash_combine(h, s.tex_units[0].env_mode);
    h = hash_combine(h, s.lighting_enabled ? 1 : 0);
    h = hash_combine(h, s.fog_enabled ? 1 : 0);
    h = hash_combine(h, s.alpha_test_enabled ? 1 : 0);
    h = hash_combine(h, s.alpha_func);
    h = hash_combine(h, s.blend_enabled ? 1 : 0);
    h = hash_combine(h, s.blend_src);
    h = hash_combine(h, s.blend_dst);
    h = hash_combine(h, s.depth_enabled ? 1 : 0);
    h = hash_combine(h, s.depth_func);
    h = hash_combine(h, s.depth_mask ? 1 : 0);
    h = hash_combine(h, s.cull_enabled ? 1 : 0);
    h = hash_combine(h, s.cull_mode);
    h = hash_combine(h, s.stencil_enabled ? 1 : 0);
    if (s.stencil_enabled) {
        h = hash_combine(h, s.stencil_func);
        h = hash_combine(h, s.stencil_sfail);
        h = hash_combine(h, s.stencil_dpfail);
        h = hash_combine(h, s.stencil_dppass);
    }
    // Color mask
    h = hash_combine(h, (s.color_mask_r ? 1 : 0) | (s.color_mask_g ? 2 : 0) |
                        (s.color_mask_b ? 4 : 0) | (s.color_mask_a ? 8 : 0));
    // Vertex array config (which arrays are enabled)
    h = hash_combine(h, s.vertex_array.enabled ? 1 : 0);
    h = hash_combine(h, s.texcoord_array.enabled ? 1 : 0);
    h = hash_combine(h, s.color_array.enabled ? 1 : 0);
    h = hash_combine(h, s.normal_array.enabled ? 1 : 0);
    return h;
}

// ============================================================================
// Vulkan blend factor mapping
// ============================================================================

static VkBlendFactor gl_to_vk_blend(uint32_t gl_factor) {
    switch (gl_factor) {
        case gl::ZERO:                return VK_BLEND_FACTOR_ZERO;
        case gl::ONE:                 return VK_BLEND_FACTOR_ONE;
        case gl::SRC_COLOR:           return VK_BLEND_FACTOR_SRC_COLOR;
        case gl::ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case gl::DST_COLOR:           return VK_BLEND_FACTOR_DST_COLOR;
        case gl::ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case gl::SRC_ALPHA:           return VK_BLEND_FACTOR_SRC_ALPHA;
        case gl::ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case gl::DST_ALPHA:           return VK_BLEND_FACTOR_DST_ALPHA;
        case gl::ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:                      return VK_BLEND_FACTOR_ONE;
    }
}

static VkCompareOp gl_to_vk_compare(uint32_t func) {
    switch (func) {
        case gl::NEVER:    return VK_COMPARE_OP_NEVER;
        case gl::LESS:     return VK_COMPARE_OP_LESS;
        case gl::EQUAL:    return VK_COMPARE_OP_EQUAL;
        case gl::LEQUAL:   return VK_COMPARE_OP_LESS_OR_EQUAL;
        case gl::GREATER:  return VK_COMPARE_OP_GREATER;
        case gl::NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case gl::GEQUAL:   return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case gl::ALWAYS:   return VK_COMPARE_OP_ALWAYS;
        default:           return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp gl_to_vk_stencil_op(uint32_t op) {
    switch (op) {
        case gl::KEEP:      return VK_STENCIL_OP_KEEP;
        case gl::ZERO:      return VK_STENCIL_OP_ZERO;
        case gl::REPLACE:   return VK_STENCIL_OP_REPLACE;
        case gl::INCR:      return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case gl::DECR:      return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case gl::INVERT:    return VK_STENCIL_OP_INVERT;
        case gl::INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case gl::DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default:            return VK_STENCIL_OP_KEEP;
    }
}

static int gl_alpha_func_to_spec(uint32_t func) {
    switch (func) {
        case gl::NEVER:    return 0;
        case gl::LESS:     return 1;
        case gl::EQUAL:    return 2;
        case gl::LEQUAL:   return 3;
        case gl::GREATER:  return 4;
        case gl::NOTEQUAL: return 5;
        case gl::GEQUAL:   return 6;
        case gl::ALWAYS:   return 7;
        default:           return 7;
    }
}

static int gl_tex_env_to_spec(uint32_t mode) {
    switch (mode) {
        case gl::TEXENV_MODULATE: return 0;
        case gl::TEXENV_REPLACE:  return 1;
        case gl::TEXENV_DECAL:    return 2;
        case gl::TEXENV_BLEND:    return 3;
        case gl::TEXENV_ADD:      return 4;
        default:                  return 0;
    }
}

// ============================================================================
// Pipeline creation (on-demand, cached by state hash)
// ============================================================================

VkPipeline VulkanBackend::get_or_create_pipeline(uint32_t mode) {
    uint64_t hash = compute_pipeline_hash(mode);
    auto it = pipeline_cache_map_.find(hash);
    if (it != pipeline_cache_map_.end()) return it->second;

    const auto& s = current_state_;

    // --- Specialization constants for fragment shader ---
    struct SpecData {
        int32_t texture_enabled;
        int32_t lighting_enabled;
        int32_t alpha_test_enabled;
        int32_t alpha_test_func;
        int32_t fog_enabled;
        int32_t tex_env_mode;
    };
    SpecData spec_data = {
        s.tex_units[0].enabled ? 1 : 0,
        s.lighting_enabled ? 1 : 0,
        s.alpha_test_enabled ? 1 : 0,
        gl_alpha_func_to_spec(s.alpha_func),
        s.fog_enabled ? 1 : 0,
        gl_tex_env_to_spec(s.tex_units[0].env_mode)
    };

    VkSpecializationMapEntry spec_entries[6];
    for (int i = 0; i < 6; i++) {
        spec_entries[i].constantID = i;
        spec_entries[i].offset = (uint32_t)(i * sizeof(int32_t));
        spec_entries[i].size = sizeof(int32_t);
    }
    VkSpecializationInfo spec_info = {};
    spec_info.mapEntryCount = 6;
    spec_info.pMapEntries = spec_entries;
    spec_info.dataSize = sizeof(spec_data);
    spec_info.pData = &spec_data;

    // --- Shader stages ---
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert_shader_;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag_shader_;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &spec_info;

    // --- Vertex input: interleaved {pos3, tc2, color4, normal3} = 12 floats = 48 bytes ---
    VkVertexInputBindingDescription binding = {};
    binding.binding = 0;
    binding.stride = 48; // 12 floats
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[4] = {};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0 };  // position
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT,      12 };  // texcoord
    attrs[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20 };  // color
    attrs[3] = { 3, 0, VK_FORMAT_R32G32B32_SFLOAT,   36 };  // normal

    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &binding;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = attrs;

    // --- Input assembly ---
    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = gl_mode_to_vk(mode);
    ia.primitiveRestartEnable = VK_FALSE;
    ia.primitiveRestartEnable = VK_FALSE;

    // --- Viewport (dynamic) ---
    VkPipelineViewportStateCreateInfo vps = {};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1;
    vps.scissorCount = 1;

    // --- Rasterization ---
    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = s.cull_enabled ?
        (s.cull_mode == gl::BACK ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT)
        : VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    // --- Multisample ---
    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // --- Depth/stencil ---
    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = s.depth_enabled ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = s.depth_mask ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = gl_to_vk_compare(s.depth_func);
    ds.stencilTestEnable = s.stencil_enabled ? VK_TRUE : VK_FALSE;
    if (s.stencil_enabled) {
        VkStencilOpState sop = {};
        sop.failOp = gl_to_vk_stencil_op(s.stencil_sfail);
        sop.depthFailOp = gl_to_vk_stencil_op(s.stencil_dpfail);
        sop.passOp = gl_to_vk_stencil_op(s.stencil_dppass);
        sop.compareOp = gl_to_vk_compare(s.stencil_func);
        sop.compareMask = s.stencil_mask;
        sop.writeMask = s.stencil_write_mask;
        sop.reference = (uint32_t)s.stencil_ref;
        ds.front = ds.back = sop;
    }

    // --- Color blend ---
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = 0;
    if (s.color_mask_r) cba.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (s.color_mask_g) cba.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (s.color_mask_b) cba.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (s.color_mask_a) cba.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = s.blend_enabled ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = gl_to_vk_blend(s.blend_src);
    cba.dstColorBlendFactor = gl_to_vk_blend(s.blend_dst);
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = gl_to_vk_blend(s.blend_src);
    cba.dstAlphaBlendFactor = gl_to_vk_blend(s.blend_dst);
    cba.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    // --- Dynamic state ---
    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    // --- Create pipeline ---
    VkGraphicsPipelineCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = pipeline_layout_;
    ci.renderPass = render_pass_;
    ci.subpass = 0;

    VkPipeline pipeline;
    VkResult res = vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &ci, nullptr, &pipeline);
    if (res != VK_SUCCESS) {
        std::cerr << "[VK] Failed to create pipeline (hash=0x" << std::hex << hash << std::dec << "): " << res << std::endl;
        return VK_NULL_HANDLE;
    }

    pipeline_cache_map_[hash] = pipeline;
    return pipeline;
}

// ============================================================================
// Helper: get type size in bytes
// ============================================================================

static int gl_type_size(uint32_t type) {
    switch (type) {
        case gl::FLOAT:          return 4;
        case gl::BYTE:           return 1;
        case gl::UNSIGNED_BYTE:  return 1;
        case gl::SHORT:          return 2;
        case gl::UNSIGNED_SHORT: return 2;
        case gl::FIXED:          return 4;
        default:                 return 4;
    }
}

// ============================================================================
// Helper: read vertex attribute from guest memory into interleaved float buffer
// ============================================================================

static void read_vertex_attrib(const VertexArrayPointer& ptr, int vertex_idx,
                               uint8_t* guest_mem, float* out, int out_components) {
    if (!ptr.enabled || ptr.pointer == 0) {
        // Fill with defaults
        for (int i = 0; i < out_components; i++) out[i] = (i < 3) ? 0.0f : 1.0f;
        return;
    }
    int elem_size = gl_type_size(ptr.type);
    int actual_stride = ptr.stride ? ptr.stride : (ptr.size * elem_size);
    const uint8_t* base = guest_mem + ptr.pointer + vertex_idx * actual_stride;

    for (int i = 0; i < out_components; i++) {
        if (i < ptr.size) {
            switch (ptr.type) {
                case gl::FLOAT:
                    out[i] = *(const float*)(base + i * 4);
                    break;
                case gl::BYTE:
                    out[i] = (float)(*(const int8_t*)(base + i)) / 127.0f;
                    break;
                case gl::UNSIGNED_BYTE:
                    out[i] = (float)(*(base + i)) / 255.0f;
                    break;
                case gl::SHORT:
                    out[i] = (float)(*(const int16_t*)(base + i * 2));
                    break;
                case gl::FIXED:
                    out[i] = (float)(*(const int32_t*)(base + i * 4)) / 65536.0f;
                    break;
                default:
                    out[i] = 0.0f;
                    break;
            }
        } else {
            out[i] = (i == 3) ? 1.0f : 0.0f; // w=1 for colors
        }
    }
}

// ============================================================================
// Draw calls — full implementation
// ============================================================================

void VulkanBackend::draw_arrays(uint32_t mode, int first, int count, uint8_t* guest_memory) {
    if (!in_render_pass_ || count <= 0 || !guest_memory) return;

    auto& sb = staging_buffers_[current_frame_];
    const size_t VERTEX_SIZE = 48; // 12 floats per vertex
    size_t data_size = (size_t)count * VERTEX_SIZE;

    // Check staging buffer capacity
    if (sb.offset + data_size > sb.capacity) {
        std::cerr << "[VK] Staging buffer overflow! Need " << data_size << " bytes" << std::endl;
        return;
    }

    // Interleave vertex data into staging buffer
    float* dst = (float*)((uint8_t*)sb.mapped_ptr + sb.offset);
    const auto& s = current_state_;

    for (int i = 0; i < count; i++) {
        int vi = first + i;
        float* v = dst + i * 12;
        // Position (3 floats)
        read_vertex_attrib(s.vertex_array, vi, guest_memory, v + 0, 3);
        // Texcoord (2 floats)
        read_vertex_attrib(s.texcoord_array, vi, guest_memory, v + 3, 2);
        // Color (4 floats)
        if (s.color_array.enabled) {
            read_vertex_attrib(s.color_array, vi, guest_memory, v + 5, 4);
        } else {
            v[5] = 1.0f; v[6] = 1.0f; v[7] = 1.0f; v[8] = 1.0f;
        }
        // Normal (3 floats)
        if (s.normal_array.enabled) {
            read_vertex_attrib(s.normal_array, vi, guest_memory, v + 9, 3);
        } else {
            v[9] = 0.0f; v[10] = 0.0f; v[11] = 1.0f;
        }
    }

    VkDeviceSize vb_offset = sb.offset;
    sb.offset += data_size;
    // Align to 16 bytes
    sb.offset = (sb.offset + 15) & ~15ULL;

    // Build push constants
    PushConstantBlock pc = {};
    mat4_multiply(pc.mvp, s.projection_stack[s.proj_depth], s.modelview_stack[s.mv_depth]);
    memcpy(pc.current_color, s.current_color, 4 * sizeof(float));

    // Fog params
    if (s.fog_enabled) {
        float fog_mode_f = 0.0f;
        if (s.fog_mode == gl::FOG_LINEAR)  fog_mode_f = 1.0f;
        else if (s.fog_mode == gl::FOG_EXP)  fog_mode_f = 2.0f;
        else if (s.fog_mode == gl::FOG_EXP2) fog_mode_f = 3.0f;
        pc.fog_params[0] = s.fog_start;
        pc.fog_params[1] = s.fog_end;
        pc.fog_params[2] = s.fog_density;
        pc.fog_params[3] = fog_mode_f;
        memcpy(pc.fog_color, s.fog_color, 4 * sizeof(float));
    }
    pc.alpha_ref = s.alpha_ref;

    // Get or create pipeline
    VkPipeline pipeline = get_or_create_pipeline(mode);
    if (pipeline == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = command_buffers_[current_frame_];

    // --- UBO: Build SceneData ---
    SceneData scene = {};
    memcpy(scene.modelview, s.modelview_stack[s.mv_depth], 16 * sizeof(float));
    calc_normal_matrix(scene.normal_matrix, s.modelview_stack[s.mv_depth]);
    memcpy(scene.light_ambient, s.light_model_ambient, 4 * sizeof(float));
    if (s.lighting_enabled && s.lights[0].enabled) {
        for(int j=0; j<4; j++) scene.light_ambient[j] += s.lights[0].ambient[j];
        memcpy(scene.light_diffuse, s.lights[0].diffuse, 4 * sizeof(float));
        memcpy(scene.light_dir, s.lights[0].position, 4 * sizeof(float));
    } else {
        scene.light_ambient[0] = scene.light_ambient[1] = scene.light_ambient[2] = scene.light_ambient[3] = 1.0f;
        scene.light_diffuse[0] = scene.light_diffuse[1] = scene.light_diffuse[2] = scene.light_diffuse[3] = 0.0f;
        scene.light_dir[0] = scene.light_dir[1] = scene.light_dir[2] = 0.0f; scene.light_dir[3] = 1.0f;
    }
    memcpy(scene.mat_ambient, s.material.ambient, 4 * sizeof(float));
    memcpy(scene.mat_diffuse, s.material.diffuse, 4 * sizeof(float));
    memcpy(scene.mat_emission, s.material.emission, 4 * sizeof(float));
    scene.tex_env_color[0] = scene.tex_env_color[1] = scene.tex_env_color[2] = scene.tex_env_color[3] = 1.0f;
    scene.alpha_ref = s.alpha_ref;

    // Write UBO to staging buffer with alignment
    VkDeviceSize ubo_offset = (sb.offset + min_ubo_alignment_ - 1) & ~((VkDeviceSize)min_ubo_alignment_ - 1);
    if (ubo_offset + sizeof(SceneData) > sb.capacity) {
        std::cerr << "[VK] Staging buffer overflow (UBO)!" << std::endl;
        return;
    }
    memcpy((uint8_t*)sb.mapped_ptr + ubo_offset, &scene, sizeof(SceneData));
    sb.offset = ubo_offset + sizeof(SceneData);

    // Bind pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Push constants
    vkCmdPushConstants(cmd, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstantBlock), &pc);

    // Bind descriptor sets
    // Set 0: Texture
    uint32_t bound_tex = s.tex_units[0].bound_texture;
    auto tex_it = textures_.find(bound_tex);
    if (tex_it == textures_.end()) tex_it = textures_.find(default_texture_id_);
    if (tex_it != textures_.end() && tex_it->second.descriptor_set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &tex_it->second.descriptor_set, 0, nullptr);
    }
    // Set 1: UBO (Dynamic)
    uint32_t dynamic_offset = (uint32_t)ubo_offset;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            1, 1, &ubo_descriptor_sets_[current_frame_], 1, &dynamic_offset);

    // Bind vertex buffer
    vkCmdBindVertexBuffers(cmd, 0, 1, &sb.buffer, &vb_offset);

    // Draw
    vkCmdDraw(cmd, count, 1, 0, 0);
    draw_call_count_++;
}

void VulkanBackend::draw_elements(uint32_t mode, int count, uint32_t type,
                                    uint32_t indices_ptr, uint8_t* guest_memory) {
    if (!in_render_pass_ || count <= 0 || !guest_memory) return;

    // Read index data to find max vertex index
    int idx_size = (type == gl::UNSIGNED_SHORT) ? 2 : 1;
    const uint8_t* idx_data = guest_memory + indices_ptr;

    int max_idx = 0;
    for (int i = 0; i < count; i++) {
        int idx;
        if (type == gl::UNSIGNED_SHORT)
            idx = ((const uint16_t*)idx_data)[i];
        else
            idx = idx_data[i];
        if (idx > max_idx) max_idx = idx;
    }
    int vertex_count = max_idx + 1;

    auto& sb = staging_buffers_[current_frame_];
    const size_t VERTEX_SIZE = 48;
    size_t vb_size = (size_t)vertex_count * VERTEX_SIZE;
    // Use uint16_t indices for Vulkan (convert if needed)
    size_t ib_size = (size_t)count * sizeof(uint16_t);
    size_t total = vb_size + ib_size;

    if (sb.offset + total > sb.capacity) {
        std::cerr << "[VK] Staging buffer overflow in draw_elements!" << std::endl;
        return;
    }

    // Write vertex data
    float* vdst = (float*)((uint8_t*)sb.mapped_ptr + sb.offset);
    const auto& s = current_state_;
    for (int i = 0; i < vertex_count; i++) {
        float* v = vdst + i * 12;
        read_vertex_attrib(s.vertex_array, i, guest_memory, v + 0, 3);
        read_vertex_attrib(s.texcoord_array, i, guest_memory, v + 3, 2);
        if (s.color_array.enabled) {
            read_vertex_attrib(s.color_array, i, guest_memory, v + 5, 4);
        } else {
            v[5] = 1.0f; v[6] = 1.0f; v[7] = 1.0f; v[8] = 1.0f;
        }
        if (s.normal_array.enabled) {
            read_vertex_attrib(s.normal_array, i, guest_memory, v + 9, 3);
        } else {
            v[9] = 0.0f; v[10] = 0.0f; v[11] = 1.0f;
        }
    }

    VkDeviceSize vb_offset = sb.offset;
    sb.offset += vb_size;
    sb.offset = (sb.offset + 15) & ~15ULL;

    // Write index data as uint16_t
    uint16_t* idst = (uint16_t*)((uint8_t*)sb.mapped_ptr + sb.offset);
    for (int i = 0; i < count; i++) {
        if (type == gl::UNSIGNED_SHORT)
            idst[i] = ((const uint16_t*)idx_data)[i];
        else
            idst[i] = (uint16_t)idx_data[i];
    }
    VkDeviceSize ib_offset = sb.offset;
    sb.offset += ib_size;
    sb.offset = (sb.offset + 15) & ~15ULL;

    // Build push constants
    PushConstantBlock pc = {};
    mat4_multiply(pc.mvp, s.projection_stack[s.proj_depth], s.modelview_stack[s.mv_depth]);
    memcpy(pc.current_color, s.current_color, 4 * sizeof(float));
    if (s.fog_enabled) {
        float fog_mode_f = 0.0f;
        if (s.fog_mode == gl::FOG_LINEAR)  fog_mode_f = 1.0f;
        else if (s.fog_mode == gl::FOG_EXP)  fog_mode_f = 2.0f;
        else if (s.fog_mode == gl::FOG_EXP2) fog_mode_f = 3.0f;
        pc.fog_params[0] = s.fog_start;
        pc.fog_params[1] = s.fog_end;
        pc.fog_params[2] = s.fog_density;
        pc.fog_params[3] = fog_mode_f;
        memcpy(pc.fog_color, s.fog_color, 4 * sizeof(float));
    }
    pc.alpha_ref = s.alpha_ref;

    VkPipeline pipeline = get_or_create_pipeline(mode);
    if (pipeline == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = command_buffers_[current_frame_];

    // --- UBO: Build SceneData ---
    SceneData scene = {};
    memcpy(scene.modelview, s.modelview_stack[s.mv_depth], 16 * sizeof(float));
    calc_normal_matrix(scene.normal_matrix, s.modelview_stack[s.mv_depth]);
    memcpy(scene.light_ambient, s.light_model_ambient, 4 * sizeof(float));
    if (s.lighting_enabled && s.lights[0].enabled) {
        for(int j=0; j<4; j++) scene.light_ambient[j] += s.lights[0].ambient[j];
        memcpy(scene.light_diffuse, s.lights[0].diffuse, 4 * sizeof(float));
        memcpy(scene.light_dir, s.lights[0].position, 4 * sizeof(float));
    } else {
        scene.light_ambient[0] = scene.light_ambient[1] = scene.light_ambient[2] = scene.light_ambient[3] = 1.0f;
        scene.light_diffuse[0] = scene.light_diffuse[1] = scene.light_diffuse[2] = scene.light_diffuse[3] = 0.0f;
        scene.light_dir[0] = scene.light_dir[1] = scene.light_dir[2] = 0.0f; scene.light_dir[3] = 1.0f;
    }
    memcpy(scene.mat_ambient, s.material.ambient, 4 * sizeof(float));
    memcpy(scene.mat_diffuse, s.material.diffuse, 4 * sizeof(float));
    memcpy(scene.mat_emission, s.material.emission, 4 * sizeof(float));
    scene.tex_env_color[0] = scene.tex_env_color[1] = scene.tex_env_color[2] = scene.tex_env_color[3] = 1.0f;
    scene.alpha_ref = s.alpha_ref;

    // Write UBO to staging buffer with alignment
    VkDeviceSize ubo_offset = (sb.offset + min_ubo_alignment_ - 1) & ~((VkDeviceSize)min_ubo_alignment_ - 1);
    if (ubo_offset + sizeof(SceneData) > sb.capacity) {
        std::cerr << "[VK] Staging buffer overflow (UBO)!" << std::endl;
        return;
    }
    memcpy((uint8_t*)sb.mapped_ptr + ubo_offset, &scene, sizeof(SceneData));
    sb.offset = ubo_offset + sizeof(SceneData);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdPushConstants(cmd, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(PushConstantBlock), &pc);

    // Bind descriptor sets
    // Set 0: Texture
    uint32_t bound_tex = s.tex_units[0].bound_texture;
    auto tex_it = textures_.find(bound_tex);
    if (tex_it == textures_.end()) tex_it = textures_.find(default_texture_id_);
    if (tex_it != textures_.end() && tex_it->second.descriptor_set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                                0, 1, &tex_it->second.descriptor_set, 0, nullptr);
    }
    // Set 1: UBO (Dynamic)
    uint32_t dynamic_offset = (uint32_t)ubo_offset;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                            1, 1, &ubo_descriptor_sets_[current_frame_], 1, &dynamic_offset);

    // Bind vertex + index buffers
    vkCmdBindVertexBuffers(cmd, 0, 1, &sb.buffer, &vb_offset);
    vkCmdBindIndexBuffer(cmd, sb.buffer, ib_offset, VK_INDEX_TYPE_UINT16);

    // Draw indexed
    vkCmdDrawIndexed(cmd, count, 1, 0, 0, 0);
    draw_call_count_++;
}

// ============================================================================
// Misc
// ============================================================================

void VulkanBackend::flush() { /* Vulkan doesn't need explicit flush */ }
void VulkanBackend::finish() {
    if (device_) vkDeviceWaitIdle(device_);
}

void VulkanBackend::pixel_storei(uint32_t pname, int param) { (void)pname; (void)param; }
void VulkanBackend::clear_depthf(float d) { (void)d; }
void VulkanBackend::line_width(float w) { (void)w; }
void VulkanBackend::point_size(float s) { (void)s; }
void VulkanBackend::hint(uint32_t target, uint32_t mode) { (void)target; (void)mode; }

// ============================================================================
// Destroy / Cleanup
// ============================================================================

void VulkanBackend::destroy() {
    if (!initialized_) return;
    vkDeviceWaitIdle(device_);

    // Destroy textures
    for (auto& [id, tex] : textures_) {
        if (tex.descriptor_set != VK_NULL_HANDLE)
            vkFreeDescriptorSets(device_, descriptor_pool_, 1, &tex.descriptor_set);
        if (tex.sampler != VK_NULL_HANDLE) vkDestroySampler(device_, tex.sampler, nullptr);
        if (tex.view != VK_NULL_HANDLE) vkDestroyImageView(device_, tex.view, nullptr);
        if (tex.image != VK_NULL_HANDLE) vmaDestroyImage(allocator_, tex.image, tex.allocation);
    }
    textures_.clear();

    // Destroy staging buffers
    for (auto& sb : staging_buffers_) {
        vmaDestroyBuffer(allocator_, sb.buffer, sb.allocation);
    }

    // Destroy pipelines
    for (auto& [hash, pipe] : pipeline_cache_map_) {
        vkDestroyPipeline(device_, pipe, nullptr);
    }

    // Destroy shader modules
    if (vert_shader_) vkDestroyShaderModule(device_, vert_shader_, nullptr);
    if (frag_shader_) vkDestroyShaderModule(device_, frag_shader_, nullptr);

    // Destroy Vulkan objects
    vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
    if (descriptor_set_layout_) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }
    if (ubo_set_layout_) {
        vkDestroyDescriptorSetLayout(device_, ubo_set_layout_, nullptr);
        ubo_set_layout_ = VK_NULL_HANDLE;
    }
    vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);

    for (auto& fb : swapchain_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    vkDestroyImageView(device_, depth_image_view_, nullptr);
    vmaDestroyImage(allocator_, depth_image_, depth_allocation_);
    vkDestroyRenderPass(device_, render_pass_, nullptr);

    for (auto& iv : swapchain_image_views_) vkDestroyImageView(device_, iv, nullptr);
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        vkDestroySemaphore(device_, image_available_[i], nullptr);
        vkDestroySemaphore(device_, render_finished_[i], nullptr);
        vkDestroyFence(device_, in_flight_fences_[i], nullptr);
    }

    vkDestroyCommandPool(device_, command_pool_, nullptr);
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);

    initialized_ = false;
    std::cout << "[VK] Vulkan backend destroyed" << std::endl;
}
