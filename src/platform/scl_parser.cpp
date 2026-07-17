#include "platform/scl_parser.h"
#include "tools/filerift.h"
#include <fstream>
#include <iostream>

namespace scl {

std::string extract_lua(const std::string& scl_filepath) {
    std::ifstream file(scl_filepath, std::ios::binary);
    if (!file) {
        std::cerr << "[SclParser] Failed to open: " << scl_filepath << std::endl;
        return "";
    }
    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    return filerift::extract_lua_generic(bytes);
}

} // namespace scl
