import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SceneBridgeLightCandidateTests(unittest.TestCase):
    def test_bounded_actor_light_sample(self):
        source = Path(__file__).resolve().parent / "scene_bridge_light_candidate_test.cpp"
        with tempfile.TemporaryDirectory(prefix="t3scene-light-") as directory:
            output = Path(directory) / "light-test"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-pedantic",
                            str(source), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
