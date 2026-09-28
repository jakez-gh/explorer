"""Generates Explorer's materials from Starter Content textures. Run headless:

    UnrealEditor-Cmd.exe <repo>/Explorer.uproject -run=pythonscript -script="<repo>/Tools/create_materials.py" -unattended

Needs /Game/StarterContent (see CLAUDE.md). Assets go to /Game/Explorer/Materials and are safe to regenerate.
"""

import unreal

FOLDER = "/Game/Explorer/Materials"
SC = "/Game/StarterContent/Textures"
lib = unreal.MaterialEditingLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()
MP = unreal.MaterialProperty


def tex(name):
    return unreal.load_asset(f"{SC}/{name}.{name}")


def new_material(name):
    path = f"{FOLDER}/{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    return tools.create_asset(name, FOLDER, unreal.Material, unreal.MaterialFactoryNew())


class Graph:
    """Small helper for building material graphs left-to-right."""

    def __init__(self, material):
        self.m = material
        self.y = 0

    def node(self, cls, **props):
        self.y += 60
        n = lib.create_material_expression(self.m, cls, -1600 + (self.y // 1200) * 300, self.y % 1200)
        for key, value in props.items():
            n.set_editor_property(key, value)
        return n

    def link(self, src, dst, dst_input="", src_output=""):
        lib.connect_material_expressions(src, src_output, dst, dst_input)
        return dst

    def const(self, v):
        return self.node(unreal.MaterialExpressionConstant, r=v)

    def color(self, r, g, b):
        return self.node(unreal.MaterialExpressionConstant3Vector, constant=unreal.LinearColor(r, g, b, 1))

    def op(self, cls, a, b, a_out="", b_out=""):
        n = self.node(cls)
        self.link(a, n, "A", a_out)
        self.link(b, n, "B", b_out)
        return n

    def mul(self, a, b, a_out="", b_out=""):
        return self.op(unreal.MaterialExpressionMultiply, a, b, a_out, b_out)

    def add(self, a, b, a_out="", b_out=""):
        return self.op(unreal.MaterialExpressionAdd, a, b, a_out, b_out)

    def lerp(self, a, b, alpha, alpha_out=""):
        n = self.node(unreal.MaterialExpressionLinearInterpolate)
        self.link(a, n, "A")
        self.link(b, n, "B")
        self.link(alpha, n, "Alpha", alpha_out)
        return n

    def mask(self, src, r=False, g=False, b=False, a=False):
        n = self.node(unreal.MaterialExpressionComponentMask, r=r, g=g, b=b, a=a)
        return self.link(src, n)

    def saturate(self, src):
        return self.link(src, self.node(unreal.MaterialExpressionSaturate))

    def sample(self, texture, uv, normal=False):
        n = self.node(unreal.MaterialExpressionTextureSample, texture=texture)
        if normal:
            n.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
        return self.link(uv, n, "UVs")

    def world_uvs(self, scale_cm):
        """World-space UVs for the three projection planes, one tile per scale_cm."""
        wp = self.node(unreal.MaterialExpressionWorldPosition)
        inv = self.const(1.0 / scale_cm)
        return (self.mul(self.mask(wp, r=True, g=True), inv),
                self.mul(self.mask(wp, r=True, b=True), inv),
                self.mul(self.mask(wp, g=True, b=True), inv))

    def triplanar(self, texture, scale_cm, weights):
        """Blend three projections by squared world normal (weights sum to one for unit normals)."""
        xy, xz, yz = self.world_uvs(scale_cm)
        wx, wy, wz = weights
        s = self.mul(self.sample(texture, yz), wx)
        s = self.add(s, self.mul(self.sample(texture, xz), wy))
        return self.add(s, self.mul(self.sample(texture, xy), wz))

    def normal_weights(self):
        n = self.node(unreal.MaterialExpressionVertexNormalWS)
        comps = []
        for axis in ("r", "g", "b"):
            c = self.mask(n, **{axis: True})
            comps.append(self.mul(c, c))
        return comps


def distance_dissolve(g, start_cm, end_cm, mask=None, mask_out=""):
    """Opacity mask that dithers smoothly to nothing between start and end distance (no hard pop)."""
    depth = g.node(unreal.MaterialExpressionPixelDepth)
    fade = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, depth, g.const(start_cm)), g.const(1.0 / (end_cm - start_cm))))
    keep = g.op(unreal.MaterialExpressionSubtract, g.const(1.0), fade)
    if mask is not None:
        keep = g.mul(keep, mask, "", mask_out)
    dither = g.node(unreal.MaterialExpressionMaterialFunctionCall)
    dither.set_material_function(unreal.load_asset("/Engine/Functions/Engine_MaterialFunctions02/Utility/DitherTemporalAA.DitherTemporalAA"))
    return g.link(keep, dither, "Alpha Threshold")


