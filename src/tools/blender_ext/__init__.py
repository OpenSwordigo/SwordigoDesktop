# Swordigo Round-Trip — Blender extension.
#
# Bridges Swordigo Desktop (Ruby asset viewer) and Blender:
#   * Ruby exports a POD model to the staging dir:  <staging>/in/model.glb
#     plus a request file                         <staging>/in/request.json
#   * This addon picks the GLB up, imports it into the scene and sets the
#     blend filepath so a plain Ctrl+S saves the session silently.
#   * On save_post the addon exports the current scene back as
#     <staging>/out/model.glb and writes <staging>/out/done.json so Ruby can
#     round-trip the result into a .pod asset and reload it in the viewer.
#
# The staging dir is provided by Ruby through the SWORDIGO_BLENDER_STAGING
# environment variable.

import bpy
import json
import os

_STAGING_ENV = "SWORDIGO_BLENDER_STAGING"
_POLL_INTERVAL = 0.5

# run id of the most recently imported request (0 = none)
_last_imported_run = 0


def _staging_dir():
    return os.environ.get(_STAGING_ENV, "")


def _in_dir():
    return os.path.join(_staging_dir(), "in")


def _out_dir():
    return os.path.join(_staging_dir(), "out")


def _read_request():
    req_path = os.path.join(_in_dir(), "request.json")
    if not os.path.isfile(req_path):
        return None
    try:
        with open(req_path, "r") as f:
            return json.load(f)
    except Exception:
        return None


def _clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        bpy.data.collections.remove(collection)


def _import_staged_glb(run_id):
    global _last_imported_run
    glb_path = os.path.join(_in_dir(), "model.glb")
    if not os.path.isfile(glb_path):
        return
    _clear_scene()
    bpy.ops.import_scene.gltf(filepath=glb_path)
    _last_imported_run = run_id
    print("[swordigo] imported", glb_path, "run", run_id)


def _poll_request():
    """Watch the staging in-dir for a new request and import it."""
    if not _staging_dir():
        return _POLL_INTERVAL
    req = _read_request()
    if not req:
        return _POLL_INTERVAL
    run_id = req.get("run", 0)
    if run_id != _last_imported_run:
        _import_staged_glb(run_id)
    return _POLL_INTERVAL


def _save_post_handler(_dummy):
    """After a scene save, export it back to the staging out-dir."""
    if not _staging_dir():
        return
    req = _read_request()
    run_id = req.get("run", 0) if req else _last_imported_run
    os.makedirs(_out_dir(), exist_ok=True)
    glb_out = os.path.join(_out_dir(), "model.glb")
    done = {"run": run_id, "status": "ok"}
    try:
        bpy.ops.export_scene.gltf(filepath=glb_out, export_format="GLB")
        print("[swordigo] exported", glb_out)
    except Exception as exc:  # noqa: BLE001
        done = {"run": run_id, "status": "error", "message": str(exc)}
    with open(os.path.join(_out_dir(), "done.json"), "w") as f:
        json.dump(done, f)


def register():
    bpy.app.timers.register(_poll_request, first_interval=1.0)
    bpy.app.handlers.save_post.append(_save_post_handler)
    print("[swordigo] Swordigo Round-Trip addon active; staging =",
          _staging_dir() or "(unset)")


def unregister():
    try:
        bpy.app.timers.unregister(_poll_request)
    except Exception:  # noqa: BLE001
        pass
    if _save_post_handler in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.remove(_save_post_handler)


if __name__ == "__main__":
    register()
