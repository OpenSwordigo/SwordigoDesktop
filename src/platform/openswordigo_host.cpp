#include "platform/openswordigo_host.h"
#include "platform/display.h"
#include "platform/pvr_loader.h"
#include "tools/pod_loader.h"
#include "tools/scene_loader.h"
#include "platform/os_external.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

struct OpenSwordigoConfig {
    const char* assets_root;
    const char* scene;
    int width;
    int height;
    void* sdl_window;
    void* sdl_gl_context;
    uint32_t (*load_texture)(const char* name, int* width, int* height);
    int (*read_asset)(const char* name, uint8_t** data, size_t* size);
    void (*free_asset)(uint8_t* data);
    void* (*load_model)(const char* name);
    void (*free_model)(void* model);
    int (*model_mesh_count)(void* model);
    int (*model_mesh)(void* model, int index, const float** positions, int* position_count,
                      const float** normals, int* normal_count, const float** uvs, int* uv_count,
                      const uint32_t** indices, int* index_count, const char** texture_name);
    void* (*load_scene)(const char* name);
    void (*free_scene)(void* scene);
    int (*scene_object_count)(void* scene);
    int (*scene_object)(void* scene, int index, const char** identifier, const char** template_name,
                        float* x, float* y, float* depth, float* rotation, float* scale, int* hidden,
                        const char** model_name, const char** background_name);
    int (*scene_ground_mesh_count)(void* scene, int object_index);
    int (*scene_ground_mesh)(void* scene, int object_index, int mesh_index,
                             const float** positions, int* position_count, const float** normals,
                             int* normal_count, const float** uvs, int* uv_count,
                             const uint32_t** indices, int* index_count, const char** texture_name);
    int (*scene_spawn_point)(void* scene, int object_index, int* facing,
                             float* offset_x, float* offset_y, float* offset_z);
    int (*scene_model_transform)(void* scene, int object_index, float* origin,
                                 float* axis, float* angle, float* speed);
    int (*scene_portal)(void* scene, int object_index, const char** destination,
                        const char** spawn_point, int* tap_to_enter);
};

struct OpenSwordigoRuntime;
using CreateFn = OpenSwordigoRuntime* (*)(const OpenSwordigoConfig*);
using UpdateFn = int (*)(OpenSwordigoRuntime*, float);
using RenderFn = int (*)(OpenSwordigoRuntime*);
using MouseFn = void (*)(OpenSwordigoRuntime*, float, float, int);
using KeyFn = void (*)(OpenSwordigoRuntime*, int, int);
using DestroyFn = void (*)(OpenSwordigoRuntime*);

template <typename T>
T load_symbol(void* library, const char* name) {
    os_external::library_error();
    auto symbol = reinterpret_cast<T>(os_external::find_symbol(library, name));
    if (!symbol) {
        std::cerr << "[OpenSwordigo/Host] Missing symbol " << name << ": "
                  << os_external::library_error() << '\n';
        return nullptr;
    }
    return symbol;
}

std::string g_native_assets_root;

uint32_t load_native_texture(const char* name, int* width, int* height) {
    if (!name || !name[0]) return 0;

    const std::string original(name);
    std::string stem = original;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos) stem.resize(dot);

    std::vector<std::string> candidates{original};
    if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, "_2x") == 0) {
        candidates.push_back(stem + ".pvr");
        candidates.push_back(stem + ".tex.png");
    } else {
        candidates.push_back(stem + "_2x.pvr");
        candidates.push_back(stem + "_2x.tex.png");
        candidates.push_back(stem + ".pvr");
        candidates.push_back(stem + ".tex.png");
    }

    for (const std::string& candidate : candidates) {
        const std::string path = g_native_assets_root + "/" + candidate;
        if (std::filesystem::is_regular_file(path)) {
            const uint32_t texture = pvr_load_texture(path.c_str(), width, height);
            if (texture != 0) return texture;
        }
    }
    return 0;
}

int read_native_asset(const char* name, uint8_t** data, size_t* size) {
    if (!name || !name[0] || !data || !size) return 0;
    const std::filesystem::path requested(name);
    const std::filesystem::path path = requested.is_absolute()
        ? requested : std::filesystem::path(g_native_assets_root) / requested;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return 0;
    const std::streamsize length = file.tellg();
    if (length < 0) return 0;
    file.seekg(0, std::ios::beg);
    auto* buffer = static_cast<uint8_t*>(std::malloc(static_cast<size_t>(length)));
    if (length > 0 && (!buffer || !file.read(reinterpret_cast<char*>(buffer), length))) {
        std::free(buffer);
        return 0;
    }
    *data = buffer;
    *size = static_cast<size_t>(length);
    return 1;
}

