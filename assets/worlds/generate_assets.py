# Generates the sample world assets: green_field.glb (the scene) and robot.glb (the avatar).
#
# Run from the repository root with Blender 5.x:
#   blender -b --factory-startup --python assets/worlds/generate_assets.py
#
# Everything is built from primitives and seeded random numbers, so a run gives the same
# models every time. The output is written next to this script.
#
# The assets keep to what the engine's glTF importer supports: metallic-roughness materials
# with scalar metallic and roughness, base-color textures only, no vertex colors, no morph
# targets, no KHR_texture_transform, at most four joints per vertex. The one exception is the
# meadow's sun and moon, which use KHR_materials_unlit (the engine draws them without lighting).

import math
import os
import random
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


# --------------------------------------------------------------------------- helpers


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 24
    return scene


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def material(name, srgb, roughness=0.8, metallic=0.0, image=None):
    """A Principled BSDF material. `srgb` is a display color; glTF stores linear factors."""
    mat = bpy.data.materials.new(name)
    nodes = mat.node_tree.nodes
    bsdf = nodes.get("Principled BSDF")
    lin = [srgb_to_linear(c) for c in srgb]
    bsdf.inputs["Base Color"].default_value = (*lin, 1.0)
    bsdf.inputs["Roughness"].default_value = roughness
    bsdf.inputs["Metallic"].default_value = metallic
    mat.use_backface_culling = True  # every mesh is closed or seen from one side: single-sided
    if image is not None:
        tex = nodes.new("ShaderNodeTexImage")
        tex.image = image
        tex.interpolation = "Linear"
        tex.extension = "REPEAT"
        mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        bsdf.inputs["Base Color"].default_value = (1.0, 1.0, 1.0, 1.0)
    return mat


