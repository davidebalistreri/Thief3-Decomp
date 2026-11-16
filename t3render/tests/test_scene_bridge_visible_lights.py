import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class VisibleLightsTests(unittest.TestCase):
    def test_bounded_visible_light_collection(self):
        source = Path(__file__).resolve().parent / "scene_bridge_visible_lights_test.cpp"
        with tempfile.TemporaryDirectory(prefix="t3scene-visible-lights-") as directory:
            output = Path(directory) / "visible-lights-test"
            root = source.parent.parent
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-pedantic", "-pthread", str(source), str(root / "ir/scene_live.cpp"),
                            str(root / "ir/scene_live_window.cpp"), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
