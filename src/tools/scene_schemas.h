#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace av {

struct SchemaField {
    std::string name;
    std::string class_name;
    bool is_message = false;
};

struct SchemaClass {
    std::string name;
    std::unordered_map<uint32_t, SchemaField> fields;
};

extern const std::unordered_map<std::string, SchemaClass> g_schemas;

} // namespace av