def unlit_material(name, srgb):
    """A flat, unlit color: a Background shader, which Blender's exporter writes as KHR_materials_unlit."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    bg = nodes.new("ShaderNodeBackground")
    bg.inputs["Color"].default_value = (*[srgb_to_linear(c) for c in srgb], 1.0)
    out = nodes.new("ShaderNodeOutputMaterial")
    mat.node_tree.links.new(bg.outputs["Background"], out.inputs["Surface"])
    return mat


def sky_disc(name, srgb, radius):
    """A sphere that looks like a flat disc from any side. The daylight script places it on the sky."""
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=24, v_segments=12, radius=radius)
    return new_object(name, bm, [unlit_material(name, srgb)])


def image_from_array(name, rgb):
    """A packed PNG image from an (h, w, 3) array of sRGB values in 0..1 (row 0 is the top)."""
    h, w, _ = rgb.shape
    img = bpy.data.images.new(name, width=w, height=h, alpha=False)
    img.colorspace_settings.name = "sRGB"
    rgba = np.ones((h, w, 4), dtype=np.float32)
    rgba[:, :, :3] = np.clip(rgb, 0.0, 1.0)
    # Blender's pixel rows run bottom to top.
    img.pixels.foreach_set(rgba[::-1].ravel())
    img.file_format = "PNG"
    img.pack()
    return img


def canonical(bm):
    """A copy of `bm` in an order that depends only on the geometry.

    Some bmesh operators (bevel) order the elements they create by memory address, so the same
    script would otherwise write a different vertex order on each run. Vertices are sorted by
    part and position, faces by material, part and vertices, and each face starts at its
    lowest vertex (keeping its winding). UVs and the per-face "bone" layer are carried over.
    """
    uv_src = bm.loops.layers.uv.verify()
    v_part = bm.verts.layers.int.get("part")
    f_bone = bm.faces.layers.int.get("bone")

    def vkey(v):
        return (v[v_part] if v_part else 0, round(v.co.x, 6), round(v.co.y, 6), round(v.co.z, 6))

    out = bmesh.new()
    uv = out.loops.layers.uv.new("UVMap")
    out_bone = out.faces.layers.int.new("bone") if f_bone else None
    bm.verts.index_update()
    order = sorted(bm.verts, key=vkey)
    new_index = [0] * len(bm.verts)
    for i, v in enumerate(order):
        out.verts.new(v.co)
        new_index[v.index] = i
    out.verts.ensure_lookup_table()
    faces = []
    for f in bm.faces:
        loops = list(f.loops)
        idx = [new_index[l.vert.index] for l in loops]
        r = idx.index(min(idx))
        loops = loops[r:] + loops[:r]
        faces.append((
            f.material_index,
            f[f_bone] if f_bone else 0,
            [new_index[l.vert.index] for l in loops],
            [tuple(l[uv_src].uv) for l in loops],
        ))
    faces.sort(key=lambda t: (t[0], t[1], t[2]))
    for mat, bone, vidx, uvs in faces:
        nf = out.faces.new([out.verts[i] for i in vidx])
        nf.material_index = mat
        if out_bone:
            nf[out_bone] = bone
        for loop, u in zip(nf.loops, uvs):
            loop[uv].uv = u
    return out


def new_object(name, bm, materials, smooth=False):
    """A mesh object from `bm`, in canonical order, flat shaded (or smooth), with UVs on every mesh."""
    bm.loops.layers.uv.verify()  # every mesh gets UVs (zeros where unused), so none is dropped
    out = canonical(bm)
    bm.free()
    mesh = bpy.data.meshes.new(name)
    out.to_mesh(mesh)
    out.free()
    for m in materials:
        mesh.materials.append(m)
    for poly in mesh.polygons:
        poly.use_smooth = smooth
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(obj)
    return obj


# Parts are added to one bmesh one at a time. Geometry whose "done" layer is still 0 is the part
# being built; `_finish` gives it its material and sets the layer. (A layer, not the bmesh tag
# flag: operators such as bevel use and clear the tags of the whole mesh.)


def _done_layers(bm):
    v = bm.verts.layers.int.get("done") or bm.verts.layers.int.new("done")
    f = bm.faces.layers.int.get("done") or bm.faces.layers.int.new("done")
    return v, f


def _new_verts(bm):
    v_done, _ = _done_layers(bm)
    return [v for v in bm.verts if not v[v_done]]


def _new_faces(bm):
    _, f_done = _done_layers(bm)
    return [f for f in bm.faces if not f[f_done]]


def _finish(bm, mat_index=None):
    v_done, f_done = _done_layers(bm)
    bm.normal_update()
    for f in bm.faces:
        if not f[f_done]:
            if mat_index is not None:
                f.material_index = mat_index
            f[f_done] = 1
    for v in bm.verts:
        v[v_done] = 1


def add_box(bm, size, center, mat_index=0, bevel=0.0, segments=2):
    """An axis-aligned box, optionally with bevelled edges."""
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=_new_verts(bm))
    if bevel > 0.0:
        bm.edges.index_update()
        # Sorted: a set of BMEdge objects iterates in memory-address order, which would make
        # the bevel's vertex order (and the exported file) differ from run to run.
        edges = sorted({e for v in _new_verts(bm) for e in v.link_edges}, key=lambda e: e.index)
        bmesh.ops.bevel(bm, geom=edges, offset=bevel, segments=segments, affect="EDGES", profile=0.5)
    bmesh.ops.translate(bm, vec=Vector(center), verts=_new_verts(bm))
    _finish(bm, mat_index)


def add_cylinder(bm, radius, depth, center, mat_index=0, segments=12, axis="Z", radius2=None):
    """A capped cylinder (or frustum with `radius2`) centred on `center` along `axis`."""
    bmesh.ops.create_cone(
        bm,
        cap_ends=True,
        cap_tris=False,
        segments=segments,
        radius1=radius,
        radius2=radius if radius2 is None else radius2,
        depth=depth,
    )
    verts = _new_verts(bm)
    if axis == "X":
        bmesh.ops.rotate(bm, cent=Vector(), matrix=Matrix.Rotation(math.pi / 2, 3, "Y"), verts=verts)
    elif axis == "Y":
        bmesh.ops.rotate(bm, cent=Vector(), matrix=Matrix.Rotation(math.pi / 2, 3, "X"), verts=verts)
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    _finish(bm, mat_index)


def add_cone(bm, radius, height, base_center, mat_index=0, segments=8):
    """A pointed cone standing on `base_center` (built by hand: no degenerate top cap)."""
    bx, by, bz = base_center
    ring = [
        bm.verts.new((bx + radius * math.cos(2 * math.pi * i / segments),
                      by + radius * math.sin(2 * math.pi * i / segments), bz))
        for i in range(segments)
    ]
    apex = bm.verts.new((bx, by, bz + height))
    for i in range(segments):
        bm.faces.new((ring[i], ring[(i + 1) % segments], apex))
    bm.faces.new(list(reversed(ring)))
    _finish(bm, mat_index)


def add_sphere(bm, radius, center, mat_index=0, subdivisions=2, scale=(1, 1, 1)):
    bmesh.ops.create_icosphere(bm, subdivisions=subdivisions, radius=radius)
    verts = _new_verts(bm)
    bmesh.ops.scale(bm, vec=Vector(scale), verts=verts)
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    _finish(bm, mat_index)


def jitter_new(bm, rng, amount, min_z=None):
    """Moves every vertex of the part being built by a seeded random offset."""
    for v in _new_verts(bm):
        if min_z is None or v.co.z > min_z:
            v.co += Vector((rng.uniform(-amount, amount), rng.uniform(-amount, amount),
                            rng.uniform(-amount, amount)))


def export_glb(path, objects, animations=False):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.gltf(
        filepath=path,
        export_format="GLB",
        use_selection=True,
        export_yup=True,
        export_apply=False,
        export_texcoords=True,
        export_normals=True,
        export_tangents=False,
        export_materials="EXPORT",
        export_image_format="AUTO",
        export_vertex_color="NONE",
        export_all_vertex_colors=False,
        export_attributes=False,
        export_cameras=False,
        export_lights=True,
        export_extras=False,
        export_skins=animations,
        export_def_bones=False,
        export_animations=animations,
        export_animation_mode="ACTIONS",
        export_force_sampling=True,
        export_frame_range=False,
        export_anim_slide_to_zero=True,
        export_reset_pose_bones=True,
        export_optimize_animation_size=False,
    )
    print(f"wrote {path} ({os.path.getsize(path)} bytes)")


# --------------------------------------------------------------------------- textures


def tileable_noise(size, rng, octaves):
    """Seamless noise in 0..1: random noise filtered in the frequency domain wraps by construction."""
    out = np.zeros((size, size), dtype=np.float64)
    fy = np.fft.fftfreq(size)[:, None]
    fx = np.fft.fftfreq(size)[None, :]
    f = np.sqrt(fx * fx + fy * fy)
    for cutoff, weight in octaves:
        spectrum = np.fft.fft2(rng.standard_normal((size, size)))
        filt = np.exp(-((f / cutoff) ** 2))
        layer = np.real(np.fft.ifft2(spectrum * filt))
        layer = (layer - layer.mean()) / (layer.std() + 1e-9)
        out += weight * layer
    out = (out - out.min()) / (out.max() - out.min())
    return out


def grass_texture():
    size = 512
    rng = np.random.default_rng(1)
    broad = tileable_noise(size, rng, [(0.01, 1.0), (0.03, 0.5)])
    fine = tileable_noise(size, rng, [(0.25, 1.0), (0.12, 0.6)])
    dark = np.array([0.20, 0.42, 0.13])
    light = np.array([0.36, 0.60, 0.20])
    dry = np.array([0.52, 0.58, 0.24])
    t = broad[:, :, None]
    rgb = dark * (1 - t) + light * t
    # A little dry grass where the broad noise is high, then blade-scale speckle.
    dryness = np.clip((broad - 0.72) / 0.28, 0, 1)[:, :, None] * 0.35
    rgb = rgb * (1 - dryness) + dry * dryness
    rgb *= (0.86 + 0.28 * fine)[:, :, None]
    return image_from_array("Grass", rgb)


def screen_placeholder_texture():
    """What the robot's screen shows before the host binds a video: a calm face on dark blue."""
    w, h = 512, 288
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    bg = np.array([0.06, 0.10, 0.20])
    fg = np.array([0.45, 0.85, 0.95])
    # Soft vignette so the flat screen reads as a display.
    cx, cy = (w - 1) / 2, (h - 1) / 2
    r = np.sqrt(((x - cx) / w) ** 2 + ((y - cy) / h) ** 2)
    rgb = bg[None, None, :] * (1.15 - 0.6 * r)[:, :, None]
    mask = np.zeros((h, w))
    for ex in (cx - 85, cx + 85):  # eyes
        d = np.sqrt((x - ex) ** 2 + ((y - (cy - 30)) * 0.8) ** 2)
        mask = np.maximum(mask, np.clip(30 - d, 0, 1))
    # Smile: a thick arc below the centre.
    d = np.sqrt((x - cx) ** 2 + (y - (cy - 20)) ** 2)
    arc = np.clip(9 - np.abs(d - 95), 0, 1) * (y > cy + 25) * (np.abs(x - cx) < 75)
    mask = np.maximum(mask, arc)
    rgb = rgb * (1 - mask[:, :, None]) + fg[None, None, :] * mask[:, :, None]
    # Faint scan lines.
    rgb *= (0.94 + 0.06 * (np.arange(h) % 3 == 0))[:, None, None]
    return image_from_array("ScreenPlaceholder", rgb)


