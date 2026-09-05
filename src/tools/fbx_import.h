#pragma once
// fbx_import.h — FBX loader backed by the single-file MIT ufbx library.
//
// Instead of parsing the FBX container by hand this module delegates all file
// reading, node-tree construction, material/texture resolution, coordinate
// system handling and skinning to ufbx (src/tools/ufbx/ufbx.c). This module is
// only a thin adapter from the ufbx scene graph into the viewer's PODModel
// structure so existing renderers can preview .fbx files:
//   - geometry is split into per-material sub-meshes,
//   - each node's world transform (geometry_to_world) is baked into the
//     vertices and normals,
//   - the scene is converted to right-handed Y-up space by ufbx,
//     and authored units are converted to metres (ufbx target_unit_meters=1.0),
//     so a cm-authored FBX is divided by 100 — NOT normalised to a unit cube;
//     callers apply opts.unit_scale/opts.scale to reach game units,
//   - each material's diffuse texture is resolved to an existing file next to
//     the .fbx (or the .fbx itself) so texture_filenames can be loaded by the
//     viewer's texture pipeline.
//
// Standalone module. Depends on tools/ufbx/ufbx.c + tools/pod_loader.h only.
// No OpenGL/ImGui dependencies.

#include <string>
#include <vector>

#include "tools/pod_loader.h"

namespace av {

// Load a .fbx file (ASCII or binary, any FBX version ufbx supports) into a
// PODModel. Returns an empty model when loading fails.
PODModel fbx_load(const std::string& path);

// Parse from in-memory bytes; hint_path is used only for texture resolution.
PODModel fbx_parse(const std::vector<uint8_t>& data, const std::string& hint_path);

} // namespace av