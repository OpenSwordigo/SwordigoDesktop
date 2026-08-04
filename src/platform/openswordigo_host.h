#pragma once

#include <string>

class Display;

struct OpenSwordigoLaunchOptions {
    std::string library_path;
    std::string assets_root;
    std::string scene = "menu.scene";
};

int run_openswordigo(Display& display, const OpenSwordigoLaunchOptions& options);