# --------------------------------------------------------------------------- the field


def tree_pine(mats):
    bm = bmesh.new()
    add_cylinder(bm, 0.16, 1.3, (0, 0, 0.65), mat_index=0, segments=7)
    for r, z, h in [(1.45, 1.0, 1.9), (1.15, 2.0, 1.7), (0.78, 3.0, 1.5)]:
        add_cone(bm, r, h, (0, 0, z), mat_index=1, segments=8)
    return new_object("TreePine", bm, [mats["bark"], mats["leaf_dark"]])


def lumpy_sphere(bm, rng, radius, center, mat_index, scale=(1, 1, 1), amount=0.12):
    bmesh.ops.create_icosphere(bm, subdivisions=1, radius=radius)
    verts = _new_verts(bm)
    bmesh.ops.scale(bm, vec=Vector(scale), verts=verts)
    jitter_new(bm, rng, amount)
    bmesh.ops.translate(bm, vec=Vector(center), verts=verts)
    _finish(bm, mat_index)


def tree_round(mats, rng):
    bm = bmesh.new()
    add_cylinder(bm, 0.2, 2.0, (0, 0, 1.0), mat_index=0, segments=7, radius2=0.14)
    lumpy_sphere(bm, rng, 1.55, (0, 0, 2.9), 1, scale=(1, 1, 0.9))
    lumpy_sphere(bm, rng, 0.95, (0.85, 0.3, 2.55), 1)
    lumpy_sphere(bm, rng, 0.85, (-0.7, -0.45, 3.35), 1)
    return new_object("TreeRound", bm, [mats["bark"], mats["leaf"]])


