#pragma once
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <cstdint>
#include <stack>
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

// Forward declaration of ViewerState (so we don't need to include asset_viewer.cpp details here)
struct ViewerState;

namespace intel {

struct StyxColor {
    ImVec4 color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    bool bold = false;
    bool italic = false;
    bool underline = false;
};

struct StyxPattern {
    std::string style_name;
    std::regex regex_pattern;
    std::string raw_regex;
};

struct StyxStyleSheet {
    std::string name;
    std::vector<std::string> extensions;
    std::string line_comment;
    std::string block_comment_start;
    std::string block_comment_end;
    std::map<std::string, StyxColor> styles;
    std::map<std::string, std::vector<std::string>> keywords; // style -> word list
    std::vector<StyxPattern> patterns;
};

enum class LexState {
    NORMAL,
    IN_BLOCK_COMMENT,
    IN_MULTILINE_STRING
};

struct EditorToken {
    std::string text;
    StyxColor style;
};

struct BracketError {
    int line = -1;
    int column = -1;
    char bracket = 0;
    std::string message;
};

struct CachedTokenLine {
    uint64_t hash = 0;
    LexState in_state = LexState::NORMAL;
    LexState out_state = LexState::NORMAL;
    std::vector<EditorToken> tokens;
};

struct SwordigoApiSymbol {
    std::string name;
    std::string category;
    std::string signature;
    std::string doc;
};

class IntelliJ {
public:
    IntelliJ();
    ~IntelliJ();

    // Load a .styx style sheet
    bool load_style(const std::string& path);
    
    // Load a .styx style sheet directly from a string in memory
    bool load_style_from_memory(const std::string& content);
    
    // Fallback default style initialization
    void load_default_style();

    // Tokenize a single line based on the currently loaded style sheet
    std::vector<EditorToken> tokenize_line(const std::string& line, bool is_dark_theme);

    // Multiline state-aware tokenization
    LexState tokenize_line_stateful(const std::string& line, LexState in_state, bool is_dark_theme, std::vector<EditorToken>& out_tokens);

    // Perform bracket matching and return any mismatch errors (string & comment aware)
    std::vector<BracketError> check_syntax_errors(const std::string& text);

    // Retrieve Swordigo SDK Autocomplete suggestions for active editor context
    std::vector<SwordigoApiSymbol> get_swordigo_autocomplete(const std::string& query);

    void draw_editor(const char* label, std::string* buffer, bool& modified, const std::string& filepath, ImFont* mono_font, ImVec4& custom_bg,
                     bool has_compile_result = false, bool compile_success = false, const std::string& compile_error_msg = "", double compile_time_ms = 0.0);

    const StyxStyleSheet& get_style() const { return style_; }

    void clear_token_cache() { token_cache_.clear(); }

private:
    StyxStyleSheet style_;
    std::string current_style_path_;
    std::vector<CachedTokenLine> token_cache_;

    // Helper for hex colors (e.g. "#FF5656" -> ImVec4)
    static ImVec4 parse_hex_color(const std::string& hex);
};

extern const char* FILERIFT_GROOVE_STYX_CONTENT;
extern const char* BAT_SYNTAX_STYX_CONTENT;

} // namespace intel
