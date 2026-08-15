#include "tools/intellij.h"

#include <cstdlib>
#include <iostream>
#include <string>

std::string g_instance_assets_dir = "assets";

int main() {
    intel::IntelliJ editor;
    if (!editor.load_style_from_memory(intel::FILERIFT_GROOVE_STYX_CONTENT)) {
        std::cerr << "embedded Grove style failed to load\n";
        return EXIT_FAILURE;
    }
    if (editor.get_style().patterns.empty() || editor.get_style().extensions.empty()) {
        std::cerr << "embedded Grove style is incomplete\n";
        return EXIT_FAILURE;
    }
    if (editor.load_style_from_memory("{}")) {
        std::cerr << "structurally invalid Styx content was accepted\n";
        return EXIT_FAILURE;
    }

    const auto errors = editor.check_syntax_errors("Object{ Identifier : 'a\\'b' -- } ignored\n}");
    if (!errors.empty()) {
        std::cerr << "Styx syntax checker mishandled escaped text or comments\n";
        return EXIT_FAILURE;
    }

    editor.load_default_style();
    if (editor.get_style().name != "Default Dark") {
        std::cerr << "default style reset retained stale state\n";
        return EXIT_FAILURE;
    }

    std::cout << "Styx smoke checks passed\n";
    return EXIT_SUCCESS;
}