def tree_birch(mats, rng):
    bm = bmesh.new()
    add_cylinder(bm, 0.11, 3.2, (0, 0, 1.6), mat_index=0, segments=6, radius2=0.08)
    lumpy_sphere(bm, rng, 0.95, (0, 0, 3.7), 1, scale=(1, 1, 1.45), amount=0.08)
    lumpy_sphere(bm, rng, 0.6, (0.45, 0.2, 2.9), 1, scale=(1, 1, 1.2), amount=0.08)
    return new_object("TreeBirch", bm, [mats["bark_light"], mats["leaf_light"]])


# The solid part of each tree: its trunk, as (radius, height) in metres at scale 1.
TRUNKS = {"TreePine": (0.16, 1.3), "TreeRound": (0.2, 2.0), "TreeBirch": (0.11, 3.2)}


def trunk_collider(name):
    """A collision-only cylinder over a tree's trunk. The node name ends in `-colonly`, which the
    importer reads as 'a static collider that is not drawn' (no material, so nothing to draw anyway)."""
    radius, height = TRUNKS[name]
    bm = bmesh.new()
    add_cylinder(bm, radius, height, (0, 0, height / 2), mat_index=0, segments=8)
    return new_object(f"{name}Trunk-colonly", bm, [])


def scatter_points(rng, count, r_min, r_max, min_gap, existing):
    points = []
    attempts = 0
    while len(points) < count and attempts < count * 200:
        attempts += 1
        a = rng.uniform(0, 2 * math.pi)
        # Uniform over the ring's area.
        r = math.sqrt(rng.uniform(r_min * r_min, r_max * r_max))
        p = (r * math.cos(a), r * math.sin(a))
        if all((p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2 >= min_gap * min_gap for q in existing + points):
            points.append(p)
    return points


# --------------------------------------------------------------------------- the terrain

TERRAIN_HALF = 80.0  # m: the ground is 160 m square
TERRAIN_STEP = 2.0  # m between the grid's vertices
CLEARING_RADIUS = 14.0  # m: flat at height 0 round the origin (where people appear)
CLEARING_BLEND = 16.0  # m over which the slopes grow back to full height
# (wavelength in metres, height in metres) of each layer of noise: gentle hills and swells.
TERRAIN_LAYERS = [(64.0, 1.6), (32.0, 0.6), (16.0, 0.15)]


class Terrain:
    """A height field made of smooth value noise: seeded, so every run gives the same land."""

    LATTICE = 64

    def __init__(self, seed):
        rs = np.random.RandomState(seed)
        self.values = [rs.uniform(-1.0, 1.0, (self.LATTICE, self.LATTICE)) for _ in TERRAIN_LAYERS]

    @staticmethod
    def _fade(t):
        return t * t * t * (t * (t * 6 - 15) + 10)

    def _noise(self, layer, x, y):
        lattice = self.values[layer]
        n = self.LATTICE
        x0, y0 = math.floor(x), math.floor(y)
        fx, fy = self._fade(x - x0), self._fade(y - y0)
        i0, j0 = int(x0) % n, int(y0) % n
        i1, j1 = (i0 + 1) % n, (j0 + 1) % n
        top = lattice[i0, j0] * (1 - fx) + lattice[i1, j0] * fx
        bottom = lattice[i0, j1] * (1 - fx) + lattice[i1, j1] * fx
        return top * (1 - fy) + bottom * fy

    def height(self, x, y):
        """The ground's height at (x, y) in metres: 0 in the clearing, hills farther out."""
        total = 0.0
        for layer, (wavelength, amplitude) in enumerate(TERRAIN_LAYERS):
            total += amplitude * self._noise(layer, x / wavelength + 7.3 * layer, y / wavelength - 3.1 * layer)
        r = math.hypot(x, y)
        t = min(max((r - CLEARING_RADIUS) / CLEARING_BLEND, 0.0), 1.0)
        return total * t * t * (3 - 2 * t)


def terrain_mesh(terrain, material):
    """The ground: a grid of TERRAIN_STEP squares over the 160 m square, smooth shaded, one texture
    tile per 4 m. It is also the static collider (the `-col` suffix)."""
    n = int(round(2 * TERRAIN_HALF / TERRAIN_STEP))
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new("UVMap")
    grid = [
        [
            bm.verts.new((x, y, terrain.height(x, y)))
            for x in (-TERRAIN_HALF + i * TERRAIN_STEP for i in range(n + 1))
        ]
        for y in (-TERRAIN_HALF + j * TERRAIN_STEP for j in range(n + 1))
    ]
    for j in range(n):
        for i in range(n):
            face = bm.faces.new([grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]])
            for loop in face.loops:
                loop[uv].uv = ((loop.vert.co.x + TERRAIN_HALF) / 4.0, (loop.vert.co.y + TERRAIN_HALF) / 4.0)
    return new_object("Ground-col", bm, [material], smooth=True)


