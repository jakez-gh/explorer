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
# Terrain, from photoscanned CC0 surfaces (Poly Haven / ambientCG, see Tools/fetch_assets.py).
# Vertex colour carries layer weights: R sand, G rock, B snow, A forest floor. UV1 = (dryness, wetness).
# Near: full-resolution scans with anti-tiling. Far: each layer fades to its average colour, and forest
# floor far away takes the colour of the canopy above it, so distant woods read as woods.
def ph(asset, kind):
    return unreal.load_asset(f"/Game/PolyHaven/Textures/{asset}/{asset}_{kind}")


m = new_material("M_Terrain")
g = Graph(m)
vc = g.node(unreal.MaterialExpressionVertexColor)
uv1 = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=1)
dryness = g.mask(uv1, r=True)
wetness = g.mask(uv1, g=True)
weights = g.normal_weights()
depth = g.node(unreal.MaterialExpressionPixelDepth)
near_fade = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, depth, g.const(4000.0)), g.const(1.0 / 30000.0)))
far_fade = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, depth, g.const(150000.0)), g.const(1.0 / 250000.0)))

macro_xy, _, _ = g.world_uvs(9000.0)
macro_xy2, _, _ = g.world_uvs(31000.0)
macro = g.mul(g.mask(g.sample(tex("T_MacroVariation"), macro_xy), r=True), g.mask(g.sample(tex("T_MacroVariation"), macro_xy2), g=True))
macro_scale = g.lerp(g.const(0.7), g.const(1.25), g.saturate(g.mul(macro, g.const(1.6))))
noise_xy, _, _ = g.world_uvs(4700.0)
blend = g.mask(g.sample(tex("T_Perlin_Noise_M"), noise_xy), r=True)
patch_xy, _, _ = g.world_uvs(1300.0)
patches = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, g.mask(g.sample(tex("T_Perlin_Noise_M"), patch_xy), r=True), g.const(0.45)), g.const(2.5)))
wp = g.node(unreal.MaterialExpressionWorldPosition)


def scan(asset, kind, scale_cm, alt_scale_cm=None, normal=False):
    """Sample a scan in world XY; blend in a second, axis-swapped sample to break up tiling."""
    xy, _, _ = g.world_uvs(scale_cm)
    a = g.sample(ph(asset, kind), xy, normal=normal)
    if not alt_scale_cm:
        return a
    swapped = g.op(unreal.MaterialExpressionAppendVector, g.mask(wp, g=True), g.mask(wp, r=True))
    b = g.sample(ph(asset, kind), g.mul(swapped, g.const(1.0 / alt_scale_cm)), normal=normal)
    return g.lerp(a, b, blend)


def far(near, far_color):
    return g.lerp(near, g.color(*far_color), near_fade)


# Meadow: lawn scan with a coarser second grass scan, soil patches, drying to straw with dryness.
grass = scan("Grass004", "diff", 220.0, 530.0)
grass = g.lerp(grass, scan("Grass001", "diff", 700.0), g.const(0.35))
grass = g.lerp(grass, scan("forrest_ground_01", "diff", 400.0), g.mul(patches, g.const(0.35)))
dry = scan("dry_ground_01", "diff", 400.0, 900.0)
grass = far(grass, (0.07, 0.1, 0.035))
dry = far(dry, (0.2, 0.16, 0.1))
color = g.lerp(grass, g.lerp(grass, dry, g.const(0.8)), g.saturate(g.mul(dryness, g.const(1.2))))

# Forest floor: leaf litter near; far away, the colour of the canopy that covers it.
litter = far(scan("forest_leaves_02", "diff", 350.0, 810.0), (0.07, 0.055, 0.035))
litter = g.lerp(litter, g.color(0.03, 0.05, 0.02), far_fade)
color = g.lerp(color, litter, vc, "A")

sand = far(scan("coast_sand_01", "diff", 400.0, 950.0), (0.5, 0.43, 0.32))
sand = g.lerp(sand, g.mul(sand, g.const(0.6)), wetness)
color = g.lerp(color, sand, vc, "R")

cliff = g.triplanar(ph("rock_face", "diff"), 1200.0, weights)
scree = g.triplanar(ph("rocky_terrain_02", "diff"), 600.0, weights)
rock = far(g.lerp(cliff, scree, g.lerp(g.const(0.35), g.const(0.75), dryness)), (0.22, 0.2, 0.18))
color = g.lerp(color, rock, vc, "G")

