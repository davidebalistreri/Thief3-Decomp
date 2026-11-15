from dataclasses import replace
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tools"))
import scene_material_overrides as overrides
import scene_ir_to_textured_packet as packet
from test_scene_textured_packet import HARNESS, Resolver, scoped_scene


def scene_fixture():
    data = scoped_scene().to_dict()
    data["conventions"]["meters_per_unit"] = overrides.SCALE
    actor = data["mesh_instances"][0]
    old_id = actor["id"]
    actor["id"] = f"actor:{overrides.MAP}:{data['source']['fingerprints']['map']}:{overrides.ACTOR}"
    for submission in data["submissions"]:
        if submission["target_id"] == old_id:
            submission["target_id"] = actor["id"]
    data["materials"][0]["name"] = "HSEfloorB_D"
    data["materials"].append({"id": "material:wall", "name": "STMwall512D_D"})

    # Coordinates are in the bed's Godot local metres; convert them to the
    # source T3 axes and the fixture's runtime bed translation.
    floor = [(0, 0, 0), (1, 0, 0), (0, 0, 1)]
    ceiling = [floor[0], floor[2], floor[1]]
    outside = [(7+x, y, z) for x, y, z in floor]
    wall = [(1, 0, 0), (1, 2, 0), (1, 0, 1)]
    points = floor + ceiling + outside + wall
    origin = actor["transform"][12:15]
    positions = [[p[0]/overrides.SCALE + origin[0], p[2]/overrides.SCALE + origin[1],
                  p[1]/overrides.SCALE + origin[2]] for p in points]
    geometry = data["bsp_blocks"][0]["geometry"]
    geometry.update(positions=positions, normals=[[0, 0, 1]] * 12,
                    uv0=[[0, 0], [1, 0], [0, 1]] * 4, tangents=[[1, 0, 0]] * 12,
                    indices=list(range(12)), sections=[
                        {"name": "floor", "first_index": 0, "index_count": 9, "material_id": "material:wood"},
                        {"name": "wall", "first_index": 9, "index_count": 3, "material_id": "material:wall"}])
    return packet.SceneSnapshot.from_dict(data)


class FakeRemaster(overrides.BedroomSelection):
    def resolve(self, name, kind):
        return {"name": name + "_" + kind, "bundle": "local_godot_remaster", "format": "RGBA8",
                "width": 1, "height": 1, "mips": [bytes([128, 128, 255, 255])],
                "source_sha256": "d" * 64, "usage": int(kind == "normal")}