def build_field():
    reset_scene()
    rng = random.Random(42)

    mats = {
        "grass": material("Grass", (1, 1, 1), roughness=0.95, image=grass_texture()),
        "bark": material("Bark", (0.36, 0.24, 0.15), roughness=0.9),
        "bark_light": material("BirchBark", (0.86, 0.84, 0.78), roughness=0.85),
        "leaf_dark": material("LeavesDark", (0.13, 0.36, 0.20), roughness=0.85),
        "leaf": material("Leaves", (0.27, 0.55, 0.22), roughness=0.85),
        "leaf_light": material("LeavesLight", (0.55, 0.72, 0.28), roughness=0.85),
    }

    # The ground: a height field (see Terrain), flat in the middle. The `-col` suffix makes the
    # drawn mesh itself a static collider, so people walk on it.
    terrain = Terrain(seed=7)
    ground = terrain_mesh(terrain, mats["grass"])

    prototypes = [tree_pine(mats), tree_round(mats, rng), tree_birch(mats, rng)]
    trunks = {proto.name: trunk_collider(proto.name) for proto in prototypes}

    # Trees: none near the centre (where people appear), a loose scatter in the meadow and a
    # dense ring around the edge that hides the end of the ground.
    meadow = scatter_points(rng, 36, 14.0, 42.0, 5.0, [])
    ring = scatter_points(rng, 280, 44.0, 68.0, 2.8, meadow)
    objects = [ground]
    for i, p in enumerate(meadow + ring):
        proto = prototypes[rng.randrange(len(prototypes))]
        inst = bpy.data.objects.new(f"{proto.name}.{i:03d}", proto.data)
        # Standing on the ground, a hair into it so no gap shows on a slope.
        inst.location = (p[0], p[1], terrain.height(p[0], p[1]) - 0.03)
        inst.rotation_euler = (0.0, 0.0, rng.uniform(0, 2 * math.pi))
        s = rng.uniform(0.8, 1.3)
        inst.scale = (s, s, s * rng.uniform(0.9, 1.15))
        bpy.context.scene.collection.objects.link(inst)
        objects.append(inst)
        # The trunk's collider is a child, so it follows the tree's place, turn and scale.
        col = bpy.data.objects.new(f"{proto.name}Trunk-colonly", trunks[proto.name].data)
        col.parent = inst
        bpy.context.scene.collection.objects.link(col)
        objects.append(col)
    # The prototypes themselves are not placed; their meshes are shared by the instances.
    for proto in prototypes:
        bpy.data.objects.remove(proto)
    for trunk in trunks.values():
        bpy.data.objects.remove(trunk)

    spawn = bpy.data.objects.new("Spawn", None)
    bpy.context.scene.collection.objects.link(spawn)
    objects.append(spawn)

    # The sun and the moon: assets/worlds/daylight.lua moves them on the sky around the camera.
    objects.append(sky_disc("Sun", (1.0, 0.95, 0.75), 5.0))
    objects.append(sky_disc("Moon", (0.85, 0.9, 1.0), 4.0))

    # Two directional lights, only so the file names the entities the script drives: it sets their
    # direction, color and intensity every frame.
    for light_name in ("SunLight", "MoonLight"):
        data = bpy.data.lights.new(light_name, "SUN")
        obj = bpy.data.objects.new(light_name, data)
        bpy.context.scene.collection.objects.link(obj)
        objects.append(obj)

    export_glb(os.path.join(OUT_DIR, "green_field.glb"), objects)