void free_native_asset(uint8_t* data) {
    std::free(data);
}

void* load_native_model(const char* name) {
    if (!name || !name[0]) return nullptr;
    const std::filesystem::path requested(name);
    std::vector<std::filesystem::path> candidates;
    if (requested.is_absolute()) {
        candidates.push_back(requested);
    } else {
        const std::filesystem::path root(g_native_assets_root);
        candidates.push_back(root / requested);
        if (!requested.has_extension()) {
            candidates.push_back(root / (requested.string() + ".POD"));
            candidates.push_back(root / (requested.string() + ".pod"));
        }
    }
    for (const auto& candidate : candidates) {
        if (!std::filesystem::is_regular_file(candidate)) continue;
        auto* model = new av::PODModel(av::pod_load(candidate.string()));
        if (!model->meshes.empty()) return model;
        delete model;
    }
    return nullptr;
}

void free_native_model(void* handle) {
    delete static_cast<av::PODModel*>(handle);
}

int native_model_mesh_count(void* handle) {
    return handle ? static_cast<int>(static_cast<av::PODModel*>(handle)->meshes.size()) : 0;
}

int native_model_mesh(void* handle, int index, const float** positions, int* position_count,
                      const float** normals, int* normal_count, const float** uvs, int* uv_count,
                      const uint32_t** indices, int* index_count, const char** texture_name) {
    if (!handle || index < 0) return 0;
    auto* model = static_cast<av::PODModel*>(handle);
    if (index >= static_cast<int>(model->meshes.size())) return 0;
    const auto& mesh = model->meshes[index];
    if (positions) *positions = mesh.positions.data();
    if (position_count) *position_count = static_cast<int>(mesh.positions.size());
    if (normals) *normals = mesh.normals.data();
    if (normal_count) *normal_count = static_cast<int>(mesh.normals.size());
    if (uvs) *uvs = mesh.uvs.data();
    if (uv_count) *uv_count = static_cast<int>(mesh.uvs.size());
    if (indices) *indices = mesh.indices.data();
    if (index_count) *index_count = static_cast<int>(mesh.indices.size());
    // Resolve the mesh's OWN texture via node -> material -> diffuse texture
    // index. texturing_filenames[0] is wrong for multi-texture PODs (hero,
    // monsters) — each mesh can carry a different material.
    if (texture_name) {
        *texture_name = nullptr;
        const int node_index = [&]() -> int {
            for (int n = 0; n < static_cast<int>(model->nodes.size()); ++n) {
                if (model->nodes[n].object_index == index) return n;
            }
            return -1;
        }();
        if (node_index >= 0 && node_index < static_cast<int>(model->nodes.size())) {
            const auto& node = model->nodes[node_index];
            if (node.material_index >= 0 &&
                node.material_index < static_cast<int>(model->materials.size())) {
                const int tex_idx = model->materials[node.material_index].diffuse_texture_index;
                if (tex_idx >= 0 && tex_idx < static_cast<int>(model->texture_filenames.size()))
                    *texture_name = model->texture_filenames[tex_idx].c_str();
            }
        }
    }
    return 1;
}

void* load_native_scene(const char* name) {
    if (!name) return nullptr;
    auto* scene = new av::SceneData(av::scene_load((std::filesystem::path(g_native_assets_root) / name).string()));
    if (scene->objects.empty()) {
        delete scene;
        return nullptr;
    }
    return scene;
}

void free_native_scene(void* scene) { delete static_cast<av::SceneData*>(scene); }

int native_scene_object_count(void* scene) {
    return scene ? static_cast<int>(static_cast<av::SceneData*>(scene)->objects.size()) : 0;
}

