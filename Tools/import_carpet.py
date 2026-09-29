"""Imports the ambientCG Carpet016 scan (CC0) used for the fibre detail of the house carpet.

Run once after Tools/fetch_carpet.py-style download (see below), before Tools/create_materials.py:
  UnrealEditor-Cmd.exe <repo>\Explorer.uproject -run=pythonscript -script=<repo>\Tools\import_carpet.py -unattended -nullrhi
Downloads Carpet016_2K-JPG.zip from ambientCG into SourceAssets/Carpet016 if it isn't there.
"""
import io, os, urllib.request, zipfile
import unreal

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceAssets", "Carpet016"))
DEST = "/Game/PolyHaven/Textures/carpet_016"
if not os.path.exists(os.path.join(ROOT, "Carpet016_2K-JPG_Color.jpg")):
    os.makedirs(ROOT, exist_ok=True)
    req = urllib.request.Request("https://ambientcg.com/get?file=Carpet016_2K-JPG.zip", headers={"User-Agent": "explorer-game/0.1"})
    zipfile.ZipFile(io.BytesIO(urllib.request.urlopen(req, timeout=300).read())).extractall(ROOT)

tasks = []
for kind, suffix in (("diff", "Color"), ("nor", "NormalGL"), ("rough", "Roughness")):
    t = unreal.AssetImportTask()
    t.set_editor_property("filename", os.path.join(ROOT, f"Carpet016_2K-JPG_{suffix}.jpg"))
    t.set_editor_property("destination_path", DEST)
    t.set_editor_property("destination_name", f"carpet_016_{kind}")
    t.set_editor_property("replace_existing", True)
    t.set_editor_property("automated", True)
    t.set_editor_property("save", True)
    tasks.append(t)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)
for kind in ("diff", "nor", "rough"):
    tex = unreal.load_asset(f"{DEST}/carpet_016_{kind}")
    if tex:
        if kind == "nor":
            tex.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP)
        if kind == "rough":
            tex.set_editor_property("srgb", False)
        unreal.EditorAssetLibrary.save_loaded_asset(tex)
        unreal.log(f"Imported {DEST}/carpet_016_{kind}")