snow = far(scan("snow_02", "diff", 500.0, 1100.0), (0.8, 0.83, 0.88))
color = g.lerp(color, snow, vc, "B")
lib.connect_material_property(g.mul(color, macro_scale), "", MP.MP_BASE_COLOR)

normal = scan("Grass004", "nor", 220.0, normal=True)
normal = g.lerp(normal, scan("forest_leaves_02", "nor", 350.0, normal=True), vc, "A")
normal = g.lerp(normal, scan("coast_sand_01", "nor", 400.0, normal=True), vc, "R")
rock_xy, _, _ = g.world_uvs(1200.0)
normal = g.lerp(normal, g.sample(ph("rock_face", "nor"), rock_xy, normal=True), vc, "G")
normal = g.lerp(normal, scan("snow_02", "nor", 500.0, normal=True), vc, "B")
normal = g.lerp(normal, g.color(0, 0, 1), g.mul(near_fade, g.const(0.8)))
lib.connect_material_property(normal, "", MP.MP_NORMAL)

rough = g.lerp(g.const(0.95), g.const(0.85), vc, "G")
rough = g.lerp(rough, g.const(0.6), vc, "B")
rough = g.lerp(rough, g.const(0.3), wetness)
lib.connect_material_property(rough, "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.const(0.3), "", MP.MP_SPECULAR)
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
def make_foliage(name, fade):
    """Foliage; fade=None never dissolves with distance (used on floating islands)."""
    m = new_material(name)
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
    if fade:
        lib.connect_material_property(distance_dissolve(g, *fade, leaves, "A"), "", MP.MP_OPACITY_MASK)
    else:
        lib.connect_material_property(leaves, "A", MP.MP_OPACITY_MASK)
    lib.connect_material_property(g.sample(tex("T_Bush_N"), mesh_uv, normal=True), "", MP.MP_NORMAL)
    lib.connect_material_property(g.mul(color, g.color(0.6, 0.8, 0.3)), "", MP.MP_SUBSURFACE_COLOR)
    lib.connect_material_property(g.const(0.75), "", MP.MP_ROUGHNESS)
    finish(m)


make_foliage("M_Foliage", VEG_FADE)
make_foliage("M_FoliageFar", None)

# ---------------------------------------------------------------------------------------------
# Roads and trails (runtime path strips). Vertex colour R: 1 = gravel road, 0 = dirt trail.
# UV0.x runs across the path (0..1): edges are ragged and blend out, with a worn centre.
m = new_material("M_Path")
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
m.set_editor_property("two_sided", True)
g = Graph(m)
vc = g.node(unreal.MaterialExpressionVertexColor)
uv = g.node(unreal.MaterialExpressionTextureCoordinate, coordinate_index=0)
across = g.mask(uv, r=True)
edge = g.op(unreal.MaterialExpressionSubtract, g.const(1.0), g.link(g.op(unreal.MaterialExpressionSubtract, g.mul(across, g.const(2.0)), g.const(1.0)), g.node(unreal.MaterialExpressionAbs)))
path_xy, _, _ = g.world_uvs(260.0)
rough_edge_xy, _, _ = g.world_uvs(140.0)
ragged = g.mask(g.sample(tex("T_Perlin_Noise_M"), rough_edge_xy), r=True)
keep = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, g.mul(edge, g.const(3.0)), g.mul(ragged, g.const(1.2))), g.const(3.0)))
gravel = g.sample(tex("T_Ground_Gravel_D"), path_xy)
road = g.mul(gravel, g.color(0.17, 0.17, 0.18))  # asphalt
trail = g.mul(gravel, g.color(0.33, 0.24, 0.16))
color = g.lerp(trail, road, vc, "R")
# Wheel ruts / foot-worn centre slightly darker and smoother.
worn = g.saturate(g.mul(edge, g.const(1.6)))
color = g.lerp(color, g.mul(color, g.const(0.82)), g.mul(worn, g.const(0.5)))
lib.connect_material_property(color, "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.sample(tex("T_Ground_Gravel_N"), path_xy, normal=True), "", MP.MP_NORMAL)
lib.connect_material_property(g.const(0.9), "", MP.MP_ROUGHNESS)
lib.connect_material_property(distance_dissolve(g, 95000.0, 130000.0, keep), "", MP.MP_OPACITY_MASK)
finish(m)

# ---------------------------------------------------------------------------------------------
# Bark for runtime-built tree meshes: UV U runs around the trunk, V along it (1 per 1.5 m).
# Noise stretched along the trunk gives vertical furrows. Custom data [0..2] tint.
def make_bark(name, fade):
    m = new_material(name)
    m.set_editor_property("used_with_instanced_static_meshes", True)
    m.set_editor_property("two_sided", True)
    g = Graph(m)
    if fade:
        m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_MASKED)
        lib.connect_material_property(distance_dissolve(g, *fade), "", MP.MP_OPACITY_MASK)
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


