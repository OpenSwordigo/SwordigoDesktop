#include "intellij.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"
#include "platform/IconsFontAwesome6.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <stack>
#include <algorithm>
#include <unordered_map>



namespace intel {
const char* FILERIFT_GROOVE_STYX_CONTENT = R"styx(
{
  "name": ["FileRift (Grove)", ".jsonl", ".scl", ".scene", ".remap", ".sounds", ".gdata", ".gstate", ".gopt", ".lua", ".fnt", ".atlas", ".so", ".scmap", ".gmesh"],
  "comments": {
    "line": "--",
    "block_start": "--[[",
    "block_end": "]]"
  },
  "styles": {
    "background": {"color": "#121715"},
    "codetag": {"color": "#A8D1C8", "bold": true},
    "comment": {"color": "#769794", "italic": true},
    "default": {"color": "#A8D1C8"},
    "docstring": {"color": "#A8D1C8", "italic": true},
    "dunder": {"color": "#53B68E"},
    "function": {"color": "#8695FF"},
    "keyword": {"color": "#C55890", "bold": true},
    "method": {"color": "#53B68E"},
    "parameter": {"color": "#C6D9FF"},
    "operator": {"color": "#53B68E"},
    "string": {"color": "#A8D1C8"},
    "number": {"color": "#769794"},
    "tableitem": {"color": "#769794"},
    "defined": {"color": "#A8D1C8", "bold": true},
    "code": {"color": "#A8D1C8"},
    "object": {"color": "#A8D1C8"},
    "bools": {"color": "#A8D1C8", "bold": true, "underline": true},
    "local": {"color": "#C55890", "bold": true}
  },
  "keywords": {
    "keyword": ["if", "then", "else", "elseif", "end", "for", "while", "repeat", "until", "break", "return", "function", "and", "or", "not", "do", "in"],
    "local": ["local"],
    "bools": ["nil", "true", "false"],
    "method": ["print", "pairs", "ipairs", "next", "type", "tostring", "tonumber", "error", "pcall", "xpcall", "assert", "collectgarbage", "require"]
  },
  "patterns": [
    {"style": "comment", "regex": "--.*|#.*|//.*"},
    {"style": "string", "regex": "\"[^\"]*\"|'[^']*'"},
    {"style": "number", "regex": "\\b\\d+(\\.\\d+)?\\b"},
    {"style": "method", "regex": "\\b[a-zA-Z_]\\w*(?=\\s*\\()"},
    {"style": "tableitem", "regex": "\\.(\\s*[a-zA-Z_]\\w*)"},
    {"style": "dunder", "regex": "\\b[a-zA-Z_]\\w*\\b(?=\\s*\\.)"},
    {"style": "object", "regex": "\\(\\s*([a-zA-Z_]\\w*)\\s*\\)"},
    {"style": "defined", "regex": "\\b([a-zA-Z_]\\w*)\\b(?=\\s*=)"},
    {"style": "codetag", "regex": "String|\\$end|\\@\\w+"}
  ]
}
)styx";

const char* BAT_SYNTAX_STYX_CONTENT = R"styx(
{
  "name": ["BatSyntax (FileRift)", ".jsonl", ".scl", ".scene", ".scmap", ".sounds", ".gdata", ".gstate", ".gopt", ".lua", ".fnt", ".atlas", ".fr", ".bootup", ".gvar", ".gmesh"],
  "comments": {
    "line": "--",
    "block_start": "--[[",
    "block_end": "]]"
  },
  "styles": {
    "background": {"color": "#161111"},
    "codetag": {"color": "#FFBBBB"},
    "comment": {"color": "#7B7B7B", "italic": true},
    "default": {"color": "#FFBBBB"},
    "docstring": {"color": "#9B9B9B", "italic": true},
    "dunder": {"color": "#FFA8A8", "bold": true},
    "function": {"color": "#FFD700"},
    "keyword": {"color": "#FF5656", "bold": true},
    "method": {"color": "#D1D1D1"},
    "parameter": {"color": "#FFA500"},
    "operator": {"color": "#FF6F6F", "bold": true},
    "operator2": {"color": "#FF7800", "bold": true},
    "string": {"color": "#FFE3E3"},
    "number": {"color": "#FF9F5E"},
    "constant": {"color": "#FFFFFF", "underline": true},
    "self": {"color": "#FFC487"},
    "type": {"color": "#FF6A6A"},
    "tagName": {"color": "#E7A1A1"},
    "variable": {"color": "#FE6B6B"},
    "shardshi": {"color": "#76C2FF"},
    "lib": {"color": "#D36565"},
    "namespace": {"color": "#FF7500"},
    "untitled": {"color": "#CA7474"},
    "call": {"color": "#A5C3FF"}
  },
  "keywords": {
    "type": ["local", "global", "boolean", "userdata"],
    "constant": ["true", "false", "nil", "T", "F"],
    "keyword": ["return", "then", "while", "and", "break", "do", "else", "elseif", "end", "for", "function", "if", "in", "not", "or", "repeat", "until", "thread", "table"],
    "self": ["self", "target", "normal", "gc"],
    "namespace": ["_G", "print", "tonumber", "tostring", "assert", "getmetatable", "dofile", "load", "pairs", "ipairs", "type"],
    "lib": ["ImportedLibrary"],
    "untitled": ["Object", "Template", "Zone"],
    "variable": ["Game", "Character", "Scene", "Program", "PhysicsObject", "TransformController", "CollisionShape", "Light", "Vector3", "Rectangle", "ModelTransformController", "Touchable", "Thread", "Keyboard", "Random", "DoorController", "EntityController", "KeyframeAnimation", "SoundLibrary", "Health", "Math", "TextBubble", "Entity", "ItemDrop"],
    "shardshi": ["Shardshi"],
    "tagName": ["$end", "$source$"]
  },
  "patterns": [
    {"style": "comment", "regex": "--.*|#.*|//.*"},
    {"style": "string", "regex": "\"[^\"]*\"|'[^']*'"},
    {"style": "number", "regex": "\\b\\d+(\\.\\d+)?\\b"},
    {"style": "call", "regex": "\\b[a-zA-Z_]\\w*(?=\\s*\\()"},
    {"style": "operator", "regex": "(\\{|\\}|\\(|\\)|\\[|\\])"},
    {"style": "operator2", "regex": "(\\+|\\-|\\*|\\/|\\=|\\,|\\:)"}
  ]
}
)styx";
} // namespace intel

