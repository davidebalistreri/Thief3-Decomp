import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
from tds_save_sandbox import IsolatedSavePrefix, SandboxError


class SaveSandboxTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="t3-save-sandbox-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bottle = self.root / "bottle"
        self.system = self.bottle / "drive_c/game/System"
        self.system.mkdir(parents=True)
        self.saves = self.system.parent / "save/SaveGames"
        self.saves.mkdir(parents=True)
        (self.saves / "SaveIndex.ion").write_bytes(b"original save")
        (self.bottle / "system.reg").write_text(
            '[Software\\\\Wow6432Node\\\\Ion Storm\\\\Thief - Deadly Shadows] 1\n'
            '"SaveGamePath"="C:\\\\game\\\\save"\n', encoding="utf-8")
        (self.bottle / "user.reg").write_text(
            '[Environment] 1\n"TEMP"="C:\\\\old-temp"\n"TMP"="C:\\\\old-temp"\n'
            '[Software\\\\Wine\\\\Credential Manager\\\\token] 1\n"secret"="do not copy"\n'
            '[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run] 1\n"Steam"="autostart"\n'
            '[Software\\\\Wine\\\\Mac Driver] 1\n"RetinaMode"="n"\n', encoding="utf-8")
        for name in ("userdef.reg", ".update-timestamp"):
            (self.bottle / name).write_text("fixture\n", encoding="utf-8")

    def test_copy_is_writable_and_source_is_preserved(self):
        sandbox = IsolatedSavePrefix.prepare(self.bottle, self.system, self.root)
        try:
            copy = sandbox.root / "save/SaveGames/SaveIndex.ion"
            self.assertEqual(copy.read_bytes(), b"original save")
            copy.write_bytes(b"changed only in sandbox")
            self.assertEqual((self.saves / "SaveIndex.ion").read_bytes(), b"original save")
            system = (sandbox.prefix / "system.reg").read_text(encoding="utf-8")
            user = (sandbox.prefix / "user.reg").read_text(encoding="utf-8")
            self.assertIn('"SaveGamePath"="Z:', system)
            self.assertNotIn('do not copy', user)
            self.assertNotIn('autostart', user)
            self.assertIn('RetinaMode', user)
            self.assertNotIn('old-temp', user)
            self.assertIn('wine-temp', user)
            self.assertIn('(deny network-outbound ', sandbox.profile)
            self.assertIn('(require-not (remote ip "localhost:*"))', sandbox.profile)
            self.assertIn(str(self.saves.parent.resolve()), sandbox.profile)
            self.assertNotIn('(subpath "' + str(self.bottle.resolve()) + '")', sandbox.profile)
            self.assertEqual((sandbox.prefix / "drive_c").resolve(), (self.bottle / "drive_c").resolve())
            env = sandbox.environment({"HTTPS_PROXY": "http://localhost:1234", "OTHER": "value"})
            self.assertNotIn("HTTPS_PROXY", env)
            self.assertEqual(env["OTHER"], "value")
            self.assertEqual(env["TEMP"], env["TMP"])
            self.assertTrue(env["TEMP"].endswith("\\wine-temp"))
        finally:
            sandbox.close(self.root / "engine", {})
        self.assertTrue(sandbox.removed)
        self.assertTrue(sandbox.saves_unchanged)
        self.assertFalse(sandbox.root.exists())

    def test_refuses_save_links(self):
        source = self.saves / "SaveIndex.ion"
        (self.saves / "alias").symlink_to(source)
        with self.assertRaisesRegex(SandboxError, "link"):
            IsolatedSavePrefix.prepare(self.bottle, self.system, self.root)
        (self.saves / "alias").unlink()
        os.link(source, self.saves / "alias")
        with self.assertRaisesRegex(SandboxError, "link"):
            IsolatedSavePrefix.prepare(self.bottle, self.system, self.root)

    def test_retains_prefix_when_server_shutdown_fails(self):
        sandbox = IsolatedSavePrefix.prepare(self.bottle, self.system, self.root)
        sandbox.started = True
        env = {"DYLD_FALLBACK_LIBRARY_PATH": "lib", "DYLD_FALLBACK_FRAMEWORK_PATH": "framework"}
        with mock.patch("tds_save_sandbox.subprocess.run", return_value=mock.Mock(returncode=1)):
            with self.assertRaisesRegex(SandboxError, "retained"):
                sandbox.close(self.root / "engine", env)
        self.assertTrue(sandbox.root.is_dir())
        self.assertFalse(sandbox.removed)
        shutil.rmtree(sandbox.root)

    def test_already_stopped_server_is_drained_before_removal(self):
        sandbox = IsolatedSavePrefix.prepare(self.bottle, self.system, self.root)
        sandbox.started = True
        env = {"DYLD_FALLBACK_LIBRARY_PATH": "lib", "DYLD_FALLBACK_FRAMEWORK_PATH": "framework"}
        with mock.patch("tds_save_sandbox.subprocess.run",
                        side_effect=[mock.Mock(returncode=1), mock.Mock(returncode=0)]) as run:
            sandbox.close(self.root / "engine", env)
        self.assertEqual(run.call_count, 2)
        self.assertTrue(sandbox.removed)
        self.assertFalse(sandbox.root.exists())

    def test_source_change_is_reported_without_restoring_user_data(self):
        sandbox = IsolatedSavePrefix.prepare(self.bottle, self.system, self.root)
        source = self.saves / "SaveIndex.ion"
        source.write_bytes(b"external change")
        with self.assertRaisesRegex(SandboxError, "original save files changed"):
            sandbox.close(self.root / "engine", {})
        self.assertFalse(sandbox.saves_unchanged)
        self.assertTrue(sandbox.removed)
        self.assertEqual(source.read_bytes(), b"external change")


if __name__ == "__main__":
    unittest.main()
