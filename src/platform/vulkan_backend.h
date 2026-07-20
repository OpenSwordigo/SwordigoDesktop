// Vulkan backend for Swordigo Desktop — GLES 1.x fixed-function pipeline emulator

#ifndef VULKAN_BACKEND_H
#define VULKAN_BACKEND_H

#include <vulkan/vulkan.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vector>
#include <unordered_map>
#include <array>
#include <cstring>
#include <cstdint>

// ---------------------------------------------------------------------------
// Runtime graphics API selection
// ---------------------------------------------------------------------------

enum class GraphicsAPI {
    OPENGL,
    VULKAN
};

// ---------------------------------------------------------------------------
// GL enum constants we need to track (mirrors GLES 1.x values)
// ---------------------------------------------------------------------------

namespace gl {

// Matrix modes
constexpr uint32_t MODELVIEW  = 0x1700;
constexpr uint32_t PROJECTION = 0x1701;

// Primitive types
constexpr uint32_t TRIANGLES      = 0x0004;
constexpr uint32_t TRIANGLE_STRIP = 0x0005;
constexpr uint32_t TRIANGLE_FAN   = 0x0006;

// Blend factors
constexpr uint32_t ZERO                = 0x0000;
constexpr uint32_t ONE                 = 0x0001;
constexpr uint32_t SRC_COLOR           = 0x0300;
constexpr uint32_t ONE_MINUS_SRC_COLOR = 0x0301;
constexpr uint32_t SRC_ALPHA           = 0x0302;
constexpr uint32_t ONE_MINUS_SRC_ALPHA = 0x0303;
constexpr uint32_t DST_ALPHA           = 0x0304;
constexpr uint32_t ONE_MINUS_DST_ALPHA = 0x0305;
constexpr uint32_t DST_COLOR           = 0x0306;
constexpr uint32_t ONE_MINUS_DST_COLOR = 0x0307;
constexpr uint32_t SRC_ALPHA_SATURATE  = 0x0308;

// Comparison functions (depth, alpha, stencil)
constexpr uint32_t NEVER    = 0x0200;
constexpr uint32_t LESS     = 0x0201;
constexpr uint32_t EQUAL    = 0x0202;
constexpr uint32_t LEQUAL   = 0x0203;
constexpr uint32_t GREATER  = 0x0204;
constexpr uint32_t NOTEQUAL = 0x0205;
constexpr uint32_t GEQUAL   = 0x0206;
constexpr uint32_t ALWAYS   = 0x0207;

// Stencil ops
constexpr uint32_t KEEP      = 0x1E00;
constexpr uint32_t REPLACE   = 0x1E01;
constexpr uint32_t INCR      = 0x1E02;
constexpr uint32_t DECR      = 0x1E03;
constexpr uint32_t INVERT    = 0x150A;
constexpr uint32_t INCR_WRAP = 0x8507;
constexpr uint32_t DECR_WRAP = 0x8508;

// Cull face
constexpr uint32_t FRONT          = 0x0404;
constexpr uint32_t BACK           = 0x0405;
constexpr uint32_t FRONT_AND_BACK = 0x0408;

// Shade model
constexpr uint32_t FLAT   = 0x1D00;
constexpr uint32_t SMOOTH = 0x1D01;

// Fog modes
constexpr uint32_t FOG_LINEAR = 0x2601;
constexpr uint32_t FOG_EXP    = 0x0800;
constexpr uint32_t FOG_EXP2   = 0x0801;

// Texture env modes
constexpr uint32_t TEXENV_MODULATE  = 0x2100;
constexpr uint32_t TEXENV_DECAL     = 0x2101;
constexpr uint32_t TEXENV_BLEND     = 0x0BE2;
constexpr uint32_t TEXENV_REPLACE   = 0x1E01;
constexpr uint32_t TEXENV_ADD       = 0x0104;
constexpr uint32_t TEXENV_COMBINE   = 0x8570;

// Texture parameters
constexpr uint32_t TEXTURE_MIN_FILTER = 0x2801;
constexpr uint32_t TEXTURE_MAG_FILTER = 0x2800;
constexpr uint32_t TEXTURE_WRAP_S     = 0x2802;
constexpr uint32_t TEXTURE_WRAP_T     = 0x2803;
constexpr uint32_t NEAREST            = 0x2600;
constexpr uint32_t LINEAR             = 0x2601;
constexpr uint32_t NEAREST_MIPMAP_NEAREST = 0x2700;
constexpr uint32_t LINEAR_MIPMAP_NEAREST  = 0x2701;
constexpr uint32_t NEAREST_MIPMAP_LINEAR  = 0x2702;
constexpr uint32_t LINEAR_MIPMAP_LINEAR   = 0x2703;
constexpr uint32_t CLAMP_TO_EDGE      = 0x812F;
constexpr uint32_t REPEAT             = 0x2901;

// Index types
constexpr uint32_t UNSIGNED_BYTE  = 0x1401;
constexpr uint32_t UNSIGNED_SHORT = 0x1403;

// Clear bits
constexpr uint32_t COLOR_BUFFER_BIT   = 0x00004000;
constexpr uint32_t DEPTH_BUFFER_BIT   = 0x00000100;
constexpr uint32_t STENCIL_BUFFER_BIT = 0x00000400;

// Data types (for vertex pointers)
constexpr uint32_t BYTE           = 0x1400;
constexpr uint32_t FLOAT          = 0x1406;
constexpr uint32_t FIXED          = 0x140C;
constexpr uint32_t SHORT          = 0x1402;

// Pixel formats
constexpr uint32_t ALPHA           = 0x1906;
constexpr uint32_t RGB             = 0x1907;
constexpr uint32_t RGBA            = 0x1908;
constexpr uint32_t LUMINANCE       = 0x1909;
constexpr uint32_t LUMINANCE_ALPHA = 0x190A;

// Texture targets
constexpr uint32_t TEXTURE_2D = 0x0DE1;

// Active texture
constexpr uint32_t TEXTURE0 = 0x84C0;
constexpr uint32_t TEXTURE1 = 0x84C1;

// Enable/disable caps (also usable as gl::BLEND, gl::LIGHTING etc.)
constexpr uint32_t BLEND        = 0x0BE2;
constexpr uint32_t DEPTH_TEST   = 0x0B71;
constexpr uint32_t ALPHA_TEST   = 0x0BC0;
constexpr uint32_t SCISSOR_TEST = 0x0C11;
constexpr uint32_t STENCIL_TEST = 0x0B90;
constexpr uint32_t CULL_FACE    = 0x0B44;
constexpr uint32_t LIGHTING     = 0x0B50;
constexpr uint32_t FOG          = 0x0B60;
constexpr uint32_t LIGHT0       = 0x4000;
constexpr uint32_t COLOR_MATERIAL = 0x0B57;
constexpr uint32_t NORMALIZE     = 0x0BA1;

// Fog parameters
constexpr uint32_t FOG_MODE    = 0x0B65;
constexpr uint32_t FOG_DENSITY = 0x0B62;
constexpr uint32_t FOG_START   = 0x0B63;
constexpr uint32_t FOG_END     = 0x0B64;
constexpr uint32_t FOG_COLOR   = 0x0B66;

// Client state arrays
constexpr uint32_t VERTEX_ARRAY        = 0x8074;
constexpr uint32_t TEXTURE_COORD_ARRAY = 0x8078;
constexpr uint32_t COLOR_ARRAY         = 0x8076;
constexpr uint32_t NORMAL_ARRAY        = 0x8075;

// Light parameters
constexpr uint32_t AMBIENT  = 0x1200;
constexpr uint32_t DIFFUSE  = 0x1201;
constexpr uint32_t SPECULAR = 0x1202;
constexpr uint32_t POSITION = 0x1203;
constexpr uint32_t SPOT_DIRECTION  = 0x1204;
constexpr uint32_t SPOT_EXPONENT   = 0x1205;
constexpr uint32_t SPOT_CUTOFF     = 0x1206;
constexpr uint32_t CONSTANT_ATTENUATION  = 0x1207;
constexpr uint32_t LINEAR_ATTENUATION    = 0x1208;
constexpr uint32_t QUADRATIC_ATTENUATION = 0x1209;

// Material parameters
constexpr uint32_t EMISSION  = 0x1600;
constexpr uint32_t SHININESS = 0x1601;
constexpr uint32_t AMBIENT_AND_DIFFUSE = 0x1602;

// Light model
constexpr uint32_t LIGHT_MODEL_AMBIENT      = 0x0B53;
constexpr uint32_t LIGHT_MODEL_TWO_SIDE     = 0x0B52;

constexpr int MAX_TEXTURE_UNITS = 2;

} // namespace gl

