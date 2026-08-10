#pragma once
/* ruby_mcp.h — Model Context Protocol (MCP) server for the Ruby SDK
 *
 * Exposes Swordigo internals to AI agents through the Model Context Protocol
 * (JSON-RPC 2.0 over stdio — the standard local-server transport used by
 * Claude Desktop, Cline, Continue, etc.). 100% native C++17, zero new
 * dependencies: the JSON engine and protocol state machine live in
 * ruby_mcp.cpp and every tool is implemented on top of existing Ruby
 * modules (filerift, scene_loader, pod_loader, pvr_loader).
 *
 * Server entry points
 *   bin/ruby     --mcp-server [--mcp-root DIR]   headless stdio server
 *   bin/ruby_cli mcp [DIR]                       same server (client-friendly)
 *
 * In-app testing: Ruby menu  Help → MCP Console  provides an interactive
 * JSON-RPC tester (request editor + response viewer + tool list) without
 * needing an external MCP client.
 *
 * Tools exposed (tools/list for full schemas):
 *   scl_decode       decode an .scl script to readable markup/Lua
 *   scene_decode     decode a .scene file to FileRift markup text
 *   scene_objects    structured object/component/entity dump of a scene
 *   scene_summary    counts + bounds + libraries + waters/lights of a scene
 *   scene_programs   onload Lua programs attached to scene objects
 *   scene_templates  add-object palette (embedded + external .scl libraries)
 *   scene_libraries  external .scl libraries referenced by a scene
 *   search           string search across a whole folder (decoded-aware)
 *   search_scl       string search inside one decoded .scl
 *   file_info       size + detected type + quick details of any asset
 *   pod_info        structural summary of a .POD model
 *   pod_blocks      POD block-id histogram (search for blocks)
 *   list_files      enumerate assets under a directory
 *   texture_info    dimensions/format of .pvr/.tex/.png textures
 *   read_file       raw file view (text, offset pagination, hex dump)
 *   list_dir        ls: directory listing with sizes/types
 *   find_files      recursive glob/substring filename search
 *
 * Resources (resources/list + resources/read) mirror the asset tree under
 * the configured root and decode .scl/.scene files to text on read.
 */

#include <string>

namespace mcp {

// ─── Server entry ─────────────────────────────────────────────────────────
// Blocking MCP stdio server (JSON-RPC 2.0 over stdin/stdout, one message per
// line, as required by the MCP stdio transport). Returns the process exit
// code; pass a root_dir to override the resource root (else MCP_ROOT env,
// else the default Swordigo assets dir, else CWD).
int RunStdioServer(const std::string& root_dir = "");

// Blocking MCP HTTP server — Streamable HTTP (+ legacy SSE on GET) on
// http://127.0.0.1:<port>/mcp . This is the transport URL-based MCP clients
// need: ChatGPT desktop (Settings → MCP Server → URL, No Auth), Claude
// remote servers, Cursor, web clients. Local-only (loopback bind).
int RunHttpServer(int port = 8765, const std::string& root_dir = "");

// Handle a single inbound JSON-RPC line (object or batch array). Appends
// every response line (one complete JSON message each) to `out`. Returns
// false only when the line is not valid JSON (nothing appended).
bool HandleLine(const std::string& line, std::string& out);

// ─── Introspection / UI helpers ───────────────────────────────────────────
// One-line-per-tool listing (used by --help and the in-app console).
std::string ToolListText();

// The resolved resource root (see RunStdioServer for the override order).
std::string DefaultRootDir();

} // namespace mcp