make_bark("M_Bark", VEG_FADE)
make_bark("M_BarkFar", None)

# ---------------------------------------------------------------------------------------------
# Floating islands (runtime meshes, world-projected textures). Vertex colour: R earth (0 grassy top,
# 1 hanging soil underneath), G exposed rock, B depth down the underside (0 rim .. 1 tip).
m = new_material("M_Island")
m.set_editor_property("used_with_instanced_static_meshes", True)
m.set_editor_property("two_sided", True)
g = Graph(m)
vc = g.node(unreal.MaterialExpressionVertexColor)
weights = g.normal_weights()
top_xy, _, _ = g.world_uvs(450.0)
grass = g.mul(g.sample(tex("T_Ground_Grass_D"), top_xy), g.color(0.78, 0.88, 0.66))
soil = g.mul(g.mask(g.triplanar(tex("T_Ground_Gravel_D"), 350.0, weights), g=True), g.color(0.42, 0.31, 0.2))
# Deeper soil is darker and damper.
soil = g.lerp(soil, g.mul(soil, g.const(0.75)), vc, "B")
rock = g.mul(g.triplanar(tex("T_Rock_Slate_D"), 900.0, weights), g.color(0.9, 0.85, 0.8))
earth = g.lerp(soil, rock, vc, "G")
color = g.lerp(grass, earth, vc, "R")
lib.connect_material_property(color, "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.lerp(g.const(0.95), g.const(0.85), vc, "G"), "", MP.MP_ROUGHNESS)
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
blade = g.mul(g.lerp(g.color(0.025, 0.04, 0.015), g.color(0.15, 0.19, 0.075), height), tint)
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

# ---------------------------------------------------------------------------------------------
# Buildings: one parent that world-projects a scanned surface (Diffuse / Normal / ARM parameters) on
# instanced boxes, and an instance per surface type. Custom data [0..2] tints (e.g. plaster colour).
m = new_material("M_BuildingSurface")
m.set_editor_property("used_with_instanced_static_meshes", True)
g = Graph(m)
cd = [g.node(unreal.MaterialExpressionPerInstanceCustomData, data_index=i) for i in range(3)]
tint = g.op(unreal.MaterialExpressionAppendVector, g.op(unreal.MaterialExpressionAppendVector, cd[0], cd[1]), cd[2])
scale = g.node(unreal.MaterialExpressionScalarParameter, parameter_name="ScaleCm", default_value=200.0)
wp = g.node(unreal.MaterialExpressionWorldPosition)
inv = g.op(unreal.MaterialExpressionDivide, g.const(1.0), scale)
planes = [g.mul(g.mask(wp, r=True, g=True), inv), g.mul(g.mask(wp, r=True, b=True), inv), g.mul(g.mask(wp, g=True, b=True), inv)]
wz, wy, wx = None, None, None
nw = g.normal_weights()


def param_triplanar(name, normal=False):
    out = None
    for uv, w in zip(planes, (nw[2], nw[1], nw[0])):
        n = g.node(unreal.MaterialExpressionTextureSampleParameter2D, parameter_name=name)
        if normal:
            n.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
            n.set_editor_property("texture", tex("T_Ground_Grass_N"))
        else:
            n.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if name == "Diffuse" else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
            n.set_editor_property("texture", tex("T_Ground_Grass_D"))
        n.set_editor_property("sampler_source", unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
        g.link(uv, n, "UVs")
        term = g.mul(n, w)
        out = term if out is None else g.add(out, term)
    return out


lib.connect_material_property(g.mul(param_triplanar("Diffuse"), tint), "", MP.MP_BASE_COLOR)
# NormalStrength < 1 flattens the relief, so painted walls read as smooth solid colour instead of spiky plaster.
strength = g.node(unreal.MaterialExpressionScalarParameter, parameter_name="NormalStrength", default_value=1.0)
lib.connect_material_property(g.lerp(g.color(0.0, 0.0, 1.0), param_triplanar("Normal", True), strength), "", MP.MP_NORMAL)
arm = param_triplanar("ARM")
lib.connect_material_property(arm, "G", MP.MP_ROUGHNESS)
lib.connect_material_property(arm, "R", MP.MP_AMBIENT_OCCLUSION)
finish(m)
parent = m

SURFACES = {
    "ExteriorWall": ("white_stucco", 250.0), "BrickWall": ("brick_wall_001", 180.0), "TimberWall": ("brown_planks_03", 200.0),
    "InteriorWall": ("white_plaster_02", 250.0), "PlankFloor": ("plank_flooring", 220.0), "TileFloor": ("floor_tiles_06", 150.0),
    "Stone": ("medieval_wall_01", 250.0), "ClayRoof": ("clay_roof_tiles", 250.0), "SlateRoof": ("roof_slates_02", 250.0),
    "Wood": ("laminate_floor_02", 150.0),
}
for name, (asset, scale_cm) in SURFACES.items():
    path = f"{FOLDER}/Building/MI_{name}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
    mi = tools.create_asset(f"MI_{name}", f"{FOLDER}/Building", unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    lib.set_material_instance_parent(mi, parent)
    for p_name, kind in (("Diffuse", "diff"), ("Normal", "nor"), ("ARM", "arm")):
        t = unreal.load_asset(f"/Game/PolyHaven/Textures/{asset}/{asset}_{kind}")
        if t:
            lib.set_material_instance_texture_parameter_value(mi, p_name, t)
    lib.set_material_instance_scalar_parameter_value(mi, "ScaleCm", scale_cm)
    if name == "InteriorWall":
        lib.set_material_instance_scalar_parameter_value(mi, "NormalStrength", 0.1)
    unreal.EditorAssetLibrary.save_loaded_asset(mi)
    unreal.log(f"Created {path}")

# Carpet: brown, dotted with tiny roundish spots of dark brown and tan (1-3 inch patches, irregular), from world-space noise.
m = new_material("M_Carpet")
m.set_editor_property("used_with_instanced_static_meshes", True)
g = Graph(m)
xy_a, _, _ = g.world_uvs(21.0)
xy_b, _, _ = g.world_uvs(13.0)
xy_c, _, _ = g.world_uvs(9.0)
na = g.mask(g.sample(tex("T_Perlin_Noise_M"), xy_a), r=True)
nb = g.mask(g.sample(tex("T_Perlin_Noise_M"), xy_b), r=True)
nc = g.mask(g.sample(tex("T_Perlin_Noise_M"), xy_c), r=True)
dark = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, g.mul(na, nc), g.const(0.30)), g.const(14.0)))
pale = g.saturate(g.mul(g.op(unreal.MaterialExpressionSubtract, g.mul(nb, g.op(unreal.MaterialExpressionSubtract, g.const(1.0), nc)), g.const(0.26)), g.const(14.0)))
carpet = g.lerp(g.lerp(g.color(0.13, 0.075, 0.04), g.color(0.030, 0.014, 0.006), dark), g.color(0.36, 0.24, 0.13), pale)
lib.connect_material_property(carpet, "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.const(0.97), "", MP.MP_ROUGHNESS)
finish(m)
carpet_parent = m
path = f"{FOLDER}/Building/MI_Carpet"
if unreal.EditorAssetLibrary.does_asset_exist(path):
    unreal.EditorAssetLibrary.delete_asset(path)
mi = tools.create_asset("MI_Carpet", f"{FOLDER}/Building", unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
lib.set_material_instance_parent(mi, carpet_parent)
unreal.EditorAssetLibrary.save_loaded_asset(mi)

# Window glass: see-through, slightly reflective.
m = new_material("M_Glass")
m.set_editor_property("used_with_instanced_static_meshes", True)
m.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
m.set_editor_property("two_sided", True)
g = Graph(m)
fres = g.node(unreal.MaterialExpressionFresnel, exponent=4.0)
lib.connect_material_property(g.color(0.02, 0.03, 0.035), "", MP.MP_BASE_COLOR)
lib.connect_material_property(g.lerp(g.const(0.12), g.const(0.6), fres), "", MP.MP_OPACITY)
lib.connect_material_property(g.const(0.05), "", MP.MP_ROUGHNESS)
lib.connect_material_property(g.const(0.8), "", MP.MP_SPECULAR)
finish(m)

for old in ("M_InstanceColor",):
    path = f"{FOLDER}/{old}"
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        unreal.EditorAssetLibrary.delete_asset(path)
