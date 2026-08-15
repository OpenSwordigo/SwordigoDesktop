#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path);
    assert(file && "failed to open regression input");
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

int main() {
    const std::filesystem::path source_dir = SRE_SOURCE_DIR;
    const std::string scene = read_file(source_dir / "sre_scene_update.c");
    const std::string api = read_file(source_dir / "sre_mini_api.c");
    const std::string gui = read_file(source_dir / "sre_gui_native.c");

    assert(scene.find("coins == 0 && g_sre_player_coins > 0") == std::string::npos);
    assert(api.find("val > 0 || g_sre_player_coins <= 0") == std::string::npos);
    assert(api.find("c==0) and _G.__last_coins>0") == std::string::npos);

    assert(gui.find("((pfn_DrawRect_N)g_orig_GUIButton_DrawRect)") != std::string::npos);
    assert(gui.find("((pfn_DrawRect_N)g_orig_GUILabel_DrawRect)") != std::string::npos);
    assert(gui.find("((pfn_DrawRect_N)g_orig_GUIFrameView_DrawRect)") != std::string::npos);
    return 0;
}