# --------------------------------------------------------------------------- the robot

# Blender is Z-up and the robot faces -Y, which the exporter turns into glTF's +Y up, +Z forward.
# Heights in metres; the robot is about 1.27 m tall with its eyes (the screen centre) at 1.06 m.

BONES = {
    # name: (head, tail, parent)
    "root": ((0, 0, 0), (0, 0.15, 0), None),
    "hips": ((0, 0, 0.45), (0, 0, 0.55), "root"),
    "torso": ((0, 0, 0.55), (0, 0, 0.86), "hips"),
    "head": ((0, 0, 0.86), (0, 0, 1.2), "torso"),
    "arm.L": ((0.25, 0, 0.8), (0.25, 0, 0.5), "torso"),
    "arm.R": ((-0.25, 0, 0.8), (-0.25, 0, 0.5), "torso"),
    "leg.L": ((0.11, 0, 0.45), (0.11, 0, 0.06), "hips"),
    "leg.R": ((-0.11, 0, 0.45), (-0.11, 0, 0.06), "hips"),
}


def build_robot_mesh(mats):
    # Material slots: 0 body, 1 joints, 2 accent, 3 bezel, 4 screen.
    BODY, JOINT, ACCENT, BEZEL, SCREEN = range(5)
    bm = bmesh.new()
    bone_names = [name for name in BONES if name != "root"]
    v_part = bm.verts.layers.int.new("part")
    f_bone = bm.faces.layers.int.new("bone")
    part_count = [0]

    def part(bone, build):
        # Mark what this part adds: its number (for the canonical vertex order) and its bone,
        # stored plus one (for skinning). New elements are the ones whose layers are still 0;
        # bevel copies the 0 of the part's own faces and vertices onto what it creates.
        build()
        part_count[0] += 1
        for v in bm.verts:
            if v[v_part] == 0:
                v[v_part] = part_count[0]
        for f in bm.faces:
            if f[f_bone] == 0:
                f[f_bone] = bone_names.index(bone) + 1

    # Legs and feet.
    for side, x in (("L", 0.11), ("R", -0.11)):
        part(f"leg.{side}", lambda x=x: add_cylinder(bm, 0.05, 0.36, (x, 0, 0.26), JOINT, segments=10))
        part(f"leg.{side}", lambda x=x: add_box(bm, (0.13, 0.22, 0.07), (x, -0.03, 0.035), BODY, bevel=0.02))
    # Pelvis.
    part("hips", lambda: add_box(bm, (0.32, 0.2, 0.1), (0, 0, 0.47), JOINT, bevel=0.02))
    # Torso with a chest panel and a small status light.
    part("torso", lambda: add_box(bm, (0.42, 0.27, 0.36), (0, 0, 0.69), BODY, bevel=0.05, segments=3))
    part("torso", lambda: add_box(bm, (0.24, 0.02, 0.14), (0, -0.14, 0.71), ACCENT, bevel=0.008))
    part("torso", lambda: add_sphere(bm, 0.025, (0.08, -0.15, 0.81), ACCENT, subdivisions=1))
    # Arms with round hands.
    for side, x in (("L", 0.25), ("R", -0.25)):
        part(f"arm.{side}", lambda x=x: add_sphere(bm, 0.055, (x, 0, 0.8), JOINT, subdivisions=2))
        part(f"arm.{side}", lambda x=x: add_cylinder(bm, 0.038, 0.26, (x, 0, 0.66), JOINT, segments=10))
        part(f"arm.{side}", lambda x=x: add_sphere(bm, 0.06, (x, 0, 0.5), BODY, subdivisions=2))
    # Neck.
    part("head", lambda: add_cylinder(bm, 0.045, 0.08, (0, 0, 0.88), JOINT, segments=10))
    # Head: a rounded box, a dark bezel standing 8 mm proud of the front and the screen 8 mm
    # proud of the bezel (no coplanar faces, so nothing fights for depth at a distance).
    part("head", lambda: add_box(bm, (0.5, 0.27, 0.32), (0, 0, 1.06), BODY, bevel=0.05, segments=3))
    part("head", lambda: add_box(bm, (0.44, 0.016, 0.262), (0, -0.135 - 0.004, 1.06), BEZEL, bevel=0.006))
    part("head", lambda: add_screen(bm, SCREEN, BEZEL))
    # Ears and antenna.
    for x in (0.255, -0.255):
        part("head", lambda x=x: add_cylinder(bm, 0.05, 0.03, (x, 0, 1.06), JOINT, segments=12, axis="X"))
    part("head", lambda: add_cylinder(bm, 0.008, 0.12, (0.12, 0, 1.27), JOINT, segments=6))
    part("head", lambda: add_sphere(bm, 0.022, (0.12, 0, 1.335), ACCENT, subdivisions=1))

    obj = new_object(
        "Robot", bm, [mats["body"], mats["joint"], mats["accent"], mats["bezel"], mats["screen"]]
    )
    # Rigid skinning: every vertex belongs to exactly one bone, by the part its face came from.
    mesh = obj.data
    face_bone = [0] * len(mesh.polygons)
    mesh.attributes["bone"].data.foreach_get("value", face_bone)
    mesh.attributes.remove(mesh.attributes["bone"])
    groups = [obj.vertex_groups.new(name=name) for name in bone_names]
    per_bone = [set() for _ in bone_names]
    for poly, bone in zip(mesh.polygons, face_bone):
        per_bone[bone - 1].update(poly.vertices)
    for group, verts in zip(groups, per_bone):
        group.add(sorted(verts), 1.0, "REPLACE")
    return obj


