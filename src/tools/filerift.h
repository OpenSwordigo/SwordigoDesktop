#pragma once
#include <string>
#include <vector>

namespace filerift {

    // Decodes a binary protobuf (e.g., "scene", "scl", "gdata", etc.)
    
    std::string decode_protobuf(const std::string& bytes, const std::string& filetype);

    // Encodes FileRift's plain text markup back into binary protobuf.
    //(e.g., "scene", "scl", etc.)
   
    std::string recode_markup(const std::string& text, const std::string& filetype);

    // Our generic lua extractor
    std::string extract_lua_generic(const std::string& bytes);

} // namespace IS filerift
