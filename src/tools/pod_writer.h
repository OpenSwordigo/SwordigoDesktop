#pragma once
// pod_writer.h — PowerVR POD model serializer (chunk-based tag-length-data)
//
// The inverse of pod_loader.h: turns a parsed PODModel back into the exact
// chunked byte layout the game / POD SDK reads. Round-trips through
// av::pod_parse() byte-for-byte for mesh, node, material and animation data.
//
// Standalone reusable module. No OpenGL or ImGui dependencies.

#include <string>

namespace av {

struct PODModel;

// Serialize the model into the POD chunk format. Returns true on success,
// false and an error message otherwise.
bool pod_write(const PODModel& model, const std::string& path, std::string* err = nullptr);

} // namespace av
