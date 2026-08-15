#include "tools/filerift.h"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

std::string g_instance_assets_dir = "assets";

namespace {

void require_contains(const std::string& value, const std::string& expected) {
    if (value.find(expected) == std::string::npos) {
        std::cerr << "expected decoded output to contain: " << expected << "\n";
        std::exit(EXIT_FAILURE);
    }
}

} // namespace

int main() {
    const std::string scene_markup = R"(# preserved FileRift comment
Object{
    Identifier : 'hero\'s spawn'
    Position{
        X : 90d
        Y : -12.5
    }
    Hidden : 0
}
Tag_999_varint : 7
)";

    const std::string encoded = filerift::recode_markup(scene_markup, "scene");
    const std::string decoded = filerift::decode_protobuf(encoded, "scene");
    require_contains(decoded, "# preserved FileRift comment");
    require_contains(decoded, "Identifier : 'hero\\'s spawn'");
    require_contains(decoded, "X : 90d");
    require_contains(decoded, "Y : -12.5");
    require_contains(decoded, "Tag_999_varint : 7");

    const std::string recoded = filerift::recode_markup(decoded, "scene");
    if (recoded != encoded) {
        std::cerr << "decode/recode round-trip changed the protobuf payload\n";
        return EXIT_FAILURE;
    }

    const std::string lua_markup = R"(OnLoad{
    String : $
return 7
$end
    Bytes : @compile
}
)";
    const std::string lua_binary = filerift::recode_markup(lua_markup, "scene");
    const std::string lua_decoded = filerift::decode_protobuf(lua_binary, "scene");
    require_contains(lua_decoded, "String : $");
    require_contains(lua_decoded, "return 7");
    require_contains(lua_decoded, "Bytes : @compile");

    // String chunks must round-trip to a fixed point: decoding the recoded
    // binary and re-encoding must reproduce the exact same binary, and a
    // second decode must reproduce the exact same markup. (Regression guard:
    // previously each cycle grew the chunk by a wrapped "\n" + indentation.)
    {
        const std::string rebin = filerift::recode_markup(lua_decoded, "scene");
        if (rebin != lua_binary) {
            std::cerr << "String chunk decode/recode round-trip is not stable\n";
            return EXIT_FAILURE;
        }
        const std::string redecode = filerift::decode_protobuf(rebin, "scene");
        if (redecode != lua_decoded) {
            std::cerr << "String chunk decode is not idempotent\n";
            return EXIT_FAILURE;
        }
    }

    try {
        (void)filerift::recode_markup("Object{ Hidden : nope }", "scene");
        std::cerr << "invalid varint markup was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::exception&) {
    }

    try {
        (void)filerift::recode_markup("Object{ Identifier : 'missing quote }", "scene");
        std::cerr << "unterminated string markup was accepted\n";
        return EXIT_FAILURE;
    } catch (const std::exception&) {
    }

    std::cout << "FileRift smoke checks passed\n";
    return EXIT_SUCCESS;
}
