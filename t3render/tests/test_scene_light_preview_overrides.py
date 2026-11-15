"""Light preview calibration is semantic, explicit and bounded."""
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import scene_light_preview_overrides as overrides


class LightPreviewOverridesTests(unittest.TestCase):
    def document(self, entries):
        return json.dumps({"schema": "t3.scene.light-preview-overrides", "version": 1, "lights": entries})

    def test_semantic_id_applies_to_all_sections_without_mutating_offline_colors(self):
        light = {"offline_light_id": "light:map:fingerprint:lamp", "mode": "point_approximation",
                 "offline_color": [1, .8, .6]}
        candidates = {17: light, 31: dict(light), 18: {"offline_light_id": "light:map:other", "mode": "skipped_spot"}}
        document = self.document([{"light_id": light["offline_light_id"], "rgb_multiplier": [2, 1.8, 1.2]}])
        result = overrides.apply(candidates, overrides.parse(document))
        self.assertEqual(result[17]["preview_rgb_multiplier"], [2, 1.8, 1.2])
        self.assertEqual(result[31]["preview_rgb_multiplier"], [2, 1.8, 1.2])
        self.assertNotIn("preview_rgb_multiplier", result[18])
        self.assertNotIn("preview_rgb_multiplier", light)
        self.assertEqual(result[17]["offline_color"], [1, .8, .6])
        with_falloff = self.document([{"light_id": light["offline_light_id"], "rgb_multiplier": [1, 1, 1],
                                       "falloff_mix": .5}])
        self.assertEqual(overrides.apply(candidates, overrides.parse(with_falloff))[17]["preview_falloff_mix"], .5)

    def test_rejects_ambiguous_out_of_range_or_unmatched_overrides(self):
        entry = {"light_id": "light:map:lamp", "rgb_multiplier": [1, 1, 1]}
        for entries in ([], [entry, entry], [dict(entry, light_id="0x1234")],
                        [dict(entry, rgb_multiplier=[1, True, 1])], [dict(entry, rgb_multiplier=[1, 5, 1])],
                        [dict(entry, rgb_multiplier=[1, float("nan"), 1])], [dict(entry, extra=1)]):
            with self.subTest(entries=entries), self.assertRaises(ValueError):
                overrides.parse(self.document(entries))
        for mix in (-1, 1.01, True, float("nan")):
            with self.subTest(mix=mix), self.assertRaises(ValueError):
                overrides.parse(self.document([dict(entry, falloff_mix=mix)]))
        with self.assertRaises(ValueError):
            overrides.parse('{"schema":"t3.scene.light-preview-overrides","version":1,"version":1,"lights":[]}')
        parsed = overrides.parse(self.document([entry]))
        for candidates in ({}, {3: {"offline_light_id": "light:map:other", "mode": "point_approximation"}},
                           {3: {"offline_light_id": "light:map:lamp", "mode": "skipped_spot"}}):
            with self.subTest(candidates=candidates), self.assertRaises(ValueError):
                overrides.apply(candidates, parsed)


if __name__ == "__main__":
    unittest.main()
