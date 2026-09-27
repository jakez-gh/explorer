"""Generates Explorer's materials. Run headless:

    UnrealEditor-Cmd.exe <repo>/Explorer.uproject -run=pythonscript -script="<repo>/Tools/create_materials.py" -unattended

Assets are written to /Game/Explorer/Materials and are safe to regenerate.
"""

import unreal

FOLDER = "/Game/Explorer/Materials"
lib = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def new_material(name):
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    return tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


def expr(mat, cls, x, y, **props):
    node = lib.create_material_expression(mat, cls, x, y)
    for key, value in props.items():
        node.set_editor_property(key, value)
    return node


def const(mat, value, x, y):
    return expr(mat, unreal.MaterialExpressionConstant, x, y, r=value)


def finish(mat):
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log(f"Created {mat.get_path_name()}")


# Terrain: colour comes from per-vertex biome colours; alpha carries wetness (0 = dry/rough, 1 = glossy).
m = new_material("M_Terrain")
vc = expr(m, unreal.MaterialExpressionVertexColor, -600, 0)
lib.connect_material_property(vc, "", unreal.MaterialProperty.MP_BASE_COLOR)
rough = expr(m, unreal.MaterialExpressionLinearInterpolate, -300, 200)
lib.connect_material_expressions(const(m, 0.9, -500, 200), "", rough, "A")
lib.connect_material_expressions(const(m, 0.35, -500, 280), "", rough, "B")
lib.connect_material_expressions(vc, "A", rough, "Alpha")
lib.connect_material_property(rough, "", unreal.MaterialProperty.MP_ROUGHNESS)
lib.connect_material_property(const(m, 0.25, -300, 360), "", unreal.MaterialProperty.MP_SPECULAR)
finish(m)

# Water: deep teal with a soft pale glow at grazing angles.
m = new_material("M_Water")
deep = expr(m, unreal.MaterialExpressionConstant3Vector, -700, -100, constant=unreal.LinearColor(0.01, 0.07, 0.11, 1))
shallow = expr(m, unreal.MaterialExpressionConstant3Vector, -700, 0, constant=unreal.LinearColor(0.05, 0.22, 0.28, 1))
fres = expr(m, unreal.MaterialExpressionFresnel, -700, 120, exponent=4.0)
color = expr(m, unreal.MaterialExpressionLinearInterpolate, -400, 0)
lib.connect_material_expressions(deep, "", color, "A")
lib.connect_material_expressions(shallow, "", color, "B")
lib.connect_material_expressions(fres, "", color, "Alpha")
lib.connect_material_property(color, "", unreal.MaterialProperty.MP_BASE_COLOR)
glow_tint = expr(m, unreal.MaterialExpressionConstant3Vector, -700, 260, constant=unreal.LinearColor(0.35, 0.3, 0.45, 1))
glow = expr(m, unreal.MaterialExpressionMultiply, -400, 220)
lib.connect_material_expressions(fres, "", glow, "A")
lib.connect_material_expressions(glow_tint, "", glow, "B")
lib.connect_material_property(glow, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(const(m, 0.06, -400, 360), "", unreal.MaterialProperty.MP_ROUGHNESS)
lib.connect_material_property(const(m, 0.6, -400, 440), "", unreal.MaterialProperty.MP_SPECULAR)
finish(m)

# Instanced props (trees, rocks, buildings, islands): per-instance custom data = R, G, B, glow.
m = new_material("M_InstanceColor")
m.set_editor_property("used_with_instanced_static_meshes", True)
channels = [expr(m, unreal.MaterialExpressionPerInstanceCustomData, -900, i * 90, data_index=i) for i in range(4)]
rg = expr(m, unreal.MaterialExpressionAppendVector, -650, 40)
lib.connect_material_expressions(channels[0], "", rg, "A")
lib.connect_material_expressions(channels[1], "", rg, "B")
rgb = expr(m, unreal.MaterialExpressionAppendVector, -450, 80)
lib.connect_material_expressions(rg, "", rgb, "A")
lib.connect_material_expressions(channels[2], "", rgb, "B")
lib.connect_material_property(rgb, "", unreal.MaterialProperty.MP_BASE_COLOR)
emissive = expr(m, unreal.MaterialExpressionMultiply, -250, 260)
lib.connect_material_expressions(rgb, "", emissive, "A")
lib.connect_material_expressions(channels[3], "", emissive, "B")
lib.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(const(m, 0.8, -250, 380), "", unreal.MaterialProperty.MP_ROUGHNESS)
finish(m)
