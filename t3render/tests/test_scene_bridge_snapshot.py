import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class SceneBridgeSnapshotTests(unittest.TestCase):
    def test_bounded_snapshot_and_actor_membership(self):
        source = Path(__file__).resolve().parent / "scene_bridge_snapshot_test.cpp"
        with tempfile.TemporaryDirectory(prefix="t3scene-snapshot-") as directory:
            output = Path(directory) / "snapshot-test"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-pedantic",
                            str(source), "-o", str(output)], check=True)
            subprocess.run([str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
