"""Host checks and Windows x86 SDK signature check for the incomplete facade."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

TESTS = Path(__file__).resolve().parent


class FactoryFacadeTests(unittest.TestCase):
    def test_host_factory_contract(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("a host C++ compiler is required")
        with tempfile.TemporaryDirectory(prefix="t3render-factory-facade-") as directory:
            binary = Path(directory) / "factory-test"
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                       "-I", str(TESTS / "factory_facade_fakes"), str(TESTS / "d3d8_factory_facade_test.cpp"),
                       "-o", str(binary)]
            result = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_i686_sdk_signatures(self):
        compiler = shutil.which("i686-w64-mingw32-g++")
        if not compiler:
            self.skipTest("the i686 MinGW-w64 SDK is required")
        with tempfile.TemporaryDirectory(prefix="t3render-factory-x86-") as directory:
            source = Path(directory) / "signature.cpp"
            source.write_text('#include <windows.h>\n'
                              '#ifdef max\n#undef max\n#endif\n#define max(a,b) broken_macro(a,b)\n'
                              '#include "d3d8_factory_facade.hpp"\n#include <type_traits>\n'
                              'static_assert(!std::is_abstract_v<t3render::fixture::FactoryFacade>);\n',
                              encoding="ascii")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I", str(TESTS),
                       "-c", str(source), "-o", str(Path(directory) / "signature.o")]
            result = subprocess.run(command, text=True, capture_output=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