# Vegetation (built within ~1.6 km) dissolves over this band so the edge is never seen popping.
VEG_FADE = (60000.0, 88000.0)


def finish(mat):
    lib.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log(f"Created {mat.get_path_name()}")


# ---------------------------------------------------------------------------------------------
# Terrain. Vertex colour carries layer weights: R sand, G rock, B snow, A forest floor.
# UV1 carries (dryness, wetness). Grass is the base layer.
m = new_material("M_Terrain")
g = Graph(m)
vc = g.node(unreal.MaterialExpressionVertexColor)
uv1 = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1)
dryness = g.mask(uv1, r=True)
wetness = g.mask(uv1, g=True)
weights = g.normal_weights()

near_xy, _, _ = g.world_uvs(350.0)
far_xy, _, _ = g.world_uvs(1900.0)
macro_xy, _, _ = g.world_uvs(9000.0)
macro = g.mask(g.sample(tex("T_MacroVariation"), macro_xy), r=True)
macro_scale = g.lerp(g.const(0.72), g.const(1.18), macro)

# Anti-tiling: a second sample with swapped axes (so its pattern runs a different way) at an unrelated scale,
# blended by large-scale noise, then faded to a flat, macro-varied colour with distance so no repeat shows.
wp = g.node(unreal.MaterialExpressionWorldPosition)
swapped = g.op(unreal.MaterialExpressionAppendVector, g.mask(wp, g=True), g.mask(wp, r=True))
swapped_xy = g.mul(swapped, g.const(1.0 / 530.0))
noise_xy, _, _ = g.world_uvs(4700.0)
blend = g.mask(g.sample(tex("T_Perlin_Noise_M"), noise_xy), r=True)
grass = g.lerp(g.sample(tex("T_Ground_Grass_D"), near_xy), g.sample(tex("T_Ground_Grass_D"), swapped_xy), blend)
grass = g.lerp(grass, g.sample(tex("T_Ground_Grass_D"), far_xy), g.const(0.3))
depth = g.node(unreal.MaterialExpressionPixelDepth)
fade = g.saturate(g.mul(depth, g.const(1.0 / 25000.0)))
grass = g.lerp(grass, g.color(0.1, 0.16, 0.045), g.mul(fade, g.const(0.85)))
dry_grass = g.mul(grass, g.color(1.45, 1.12, 0.5))
color = g.lerp(grass, dry_grass, dryness)

moss_xy, _, _ = g.world_uvs(420.0)
moss = g.mul(g.sample(tex("T_ground_Moss_D"), moss_xy), g.color(0.75, 0.85, 0.7))
moss = g.lerp(moss, g.color(0.05, 0.09, 0.03), g.mul(fade, g.const(0.8)))
color = g.lerp(color, moss, vc, "A")

sand_xy, _, _ = g.world_uvs(300.0)
gravel = g.sample(tex("T_Ground_Gravel_D"), sand_xy)
sand = g.lerp(g.color(0.78, 0.66, 0.47), g.mul(gravel, g.color(1.3, 1.1, 0.8)), g.const(0.3))
wet_sand = g.mul(sand, g.const(0.6))
sand = g.lerp(sand, wet_sand, wetness)
color = g.lerp(color, sand, vc, "R")

slate = g.triplanar(tex("T_Rock_Slate_D"), 900.0, weights)
sandstone = g.triplanar(tex("T_Rock_Sandstone_D"), 900.0, weights)
rock = g.lerp(slate, sandstone, dryness)
color = g.lerp(color, rock, vc, "G")

snow = g.mul(g.color(0.86, 0.89, 0.94), g.lerp(g.const(0.93), g.const(1.0), macro))
color = g.lerp(color, snow, vc, "B")
lib.connect_material_property(g.mul(color, macro_scale), "", MP.MP_BASE_COLOR)

normal = g.sample(tex("T_Ground_Grass_N"), near_xy, normal=True)
normal = g.lerp(normal, g.sample(tex("T_Ground_Moss_N"), moss_xy, normal=True), vc, "A")
normal = g.lerp(normal, g.sample(tex("T_Ground_Gravel_N"), sand_xy, normal=True), vc, "R")
rock_n_xy, _, _ = g.world_uvs(900.0)
normal = g.lerp(normal, g.sample(tex("T_Rock_Slate_N"), rock_n_xy, normal=True), vc, "G")
normal = g.lerp(normal, g.color(0, 0, 1), vc, "B")
lib.connect_material_property(normal, "", MP.MP_NORMAL)

