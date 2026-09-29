"""Imports SourceAssets/ (from Tools/fetch_assets.py) into /Game/PolyHaven. Run headless:

    UnrealEditor-Cmd.exe <repo>/Explorer.uproject -run=pythonscript -script="<repo>/Tools/import_assets.py" -unattended

Textures get the right colour space / compression; glTF models import through Interchange as static
meshes with their materials, Nanite enabled. Safe to re-run (existing assets are replaced).
"""

import pathlib
import unreal

ROOT = pathlib.Path(__file__).resolve().parent.parent / "SourceAssets"
DEST = "/Game/PolyHaven"
tools = unreal.AssetToolsHelpers.get_asset_tools()


def import_files(files, dest):
    tasks = []
    for f in files:
        t = unreal.AssetImportTask()
        t.filename = str(f)
        t.destination_path = dest
        t.automated = True
        t.replace_existing = True
        t.save = True
        tasks.append(t)
    tools.import_asset_tasks(tasks)
    return [p for t in tasks for p in t.imported_object_paths]


def import_textures():
    for folder in sorted((ROOT / "Textures").iterdir()):
        dest = f"{DEST}/Textures/{folder.name}"
        if unreal.EditorAssetLibrary.does_directory_have_assets(dest):
            continue
        for path in import_files(sorted(folder.glob("*.jpg")), dest):
            tex = unreal.load_asset(path)
            if not isinstance(tex, unreal.Texture2D):
                continue
            name = tex.get_name().lower()
            if name.endswith("_nor"):
                tex.set_editor_property("srgb", False)
                tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
            elif not name.endswith("_diff"):
                tex.set_editor_property("srgb", False)
                tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_MASKS)
            unreal.EditorAssetLibrary.save_loaded_asset(tex)
        unreal.log(f"IMPORTED textures {folder.name}")


def import_models():
    for folder in sorted((ROOT / "Models").iterdir()):
        gltf = next(folder.glob("*.gltf"), None)
        if not gltf:
            continue
        dest = f"{DEST}/Models/{folder.name}"
        if unreal.EditorAssetLibrary.does_directory_have_assets(dest):
            continue  # already imported (re-runs resume after an out-of-memory stop)
        paths = import_files([gltf], dest)
        meshes = 0
        for path in paths:
            asset = unreal.load_asset(path)
            if isinstance(asset, unreal.StaticMesh):
                settings = asset.get_editor_property("nanite_settings")
                settings.enabled = True
                asset.set_editor_property("nanite_settings", settings)
                unreal.EditorAssetLibrary.save_loaded_asset(asset)
                meshes += 1
        unreal.log(f"IMPORTED model {folder.name}: {meshes} meshes, {len(paths)} assets")


import_textures()
import_models()