// ---------------------------------------------------------------------------
// Vertex array pointer descriptor
// ---------------------------------------------------------------------------

struct VertexArrayPointer {
    bool     enabled  = false;
    int      size     = 4;       // number of components (2, 3, or 4)
    uint32_t type     = gl::FLOAT;
    int      stride   = 0;
    uint32_t pointer  = 0;       // guest-memory offset (not a host pointer)
};

// ---------------------------------------------------------------------------
// Light state (per-light, up to 8)
// ---------------------------------------------------------------------------

struct LightParams {
    bool  enabled = false;
    float position[4]       = { 0.0f, 0.0f, 1.0f, 0.0f };
    float ambient[4]        = { 0.0f, 0.0f, 0.0f, 1.0f };
    float diffuse[4]        = { 0.0f, 0.0f, 0.0f, 1.0f }; // GL default for light >0
    float specular[4]       = { 0.0f, 0.0f, 0.0f, 1.0f };
    float spot_direction[3] = { 0.0f, 0.0f, -1.0f };
    float spot_exponent     = 0.0f;
    float spot_cutoff       = 180.0f;
    float constant_atten    = 1.0f;
    float linear_atten      = 0.0f;
    float quadratic_atten   = 0.0f;
};

// ---------------------------------------------------------------------------
// Material state
// ---------------------------------------------------------------------------