def decoded_draws(raw):
    version = int(raw[5:7])
    textures, draws = struct.unpack_from("<II", raw, 152)
    offset = 160
    for _ in range(textures):
        w, h, levels = struct.unpack_from("<III", raw, offset)
        offset += 12
        for _ in range(levels):
            offset += w*h*4
            w, h = max(1, w//2), max(1, h//2)
    result = []
    for _ in range(draws):
        texture, count = struct.unpack_from("<II", raw, offset)
        header = 160 if version >= 9 else 56
        shading = struct.unpack_from("<I", raw, offset+56)[0] if version >= 4 else 0
        offset += header
        stride = 48 if version >= 9 else 20
        vertices = [struct.unpack_from("<5f", raw, offset+i*stride) for i in range(count)]
        offset += count*stride
        result.append((texture, shading, vertices))
    assert offset == len(raw)
    return result


class BedroomSelectionTests(unittest.TestCase):
    def test_bsp_floor_ceiling_wall_and_region(self):
        scene = scene_fixture()
        selection = overrides.BedroomSelection(scene)
        geometry = scene.bsp_blocks[0].geometry
        floor = [(*p, 0, 0) for p in geometry.positions[:9]]
        wall = [(*p, 0, 0) for p in geometry.positions[9:]]
        self.assertEqual(selection.partition("bsp_block", "hsefloorb_d", floor),
                         [(0, 3, "wood_floor_v2"), (3, 9, None)])
        self.assertEqual(selection.partition("bsp_block", "STMwall512D_D", wall), [(0, 3, "stone_wall_v2")])
        self.assertEqual(selection.partition("mesh_instance", "STMwall512D_D", wall), [(0, 3, None)])
        self.assertEqual(selection.partition("bsp_block", "other", wall), [(0, 3, None)])
        self.assertEqual(selection.partition("bsp_block", "STMwall512D_D", [wall[0]]*3), [(0, 3, None)])

    def test_rotation_scale_and_translation_are_relative_to_anchor(self):
        scene = scene_fixture()
        matrix = [0, 2, 0, 0, -3, 0, 0, 0, 0, 0, 4, 0, 11, -7, 9, 1]
        instance = replace(scene.mesh_instances[0], transform=tuple(matrix))
        moved = replace(scene, mesh_instances=(instance,))
        original = overrides.BedroomSelection(scene)
        transformed = overrides.BedroomSelection(moved)
        old_matrix = scene.mesh_instances[0].transform
        points = scene.bsp_blocks[0].geometry.positions[:9]
        # Bring the fixture's world points back to local, then transform by the new anchor.
        local = [[p[i] - old_matrix[12+i] for i in range(3)] for p in points]
        moved_points = [packet.transform_point(p, matrix)[:3] for p in local]
        self.assertEqual(original.partition("bsp_block", "HSEfloorB_D", points),
                         transformed.partition("bsp_block", "HSEfloorB_D", moved_points))

    def test_rejects_wrong_map_anchor_scale_and_singular_transform(self):
        scene = scene_fixture()
        for changed in (replace(scene, source=replace(scene.source, map="other")),
                        replace(scene, source=replace(scene.source, camera_synchronized=False)),
                        replace(scene, conventions=replace(scene.conventions, meters_per_unit=None)),
                        replace(scene, mesh_instances=()),
                        replace(scene, static_mesh_resources=(replace(scene.static_mesh_resources[0], name="other"),)),
                        replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=(0,)*16),))):
            with self.subTest(scene=changed), self.assertRaises(ValueError):
                overrides.BedroomSelection(changed)

    def test_selection_cannot_be_reused_for_a_different_frame_or_anchor(self):
        scene = scene_fixture()
        selector = overrides.BedroomSelection(scene)
        selector.validate_scene(scene)
        with self.assertRaisesRegex(ValueError, "does not belong"):
            selector.validate_scene(replace(scene, source=replace(scene.source, frame_id=4)))
        transform = list(scene.mesh_instances[0].transform)
        transform[12] += 1
        changed = replace(scene, mesh_instances=(replace(scene.mesh_instances[0], transform=tuple(transform)),))
        with self.assertRaisesRegex(ValueError, "does not belong"):
            selector.validate_scene(changed)

    @unittest.skipUnless(shutil.which("magick"), "ImageMagick 7 required")
    def test_actual_png_conversion_hash_mips_and_cache(self):
        with tempfile.TemporaryDirectory(prefix="t3render-material-") as directory:
            root = Path(directory)
            path = root / "wood_floor_v2" / "wood_floor_v2_albedo.png"
            path.parent.mkdir()
            subprocess.run(["magick", "-size", "1024x1024", "xc:#123456", str(path)], check=True, timeout=30)
            textures = overrides.GodotTextures(root)
            result = textures.resolve("wood_floor_v2", "albedo")
            self.assertEqual(len(result["mips"]), 11)
            self.assertEqual(result["mips"][0][:4], bytes((0x12, 0x34, 0x56, 255)))
            self.assertEqual(result["mips"][-1], bytes((0x12, 0x34, 0x56, 255)))
            self.assertIs(textures.resolve("wood_floor_v2", "albedo"), result)
            for channel in ("normal", "roughness", "ao"):
                shutil.copyfile(path, path.with_name(f"wood_floor_v2_{channel}.png"))
            smaller = overrides.GodotTextures(root, texture_size=512)
            for channel in ("albedo", "normal", "roughness", "ao"):
                value = smaller.resolve("wood_floor_v2", channel)
                self.assertEqual((value["width"], value["height"], len(value["mips"])), (512, 512, 10))
                self.assertEqual(value["mips"][-1], bytes((0x12, 0x34, 0x56, 255)))
            for invalid in (0, 3, 2048, True):
                with self.subTest(size=invalid), self.assertRaisesRegex(ValueError, "power of two"):
                    overrides.GodotTextures(root, texture_size=invalid)
            subprocess.run(["magick", "-size", "1024x1024", "xc:black", "-fill", "white", "-draw",
                            "rectangle 512,0 1023,1023", str(path)], check=True, timeout=30)
            linear = overrides.GodotTextures(root, texture_size=512, linear_albedo_mips=True)
            ordinary = overrides.GodotTextures(root, texture_size=512)
            self.assertTrue(186 <= linear.resolve("wood_floor_v2", "albedo")["mips"][-1][0] <= 190)
            self.assertTrue(126 <= ordinary.resolve("wood_floor_v2", "albedo")["mips"][-1][0] <= 130)
            with self.assertRaises(ValueError):
                textures.resolve("../wood_floor_v2", "albedo")

    @unittest.skipUnless(shutil.which("magick"), "ImageMagick 7 required")
    def test_rectangular_beam_texture_keeps_normalized_uv_layout(self):
        with tempfile.TemporaryDirectory(prefix="t3render-beam-material-") as directory:
            path = Path(directory)/"beam_wood_v2"/"beam_wood_v2_albedo.png"
            path.parent.mkdir()
            subprocess.run(["magick", "-size", "512x256", "xc:black", "-fill", "white", "-draw",
                            "rectangle 256,0 511,255", str(path)], check=True, timeout=30)
            result = overrides.GodotTextures(directory, texture_size=512).resolve("beam_wood_v2", "albedo")
            pixels = result["mips"][0]
            self.assertEqual(pixels[(128*512+128)*4:(128*512+128)*4+4], bytes((0, 0, 0, 255)))
            self.assertEqual(pixels[(128*512+384)*4:(128*512+384)*4+4], bytes((255, 255, 255, 255)))
            subprocess.run(["magick", "-size", "256x512", "xc:black", str(path)], check=True, timeout=30)
            with self.assertRaisesRegex(ValueError, "dimensions"):
                overrides.GodotTextures(directory).resolve("beam_wood_v2", "albedo")


class PacketOverrideTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            raise unittest.SkipTest("host C++ compiler required")
        cls.temp = tempfile.TemporaryDirectory(prefix="t3render-material-parser-")
        cls.binary = Path(cls.temp.name) / "parser"
        subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(HERE.parent / "src"),
                        "-x", "c++", "-o", str(cls.binary), "-"], input=HARNESS, text=True,
                       check=True, capture_output=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_split_preserves_triangle_order_geometry_and_untouched_meshes(self):
        scene = scene_fixture()
        baseline, _ = packet.create_packet(scene, Resolver())
        result, stats = packet.create_packet(scene, Resolver(), remaster=FakeRemaster(scene))
        self.assertEqual(subprocess.run([str(self.binary)], input=result, capture_output=True).returncode, 0)
        base, replacement = decoded_draws(baseline), decoded_draws(result)
        self.assertEqual([d["draw"] for d in stats["draws"]], [2, 2, 3, 4, 5])
        self.assertEqual([v for draw in base for v in draw[2]], [v for draw in replacement for v in draw[2]])
        self.assertEqual([(d["remaster_override"]["name"], d["remaster_override"]["channel"])
                          for d in stats["draws"] if "remaster_override" in d],
                         [("wood_floor_v2", "albedo"), ("stone_wall_v2", "albedo")])
        self.assertEqual(replacement[1][0], base[0][0])  # ceiling + outside keep the original
        self.assertEqual(replacement[-1][0], base[-1][0])  # bed mesh remains untouched

    def test_direct_normal_uses_override_only_for_selected_triangles(self):
        scene = scene_fixture()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        states[2].update(depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
        light = {"mode": "point_approximation", "position": [1, 2, 3], "inverse_radius_candidate": .1,
                 "offline_color": [1, .8, .6], "offline_energy": .5, "gain": 1}
        result, stats = packet.create_packet(scene, Resolver(), draw_states=states, direct_candidates={2: light},
                                             remaster=FakeRemaster(scene))
        self.assertEqual(result[:8], b"T3SCN09\0")
        self.assertEqual(subprocess.run([str(self.binary)], input=result, capture_output=True).returncode, 0)
        draws = decoded_draws(result)
        self.assertEqual([d[1] for d in draws[:2]], [7, 2])
        self.assertEqual(stats["draws"][0]["remaster_override"]["channel"], "normal")
        self.assertEqual(stats["draws"][0]["remaster_override"]["basis"], "inferred_uv")
        self.assertNotIn("remaster_override", stats["draws"][1])
        data = scene.to_dict()
        data["bsp_blocks"][0]["geometry"]["tangents"] = None
        with self.assertRaisesRegex(ValueError, "requires tangents"):
            packet.create_packet(packet.SceneSnapshot.from_dict(data), Resolver(), draw_states=states,
                                 direct_candidates={2: light}, remaster=FakeRemaster(scene))

    def test_override_normal_uses_uv0_and_keeps_legacy_basis_on_remaining_triangles(self):
        class NormalResolver(Resolver):
            def resolve_normal(self, name):
                return {"name": "normal", "bundle": "fixture.ibt", "format": "A8R8G8B8", "width": 2,
                        "height": 2, "mips": [bytes([128, 128, 255, 255]*4)], "usage": 1}
        scene = scene_fixture()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        states[2].update(depth_write=0, blend_enable=1, blend_src=1, blend_dst=1, color_mask=7)
        observed = {"status": "candidate", "stage": 0, "lod_bias": -1, "width": 2, "height": 2,
                    "levels": 1, "format": 21}
        light = {"mode": "point_approximation", "position": [1, 2, 3], "inverse_radius_candidate": .1,
                 "offline_color": [1, .8, .6], "offline_energy": .5, "gain": 1,
                 "program_hypothesis": "OmniLightNormal11_Light_Vertex", "normal_sampler_candidate": observed,
                 "shader_basis_candidate": {"uv_offset": [.25, -.5], "tangent_scale": -1}}
        raw, info = packet.create_packet(scene, NormalResolver(), draw_states=states, direct_candidates={2: light},
                                         remaster=FakeRemaster(scene))
        self.assertEqual(raw[:8], b"T3SCN10\0")
        self.assertEqual(subprocess.run([str(self.binary)], input=raw, capture_output=True).returncode, 0)
        draws = decoded_draws(raw)
        self.assertEqual([d[1] for d in draws[:2]], [7, 10])
        self.assertEqual(draws[0][2][0][3:], (0, 0))
        self.assertEqual(draws[1][2][0][3:], (.25, -.5))
        self.assertIn("original_normal_map_candidate", info["draws"][0])
        self.assertEqual(info["draws"][0]["sampler"]["status"], "remaster_preview")
        self.assertEqual(info["draws"][1]["sampler"]["mip_lod_bias"], -1)

    def test_ambient_and_fog_do_not_replace_textures(self):
        scene = scene_fixture()
        states = {s.draw_id: dict(packet.legacy_states.DEFAULT) for s in scene.submissions}
        _, info = packet.create_packet(scene, Resolver(), draw_states=states,
                                       ambient_candidates={2: [.1, .2, .3, 1]}, remaster=FakeRemaster(scene))
        self.assertNotIn("remaster_override", info["draws"][0])
        states[2].update(depth_write=0, blend_enable=1, blend_src=4, blend_dst=5, color_mask=7)
        fog = {"camera_position": [0, 0, 0], "start": 1, "inverse_range": .5, "offline_color": [.25, .5, .75]}
        _, info = packet.create_packet(scene, Resolver(), draw_states=states, fog_candidates={2: fog},
                                       remaster=FakeRemaster(scene))
        self.assertNotIn("remaster_override", info["draws"][0])

    def test_replacement_rejects_nonopaque_albedo(self):
        class TransparentResolver(Resolver):
            def resolve(self, name):
                result = super().resolve(name)
                result["mips"] = [bytes([255, 255, 255, 0] * 4)]
                return result
        scene = scene_fixture()
        with self.assertRaisesRegex(ValueError, "opaque source"):
            packet.create_packet(scene, TransparentResolver(), remaster=FakeRemaster(scene))


if __name__ == "__main__":
    unittest.main()