def add_screen(bm, screen_index, side_index):
    """The display: a 16:9 slab whose front face carries the Screen material with 0..1 UVs."""
    w, h, d = 0.4, 0.225, 0.016
    front_y = -0.135 - 0.012 - d / 2  # sits on the bezel, whose front is at y = -0.147
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector((w, d, h)), verts=_new_verts(bm))
    bmesh.ops.translate(bm, vec=Vector((0, front_y, 1.06)), verts=_new_verts(bm))
    bm.normal_update()
    uv = bm.loops.layers.uv.verify()
    for f in _new_faces(bm):
        if f.normal.y < -0.9:
            f.material_index = screen_index
            # Someone facing the robot looks along +Y, so their right is +X: u grows with X
            # and v with Z, and the image appears unmirrored to the people in front of it.
            for loop in f.loops:
                x, _, z = loop.vert.co
                loop[uv].uv = ((x + w / 2) / w, (z - (1.06 - h / 2)) / h)
        else:
            f.material_index = side_index
    _finish(bm)


def build_robot_armature():
    arm_data = bpy.data.armatures.new("RobotRig")
    rig = bpy.data.objects.new("RobotRig", arm_data)
    bpy.context.scene.collection.objects.link(rig)
    bpy.context.view_layer.objects.active = rig
    bpy.ops.object.mode_set(mode="EDIT")
    for name, (head, tail, parent) in BONES.items():
        b = arm_data.edit_bones.new(name)
        b.head = head
        b.tail = tail
        b.roll = 0.0
        if parent:
            b.parent = arm_data.edit_bones[parent]
    bpy.ops.object.mode_set(mode="OBJECT")
    return rig


