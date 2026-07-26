# Caver Level & Asset File Formats Specification

## 1. System Overview & Purpose

This document provides a comprehensive technical specification for all file formats used in Swordigo: `.scene` map files, `.scl` script files, `.pod` 3D mesh files, `.pvr` texture files, and `.pb` protobuf binaries.

It acts as the authoritative reference for tool developers working with **Boulder**, **FileRift**, **SwordigoEditor**, and the C++ PC port (`swd`).

---

## 2. Master File Formats Matrix

| File Extension | Format Type | Primary Content | Parsed By | Output / Engine Role |
| :--- | :--- | :--- | :--- | :--- |
| `.scene` | XML / Binary Protobuf | Level geometry, entity templates, polylines, portals, object links. | `Boulder`, `GameSceneController` | Full level scene graph structure. |
| `.scl` | Protobuf / Lua Bytecode | Scene Script Language routines, triggers, cutscenes. | `SCL Extractor`, `LuaEngine` | Scripted event & puzzle logic. |
| `.pod` | Binary (PowerVR POD) | 3D mesh geometry, bone hierarchies, material properties. | `FileRift`, `PODLoader` | Entity & prop 3D models. |
| `.pvr` | Compressed Texture | PVRTC / ETC1 mobile texture atlases. | `FileRift`, `TextureLibrary` | Game textures & UI atlases. |
| `.dat` / `.bin` | Binary / Protobuf | Serialized player profile save slots (`PlayerProfile`). | `SaveManager` | Saved game state & settings. |

---

## 3. `.scene` Level File Schema

A `.scene` file contains three main sections:

```xml
<Scene name="cairnwood_forest">
    <MapProperties bgm="bgm_forest.ogg" mapID="cairnwood_forest" />
    
    <!-- Section 1: Terrain Polylines -->
    <GroundPolygons>
        <Polygon layer="0x0001">
            <Vertex x="0.0" y="0.0" />
            <Vertex x="150.0" y="0.0" />
            <Vertex x="150.0" y="-20.0" />
            <Vertex x="0.0" y="-20.0" />
        </Polygon>
    </GroundPolygons>

    <!-- Section 2: Entity Templates & Components -->
    <Entities>
        <Entity name="door_forest_exit" type="DoorControllerComponent">
            <Transform x="140.0" y="5.0" z="0.0" />
            <Properties keyDoor="true" saveFlag="door_forest_cleared" />
        </Entity>
    </Entities>

    <!-- Section 3: Object Link Signal Wiring -->
    <ObjectLinks>
        <Link source="switch_plate_1" target="door_forest_exit" signal="OnActivated" action="Open" />
    </ObjectLinks>
</Scene>
```

---

## 4. Reverse Engineering & Tools Integration Notes

- **Boulder Editor**: Boulder parses `.scene` XML/binary trees into 2D/3D viewport rendering primitives, allowing visual polyline editing and entity placement.
- **FileRift Asset Extractor**: FileRift converts binary `.pod` and `.pvr` files into standard `.gltf` and `.png` formats for desktop editing.

---

## 5. PC Port (`swd`) Implementation Strategy

1. **Human-Readable JSON Conversion**: Support loading `.json` variants for all file formats (`level.json`, `item.json`, `save.json`) alongside legacy binary formats.
2. **Unified Asset Pipeline**: Provide an integrated asset converter utility in `swd` to batch-convert `.scene`, `.scl`, `.pod`, and `.pvr` files into modern desktop formats automatically.