// True when the object carries a model-bearing component (its own or inherited
// from its template library entry). scene_loader's template resolution merges
// library components into resolved_components and only then fills mesh_name;
// without this check, pure non-visual objects (Background, Light, CollisionShape,
// SpawnPoint, portals, AI controllers) would inherit the mesh_name = template_name
// fallback and trigger bogus model loads.
static bool object_has_model_component(const av::SceneObject& object) {
    const auto& comps = object.resolved_components.empty() ? object.components
                                                           : object.resolved_components;
    for (const auto& c : comps) {
        const std::string& t = c.type_name;
        if (t.find("MeshRenderer") != std::string::npos ||
            t.find("SkinnedMeshRenderer") != std::string::npos ||
            t.find("ModelComponent") != std::string::npos ||
            t.find("Sprite") != std::string::npos)
            return true;
    }
    return false;
}

int native_scene_object(void* handle, int index, const char** identifier, const char** template_name,
                        float* x, float* y, float* depth, float* rotation, float* scale, int* hidden,
                        const char** model_name, const char** background_name) {
    if (!handle || index < 0) return 0;
    auto* scene = static_cast<av::SceneData*>(handle);
    if (index >= static_cast<int>(scene->objects.size())) return 0;
    const auto& object = scene->objects[index];
    if (identifier) *identifier = object.name.c_str();
    if (template_name) *template_name = object.template_name.c_str();
    if (x) *x = object.pos_x;
    if (y) *y = object.pos_y;
    if (depth) *depth = object.pos_z;
    if (rotation) *rotation = object.rot_y;
    if (scale) *scale = object.scale_x * object.template_scaling;
    if (hidden) *hidden = object.hidden ? 1 : 0;
    if (model_name) {
        *model_name = nullptr;
        if (object_has_model_component(object) && !object.mesh_name.empty())
            *model_name = object.mesh_name.c_str();
    }
    if (background_name) *background_name = object.background_name.empty() ? nullptr : object.background_name.c_str();
    return 1;
}

int native_scene_ground_mesh_count(void* handle, int object_index) {
    if (!handle || object_index < 0) return 0;
    auto* scene = static_cast<av::SceneData*>(handle);
    return object_index < static_cast<int>(scene->objects.size())
        ? static_cast<int>(scene->objects[object_index].ground_meshes.size()) : 0;
}

int native_scene_ground_mesh(void* handle, int object_index, int mesh_index,
                             const float** positions, int* position_count, const float** normals,
                             int* normal_count, const float** uvs, int* uv_count,
                             const uint32_t** indices, int* index_count, const char** texture_name) {
    if (!handle || object_index < 0 || mesh_index < 0) return 0;
    auto* scene = static_cast<av::SceneData*>(handle);
    if (object_index >= static_cast<int>(scene->objects.size())) return 0;
    const auto& object = scene->objects[object_index];
    if (mesh_index >= static_cast<int>(object.ground_meshes.size())) return 0;
    const auto& mesh = object.ground_meshes[mesh_index];
    if (positions) *positions = mesh.positions.data();
    if (position_count) *position_count = static_cast<int>(mesh.positions.size());
    if (normals) *normals = mesh.normals.data();
    if (normal_count) *normal_count = static_cast<int>(mesh.normals.size());
    if (uvs) *uvs = mesh.uvs.data();
    if (uv_count) *uv_count = static_cast<int>(mesh.uvs.size());
    if (indices) *indices = mesh.indices.data();
    if (index_count) *index_count = static_cast<int>(mesh.indices.size());
    if (texture_name) *texture_name = mesh_index < static_cast<int>(object.ground_mesh_textures.size())
        ? object.ground_mesh_textures[mesh_index].c_str() : nullptr;
    return 1;
}

int native_scene_spawn_point(void* handle, int object_index, int* facing,
                             float* offset_x, float* offset_y, float* offset_z) {
    if (!handle || object_index < 0) return 0;
    auto* scene = static_cast<av::SceneData*>(handle);
    if (object_index >= static_cast<int>(scene->objects.size())) return 0;
    const auto& object = scene->objects[object_index];
    if (!object.is_spawn_point) return 0;
    if (facing) *facing = object.spawn_facing;
    if (offset_x) *offset_x = object.spawn_offset[0];
    if (offset_y) *offset_y = object.spawn_offset[1];
    if (offset_z) *offset_z = object.spawn_offset[2];
    return 1;
}