namespace intel {

IntelliJ::IntelliJ() {
    load_default_style();
}

IntelliJ::~IntelliJ() {}

ImVec4 IntelliJ::parse_hex_color(const std::string& hex) {
    if (hex.empty()) return ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    std::string s = hex;
    if (s[0] == '#') s = s.substr(1);
    
    unsigned int r = 255, g = 255, b = 255, a = 255;
    if (s.size() >= 8) {
        sscanf(s.c_str(), "%02x%02x%02x%02x", &r, &g, &b, &a);
    } else if (s.size() >= 6) {
        sscanf(s.c_str(), "%02x%02x%02x", &r, &g, &b);
    }
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

static size_t find_balanced(const std::string& str, char open, char close, size_t start) {
    if (start == std::string::npos || start >= str.size()) return std::string::npos;
    int depth = 0;
    bool in_quote = false;
    for (size_t i = start; i < str.size(); i++) {
        if (str[i] == '"' && (i == 0 || str[i-1] != '\\')) {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote) {
            if (str[i] == open) {
                depth++;
            } else if (str[i] == close) {
                depth--;
                if (depth == 0) return i;
            }
        }
    }
    return std::string::npos;
}

static std::string escape_regex(const std::string& str) {
    static const std::string meta = ".^$*+?()[]{}\\|-";
    std::string out;
    for (char c : str) {
        if (meta.find(c) != std::string::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

static bool parse_bool_prop(const std::string& props, const std::string& key) {
    size_t pos = props.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    size_t colon = props.find(':', pos);
    if (colon == std::string::npos) return false;
    size_t comma = props.find_first_of(",}", colon);
    std::string val = props.substr(colon + 1, comma == std::string::npos ? std::string::npos : comma - colon - 1);
    return val.find("true") != std::string::npos;
}

static void compile_keywords(StyxStyleSheet& sheet) {
    for (const auto& kw_pair : sheet.keywords) {
        if (kw_pair.second.empty()) continue;
        std::string pattern = "(";
        for (size_t i = 0; i < kw_pair.second.size(); i++) {
            if (i > 0) pattern += "|";
            const std::string& item = kw_pair.second[i];
            std::string escaped = escape_regex(item);
            
            bool start_word = isalnum((unsigned char)item.front()) || item.front() == '_';
            bool end_word   = isalnum((unsigned char)item.back())  || item.back() == '_';
            
            std::string item_pat;
            if (start_word) item_pat += "\\b";
            item_pat += escaped;
            if (end_word)   item_pat += "\\b";
            pattern += item_pat;
        }
        pattern += ")";
        try {
            sheet.patterns.push_back({kw_pair.first, std::regex(pattern), pattern});
        } catch (const std::regex_error& e) {
            std::cerr << "[intellij] Regex compilation error for keyword group " << kw_pair.first << ": " << e.what() << "\n";
        }
    }
}

// Relaxed JSON parser for .styx format
bool IntelliJ::load_style(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[intellij] Warning: Cannot open style file: " << path << ", using fallback.\n";
        return false;
    }
    
    std::stringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    file.close();

    // Reset style structure
    StyxStyleSheet new_style;
    new_style.line_comment = "--"; // default
    
    try {
        // Strip block and line comments from configuration text (string-quote aware)
        std::string clean;
        size_t idx = 0;
        bool in_quote = false;
        char quote_char = 0;
        while (idx < content.size()) {
            char c = content[idx];
            if ((c == '"' || c == '\'') && (idx == 0 || content[idx-1] != '\\')) {
                if (!in_quote) {
                    in_quote = true;
                    quote_char = c;
                } else if (quote_char == c) {
                    in_quote = false;
                }
                clean += content[idx++];
            } else if (!in_quote && idx + 1 < content.size() && content[idx] == '/' && content[idx+1] == '/') {
                while (idx < content.size() && content[idx] != '\n') idx++;
            } else if (!in_quote && idx + 1 < content.size() && content[idx] == '-' && content[idx+1] == '-') {
                while (idx < content.size() && content[idx] != '\n') idx++;
            } else {
                clean += content[idx++];
            }
        }
        
        // Parse name/extensions
        size_t name_pos = clean.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t start_arr = clean.find('[', name_pos);
            size_t end_arr = find_balanced(clean, '[', ']', start_arr);
            if (start_arr != std::string::npos && end_arr != std::string::npos) {
                std::string arr_content = clean.substr(start_arr + 1, end_arr - start_arr - 1);
                size_t p = 0;
                while (true) {
                    size_t quote_start = arr_content.find('"', p);
                    if (quote_start == std::string::npos) break;
                    size_t quote_end = arr_content.find('"', quote_start + 1);
                    if (quote_end == std::string::npos) break;
                    std::string val = arr_content.substr(quote_start + 1, quote_end - quote_start - 1);
                    if (!val.empty()) {
                        if (new_style.name.empty()) new_style.name = val;
                        else new_style.extensions.push_back(val);
                    }
                    p = quote_end + 1;
                }
            }
        }

        // Parse comments
        size_t comments_pos = clean.find("\"comments\"");
        if (comments_pos != std::string::npos) {
            size_t start_obj = clean.find('{', comments_pos);
            size_t end_obj = find_balanced(clean, '{', '}', start_obj);
            if (start_obj != std::string::npos && end_obj != std::string::npos) {
                std::string obj_content = clean.substr(start_obj + 1, end_obj - start_obj - 1);
                size_t line_pos = obj_content.find("\"line\"");
                if (line_pos != std::string::npos) {
                    size_t q1 = obj_content.find('"', line_pos + 6);
                    size_t q2 = obj_content.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        new_style.line_comment = obj_content.substr(q1 + 1, q2 - q1 - 1);
                }
                size_t bs_pos = obj_content.find("\"block_start\"");
                if (bs_pos != std::string::npos) {
                    size_t q1 = obj_content.find('"', bs_pos + 13);
                    size_t q2 = obj_content.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        new_style.block_comment_start = obj_content.substr(q1 + 1, q2 - q1 - 1);
                }
                size_t be_pos = obj_content.find("\"block_end\"");
                if (be_pos != std::string::npos) {
                    size_t q1 = obj_content.find('"', be_pos + 11);
                    size_t q2 = obj_content.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        new_style.block_comment_end = obj_content.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        // Parse styles
        size_t styles_pos = clean.find("\"styles\"");
        if (styles_pos != std::string::npos) {
            size_t start_obj = clean.find('{', styles_pos);
            size_t end_obj = find_balanced(clean, '{', '}', start_obj);
            if (start_obj != std::string::npos && end_obj != std::string::npos) {
                std::string obj_content = clean.substr(start_obj + 1, end_obj - start_obj - 1);
                size_t p = 0;
                while (true) {
                    size_t name_start = obj_content.find('"', p);
                    if (name_start == std::string::npos) break;
                    size_t name_end = obj_content.find('"', name_start + 1);
                    if (name_end == std::string::npos) break;
                    std::string style_name = obj_content.substr(name_start + 1, name_end - name_start - 1);
                    
                    size_t colon = obj_content.find(':', name_end);
                    if (colon != std::string::npos) {
                        size_t val_start = obj_content.find('{', colon);
                        size_t val_end = find_balanced(obj_content, '{', '}', val_start);
                        if (val_start != std::string::npos && val_end != std::string::npos) {
                            std::string style_props = obj_content.substr(val_start + 1, val_end - val_start - 1);
                            StyxColor sc;
                            size_t col_pos = style_props.find("\"color\"");
                            if (col_pos != std::string::npos) {
                                size_t q1 = style_props.find('"', col_pos + 7);
                                size_t q2 = style_props.find('"', q1 + 1);
                                if (q1 != std::string::npos && q2 != std::string::npos)
                                    sc.color = parse_hex_color(style_props.substr(q1 + 1, q2 - q1 - 1));
                            }
                            sc.bold = parse_bool_prop(style_props, "bold");
                            sc.italic = parse_bool_prop(style_props, "italic");
                            sc.underline = parse_bool_prop(style_props, "underline");
                            new_style.styles[style_name] = sc;
                            p = val_end + 1;
                        } else {
                            p = name_end + 1;
                        }
                    } else {
                        p = name_end + 1;
                    }
                }
            }
        }

        // Parse keywords
        size_t kw_pos = clean.find("\"keywords\"");
        if (kw_pos != std::string::npos) {
            size_t start_obj = clean.find('{', kw_pos);
            size_t end_obj = find_balanced(clean, '{', '}', start_obj);
            if (start_obj != std::string::npos && end_obj != std::string::npos) {
                std::string obj_content = clean.substr(start_obj + 1, end_obj - start_obj - 1);
                size_t p = 0;
                while (true) {
                    size_t name_start = obj_content.find('"', p);
                    if (name_start == std::string::npos) break;
                    size_t name_end = obj_content.find('"', name_start + 1);
                    if (name_end == std::string::npos) break;
                    std::string kw_style = obj_content.substr(name_start + 1, name_end - name_start - 1);
                    
                    size_t arr_start = obj_content.find('[', name_end);
                    size_t arr_end = find_balanced(obj_content, '[', ']', arr_start);
                    if (arr_start != std::string::npos && arr_end != std::string::npos) {
                        std::string arr_content = obj_content.substr(arr_start + 1, arr_end - arr_start - 1);
                        size_t ap = 0;
                        while (true) {
                            size_t q1 = arr_content.find('"', ap);
                            if (q1 == std::string::npos) break;
                            size_t q2 = arr_content.find('"', q1 + 1);
                            if (q2 == std::string::npos) break;
                            new_style.keywords[kw_style].push_back(arr_content.substr(q1 + 1, q2 - q1 - 1));
                            ap = q2 + 1;
                        }
                        p = arr_end + 1;
                    } else {
                        p = name_end + 1;
                    }
                }
            }
        }

        // Parse patterns
        size_t pat_pos = clean.find("\"patterns\"");
        if (pat_pos != std::string::npos) {
            size_t start_arr = clean.find('[', pat_pos);
            size_t end_arr = find_balanced(clean, '[', ']', start_arr);
            if (start_arr != std::string::npos && end_arr != std::string::npos) {
                std::string arr_content = clean.substr(start_arr + 1, end_arr - start_arr - 1);
                size_t p = 0;
                while (true) {
                    size_t start_obj = arr_content.find('{', p);
                    if (start_obj == std::string::npos) break;
                    size_t end_obj = find_balanced(arr_content, '{', '}', start_obj);
                    if (end_obj == std::string::npos) break;
                    std::string obj_content = arr_content.substr(start_obj + 1, end_obj - start_obj - 1);
                    
                    std::string style_name, regex_str;
                    size_t style_pos = obj_content.find("\"style\"");
                    if (style_pos != std::string::npos) {
                        size_t q1 = obj_content.find('"', style_pos + 7);
                        size_t q2 = obj_content.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos)
                            style_name = obj_content.substr(q1 + 1, q2 - q1 - 1);
                    }
                    size_t reg_pos = obj_content.find("\"regex\"");
                    if (reg_pos != std::string::npos) {
                        size_t q1 = obj_content.find('"', reg_pos + 7);
                        size_t q2 = obj_content.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos)
                            regex_str = obj_content.substr(q1 + 1, q2 - q1 - 1);
                    }
                    
                    if (!style_name.empty() && !regex_str.empty()) {
                        try {
                            new_style.patterns.push_back({style_name, std::regex(regex_str), regex_str});
                        } catch (const std::regex_error& e) {
                            std::cerr << "[intellij] Regex compilation error for: " << regex_str << " - " << e.what() << "\n";
                        }
                    }
                    p = end_obj + 1;
                }
            }
        }
    } catch (...) {
        std::cerr << "[intellij] Critical parsing error while loading " << path << "\n";
        return false;
    }
    
    compile_keywords(new_style);
    style_ = new_style;
    current_style_path_ = path;
    std::cout << "[intellij] Successfully loaded style sheet: " << style_.name << "\n";
    return true;
}

bool IntelliJ::load_style_from_memory(const std::string& content) {
    // Reset style structure
    StyxStyleSheet new_style;
    new_style.line_comment = "--"; // default
    
    try {
        // Strip block and line comments from configuration text (string-quote aware)
        std::string clean;
        size_t idx = 0;
        bool in_quote = false;
        char quote_char = 0;
        while (idx < content.size()) {
            char c = content[idx];
            if ((c == '"' || c == '\'') && (idx == 0 || content[idx-1] != '\\')) {
                if (!in_quote) {
                    in_quote = true;
                    quote_char = c;
                } else if (quote_char == c) {
                    in_quote = false;
                }
                clean += content[idx++];
            } else if (!in_quote && idx + 1 < content.size() && content[idx] == '/' && content[idx+1] == '/') {
                while (idx < content.size() && content[idx] != '\n') idx++;
            } else if (!in_quote && idx + 1 < content.size() && content[idx] == '-' && content[idx+1] == '-') {
                while (idx < content.size() && content[idx] != '\n') idx++;
            } else {
                clean += content[idx++];
            }
        }
        
        // Parse name/extensions
        size_t name_pos = clean.find("\"name\"");
        if (name_pos != std::string::npos) {
            size_t start_arr = clean.find('[', name_pos);
            size_t end_arr = find_balanced(clean, '[', ']', start_arr);
            if (start_arr != std::string::npos && end_arr != std::string::npos) {
                std::string arr_content = clean.substr(start_arr + 1, end_arr - start_arr - 1);
                size_t p = 0;
                while (true) {
                    size_t quote_start = arr_content.find('"', p);
                    if (quote_start == std::string::npos) break;
                    size_t quote_end = arr_content.find('"', quote_start + 1);
                    if (quote_end == std::string::npos) break;
                    std::string val = arr_content.substr(quote_start + 1, quote_end - quote_start - 1);
                    if (!val.empty()) {
                        if (new_style.name.empty()) new_style.name = val;
                        else new_style.extensions.push_back(val);
                    }
                    p = quote_end + 1;
                }
            }
        }

        // Parse comments
        size_t comments_pos = clean.find("\"comments\"");
        if (comments_pos != std::string::npos) {
            size_t start_obj = clean.find('{', comments_pos);
            size_t end_obj = find_balanced(clean, '{', '}', start_obj);
            if (start_obj != std::string::npos && end_obj != std::string::npos) {
                std::string obj_content = clean.substr(start_obj + 1, end_obj - start_obj - 1);
                size_t line_pos = obj_content.find("\"line\"");
                if (line_pos != std::string::npos) {
                    size_t q1 = obj_content.find('"', line_pos + 6);
                    size_t q2 = obj_content.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        new_style.line_comment = obj_content.substr(q1 + 1, q2 - q1 - 1);
                }
                size_t bs_pos = obj_content.find("\"block_start\"");
                if (bs_pos != std::string::npos) {
                    size_t q1 = obj_content.find('"', bs_pos + 13);
                    size_t q2 = obj_content.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        new_style.block_comment_start = obj_content.substr(q1 + 1, q2 - q1 - 1);
                }
                size_t be_pos = obj_content.find("\"block_end\"");
                if (be_pos != std::string::npos) {
                    size_t q1 = obj_content.find('"', be_pos + 11);
                    size_t q2 = obj_content.find('"', q1 + 1);
                    if (q1 != std::string::npos && q2 != std::string::npos)
                        new_style.block_comment_end = obj_content.substr(q1 + 1, q2 - q1 - 1);
                }
            }
        }

        // Parse styles
        size_t styles_pos = clean.find("\"styles\"");
        if (styles_pos != std::string::npos) {
            size_t start_obj = clean.find('{', styles_pos);
            size_t end_obj = find_balanced(clean, '{', '}', start_obj);
            if (start_obj != std::string::npos && end_obj != std::string::npos) {
                std::string obj_content = clean.substr(start_obj + 1, end_obj - start_obj - 1);
                size_t p = 0;
                while (true) {
                    size_t name_start = obj_content.find('"', p);
                    if (name_start == std::string::npos) break;
                    size_t name_end = obj_content.find('"', name_start + 1);
                    if (name_end == std::string::npos) break;
                    std::string style_name = obj_content.substr(name_start + 1, name_end - name_start - 1);
                    
                    size_t colon = obj_content.find(':', name_end);
                    if (colon != std::string::npos) {
                        size_t val_start = obj_content.find('{', colon);
                        size_t val_end = find_balanced(obj_content, '{', '}', val_start);
                        if (val_start != std::string::npos && val_end != std::string::npos) {
                            std::string style_props = obj_content.substr(val_start + 1, val_end - val_start - 1);
                            StyxColor sc;
                            size_t col_pos = style_props.find("\"color\"");
                            if (col_pos != std::string::npos) {
                                size_t q1 = style_props.find('"', col_pos + 7);
                                size_t q2 = style_props.find('"', q1 + 1);
                                if (q1 != std::string::npos && q2 != std::string::npos)
                                    sc.color = parse_hex_color(style_props.substr(q1 + 1, q2 - q1 - 1));
                            }
                            sc.bold = parse_bool_prop(style_props, "bold");
                            sc.italic = parse_bool_prop(style_props, "italic");
                            sc.underline = parse_bool_prop(style_props, "underline");
                            new_style.styles[style_name] = sc;
                            p = val_end + 1;
                        } else {
                            p = name_end + 1;
                        }
                    } else {
                        p = name_end + 1;
                    }
                }
            }
        }

        // Parse keywords
        size_t kw_pos = clean.find("\"keywords\"");
        if (kw_pos != std::string::npos) {
            size_t start_obj = clean.find('{', kw_pos);
            size_t end_obj = find_balanced(clean, '{', '}', start_obj);
            if (start_obj != std::string::npos && end_obj != std::string::npos) {
                std::string obj_content = clean.substr(start_obj + 1, end_obj - start_obj - 1);
                size_t p = 0;
                while (true) {
                    size_t name_start = obj_content.find('"', p);
                    if (name_start == std::string::npos) break;
                    size_t name_end = obj_content.find('"', name_start + 1);
                    if (name_end == std::string::npos) break;
                    std::string kw_style = obj_content.substr(name_start + 1, name_end - name_start - 1);
                    
                    size_t arr_start = obj_content.find('[', name_end);
                    size_t arr_end = find_balanced(obj_content, '[', ']', arr_start);
                    if (arr_start != std::string::npos && arr_end != std::string::npos) {
                        std::string arr_content = obj_content.substr(arr_start + 1, arr_end - arr_start - 1);
                        size_t ap = 0;
                        while (true) {
                            size_t q1 = arr_content.find('"', ap);
                            if (q1 == std::string::npos) break;
                            size_t q2 = arr_content.find('"', q1 + 1);
                            if (q2 == std::string::npos) break;
                            new_style.keywords[kw_style].push_back(arr_content.substr(q1 + 1, q2 - q1 - 1));
                            ap = q2 + 1;
                        }
                        p = arr_end + 1;
                    } else {
                        p = name_end + 1;
                    }
                }
            }
        }

        // Parse patterns
        size_t pat_pos = clean.find("\"patterns\"");
        if (pat_pos != std::string::npos) {
            size_t start_arr = clean.find('[', pat_pos);
            size_t end_arr = find_balanced(clean, '[', ']', start_arr);
            if (start_arr != std::string::npos && end_arr != std::string::npos) {
                std::string arr_content = clean.substr(start_arr + 1, end_arr - start_arr - 1);
                size_t p = 0;
                while (true) {
                    size_t start_obj = arr_content.find('{', p);
                    if (start_obj == std::string::npos) break;
                    size_t end_obj = find_balanced(arr_content, '{', '}', start_obj);
                    if (end_obj == std::string::npos) break;
                    std::string obj_content = arr_content.substr(start_obj + 1, end_obj - start_obj - 1);
                    
                    std::string style_name, regex_str;
                    size_t style_pos = obj_content.find("\"style\"");
                    if (style_pos != std::string::npos) {
                        size_t q1 = obj_content.find('"', style_pos + 7);
                        size_t q2 = obj_content.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos)
                            style_name = obj_content.substr(q1 + 1, q2 - q1 - 1);
                    }
                    size_t reg_pos = obj_content.find("\"regex\"");
                    if (reg_pos != std::string::npos) {
                        size_t q1 = obj_content.find('"', reg_pos + 7);
                        size_t q2 = obj_content.find('"', q1 + 1);
                        if (q1 != std::string::npos && q2 != std::string::npos)
                            regex_str = obj_content.substr(q1 + 1, q2 - q1 - 1);
                    }
                    
                    if (!style_name.empty() && !regex_str.empty()) {
                        try {
                            new_style.patterns.push_back({style_name, std::regex(regex_str), regex_str});
                        } catch (const std::regex_error& e) {
                            std::cerr << "[intellij] Regex compilation error for: " << regex_str << " - " << e.what() << "\n";
                        }
                    }
                    p = end_obj + 1;
                }
            }
        }
    } catch (...) {
        std::cerr << "[intellij] Critical parsing error while loading memory style\n";
        return false;
    }
    
    compile_keywords(new_style);
    style_ = new_style;
    current_style_path_ = "memory://" + style_.name;
    std::cout << "[intellij] Loaded embedded style sheet: " << style_.name << "\n";
    return true;
}

void IntelliJ::load_default_style() {
    style_.name = "Default Dark";
    style_.line_comment = "--";
    style_.block_comment_start = "--[[";
    style_.block_comment_end = "]]";
    
    // IntelliJ Darcula / VS Code dark default style mapping
    style_.styles["keyword"] = {ImVec4(0.80f, 0.47f, 0.20f, 1.0f), true, false, false}; // Orange
    style_.styles["string"] = {ImVec4(0.41f, 0.53f, 0.35f, 1.0f), false, false, false};  // Green
    style_.styles["number"] = {ImVec4(0.41f, 0.59f, 0.73f, 1.0f), false, false, false};  // Blue-cyan
    style_.styles["comment"] = {ImVec4(0.50f, 0.50f, 0.50f, 1.0f), false, true, false};  // Gray
    style_.styles["type"] = {ImVec4(0.86f, 0.35f, 0.35f, 1.0f), false, false, false};     // Red-orange
    style_.styles["call"] = {ImVec4(0.66f, 0.78f, 0.98f, 1.0f), false, false, false};     // Light blue
    style_.styles["constant"] = {ImVec4(0.90f, 0.90f, 0.90f, 1.0f), true, false, false};  // White/gray
    style_.styles["default"] = {ImVec4(0.85f, 0.85f, 0.85f, 1.0f), false, false, false};  // Off white

    style_.keywords["keyword"] = {"local", "global", "return", "then", "while", "and", "break", "do", "else", "elseif", "end", "for", "function", "if", "in", "not", "or", "repeat", "until"};
    style_.keywords["constant"] = {"true", "false", "nil", "T", "F"};
    style_.keywords["type"] = {"boolean", "userdata", "String", "Object", "Template", "Zone", "Program"};

    style_.patterns.push_back({"comment", std::regex("--.*|#.*|//.*"), "--.*|#.*|//.*"});
    style_.patterns.push_back({"string", std::regex("\"[^\"]*\"|'[^']*'"), "\"[^\"]*\"|'[^']*'"});
    style_.patterns.push_back({"number", std::regex("\\b\\d+(\\.\\d+)?\\b"), "\\b\\d+(\\.\\d+)?\\b"});
    style_.patterns.push_back({"call", std::regex("\\b[a-zA-Z_]\\w*(?=\\s*\\()"), "\\b[a-zA-Z_]\\w*(?=\\s*\\()"});
    compile_keywords(style_);
}

std::vector<EditorToken> IntelliJ::tokenize_line(const std::string& line, bool is_dark_theme) {
    std::vector<EditorToken> tokens;
    tokenize_line_stateful(line, LexState::NORMAL, is_dark_theme, tokens);
    return tokens;
}

LexState IntelliJ::tokenize_line_stateful(const std::string& line, LexState in_state, bool is_dark_theme, std::vector<EditorToken>& out_tokens) {
    out_tokens.clear();
    if (line.empty()) return in_state;

    ImVec4 def_col = is_dark_theme ? ImVec4(0.85f, 0.85f, 0.85f, 1.0f) : ImVec4(0.15f, 0.15f, 0.15f, 1.0f);
    StyxColor default_sty = {def_col, false, false, false};
    if (style_.styles.find("default") != style_.styles.end()) default_sty = style_.styles["default"];
    StyxColor comment_sty = style_.styles.count("comment") ? style_.styles["comment"] : default_sty;

    // Handle multiline block comment continuation
    if (in_state == LexState::IN_BLOCK_COMMENT) {
        size_t end_pos = line.find("]]");
        if (end_pos != std::string::npos) {
            out_tokens.push_back({line.substr(0, end_pos + 2), comment_sty});
            std::string remainder = line.substr(end_pos + 2);
            if (!remainder.empty()) {
                std::vector<EditorToken> rem_tokens;
                tokenize_line_stateful(remainder, LexState::NORMAL, is_dark_theme, rem_tokens);
                out_tokens.insert(out_tokens.end(), rem_tokens.begin(), rem_tokens.end());
            }
            return LexState::NORMAL;
        } else {
            out_tokens.push_back({line, comment_sty});
            return LexState::IN_BLOCK_COMMENT;
        }
    }

    // Standard pattern matching
    struct MatchedRange {
        size_t start;
        size_t end;
        StyxColor style;
        size_t pattern_index;
    };
    std::vector<MatchedRange> matches;

    for (size_t p_idx = 0; p_idx < style_.patterns.size(); ++p_idx) {
        const auto& pat = style_.patterns[p_idx];
        try {
            auto words_begin = std::sregex_iterator(line.begin(), line.end(), pat.regex_pattern);
            auto words_end = std::sregex_iterator();
            
            for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
                std::smatch match = *i;
                size_t start = match.position();
                size_t end = start + match.length();
                StyxColor sty = default_sty;
                if (style_.styles.find(pat.style_name) != style_.styles.end()) {
                    sty = style_.styles[pat.style_name];
                }
                matches.push_back({start, end, sty, p_idx});
            }
        } catch (...) {}
    }

    std::sort(matches.begin(), matches.end(), [](const MatchedRange& a, const MatchedRange& b) {
        if (a.start != b.start) return a.start < b.start;
        size_t len_a = a.end - a.start;
        size_t len_b = b.end - b.start;
        if (len_a != len_b) return len_a > len_b;
        return a.pattern_index < b.pattern_index;
    });

    std::vector<MatchedRange> clean_matches;
    size_t last_end = 0;
    for (const auto& m : matches) {
        if (m.start >= last_end) {
            clean_matches.push_back(m);
            last_end = m.end;
        }
    }

    size_t current_idx = 0;
    for (const auto& m : clean_matches) {
        if (m.start > current_idx) {
            std::string text = line.substr(current_idx, m.start - current_idx);
            out_tokens.push_back({text, default_sty});
        }
        std::string text = line.substr(m.start, m.end - m.start);
        out_tokens.push_back({text, m.style});
        current_idx = m.end;
    }
    if (current_idx < line.size()) {
        std::string text = line.substr(current_idx);
        out_tokens.push_back({text, default_sty});
    }

    // Check if line opens an unclosed block comment `--[[`
    size_t block_start = line.find("--[[");
    if (block_start != std::string::npos && line.find("]]", block_start + 4) == std::string::npos) {
        return LexState::IN_BLOCK_COMMENT;
    }

    return LexState::NORMAL;
}

// String- and Comment-aware stack bracket checker
std::vector<BracketError> IntelliJ::check_syntax_errors(const std::string& text) {
    std::vector<BracketError> errors;
    
    struct OpenBracket {
        char ch;
        int line;
        int col;
        size_t index;
    };
    std::stack<OpenBracket> stack;

    int line_num = 1;
    int col_num = 1;
    
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool in_line_comment = false;
    bool in_block_comment = false;

    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        
        if (c == '\n') {
            line_num++;
            col_num = 1;
            in_line_comment = false;
            in_single_quote = false;
            in_double_quote = false;
            continue;
        }

        // Check comment start/end
        if (!in_single_quote && !in_double_quote) {
            if (!in_block_comment && i + 3 < text.size() && text[i] == '-' && text[i+1] == '-' && text[i+2] == '[' && text[i+3] == '[') {
                in_block_comment = true;
                i += 3;
                col_num += 3;
                continue;
            }
            if (in_block_comment && i + 1 < text.size() && text[i] == ']' && text[i+1] == ']') {
                in_block_comment = false;
                i += 1;
                col_num += 1;
                continue;
            }
            if (!in_block_comment && i + 1 < text.size() && ((text[i] == '-' && text[i+1] == '-') || (text[i] == '/' && text[i+1] == '/'))) {
                in_line_comment = true;
            }
        }

        // Check quote state
        if (!in_line_comment && !in_block_comment) {
            if (c == '"' && (i == 0 || text[i-1] != '\\') && !in_single_quote) {
                in_double_quote = !in_double_quote;
            } else if (c == '\'' && (i == 0 || text[i-1] != '\\') && !in_double_quote) {
                in_single_quote = !in_single_quote;
            }
        }

        // ONLY process brackets when in NORMAL code state
        if (!in_line_comment && !in_block_comment && !in_single_quote && !in_double_quote) {
            if (c == '{' || c == '(' || c == '[') {
                stack.push({c, line_num, col_num, i});
            } else if (c == '}' || c == ')' || c == ']') {
                char expected = 0;
                if (c == '}') expected = '{';
                else if (c == ')') expected = '(';
                else if (c == ']') expected = '[';

                if (stack.empty()) {
                    errors.push_back({line_num, col_num, c, "Unmatched closing bracket '" + std::string(1, c) + "'"});
                } else {
                    OpenBracket top = stack.top();
                    if (top.ch != expected) {
                        errors.push_back({line_num, col_num, c, "Mismatched bracket: expected '" + std::string(1, expected) + "' for '" + std::string(1, top.ch) + "'"});
                    }
                    stack.pop();
                }
            }
        }
        col_num++;
    }

    while (!stack.empty()) {
        OpenBracket top = stack.top();
        errors.push_back({top.line, top.col, top.ch, "Unmatched opening bracket '" + std::string(1, top.ch) + "'"});
        stack.pop();
    }

    return errors;
}

std::vector<SwordigoApiSymbol> IntelliJ::get_swordigo_autocomplete(const std::string& query) {
    static const std::vector<SwordigoApiSymbol> api_database = {
        {"Game.GetHero()", "Game", "Character* Game.GetHero()", "Returns pointer to the primary player character hero instance."},
        {"Game.LoadScene(scene_name)", "Game", "void Game.LoadScene(string name)", "Asynchronously streams and transitions to target scene file."},
        {"Game.GetLevelName()", "Game", "string Game.GetLevelName()", "Returns active scene map/dungeon filename."},
        {"Game.PlaySound(sound_id)", "Game", "void Game.PlaySound(string id)", "Plays dynamic spatial sound effect from SoundLibrary."},
        {"Character.GetHealth()", "Character", "float Character.GetHealth()", "Returns current hero health points."},
        {"Character.SetHealth(val)", "Character", "void Character.SetHealth(float val)", "Updates hero health and notifies HealthBar GUI."},
        {"Character.AddCoins(amount)", "Character", "void Character.AddCoins(int count)", "Adds currency coins to hero inventory."},
        {"Character.ApplyArmorTrinket(item)", "Character", "void Character.ApplyArmorTrinket(Item* item)", "Equips armor trinket modifiers."},
        {"Character.ApplyWeaponTrinket(item)", "Character", "void Character.ApplyWeaponTrinket(Item* item)", "Equips sword magic element trinket."},
        {"Scene.FindObject(name)", "Scene", "SceneObject* Scene.FindObject(string name)", "Queries scene tree for object by string ID."},
        {"Scene.SpawnEntity(proto, pos)", "Scene", "Entity* Scene.SpawnEntity(string proto, Vector3 pos)", "Instantiates new game entity at target coordinates."},
        {"PhysicsObject.SetVelocity(vec)", "PhysicsObject", "void PhysicsObject.SetVelocity(Vector3 v)", "Applies linear velocity vector."},
        {"Vector3(x, y, z)", "Math", "Vector3 Vector3(float x, float y, float z)", "3D spatial coordinate vector constructor."},
        {"Shardshi.ExecuteCommand(cmd)", "Engine", "void Shardshi.ExecuteCommand(string cmd)", "Executes low-level Shardshi console engine command."}
    };

    std::vector<SwordigoApiSymbol> results;
    for (const auto& sym : api_database) {
        if (query.empty() || sym.name.find(query) != std::string::npos || sym.category.find(query) != std::string::npos) {
            results.push_back(sym);
        }
    }
    return results;
}

// Render unified code editor + viewer
void IntelliJ::draw_editor(const char* label, std::string* buffer, bool& modified, const std::string& filepath, ImFont* mono_font, ImVec4& custom_bg,
                           bool has_compile_result, bool compile_success, const std::string& compile_error_msg, double compile_time_ms) {
    ImGuiContext& g = *GImGui;
    bool is_dark_theme = (g.Style.Colors[ImGuiCol_Text].x + g.Style.Colors[ImGuiCol_Text].y + g.Style.Colors[ImGuiCol_Text].z) / 3.0f > 0.5f;
    
    static float zoom = 1.0f;
    
    // Calculate lines
    std::vector<std::string> lines;
    {
        std::istringstream iss(*buffer);
        std::string line;
        while (std::getline(iss, line)) {
            lines.push_back(line);
        }
        if (buffer->empty() || buffer->back() == '\n') {
            lines.push_back("");
        }
    }
    
    // Check for syntax errors (comment and string aware)
    std::vector<BracketError> errors = check_syntax_errors(*buffer);
    
    float line_h = ImGui::GetTextLineHeight();
    char max_line_str[32];
    snprintf(max_line_str, sizeof(max_line_str), "%d", (int)lines.size());
    float max_line_w = ImGui::CalcTextSize(max_line_str).x;
    float gutter_w = max_line_w + 24.0f;
    
    ImGui::BeginChild("EditorContainer", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 4));
    ImGui::BeginChild("Toolbar", ImVec2(0, 36.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), ICON_FA_CODE " Swordfare Editor");
    ImGui::SameLine();
    ImGui::TextDisabled("— %s", filepath.c_str());
    
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 220.0f);
    ImGui::SetNextItemWidth(70.0f);
    ImGui::SliderFloat("##zoom", &zoom, 0.7f, 2.0f, "%.1fx");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Font Zoom Scale");
    
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_COPY " Copy All")) {
        ImGui::SetClipboardText(buffer->c_str());
    }
    
    ImGui::EndChild();
    ImGui::PopStyleVar();
    
    ImGui::Separator();
    
    ImVec4 editor_bg = is_dark_theme ? ImVec4(0.10f, 0.10f, 0.13f, 1.0f) : ImVec4(0.97f, 0.97f, 0.98f, 1.0f);
    if (custom_bg.w > 0.0f) {
        editor_bg = custom_bg;
    } else if (style_.styles.find("background") != style_.styles.end()) {
        editor_bg = style_.styles["background"].color;
    }
    
    ImVec4 gutter_bg = editor_bg;
    if (is_dark_theme) {
        gutter_bg.x = std::max(0.0f, gutter_bg.x - 0.02f);
        gutter_bg.y = std::max(0.0f, gutter_bg.y - 0.02f);
        gutter_bg.z = std::max(0.0f, gutter_bg.z - 0.02f);
    } else {
        gutter_bg.x = std::min(1.0f, gutter_bg.x + 0.02f);
        gutter_bg.y = std::min(1.0f, gutter_bg.y + 0.02f);
        gutter_bg.z = std::min(1.0f, gutter_bg.z + 0.02f);
    }
    
    float editor_w = ImGui::GetContentRegionAvail().x - gutter_w;
    float editor_h = ImGui::GetContentRegionAvail().y - 24.0f;
    
    static std::unordered_map<std::string, ImVec2> s_editor_scroll;
    std::string label_key(label);
    ImVec2 prev_scroll = s_editor_scroll.count(label_key) ? s_editor_scroll.at(label_key) : ImVec2(0.f, 0.f);
    float scroll_y = prev_scroll.y;
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, gutter_bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 8.0f));
    ImGui::BeginChild("GutterChild", ImVec2(gutter_w, editor_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetScrollY(scroll_y);
    
    ImDrawList* gutter_dl = ImGui::GetWindowDrawList();
    ImVec2 gutter_start = ImGui::GetWindowPos() + ImVec2(0.0f, 8.0f) - ImVec2(0.0f, scroll_y);
    
    if (mono_font) ImGui::PushFont(mono_font);
    
    int first_visible = (int)(scroll_y / line_h);
    int last_visible = first_visible + (int)(editor_h / line_h) + 2;
    first_visible = std::max(0, first_visible);
    last_visible = std::min((int)lines.size(), last_visible);
    
    std::vector<int> error_lines;
    for (const auto& err : errors) {
        error_lines.push_back(err.line - 1);
    }
    
    for (int line_idx = first_visible; line_idx < last_visible; line_idx++) {
        ImVec2 line_pos = gutter_start + ImVec2(0.0f, line_idx * line_h);
        char line_str[32];
        snprintf(line_str, sizeof(line_str), "%d", line_idx + 1);
        float num_w = ImGui::CalcTextSize(line_str).x;
        ImVec2 num_pos = ImVec2(ImGui::GetWindowPos().x + gutter_w - 8.0f - num_w, line_pos.y);
        
        bool has_error = std::find(error_lines.begin(), error_lines.end(), line_idx) != error_lines.end();
        if (has_error) {
            gutter_dl->AddCircleFilled(ImVec2(ImGui::GetWindowPos().x + 8.0f, line_pos.y + line_h*0.5f), 3.5f, IM_COL32(230, 60, 60, 255));
            gutter_dl->AddText(num_pos, IM_COL32(230, 80, 80, 255), line_str);
        } else {
            gutter_dl->AddText(num_pos, is_dark_theme ? IM_COL32(110, 110, 115, 255) : IM_COL32(145, 145, 150, 255), line_str);
        }
    }
    
    gutter_dl->AddLine(
        ImVec2(ImGui::GetWindowPos().x + gutter_w - 1.0f, ImGui::GetWindowPos().y),
        ImVec2(ImGui::GetWindowPos().x + gutter_w - 1.0f, ImGui::GetWindowPos().y + editor_h),
        is_dark_theme ? IM_COL32(50, 50, 55, 255) : IM_COL32(200, 200, 205, 255),
        1.0f
    );
    
    if (mono_font) ImGui::PopFont();
    ImGui::EndChild(); // GutterChild
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    
    ImGui::SameLine(0.0f, 0.0f);
    
    ImGui::PushStyleColor(ImGuiCol_ChildBg, editor_bg);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0.004f));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, is_dark_theme ? ImVec4(0.18f, 0.35f, 0.55f, 0.70f) : ImVec4(0.65f, 0.80f, 0.95f, 0.80f));
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    
    if (mono_font) ImGui::PushFont(mono_font);
    
    ImGui::BeginChild("EditorTextChild", ImVec2(editor_w, editor_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    
    std::string id_label = std::string("##editor_") + label;
    if (buffer->capacity() < buffer->size() + 1024) {
        buffer->reserve(buffer->size() + 2048);
    }
    
    auto editor_cb = [](ImGuiInputTextCallbackData* cb_data) -> int {
        if (cb_data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            std::string* str = (std::string*)cb_data->UserData;
            str->resize(cb_data->BufTextLen);
            cb_data->Buf = str->data();
        }
        return 0;
    };
    
    bool changed = ImGui::InputTextMultiline(
        id_label.c_str(), 
        buffer->data(), 
        buffer->capacity() + 1, 
        ImGui::GetContentRegionAvail(), 
        ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackResize, 
        editor_cb, 
        buffer
    );
    
    if (changed) {
        modified = true;
        token_cache_.clear(); // Invalidate line token cache on edits
    }
    
    ImGuiWindow* etch_win = ImGui::GetCurrentWindow();
    ImGuiWindow* text_inner_win = nullptr;
    for (int i = 0; i < etch_win->DC.ChildWindows.Size; i++) {
        if (etch_win->DC.ChildWindows[i]) {
            text_inner_win = etch_win->DC.ChildWindows[i];
            break;
        }
    }

    ImVec2 inner_scroll(0.f, 0.f);
    if (text_inner_win) inner_scroll = text_inner_win->Scroll;

    s_editor_scroll[label_key] = inner_scroll;
    ImDrawList* dl = text_inner_win ? text_inner_win->DrawList : etch_win->DrawList;
    ImVec2 start_pos = etch_win->Pos + ImVec2(4.0f, 8.0f) - inner_scroll;

    first_visible = (int)(inner_scroll.y / line_h);
    last_visible = first_visible + (int)(etch_win->Size.y / line_h) + 2;
    first_visible = std::max(0, first_visible);
    last_visible = std::min((int)lines.size(), last_visible);

    // Multiline state propagation across visible lines
    LexState line_lex_state = LexState::NORMAL;
    for (int i = 0; i < first_visible; i++) {
        std::vector<EditorToken> dummy;
        line_lex_state = tokenize_line_stateful(lines[i], line_lex_state, is_dark_theme, dummy);
    }
    
    for (int line_idx = first_visible; line_idx < last_visible; line_idx++) {
        ImVec2 line_pos = start_pos + ImVec2(0.0f, line_idx * line_h);
        bool has_error = std::find(error_lines.begin(), error_lines.end(), line_idx) != error_lines.end();
        
        const std::string& current_line = lines[line_idx];
        std::vector<EditorToken> tokens;
        line_lex_state = tokenize_line_stateful(current_line, line_lex_state, is_dark_theme, tokens);
        
        ImVec2 draw_cursor = line_pos;
        for (const auto& tok : tokens) {
            if (tok.text.empty()) continue;
            
            ImU32 col_val = ImGui::ColorConvertFloat4ToU32(tok.style.color);
            dl->AddText(draw_cursor, col_val, tok.text.c_str());
            
            if (tok.style.underline) {
                float text_w = ImGui::CalcTextSize(tok.text.c_str()).x;
                dl->AddLine(draw_cursor + ImVec2(0.0f, line_h - 1.0f), draw_cursor + ImVec2(text_w, line_h - 1.0f), col_val, 1.0f);
            }
            
            draw_cursor.x += ImGui::CalcTextSize(tok.text.c_str()).x;
        }
        
        if (has_error) {
            float line_w = ImGui::CalcTextSize(current_line.c_str()).x;
            if (line_w < 10.0f) line_w = 20.0f;
            ImVec2 err_start = line_pos + ImVec2(0.0f, line_h - 2.0f);
            ImVec2 err_end = err_start + ImVec2(line_w, 0.0f);
            dl->AddLine(err_start, err_end, IM_COL32(230, 60, 60, 200), 1.5f);
        }
    }
    
    ImGui::EndChild(); // EditorTextChild
    
    if (mono_font) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    
    // Status Bar / Footer with Swordigo Autocomplete trigger
    ImGui::Separator();
    ImGui::BeginChild("StatusBar", ImVec2(0, 20.0f), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 0.6f, 1.0f), ICON_FA_CIRCLE_INFO " Style: %s", style_.name.c_str());
    ImGui::SameLine(0.0f, 25.0f);
    ImGui::Text("Lines: %d", (int)lines.size());
    
    if (has_compile_result) {
        ImGui::SameLine(0.0f, 25.0f);
        if (compile_success) {
            ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.40f, 1.0f), ICON_FA_CHECK " Compiled successfully in %.1f ms", compile_time_ms);
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), ICON_FA_XMARK " Compile Error: %s", compile_error_msg.c_str());
        }
    } else {
        if (!errors.empty()) {
            ImGui::SameLine(0.0f, 25.0f);
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s (Line %d, Col %d)", errors[0].message.c_str(), errors[0].line, errors[0].column);
        } else {
            ImGui::SameLine(0.0f, 25.0f);
            ImGui::TextColored(ImVec4(0.40f, 0.85f, 0.40f, 1.0f), ICON_FA_CHECK " Code Syntax Ok");
        }
    }
    
    ImGui::EndChild(); // StatusBar
    ImGui::EndChild(); // EditorContainer
}

} // namespace intel
