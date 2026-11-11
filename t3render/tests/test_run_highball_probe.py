"""Exercise the Highball launcher with a fake Wine process, never Wine itself."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "run_highball_probe.sh"

FAKE_WINE = r'''#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *value(const char *name) {
    const char *v = getenv(name);
    return v ? v : "unset";
}
int main(int argc, char **argv) {
    if (argc != 4) return 98;
    printf("prefix=%s\nserver=%s\ndllpath=%s\n", value("WINEPREFIX"), value("WINESERVER"), value("WINEDLLPATH_PREPEND"));
    printf("libraries=%s\nframeworks=%s\noverrides=%s\ndebug=%s\n", value("DYLD_FALLBACK_LIBRARY_PATH"),
           value("DYLD_FALLBACK_FRAMEWORK_PATH"), value("WINEDLLOVERRIDES"), value("WINEDEBUG"));
    printf("inherited=%s,%s,%s\n", value("WINEDLLPATH"), value("DYLD_LIBRARY_PATH"), value("DYLD_FRAMEWORK_PATH"));
    printf("argc=%d\narg1=%s\narg2=%s\narg3=%s\n", argc - 1, argv[1], argv[2], argv[3]);
    if (!getenv("FAKE_IMAGE") || strcmp(getenv("FAKE_IMAGE"), "0")) {
        if (strncmp(argv[3], "Z:", 2)) return 97;
        char *path = strdup(argv[3] + 2);
        if (!path) return 96;
        for (char *p = path; *p; ++p) if (*p == '\\') *p = '/';
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        free(path);
        if (fd < 0) return 99;
        const char content[] = "fake image\n";
        if (write(fd, content, sizeof(content)-1) != sizeof(content)-1) { close(fd); return 95; }
        close(fd);
    }
    return getenv("FAKE_STATUS") ? atoi(getenv("FAKE_STATUS")) : 0;
}
'''


class HighballProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.compiler = shutil.which("i686-w64-mingw32-gcc")
        host_cc = shutil.which("clang") or shutil.which("cc")
        if not cls.compiler or not host_cc or not shutil.which("file"):
            raise unittest.SkipTest("x86 MinGW, host C compiler and file are required")
        cls.artifacts = tempfile.TemporaryDirectory(prefix="t3render-probe-test-bin-")
        cls.probe = Path(cls.artifacts.name) / "test.exe"
        subprocess.run([cls.compiler, "-x", "c", "-o", str(cls.probe), "-"], input="int main(void){return 0;}\n",
                       text=True, capture_output=True, check=True, timeout=60)
        cls.fake_wine = Path(cls.artifacts.name) / "fake-wine"
        subprocess.run([host_cc, "-x", "c", "-o", str(cls.fake_wine), "-"], input=FAKE_WINE,
                       text=True, capture_output=True, check=True, timeout=60)

    @classmethod
    def tearDownClass(cls):
        cls.artifacts.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3render-highball-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.engine = self.root / "test engine"
        self.bottle = self.root / "test bottle"
        self.bin = self.engine / "engine" / "bin"
        self.bin.mkdir(parents=True)
        (self.engine / "frameworks").mkdir()
        (self.bottle / "dosdevices").mkdir(parents=True)
        (self.bottle / "dosdevices" / "z:").symlink_to("/")
        (self.bottle / "bottle.json").write_text('{"marker":true}\n')
        shutil.copyfile(self.fake_wine, self.bin / "wine")
        (self.bin / "wine").chmod(0o755)
        (self.bin / "wineserver").write_text("#!/bin/sh\nexit 0\n")
        (self.bin / "wineserver").chmod(0o755)
        self.prefix = self.root / "capture name"

    def run_probe(self, *, prefix=None, engine=None, bottle=None, probe=None, extra_env=None):
        env = os.environ.copy()
        env.update({"WINEDLLPATH": "poison/dxmt", "DYLD_LIBRARY_PATH": "poison/dxmt",
                    "DYLD_FRAMEWORK_PATH": "poison/dxmt", "WINEDLLPATH_PREPEND": "poison/dxmt"})
        env.update(extra_env or {})
        return subprocess.run([str(SCRIPT), str(engine or self.engine), str(bottle or self.bottle),
                               str(probe or self.probe), str(prefix or self.prefix)], env=env,
                              text=True, capture_output=True, timeout=20)

    def test_child_environment_and_windows_output_path(self):
        result = self.run_probe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((self.root / "capture name.ppm").read_text(), "fake image\n")
        log = (self.root / "capture name.log").read_text()
        dll = (f"{self.bottle}/.highball-dlss/wine:{self.engine}/renderers/d3dmetal-tsshim/wine:"
               f"{self.engine}/renderers/d3dmetal/wine")
        framework = f"{self.engine}/frameworks"
        self.assertIn(f"prefix={self.bottle}\nserver={self.bin}/wineserver\ndllpath={dll}\n", log)
        self.assertIn(f"libraries={framework}:{framework}/GStreamer.framework/Versions/1.0/lib\n", log)
        self.assertIn(f"frameworks={framework}\n", log)
        self.assertIn("overrides=nvapi64,nvngx=n,b;winemenubuilder.exe=d\ndebug=-all,+loaddll\n", log)
        self.assertIn("inherited=unset,unset,unset\n", log)
        self.assertIn(f"argc=3\narg1={self.probe}\narg2=--output\n", log)
        expected_path = "Z:" + str(self.root / "capture name.ppm").replace("/", "\\")
        self.assertIn(f"arg3={expected_path}\n", log)
        self.assertIn("wine_exit_status=0\n", log)
        self.assertNotIn("dxmt", log.lower())
        self.assertEqual((self.bottle / "bottle.json").read_text(), '{"marker":true}\n')

    def test_return_status_and_complete_log_on_failure(self):
        result = self.run_probe(extra_env={"FAKE_STATUS": "37", "FAKE_IMAGE": "0"})
        self.assertEqual(result.returncode, 37)
        self.assertFalse((self.root / "capture name.ppm").exists())
        log = (self.root / "capture name.log").read_text()
        self.assertIn("arg2=--output\n", log)
        self.assertIn("wine_exit_status=37\n", log)

    def test_existing_log_or_image_is_never_overwritten(self):
        for suffix in (".log", ".ppm"):
            with self.subTest(suffix=suffix):
                output = self.root / ("capture name" + suffix)
                output.write_text("keep\n")
                result = self.run_probe()
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_text(), "keep\n")
                other = self.root / ("capture name" + (".ppm" if suffix == ".log" else ".log"))
                self.assertFalse(other.exists())
                output.unlink()

    def test_dangling_output_symlink_is_rejected(self):
        (self.root / "capture name.ppm").symlink_to(self.root / "missing")
        result = self.run_probe()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "capture name.log").exists())
        self.assertFalse((self.root / "missing").exists())

    def test_invalid_paths_are_rejected_before_output(self):
        for kwargs in ({"prefix": Path("relative")}, {"engine": self.root / "missing"},
                       {"bottle": self.root / "missing"}, {"probe": self.root / "missing.exe"},
                       {"prefix": self.root / "bad:name"}):
            with self.subTest(kwargs=kwargs):
                result = self.run_probe(**kwargs)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse((self.root / "capture name.log").exists())

    def test_bottle_must_map_z_to_root(self):
        link = self.bottle / "dosdevices" / "z:"
        link.unlink()
        link.symlink_to(self.root)
        result = self.run_probe()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "capture name.log").exists())

    def test_x64_binary_is_rejected(self):
        compiler = shutil.which("x86_64-w64-mingw32-gcc")
        if not compiler:
            self.skipTest("x64 MinGW compiler is unavailable")
        binary = self.root / "x64.exe"
        subprocess.run([compiler, "-x", "c", "-o", str(binary), "-"], input="int main(void){return 0;}\n",
                       text=True, capture_output=True, check=True, timeout=60)
        result = self.run_probe(probe=binary)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / "capture name.log").exists())


if __name__ == "__main__":
    unittest.main()
