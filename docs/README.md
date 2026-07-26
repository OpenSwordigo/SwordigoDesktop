# SwordigoDesktop & SRE Runtime Documentation Hub

Welcome to the central developer documentation and technical research library for **SwordigoDesktop** and the **Swordigo Runtime Engine (SRE)**.

---

## 1. Documentation Organization

```
docs/
|-- README.md                           <- Documentation Hub Index (this file)
|
|-- architecture/                       <- High-level engine & platform architecture specs
|   |-- sre_engine_architecture.md
|   |-- sre_platform_compatibility_master_plan.md
|   |-- swordigo_legacy_engine_removal_blueprint.md
|   |-- yuzu_arm64_dynarmic_optimization_research.md
|   |-- swkiwi_sre_master_plan.md
|   |-- swkiwi_sre_implementation_challenges.md
|   |-- linux_so_loader_design.md
|   |-- sdl3_migration_master_plan.md
|   |-- mini_api_integration_plan.md
|   `-- ps_vita_port_implementation_plan.md
|
|-- emulation_and_arm64/               <- ARM64 JIT execution, memory management & CPU debugging
|   |-- arm64_jit_correctness_and_exclusives.md
|   |-- arm64_freeze_investigation.md
|   |-- arm_execution_backends_comparison.md
|   |-- elf_relocation_handling_inventory.md
|   |-- elf_relocation_instrumentation_guide.md
|   |-- engine_boot_crash_analysis.md
|   |-- setup_application_root_cause_analysis.md
|   |-- unmapped_memory_fault_investigation.md
|   |-- pointer_provenance_and_memory_tracing.md
|   |-- guest_memory_layout_audit.md
|   |-- memory_crash_elimination_report.md
|   `-- caver_engine_v6_reversal.md
|
|-- graphics_and_rendering/            <- Render pipelines, GLES state management & shaders
|   |-- gles2_rendering_pipeline_plan.md
|   |-- fbo_viewport_scaler_design.md
|   |-- postfx_rendering_pipeline.md
|   |-- vanilla_postfx_reconstruction.md
|   |-- postfx_remaster_master_plan.md
|   |-- gpu_rendering_thread_architecture.md
|   |-- rendering_tweaks_and_shaders.md
|   |-- gles_context_bootstrap_matrix.md
|   `-- first_frame_render_roadmap.md
|
|-- apis_and_subsystems/               <- Engine subsystem APIs, hooks, Lua scripting & GUI
|   |-- sre_native_hooks_reference.md
|   |-- gui_and_overlay_api_reference.md
|   |-- input_system_architecture.md
|   |-- lua_scripting_api_catalog.md
|   |-- lua_debug_console_and_execution.md
|   |-- audio_and_music_system_api.md
|   |-- vfs_virtual_filesystem_api.md
|   |-- binary_selector_and_version_loader.md
|   |-- jni_bridge_compatibility_findings.md
|   |-- mod_configuration_system.md
|   |-- trampoline_hooks_and_touch_controls.md
|   `-- garbage_collection_and_overlay_findings.md
|
|-- formats_and_schemas/               <- Binary asset file formats & data serialization specs
|   |-- pod_3d_model_format_spec.md
|   |-- pvr_texture_format_spec.md
|   |-- scene_file_format_spec.md
|   |-- protobuf_wire_schema_spec.md
|   |-- save_file_format_and_editor.md
|   |-- game_data_layout_spec.md
|   `-- engine_json_schema_spec.md
|
|-- modding_and_swkiwi/                <- Modding framework & SwKiwi API specs
|   |-- swordigo_modding_guide.md
|   |-- swordigo_mod_loader_architecture.md
|   |-- swkiwi_architecture_analysis.md
|   |-- swkiwi_api_audit_and_symbol_map.md
|   |-- rlswordigo_70_mod_analysis.md
|   |-- rlswordigo_v6_mod_reversal.md
|   |-- swmc_mod_compatibility_audit.md
|   |-- swmc_rlswordigo_feature_roadmap.md
|   `-- swmini_feature_inventory.md
|
|-- release_notes/                      <- Version release notes & changelogs (v1.0 - v8.0)
|   `-- RELEASE_NOTES_v1.0.md through RELEASE_NOTES_v8.0_Beta_1.md
|
`-- misc/                              <- Diagnostic traces, compatibility matrices & build guides
    |-- bug_detection_audit.md
    |-- bug_ride_phase2.md
    |-- boot_sequence_trace.md
    |-- boot_observations_log.md
    |-- development_progress_log.md
    |-- early_research_notes.md
    |-- open_research_questions.md
    |-- runtime_integrity_audit.md
    |-- scene_component_library_notes.md
    |-- windows_build_guide.md
    `-- vita_compatibility_matrix.md
```

---

## 2. Quick Reference by Role

| Target Persona | Key Documentation Modules |
| :--- | :--- |
| **Engine Developers** | `architecture/sre_engine_architecture.md`, `emulation_and_arm64/arm64_jit_correctness_and_exclusives.md`, `apis_and_subsystems/sre_native_hooks_reference.md` |
| **Graphics / Shader Engineers** | `graphics_and_rendering/fbo_viewport_scaler_design.md`, `graphics_and_rendering/postfx_rendering_pipeline.md` |
| **Mod Developers** | `modding_and_swkiwi/swordigo_modding_guide.md`, `apis_and_subsystems/lua_scripting_api_catalog.md` |
| **Tool / Exporter Authors** | `formats_and_schemas/pod_3d_model_format_spec.md`, `formats_and_schemas/pvr_texture_format_spec.md` |