struct MaterialParams {
    float ambient[4]  = { 0.2f, 0.2f, 0.2f, 1.0f };
    float diffuse[4]  = { 0.8f, 0.8f, 0.8f, 1.0f };
    float specular[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float emission[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    float shininess   = 0.0f;
};

// ---------------------------------------------------------------------------
// FixedFunctionState — mirrors all tracked GLES 1.x state
// ---------------------------------------------------------------------------

struct FixedFunctionState {

    // --- Matrix stacks ---
    static constexpr int MAX_MV_DEPTH   = 32;
    static constexpr int MAX_PROJ_DEPTH = 4;

    float    modelview_stack[MAX_MV_DEPTH][16];
    int      mv_depth = 0;

    float    projection_stack[MAX_PROJ_DEPTH][16];
    int      proj_depth = 0;

    uint32_t matrix_mode = gl::MODELVIEW;

    // --- Current color (glColor4f) ---
    float current_color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    // --- Vertex array pointers ---
    VertexArrayPointer vertex_array;
    VertexArrayPointer texcoord_array;
    VertexArrayPointer color_array;
    VertexArrayPointer normal_array;

    // --- Lighting ---
    bool           lighting_enabled = false;
    LightParams    lights[8];
    MaterialParams material;
    float          light_model_ambient[4] = { 0.2f, 0.2f, 0.2f, 1.0f };

    // --- Fog ---
    bool     fog_enabled = false;
    uint32_t fog_mode    = gl::FOG_EXP;
    float    fog_color[4]  = { 0.0f, 0.0f, 0.0f, 0.0f };
    float    fog_density   = 1.0f;
    float    fog_start     = 0.0f;
    float    fog_end       = 1.0f;

    // --- Blending ---
    bool     blend_enabled = false;
    uint32_t blend_src     = gl::ONE;
    uint32_t blend_dst     = gl::ZERO;

    // --- Depth ---
    bool     depth_enabled = false;
    uint32_t depth_func    = gl::LESS;
    bool     depth_mask    = true;

    // --- Alpha test ---
    bool     alpha_test_enabled = false;
    uint32_t alpha_func         = gl::ALWAYS;
    float    alpha_ref          = 0.0f;

    // --- Stencil ---
    bool     stencil_enabled    = false;
    uint32_t stencil_func       = gl::ALWAYS;
    int      stencil_ref        = 0;
    uint32_t stencil_mask       = 0xFFFFFFFF;
    uint32_t stencil_write_mask = 0xFFFFFFFF;
    uint32_t stencil_sfail      = gl::KEEP;
    uint32_t stencil_dpfail     = gl::KEEP;
    uint32_t stencil_dppass     = gl::KEEP;

    // --- Scissor ---
    bool scissor_enabled = false;

    // --- Culling ---
    bool     cull_enabled = false;
    uint32_t cull_mode    = gl::BACK;

    // --- Color mask ---
    bool color_mask_r = true;
    bool color_mask_g = true;
    bool color_mask_b = true;
    bool color_mask_a = true;

    // --- Shade model ---
    uint32_t shade_model = gl::SMOOTH;

    // --- Per-texture-unit state ---
    struct TexUnit {
        bool     enabled        = false;
        uint32_t bound_texture  = 0;
        uint32_t env_mode       = gl::TEXENV_MODULATE;
        float    env_color[4]   = { 0, 0, 0, 0 };
    };
    int     active_texture = 0;
    TexUnit tex_units[gl::MAX_TEXTURE_UNITS];

    // --- Clear color ---
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    static void identity(float m[16]) {
        memset(m, 0, sizeof(float) * 16);
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    void reset() {
        *this = FixedFunctionState{};
        identity(modelview_stack[0]);
        identity(projection_stack[0]);
        lights[0].diffuse[0]  = 1.0f; lights[0].diffuse[1]  = 1.0f;
        lights[0].diffuse[2]  = 1.0f; lights[0].diffuse[3]  = 1.0f;
        lights[0].specular[0] = 1.0f; lights[0].specular[1] = 1.0f;
        lights[0].specular[2] = 1.0f; lights[0].specular[3] = 1.0f;
    }

    float* current_matrix() {
        return (matrix_mode == gl::PROJECTION)
            ? projection_stack[proj_depth]
            : modelview_stack[mv_depth];
    }
    const float* current_matrix() const {
        return (matrix_mode == gl::PROJECTION)
            ? projection_stack[proj_depth]
            : modelview_stack[mv_depth];
    }
};

// ---------------------------------------------------------------------------
// VulkanTexture — per-texture GPU resources
// ---------------------------------------------------------------------------

struct VmaAllocation_T; // forward-declare VMA opaque handle
typedef VmaAllocation_T* VmaAllocation;

struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;

struct VulkanTexture {
    VkImage           image          = VK_NULL_HANDLE;
    VmaAllocation     allocation     = VK_NULL_HANDLE;
    VkImageView       view           = VK_NULL_HANDLE;
    VkSampler         sampler        = VK_NULL_HANDLE;
    VkDescriptorSet   descriptor_set = VK_NULL_HANDLE;
    uint32_t          width          = 0;
    uint32_t          height         = 0;
    VkFormat          format         = VK_FORMAT_R8G8B8A8_UNORM;

    // Tracked sampler params so we can recreate sampler on glTexParameteri
    uint32_t min_filter = gl::NEAREST_MIPMAP_LINEAR;
    uint32_t mag_filter = gl::LINEAR;
    uint32_t wrap_s     = gl::REPEAT;
    uint32_t wrap_t     = gl::REPEAT;
};

// ---------------------------------------------------------------------------
// Staging buffer — used to upload vertex/index/texture data each frame
// ---------------------------------------------------------------------------

struct StagingBuffer {
    VkBuffer       buffer     = VK_NULL_HANDLE;
    VmaAllocation  allocation = VK_NULL_HANDLE;
    void*          mapped_ptr = nullptr;
    size_t         offset     = 0;
    size_t         capacity   = 0;
};

// ---------------------------------------------------------------------------
// VulkanBackend — the main interface
// ---------------------------------------------------------------------------

class VulkanBackend {
public:
    VulkanBackend() = default;
    ~VulkanBackend() = default;

    // Non-copyable
    VulkanBackend(const VulkanBackend&) = delete;
    VulkanBackend& operator=(const VulkanBackend&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------
    bool init(SDL_Window* window, int game_w, int game_h);
    void destroy();

    void begin_frame();
    void end_frame_and_present();

    // -----------------------------------------------------------------------
    // Matrix operations
    // -----------------------------------------------------------------------
    void matrix_mode(uint32_t mode);
    void load_identity();
    void load_matrixf(const float m[16]);
    void mult_matrixf(const float m[16]);
    void push_matrix();
    void pop_matrix();
    void translatef(float x, float y, float z);
    void rotatef(float angle, float x, float y, float z);
    void scalef(float x, float y, float z);
    void orthof(float left, float right, float bottom, float top,
                float near_val, float far_val);
    void frustumf(float left, float right, float bottom, float top,
                  float near_val, float far_val);

    // -----------------------------------------------------------------------
    // Vertex array pointers
    // -----------------------------------------------------------------------
    void vertex_pointer(int size, uint32_t type, int stride, uint32_t pointer);
    void texcoord_pointer(int size, uint32_t type, int stride, uint32_t pointer);
    void color_pointer(int size, uint32_t type, int stride, uint32_t pointer);
    void normal_pointer(uint32_t type, int stride, uint32_t pointer);

    void enable_client_state(uint32_t cap);
    void disable_client_state(uint32_t cap);
    void client_active_texture(uint32_t texture);

    // -----------------------------------------------------------------------
    // State setting (glEnable / glDisable family)
    // -----------------------------------------------------------------------
    void enable(uint32_t cap);
    void disable(uint32_t cap);

    void blend_func(uint32_t sfactor, uint32_t dfactor);
    void depth_func(uint32_t func);
    void depth_mask(bool flag);
    void alpha_func(uint32_t func, float ref);
    void cull_face(uint32_t mode);

    void stencil_func(uint32_t func, int ref, uint32_t mask);
    void stencil_mask(uint32_t mask);
    void stencil_op(uint32_t sfail, uint32_t dpfail, uint32_t dppass);

    void color_mask(bool r, bool g, bool b, bool a);
    void scissor(int x, int y, int w, int h);
    void viewport(int x, int y, int w, int h);

    void shade_model(uint32_t mode);
    void color4f(float r, float g, float b, float a);
    void color4ub(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void active_texture(uint32_t texture);

    // -----------------------------------------------------------------------
    // Lighting
    // -----------------------------------------------------------------------
    void lightf(uint32_t light, uint32_t pname, float param);
    void lightfv(uint32_t light, uint32_t pname, const float* params);
    void materialf(uint32_t face, uint32_t pname, float param);
    void materialfv(uint32_t face, uint32_t pname, const float* params);
    void light_modelf(uint32_t pname, float param);
    void light_modelfv(uint32_t pname, const float* params);

    // -----------------------------------------------------------------------
    // Fog
    // -----------------------------------------------------------------------
    void fogf(uint32_t pname, float param);
    void fogfv(uint32_t pname, const float* params);
    void fogi(uint32_t pname, int param);

    // -----------------------------------------------------------------------
    // Texture environment
    // -----------------------------------------------------------------------
    void tex_envi(uint32_t target, uint32_t pname, int param);
    void tex_envfv(uint32_t target, uint32_t pname, const float* params);

    // -----------------------------------------------------------------------
    // Texture management
    // -----------------------------------------------------------------------
    void gen_textures(int n, uint32_t* textures);
    void delete_textures(int n, const uint32_t* textures);
    void bind_texture(uint32_t target, uint32_t texture);
    void tex_image_2d(uint32_t target, int level, int internal_format,
                      int width, int height, int border,
                      uint32_t format, uint32_t type, const void* pixels);
    void tex_sub_image_2d(uint32_t target, int level,
                          int xoffset, int yoffset,
                          int width, int height,
                          uint32_t format, uint32_t type, const void* pixels);
    void tex_parameteri(uint32_t target, uint32_t pname, int param);

    // -----------------------------------------------------------------------
    // Drawing
    // -----------------------------------------------------------------------
    void draw_arrays(uint32_t mode, int first, int count,
                     uint8_t* guest_memory);
    void draw_elements(uint32_t mode, int count, uint32_t type,
                       uint32_t indices_ptr, uint8_t* guest_memory);

    // -----------------------------------------------------------------------
    // Clear
    // -----------------------------------------------------------------------
    void clear(uint32_t mask);
    void clear_color(float r, float g, float b, float a);

    // -----------------------------------------------------------------------
    // Synchronisation
    // -----------------------------------------------------------------------
    void flush();
    void finish();

    // -----------------------------------------------------------------------
    // Misc GL stubs
    // -----------------------------------------------------------------------
    void pixel_storei(uint32_t pname, int param);
    void clear_depthf(float depth);
    void line_width(float width);
    void point_size(float size);
    void hint(uint32_t target, uint32_t mode);
    void tex_parameterf(uint32_t target, uint32_t pname, float param);

    // -----------------------------------------------------------------------
    // State accessor
    // -----------------------------------------------------------------------
    FixedFunctionState&       state()       { return current_state_; }
    const FixedFunctionState& state() const { return current_state_; }

    // -----------------------------------------------------------------------
    // Expose Vulkan handles for Dear ImGui integration
    // -----------------------------------------------------------------------
    VkInstance get_instance() const { return instance_; }
    VkPhysicalDevice get_physical_device() const { return physical_device_; }
    VkDevice get_device() const { return device_; }
    VkQueue get_graphics_queue() const { return graphics_queue_; }
    uint32_t get_graphics_family() const { return graphics_family_; }
    VkRenderPass get_render_pass() const { return render_pass_; }
    VkDescriptorPool get_descriptor_pool() const { return descriptor_pool_; }
    VkCommandBuffer get_current_command_buffer() const { return command_buffers_[current_frame_]; }
    uint32_t get_image_count() const { return (uint32_t)swapchain_images_.size(); }
    VkPipelineCache get_pipeline_cache() const { return pipeline_cache_; }

private:
    // -----------------------------------------------------------------------
    // Vulkan core handles
    // -----------------------------------------------------------------------
    VkInstance       instance_        = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkDevice         device_          = VK_NULL_HANDLE;
    VkQueue          graphics_queue_  = VK_NULL_HANDLE;
    VkQueue          present_queue_   = VK_NULL_HANDLE;
    uint32_t         graphics_family_ = 0;
    uint32_t         present_family_  = 0;

    VkSurfaceKHR     surface_         = VK_NULL_HANDLE;

    // -----------------------------------------------------------------------
    // Swapchain
    // -----------------------------------------------------------------------
    VkSwapchainKHR              swapchain_         = VK_NULL_HANDLE;
    VkSurfaceFormatKHR          swapchain_format_  = {};
    VkExtent2D                  swapchain_extent_  = { 0, 0 };
    std::vector<VkImage>        swapchain_images_;
    std::vector<VkImageView>    swapchain_image_views_;
    std::vector<VkFramebuffer>  swapchain_framebuffers_;

    // -----------------------------------------------------------------------
    // Depth / stencil attachment
    // -----------------------------------------------------------------------
    VkImage        depth_image_      = VK_NULL_HANDLE;
    VmaAllocation  depth_allocation_ = VK_NULL_HANDLE;
    VkImageView    depth_image_view_ = VK_NULL_HANDLE;

    // -----------------------------------------------------------------------
    // Render pass & pipeline infrastructure
    // -----------------------------------------------------------------------
    VkRenderPass     render_pass_     = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipelineCache  pipeline_cache_  = VK_NULL_HANDLE;

    VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool      descriptor_pool_       = VK_NULL_HANDLE;

    // -----------------------------------------------------------------------
    // Command recording
    // -----------------------------------------------------------------------
    VkCommandPool                 command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer>  command_buffers_;

    // -----------------------------------------------------------------------
    // Synchronisation primitives (per frame-in-flight)
    // -----------------------------------------------------------------------
    std::vector<VkSemaphore> image_available_;
    std::vector<VkSemaphore> render_finished_;
    std::vector<VkFence>     in_flight_fences_;

    // -----------------------------------------------------------------------
    // VMA allocator
    // -----------------------------------------------------------------------
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // -----------------------------------------------------------------------
    // Staging buffers (one per frame-in-flight)
    // -----------------------------------------------------------------------
    std::vector<StagingBuffer> staging_buffers_;

    // -----------------------------------------------------------------------
    // Texture storage
    // -----------------------------------------------------------------------
    std::unordered_map<uint32_t, VulkanTexture> textures_;
    uint32_t next_texture_id_ = 1;
    uint32_t default_texture_id_ = 0;

    // -----------------------------------------------------------------------
    // Pipeline cache (keyed by state hash)
    // -----------------------------------------------------------------------
    std::unordered_map<uint64_t, VkPipeline> pipeline_cache_map_;

    // -----------------------------------------------------------------------
    // Shaders
    // -----------------------------------------------------------------------
    VkShaderModule vert_shader_ = VK_NULL_HANDLE;
    VkShaderModule frag_shader_ = VK_NULL_HANDLE;

    // -----------------------------------------------------------------------
    // Fixed-function state
    // -----------------------------------------------------------------------
    FixedFunctionState current_state_;

    // -----------------------------------------------------------------------
    // Frame tracking
    // -----------------------------------------------------------------------
    uint32_t current_frame_       = 0;
    uint32_t current_image_index_ = 0;
    int      game_w_              = 0;
    int      game_h_              = 0;
    bool     initialized_         = false;
    bool     in_render_pass_      = false;
    uint32_t draw_call_count_     = 0;

    // -----------------------------------------------------------------------
    // Private helpers
    // -----------------------------------------------------------------------
    bool create_swapchain(SDL_Window* window);
    bool create_render_pass();
    bool create_framebuffers();
    bool compile_shaders();
    void create_default_texture();

    VkPipeline     get_or_create_pipeline();
    uint64_t       compute_pipeline_hash() const;
    VkShaderModule create_shader_module(const uint32_t* code, size_t size);
};

#endif // VULKAN_BACKEND_H