int native_scene_model_transform(void* handle, int object_index, float* origin,
                                 float* axis, float* angle, float* speed) {
    if (!handle || object_index < 0) return 0;
    auto* scene = static_cast<av::SceneData*>(handle);
    if (object_index >= static_cast<int>(scene->objects.size())) return 0;
    const auto& object = scene->objects[object_index];
    if (!object.has_model_transform) return 0;
    if (origin) std::copy_n(object.model_transform_origin, 3, origin);
    if (axis) std::copy_n(object.model_transform_axis, 3, axis);
    if (angle) *angle = object.model_transform_angle;
    if (speed) *speed = object.model_transform_speed;
    return 1;
}

int native_scene_portal(void* handle, int object_index, const char** destination,
                        const char** spawn_point, int* tap_to_enter) {
    if (!handle || object_index < 0) return 0;
    auto* scene = static_cast<av::SceneData*>(handle);
    if (object_index >= static_cast<int>(scene->objects.size())) return 0;
    const auto& object = scene->objects[object_index];
    if (!object.is_portal) return 0;
    if (destination) *destination = object.portal_destination.c_str();
    if (spawn_point) *spawn_point = object.portal_spawn_point.c_str();
    if (tap_to_enter) *tap_to_enter = object.portal_tap_to_enter ? 1 : 0;
    return 1;
}

} // namespace

int run_openswordigo(Display& display, const OpenSwordigoLaunchOptions& options) {
    void* library = os_external::load_library(options.library_path);
    if (!library) {
        std::cerr << "[OpenSwordigo/Host] Failed to load " << options.library_path
                  << ": " << os_external::library_error() << '\n';
        return 1;
    }

    CreateFn create = load_symbol<CreateFn>(library, "openswordigo_create");
    UpdateFn update = load_symbol<UpdateFn>(library, "openswordigo_update");
    RenderFn render = load_symbol<RenderFn>(library, "openswordigo_render");
    MouseFn mouse = load_symbol<MouseFn>(library, "openswordigo_mouse");
    KeyFn key = load_symbol<KeyFn>(library, "openswordigo_key");
    DestroyFn destroy = load_symbol<DestroyFn>(library, "openswordigo_destroy");
    if (!create || !update || !render || !mouse || !key || !destroy) {
        os_external::close_library(library);
        return 1;
    }

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(display.get_window(), &width, &height);
    OpenSwordigoConfig config{
        options.assets_root.c_str(), options.scene.c_str(), width, height,
        display.get_window(), display.get_gl_context(),
        load_native_texture, read_native_asset, free_native_asset,
        load_native_model, free_native_model, native_model_mesh_count, native_model_mesh,
        load_native_scene, free_native_scene, native_scene_object_count, native_scene_object,
         native_scene_ground_mesh_count, native_scene_ground_mesh, native_scene_spawn_point,
         native_scene_model_transform, native_scene_portal
    };
    g_native_assets_root = options.assets_root;
    OpenSwordigoRuntime* runtime = create(&config);
    if (!runtime) {
        std::cerr << "[OpenSwordigo/Host] Native runtime initialization failed\n";
        os_external::close_library(library);
        return 1;
    }

    std::cout << "[OpenSwordigo/Host] Running native engine: scene=" << options.scene
              << " assets=" << options.assets_root << '\n';

    bool running = true;
    auto previous = std::chrono::steady_clock::now();
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_RESIZED:
                    SDL_GetWindowSizeInPixels(display.get_window(), &width, &height);
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    if (event.key.key == SDLK_ESCAPE && event.type == SDL_EVENT_KEY_DOWN) {
                        running = false;
                    } else {
                        key(runtime, static_cast<int>(event.key.key),
                            event.type == SDL_EVENT_KEY_DOWN ? 1 : 0);
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP: {
                    int logical_w = 0;
                    int logical_h = 0;
                    SDL_GetWindowSize(display.get_window(), &logical_w, &logical_h);
                    const float scale_x = logical_w > 0 ? static_cast<float>(width) / logical_w : 1.0f;
                    const float scale_y = logical_h > 0 ? static_cast<float>(height) / logical_h : 1.0f;
                    mouse(runtime, event.button.x * scale_x,
                          height - event.button.y * scale_y,
                          event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : 0);
                    break;
                }
                default:
                    break;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        dt = std::clamp(dt, 1.0f / 240.0f, 0.1f);
        if (!update(runtime, dt) || !render(runtime)) {
            std::cerr << "[OpenSwordigo/Host] Runtime requested shutdown\n";
            break;
        }
        display.swap();
    }

    destroy(runtime);
    os_external::close_library(library);
    return 0;
}