rough = g.lerp(g.const(0.94), g.const(0.82), vc, "G")
rough = g.lerp(rough, g.const(0.55), vc, "B")
rough = g.lerp(rough, g.const(0.3), wetness)
lib.connect_material_property(rough, "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.const(0.35), "", MP.MP_SPECULAR)
finish(m)

# ---------------------------------------------------------------------------------------------
# Water: deep teal, two layers of panning ripples, glossy.
m = new_material("M_Water")
g = Graph(m)
big, _, _ = g.world_uvs(2600.0)
small, _, _ = g.world_uvs(700.0)
pan_a = g.link(big, g.node(unreal.MaterialExpressionPanner, speed_x=0.012, speed_y=0.008), "Coordinate")
pan_b = g.link(small, g.node(unreal.MaterialExpressionPanner, speed_x=-0.02, speed_y=0.025), "Coordinate")
water_n = g.add(g.sample(tex("T_Water_N"), pan_a, normal=True), g.sample(tex("T_Water_N"), pan_b, normal=True))
water_n = g.lerp(g.color(0, 0, 1), water_n, g.const(0.45))
lib.connect_material_property(water_n, "", MP.MP_NORMAL)
fres = g.node(unreal.MaterialExpressionFresnel, exponent=5.0)
water_c = g.lerp(g.color(0.006, 0.035, 0.045), g.color(0.03, 0.09, 0.1), fres)
lib.connect_material_property(water_c, "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.const(0.03), "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.const(0.9), "", MP.MP_SPECULAR)
finish(m)

# ---------------------------------------------------------------------------------------------
# Props (buildings, islands): world-projected surfaces. Per-instance custom data:
# [0..2] tint, [3] glow, [4] surface: 0 rock, 1 concrete, 2 brick, 3 slate roof, 4 glass, 5 grass.
m = new_material("M_Prop")
m.set_editor_property("used_with_instanced_static_meshes", True)
g = Graph(m)
cd = [g.node(unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(5)]
tint = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, cd[0], cd[1]), cd[2])
weights = g.normal_weights()
surface = cd[4]


def step(k):
    return g.saturate(g.op(unreal.MaterialExpressionSubtract, surface, g.const(k)))