def key_pose(rig, frame, pose):
    """`pose` maps bone -> {"rot": (x, y, z) degrees in the bone's local space, "loc": (x, y, z)}."""
    for name, channels in pose.items():
        pb = rig.pose.bones[name]
        pb.rotation_mode = "XYZ"
        if "rot" in channels:
            pb.rotation_euler = [math.radians(a) for a in channels["rot"]]
            pb.keyframe_insert("rotation_euler", frame=frame)
        if "loc" in channels:
            pb.location = channels["loc"]
            pb.keyframe_insert("location", frame=frame)


def make_action(rig, name, frames, pose_at):
    action = bpy.data.actions.new(name)
    rig.animation_data_create()
    rig.animation_data.action = action
    for f in frames:
        key_pose(rig, f, pose_at(f))
    action.use_frame_range = True
    action.frame_start = frames[0]
    action.frame_end = frames[-1]
    action.use_fake_user = True
    # Keep the action for export without leaving it assigned.
    rig.animation_data.action = None
    for pb in rig.pose.bones:
        pb.rotation_euler = (0, 0, 0)
        pb.location = (0, 0, 0)
    return action


def walk_pose(frame):
    # One stride cycle in 24 frames (1 s at 24 fps). Bones point down for the legs and arms,
    # so a rotation about the bone's local X swings them forward and back.
    t = 2 * math.pi * frame / 24.0
    swing = 26.0 * math.sin(t)
    arm = 20.0 * math.sin(t)
    bob = 0.025 * (1 - math.cos(2 * t)) / 2
    return {
        "leg.L": {"rot": (swing, 0, 0)},
        "leg.R": {"rot": (-swing, 0, 0)},
        "arm.L": {"rot": (-arm, 0, 0)},
        "arm.R": {"rot": (arm, 0, 0)},
        "hips": {"loc": (0, bob, 0), "rot": (0, 4.0 * math.sin(t), 0)},
        "torso": {"rot": (3.0, -3.0 * math.sin(t), 0)},
        "head": {"rot": (2.0 + 1.5 * math.sin(2 * t), 2.0 * math.sin(t), 0)},
    }


def idle_pose(frame):
    # A slow breath over 48 frames (2 s), with a slight look around.
    t = 2 * math.pi * frame / 48.0
    return {
        "leg.L": {"rot": (0, 0, 0)},
        "leg.R": {"rot": (0, 0, 0)},
        "arm.L": {"rot": (1.5 * math.sin(t), 0, 0)},
        "arm.R": {"rot": (1.5 * math.sin(t), 0, 0)},
        "hips": {"loc": (0, 0.006 * math.sin(t), 0), "rot": (0, 0, 0)},
        "torso": {"rot": (1.0 * math.sin(t), 0, 0)},
        "head": {"rot": (2.0 * math.sin(t), 4.0 * math.sin(t / 2 + 0.5), 0)},
    }


def build_robot():
    reset_scene()
    mats = {
        "body": material("Body", (0.92, 0.92, 0.9), roughness=0.45),
        "joint": material("Joint", (0.22, 0.24, 0.27), roughness=0.55, metallic=0.6),
        "accent": material("Accent", (0.98, 0.55, 0.12), roughness=0.4),
        "bezel": material("Bezel", (0.06, 0.06, 0.07), roughness=0.35),
        "screen": material("Screen", (1, 1, 1), roughness=0.05, image=screen_placeholder_texture()),
    }
    body = build_robot_mesh(mats)
    rig = build_robot_armature()
    body.parent = rig
    mod = body.modifiers.new("Armature", "ARMATURE")
    mod.object = rig

    frames_walk = list(range(0, 25))
    frames_idle = list(range(0, 49))
    walk = make_action(rig, "Walk", frames_walk, walk_pose)
    idle = make_action(rig, "Idle", frames_idle, idle_pose)
    # The exporter's ACTIONS mode exports every action that can drive the armature; give
    # each a slot for it.
    rig.animation_data.action = idle
    rig.animation_data.action = None

    export_glb(os.path.join(OUT_DIR, "robot.glb"), [rig, body], animations=True)


if __name__ == "__main__":
    which = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else ["field", "robot"]
    if "field" in which:
        build_field()
    if "robot" in which:
        build_robot()
