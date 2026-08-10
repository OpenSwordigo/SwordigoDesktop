/* ruby_cli.h — native FileRift-compatible command line tool (Ruby CLI).
 *
 * Mirrors FileRift 5.8.5's standalone CLI (decode / recode / both / user /
 * force / build / info) plus Ruby-specific extensions:
 *
 *   - APK extractor   : unpack a full APK (all entries) to a directory
 *   - APK signer      : sign an APK with the Android apksigner.jar
 *   - APK builder     : build an APK from a .frproject file (base + add +
 *                        recode + sign) — mirrors FileRift build.py
 *   - Batch converter : drive the Ruby batch texture converter engine
 *                       headlessly (export PVR/TEX -> PNG, import PNG ->
 *                       PVR/TEX) via batch::run_batch_headless()
 *
 * The decode/recode commands share one native backend (filerift::decode_protobuf
 * / filerift::recode_markup) and handle whole directories recursively, writing
 * output that mirrors the input tree. No Python subprocess is ever used.
 */

#pragma once

#include <string>
#include <vector>

namespace rubycli {

// ─── FileRift mode map (config.rift_mode equivalent) ─────────────────────────
enum class Mode {
    DECODE,   // binary .scene/.scl/etc -> markup   (de_in -> de_out)
    RECODE,   // markup -> binary                   (re_in -> re_out)
    BOTH,     // recode then decode
    USER,     // decode de_in/<user_folder>
    FORCE,    // recode with allways_recode
    BUILD,    // build APK from a .frproject
    PASS,     // nothing to do
};

// ─── CLI options (config.py equivalent, kept minimal) ────────────────────────
struct Options {
    Mode mode = Mode::PASS;
    bool allways_recode = false;   // -f / --force
    bool no_colour = false;        // -n / --no-colour
    bool style_snake_case = false; // --snake
    bool lua_checking = true;      // --lua-check / --no-lua-check
    std::string compile_mode = "trigger"; // never|trigger|all|auto|skip
    std::string user_folder = "user";
    std::string working_dir = ".";
    std::string output = "";       // -o / --output
    std::string file_type = "";    // -t / --file-type (stdin)
    std::string project_name = "default";
    std::vector<std::string> paths; // positional args to decode/recode

    // APK subcommands
    enum class ApkOp { NONE, EXTRACT, SIGN, BUILD } apk_op = ApkOp::NONE;
    std::string apk_input;        // apk path (extract/sign)
    std::string apk_out;          // output apk or dest dir
    std::string apksigner_path;   // --apksigner
    std::string apk_project_file; // .frproject path for build

    // Batch converter subcommand
    bool batch = false;
    bool batch_export = true;     // true=export game->PNG, false=import PNG->game
    bool batch_recurse = true;
    bool batch_skip_existing = false;
    int  batch_compress = 0;      // 0=ETC1, 1=PVRTC_4BPP, 3=RGBA8888
};

/* Entry point for `bin/ruby_cli`. Returns process exit code. */
int run_cli(int argc, char** argv);

/* ─── Shared backend (also usable by other tooling) ─────────────────────────── */

// Decode a single binary file to markup. Returns false + sets `error` on failure.
bool decode_file(const std::string& path, const Options& opt,
                 std::string& markup, std::string& error);

// Recode a single markup file to binary. Returns false + sets `error` on failure.
bool recode_file(const std::string& path, const Options& opt,
                 std::string& bytes, std::string& error);

// Recursively decode/recode a directory tree, writing to `out_dir` mirroring
// the relative layout under `in_dir`. Returns number of files handled.
int decode_directory(const std::string& in_dir, const std::string& out_dir,
                     const Options& opt);
int recode_directory(const std::string& in_dir, const std::string& out_dir,
                     const Options& opt);

// Filetype by extension (e.g. "scene", "scl", "gdata", "fr"). Lowercased.
std::string filetype_for_path(const std::string& path);

// ─── APK helpers ──────────────────────────────────────────────────────────────

// Extract every entry of `apk_path` into `dest_dir` (safe against traversal).
// Returns 0 on success, non-zero on error.
int apk_extract(const std::string& apk_path, const std::string& dest_dir);

// Sign `apk_path` in place using the Android apksigner.jar (java -jar ... -a).
// `apksigner` must point at apksigner.jar. Returns 0 on success.
int apk_sign(const std::string& apk_path, const std::string& apksigner);

// Build an APK from a FileRift .frproject. Returns 0 on success.
int apk_build(const std::string& project_file, const Options& opt);

// ─── Batch converter hook ─────────────────────────────────────────────────────
int run_batch_command(const Options& opt);

} // namespace rubycli
