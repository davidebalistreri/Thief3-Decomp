"""Fake COM and Win32 SDK checks for the isolated resource facade."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


class ResourceFacadeTests(unittest.TestCase):
    def test_host_resource_graph_and_lifetime(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-resource-facade-") as directory:
            binary = Path(directory) / "resource-test"
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I", str(TESTS / "resource_facade_fakes"), str(TESTS / "d3d8_resource_facade_test.cpp"),
                       "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_i686_sdk_signatures(self):
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler:
            self.skipTest("the i686 MinGW-w64 SDK is required")
        with tempfile.TemporaryDirectory(prefix="t3render-resource-sdk-") as directory:
            source = Path(directory) / "signature.cpp"
            source.write_text('#include <windows.h>\n#undef max\n'
                              '#define max(a,b) (((a) > (b)) ? (a) : (b))\n'
                              '#include "d3d8_resource_facade.hpp"\n#include <type_traits>\n'
                              'static_assert(!std::is_abstract_v<t3render::resource_fixture::Wrapper<IDirect3DSurface8>>);\n'
                              'static_assert(!std::is_abstract_v<t3render::resource_fixture::Wrapper<IDirect3DTexture8>>);\n'
                              'static_assert(!std::is_abstract_v<t3render::resource_fixture::Wrapper<IDirect3DCubeTexture8>>);\n',
                              encoding="ascii")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(TESTS),
                                     "-c", str(source), "-o", str(Path(directory) / "signature.o")],
                                    text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
