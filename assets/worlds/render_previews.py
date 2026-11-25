# Imports green_field.glb and robot.glb back into Blender and renders four preview images, to
# check the exported files (not the Blender scenes) after a change to generate_assets.py.
#
#   blender -b --factory-startup --python assets/worlds/render_previews.py -- OUTPUT_DIR
#
# Renders with Cycles on the CPU (a minute or two); the sky and the sun are preview-only.

import math
import os
import sys

import bpy
from mathutils import Vector

ASSETS = os.path.dirname(os.path.abspath(__file__))


def setup():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 24
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = 960, 540
    world = bpy.data.worlds.new("Sky")
    scene.world = world
    background = world.node_tree.nodes["Background"]
    background.inputs[0].default_value = (0.55, 0.72, 0.95, 1)
    background.inputs[1].default_value = 0.8
    sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
    sun.data.energy = 3.5
    sun.rotation_euler = (math.radians(50), 0, math.radians(35))
    scene.collection.objects.link(sun)
    return scene


def import_glb(name):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(ASSETS, name))
    return [o for o in bpy.data.objects if o not in before]


def camera(scene, location, target, lens=35):
    cam = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
    cam.data.lens = lens
    scene.collection.objects.link(cam)
    cam.location = location
    cam.rotation_euler = (Vector(target) - Vector(location)).to_track_quat("-Z", "Y").to_euler()
    scene.camera = cam


def play(rig, action_name):
    action = next(a for a in bpy.data.actions if a.name.startswith(action_name))
    rig.animation_data_create()
    rig.animation_data.action = action
    if action.slots:
        rig.animation_data.action_slot = action.slots[0]


def robot(location, yaw_degrees, action_name):
    objects = import_glb("robot.glb")
    for o in objects:
        if o.parent is None:
            o.location = location
            o.rotation_euler = (0, 0, math.radians(yaw_degrees))
    rig = next(o for o in objects if o.type == "ARMATURE")
    play(rig, action_name)
    return rig


def render(scene, out_dir, name, frame):
    scene.frame_set(frame)
    scene.render.filepath = os.path.join(out_dir, name)
    bpy.ops.render.render(write_still=True)
    print(f"rendered {scene.render.filepath}")


def main(out_dir):
    scene = setup()
    rig = robot((0, 0, 0), 0, "Idle")
    camera(scene, (1.3, -2.6, 1.35), (0, 0, 0.75), lens=50)
    render(scene, out_dir, "robot_front.png", 0)
    play(rig, "Walk")
    camera(scene, (2.6, 0.2, 1.0), (0, 0, 0.7), lens=50)
    render(scene, out_dir, "robot_walk_side.png", 6)

    scene = setup()
    import_glb("green_field.glb")
    robot((-1.2, 4.0, 0), 200, "Idle")
    robot((1.6, 6.5, 0), 150, "Walk")
    robot((-3.5, 9.0, 0), 100, "Walk")
    camera(scene, (0, -1.0, 1.05), (0, 8, 0.9), lens=28)  # eye height of a robot
    render(scene, out_dir, "field_first_person.png", 6)
    camera(scene, (55, -55, 38), (0, 0, 0), lens=30)
    render(scene, out_dir, "field_overview.png", 0)


if __name__ == "__main__":
    args = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    if not args:
        sys.exit("usage: blender -b --factory-startup --python render_previews.py -- OUTPUT_DIR")
    main(os.path.abspath(args[0]))
