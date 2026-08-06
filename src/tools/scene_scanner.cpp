/* scene_scanner.cpp — CLI diagnostics for Swordigo .scene files
 *
 * Scans one or more .scene files (or a directory) and reports:
 *   • objects that reference a template but could not resolve any renderable
 *     mesh (these show up as proxy dots in the Ruby scene visualizer)
 *   • imported .scl object libraries that were not found on disk
 *   • embedded vs. external library counts
 *
 * Build:  make bin/scanscene
 * Usage:  ./bin/scanscene scene1.scene [scene2 ... | dir ...]
 * Exit:   0 if every scene resolved cleanly, 1 if any problems were found.
 */

#include "tools/scene_loader.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int scan_scene(const std::string& path, bool& any_problem) {
    av::SceneData sc = av::scene_load(path);
    if (sc.objects.empty() && sc.object_libraries.empty()) {
        std::printf("[scanscene] %-28s could not be parsed (empty or corrupt)\n",
                    path.c_str());
        any_problem = true;
        return 1;
    }

    // Objects whose components are purely non-visual (lights, portals,
    // collision shapes, spawn points, controllers) resolve fine — they just
    // have nothing to draw, so they are NOT flagged as missing models.
    int resolved = 0, unresolved = 0, proxy = 0, non_visual = 0;
    std::vector<std::string> unresolved_names;
    for (const auto& o : sc.objects) {
        const bool has_mesh   = !o.mesh_name.empty();
        const bool has_bg     = !o.background_name.empty();
        const bool has_ground = !o.ground_meshes.empty();
        if (has_mesh || has_bg || has_ground) {
            ++resolved;
        } else if (o.is_non_visual()) {
            ++non_visual;  // light / portal / collider / spawn point
        } else {
            ++unresolved;
            if (!o.template_name.empty() && o.template_name != "SceneObject") {
                // References a mesh that never resolved → renders as a dot.
                ++proxy;
                unresolved_names.push_back(o.name + " (template: " + o.template_name + ")");
            }
        }
    }

    std::printf("[scanscene] %-28s objects=%3d resolved=%3d non-visual=%3d missing-models=%d\n",
                fs::path(path).filename().string().c_str(),
                (int)sc.objects.size(), resolved, non_visual, proxy);

    bool scene_ok = true;
    if (proxy > 0) {
        for (const auto& n : unresolved_names)
            std::printf("    ! MISSING MODEL renders as a dot: %s\n", n.c_str());
        scene_ok = false;
    }
    if (!sc.missing_libraries.empty()) {
        std::printf("    ! %zu imported library file(s) not found:\n",
                    sc.missing_libraries.size());
        for (const auto& lib : sc.missing_libraries)
            std::printf("        %s.scl\n", lib.c_str());
        scene_ok = false;
    } else {
        std::printf("    external libraries loaded: %zu (embedded: %zu)\n",
                    sc.external_libraries.size(), sc.object_libraries.size());
    }
    if (!scene_ok) any_problem = true;
    return scene_ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <scene.d | directory> [more...]\n"
                     "Reports unresolved template objects (proxy dots) and\n"
                     "missing .scl object libraries across Swordigo scenes.\n",
                     argv[0]);
        return 2;
    }

    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        std::error_code ec;
        if (fs::is_directory(argv[i], ec)) {
            for (const auto& entry : fs::directory_iterator(argv[i], ec)) {
                if (!ec && entry.path().extension() == ".scene")
                    inputs.push_back(entry.path().string());
            }
        } else {
            inputs.push_back(argv[i]);
        }
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "no .scene files found\n");
        return 2;
    }

    bool any_problem = false;
    int total_unresolved = 0;
    for (const auto& path : inputs) {
        total_unresolved += scan_scene(path, any_problem);
    }
    std::printf("[scanscene] %zu scene(s) checked — %s\n", inputs.size(),
                any_problem ? "PROBLEMS FOUND" : "all clean");
    return any_problem ? 1 : 0;
}