surf = g.triplanar(tex("T_Rock_Basalt_D"), 700.0, weights)
surf = g.lerp(surf, g.triplanar(tex("T_Concrete_Poured_D"), 500.0, weights), step(0.0))
surf = g.lerp(surf, g.triplanar(tex("T_Brick_Clay_Old_D"), 250.0, weights), step(1.0))
surf = g.lerp(surf, g.triplanar(tex("T_Rock_Slate_D"), 300.0, weights), step(2.0))
surf = g.lerp(surf, g.color(0.05, 0.07, 0.09), step(3.0))
grass_xy, _, _ = g.world_uvs(400.0)
surf = g.lerp(surf, g.sample(tex("T_Ground_Grass_D"), grass_xy), step(4.0))
base = g.mul(surf, tint)
lib.connect_material_property(base, "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.mul(base, cd[3]), "", MP.MP_EMISSIVE_COLOR)
rough = g.lerp(g.const(0.85), g.const(0.72), step(0.0))
rough = g.lerp(rough, g.const(0.08), step(3.0))
rough = g.lerp(rough, g.const(0.92), step(4.0))
lib.connect_material_property(rough, "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.lerp(g.const(0.0), g.const(0.6), step(3.0)), "", MP.MP_METALLIC)
finish(m)

# ---------------------------------------------------------------------------------------------
# Rocks: scanned rock mesh normals with world-projected basalt. Custom data [0..2] tint.
m = new_material("M_RockMesh")
m.set_editor_property("used_with_instanced_static_meshes", True)
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
g = Graph(m)
lib.connect_material_property(distance_dissolve(g, *VEG_FADE), "", MP.MP_OPACITY_MASK)
cd = [g.node(unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(3)]
tint = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, cd[0], cd[1]), cd[2])
mesh_uv = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
rock = g.triplanar(tex("T_Rock_Basalt_D"), 600.0, g.normal_weights())
lib.connect_material_property(g.mul(rock, tint), "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.sample(tex("T_RockMesh_N"), mesh_uv, normal=True), "", MP.MP_NORMAL)
lib.connect_material_property(g.const(0.88), "", MP.MP_ROUGHNESS)
finish(m)

# ---------------------------------------------------------------------------------------------
# Foliage: scanned bush leaves, two-sided with light passing through. Custom data [0..2] tint.
m = new_material("M_Foliage")
m.set_editor_property("used_with_instanced_static_meshes", True)
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
m.set_editor_property("two_sided", True)
g = Graph(m)
cd = [g.node(unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(3)]
tint = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, cd[0], cd[1]), cd[2])
mesh_uv = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
leaves = g.sample(tex("T_Bush_D"), mesh_uv)
color = g.mul(leaves, tint)
lib.connect_material_property(color, "", MP.MP_BASE_COLOR)
lib.connect_material_property(distance_dissolve(g, *VEG_FADE, leaves, "A"), "", MP.MP_OPACITY_MASK)
lib.connect_material_property(g.sample(tex("T_Bush_N"), mesh_uv, normal=True), "", MP.MP_NORMAL)
lib.connect_material_property(g.mul(color, g.color(0.6, 0.8, 0.3)), "", MP.MP_SUBSURFACE_COLOR)
lib.connect_material_property(g.const(0.75), "", MP.MP_ROUGHNESS)
finish(m)

# ---------------------------------------------------------------------------------------------
# Bark for runtime-built tree meshes: UV U runs around the trunk, V along it (1 per 1.5 m).
# Noise stretched along the trunk gives vertical furrows. Custom data [0..2] tint.
m = new_material("M_Bark")
m.set_editor_property("used_with_instanced_static_meshes", True)
m.set_editor_property("two_sided", True)
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
g = Graph(m)
lib.connect_material_property(distance_dissolve(g, *VEG_FADE), "", MP.MP_OPACITY_MASK)
cd = [g.node(unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(3)]
tint = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, cd[0], cd[1]), cd[2])
uv = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
furrow_uv = g.mul(uv, g.op(unreal.MaterialExpressionAppendVector, g.const(3.0), g.const(0.35)))
ridge_uv = g.mul(uv, g.op(unreal.MaterialExpressionAppendVector, g.const(7.0), g.const(0.9)))
furrow = g.mask(g.sample(tex("T_Perlin_Noise_M"), furrow_uv), r=True)
ridge = g.mask(g.sample(tex("T_Perlin_Noise_M"), ridge_uv), r=True)
relief = g.saturate(g.mul(g.mul(furrow, ridge), g.const(2.6)))
bark = g.lerp(g.color(0.03, 0.024, 0.018), g.color(0.17, 0.14, 0.11), relief)
lib.connect_material_property(g.mul(bark, tint), "", MP.MP_BASE_COLOR)
normal_uv = g.mul(uv, g.op(unreal.MaterialExpressionAppendVector, g.const(2.0), g.const(0.5)))
lib.connect_material_property(g.sample(tex("T_Detail_Rocky_N"), normal_uv, normal=True), "", MP.MP_NORMAL)
lib.connect_material_property(g.const(0.92), "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.const(0.2), "", MP.MP_SPECULAR)
finish(m)

# ---------------------------------------------------------------------------------------------
# Grass blades (runtime-built clump mesh): vertex colour R is height along the blade (0 root, 1 tip).
# Custom data [0..2] tint. Blades sway with a wind wave that travels across the field.
m = new_material("M_Grass")
m.set_editor_property("used_with_instanced_static_meshes", True)
m.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
m.set_editor_property("two_sided", True)
g = Graph(m)
cd = [g.node(unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(3)]
tint = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, cd[0], cd[1]), cd[2])
vc = g.node(unreal.MaterialExpressionVertexColor)
height = g.mask(vc, r=True)
blade = g.mul(g.lerp(g.color(0.02, 0.045, 0.01), g.color(0.13, 0.21, 0.045), height), tint)
# Blend towards a flat ground-like tone with distance so the edge of the grass field isn't noticed
# (grass stays opaque: masked grass is far too expensive at this density).
far = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, g.node(unreal.MaterialExpressionPixelDepth), g.const(6000.0)), g.const(1.0 / 5000.0)))
blade = g.lerp(blade, g.mul(g.color(0.07, 0.11, 0.03), tint), far)
lib.connect_material_property(blade, "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.mul(blade, g.color(0.9, 1.2, 0.5)), "", MP.MP_SUBSURFACE_COLOR)
lib.connect_material_property(g.const(0.6), "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.const(0.3), "", MP.MP_SPECULAR)
wp = g.node(unreal.MaterialExpressionWorldPosition)
phase = g.mul(g.add(g.mask(wp, r=True), g.mul(g.mask(wp, g=True), g.const(0.7))), g.const(0.004))
wave = g.link(g.add(g.mul(g.node(unreal.MaterialExpressionTime), g.const(1.8)), phase), g.node(unreal.MaterialExpressionSine))
sway = g.mul(g.mul(wave, g.const(9.0)), g.mul(height, height))
offset = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, sway, g.mul(sway, g.const(0.6))), g.const(0.0))
# World position offset isn't in the Python MaterialProperty enum; set the material input directly.
try:
    wpo = m.get_editor_property("world_position_offset")
    wpo.set_editor_property("expression", offset)
    m.set_editor_property("world_position_offset", wpo)
    unreal.log("M_Grass: wind sway connected")
except Exception as ex:
    unreal.log_warning(f"M_Grass: wind sway not connected ({ex}); grass will be still")
finish(m)

for old in ("M_InstanceColor",):
    path = f"{FOLDER}/{old}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
