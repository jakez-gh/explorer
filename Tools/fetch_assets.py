"""Downloads Explorer's free CC0 source assets (Poly Haven + ambientCG) into SourceAssets/.

    python Tools/fetch_assets.py

Everything here is CC0 (public domain). SourceAssets/ is not committed; run this after cloning,
then import with Tools/import_assets.py (see CLAUDE.md).
"""

import json
import pathlib
import urllib.request
import zipfile
import io

ROOT = pathlib.Path(__file__).resolve().parent.parent / "SourceAssets"
UA = {"User-Agent": "ExplorerGame/1.0"}
RES = "2k"

# Ground surfaces for the terrain material.
TEXTURES = [
    "forest_leaves_02",     # forest floor litter
    "forrest_ground_01",    # forest soil
    "rocky_terrain_02",     # rocky ground / slopes
    "rock_face",            # cliffs
    "snow_02",
    "coast_sand_01",        # beaches
    "dry_ground_01",        # dry grassland / savanna soil
    "gravel_road",          # roads
    # house interiors and exteriors
    "plank_flooring", "laminate_floor_02", "floor_tiles_06", "white_plaster_02", "beige_wall_001",
    "painted_brick", "brick_wall_001", "white_stucco", "medieval_wall_01", "clay_roof_tiles", "roof_slates_02", "brown_planks_03",
]

# Lush lawn grass photos (ambientCG).
AMBIENTCG = ["Grass004", "Grass001"]

MODELS = [
    # conifers and other trees
    "fir_tree_01", "pine_tree_01", "fir_sapling_medium", "pine_sapling_medium",
    "jacaranda_tree", "island_tree_01", "island_tree_02", "quiver_tree_01",
    "tree_stump_01", "dead_tree_trunk_02",
    # rocks
    "boulder_01", "rock_07", "rock_09", "rock_moss_set_01", "rock_moss_set_02",
    "namaqualand_boulder_02", "namaqualand_boulder_04", "namaqualand_cliff_01",
    # ground cover
    "shrub_01", "shrub_02", "shrub_03", "shrub_04", "grass_medium_01", "grass_medium_02",
    "dandelion_01", "nettle_plant", "moss_01",
    # furniture, by room
    "Sofa_01", "sofa_02", "ArmChair_01", "CoffeeTable_01", "modern_coffee_table_01", "wooden_bookshelf_worn",
    "dining_table", "dining_chair_02", "WoodenChair_01", "WoodenTable_02", "electric_stove", "painted_wooden_cabinet",
    "vintage_cabinet_01", "old_bed_frame", "GothicBed_01", "painted_wooden_nightstand", "ClassicNightstand_01",
    "vintage_wooden_drawer_01", "ornate_mirror_01", "desk_lamp_arm_01", "modern_ceiling_lamp_01", "Chandelier_01",
    "wall_clock", "scandinavian_masonry_heater", "Shelf_01",
    # modern buildings
    "modular_urban_apartments_facade", "modular_factory_facade",
]


def get(url):
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def fetch_texture(asset):
    files = json.loads(get(f"https://api.polyhaven.com/files/{asset}"))
    for kind, key in (("Diffuse", "diff"), ("nor_dx", "nor"), ("arm", "arm")):
        url = files[kind][RES]["jpg"]["url"]
        out = ROOT / "Textures" / asset / f"{asset}_{key}.jpg"
        if not out.exists():
            save(out, get(url))
    print("texture", asset)


def fetch_ambientcg(asset):
    out = ROOT / "Textures" / asset
    if out.exists():
        return
    data = get(f"https://ambientcg.com/get?file={asset}_2K-JPG.zip")
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        for name in z.namelist():
            low = name.lower()
            for suffix, key in (("_color.jpg", "diff"), ("_normaldx.jpg", "nor"), ("_roughness.jpg", "rough"), ("_ambientocclusion.jpg", "ao")):
                if low.endswith(suffix):
                    save(out / f"{asset}_{key}.jpg", z.read(name))
    print("texture", asset)


def fetch_model(asset):
    files = json.loads(get(f"https://api.polyhaven.com/files/{asset}"))
    entry = files["gltf"][RES]["gltf"]
    base = ROOT / "Models" / asset
    main = base / pathlib.Path(entry["url"]).name
    if not main.exists():
        save(main, get(entry["url"]))
        for rel, inc in entry.get("include", {}).items():
            save(base / rel, get(inc["url"]))
    print("model", asset)


if __name__ == "__main__":
    for a in TEXTURES:
        fetch_texture(a)
    for a in AMBIENTCG:
        fetch_ambientcg(a)
    for a in MODELS:
        fetch_model(a)
