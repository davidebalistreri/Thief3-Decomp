"""Temporary Wine prefix with a writable copy of protected save data.

This is an experimental test environment, not a general-purpose Wine bottle.
Loading and playing the real game in it have not yet been verified.
"""

import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


class SandboxError(RuntimeError):
    pass


def _snapshot(root):
    result, size = {}, 0
    if root.is_symlink() or not root.is_dir():
        raise SandboxError("save source must be an existing real directory")
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = Path(directory) / name
            info = path.lstat()
            if stat.S_ISDIR(info.st_mode):
                continue
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise SandboxError("save source contains a link or non-regular file")
            size += info.st_size
            if size > 256 * 1024 * 1024 or len(result) >= 10000:
                raise SandboxError("save source exceeds the sandbox copy limit")
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            result[str(path.relative_to(root))] = digest.hexdigest()
    return result


def _registry_section(line):
    return line.split("]", 1)[0].lower() + "]" if line.startswith("[") and "]" in line else None


def _copy_system_registry(source, target, save_path):
    section = r"[software\\wow6432node\\ion storm\\thief - deadly shadows]"
    inside, replacements, result = False, 0, []
    for line in source.read_text(encoding="utf-8").splitlines(keepends=True):
        header = _registry_section(line)
        if header is not None:
            inside = header == section
        if inside and line.startswith('"SaveGamePath"='):
            line = '"SaveGamePath"=' + json.dumps(save_path, ensure_ascii=True) + "\n"
            replacements += 1
        result.append(line)
    if replacements != 1:
        raise SandboxError("game save registry key is missing or ambiguous")
    target.write_text("".join(result), encoding="utf-8")


def _copy_user_registry(source, target, temp_path):
    run = {r"[software\\microsoft\\windows\\currentversion\\run]",
           r"[software\\microsoft\\windows\\currentversion\\runonce]"}
    omit, section, replacements, result = False, None, set(), []
    for line in source.read_text(encoding="utf-8").splitlines(keepends=True):
        header = _registry_section(line)
        if header is not None:
            section = header
            omit = header.startswith(r"[software\\wine\\credential manager") or header in run
        if section == "[environment]":
            for name in ("TEMP", "TMP"):
                if line.startswith(f'"{name}"='):
                    if name in replacements:
                        raise SandboxError("Wine temporary directory registry values are ambiguous")
                    line = f'"{name}"=' + json.dumps(temp_path, ensure_ascii=True) + "\n"
                    replacements.add(name)
        if not omit:
            result.append(line)
    if replacements != {"TEMP", "TMP"}:
        raise SandboxError("Wine temporary directory registry values are missing")
    target.write_text("".join(result), encoding="utf-8")


def _windows_path(path):
    return "Z:" + str(path).replace("/", "\\")


class IsolatedSavePrefix:
    def __init__(self, root, bottle, original, baseline):
        self.root, self.bottle, self.original, self.baseline = root, bottle, original, baseline
        self.prefix = root / "prefix"
        self.wine_temp = root / "wine-temp"
        info = root.lstat()
        self.identity = (info.st_dev, info.st_ino)
        self.started = False
        self.removed = False
        self.saves_unchanged = None
        protected = [original.parent.resolve(), original.resolve(), (Path.home() / "Documents").resolve()]
        users = bottle / "drive_c/users"
        if users.is_dir():
            for user in users.iterdir():
                if user.is_dir():
                    protected.extend(((user / "Documents/Thief - Deadly Shadows/SaveGames").resolve(),
                                      (user / "AppData/Local/VirtualStore").resolve(),
                                      (user / "AppData/Local/Temp").resolve()))
        steam = original.parent.parent.parent.parent.parent
        userdata = steam / "userdata"
        if userdata.is_dir():
            protected.extend((user / "6980").resolve() for user in userdata.iterdir() if user.is_dir())
        paths = "".join("(subpath " + json.dumps(str(path), ensure_ascii=False) + ")"
                        for path in dict.fromkeys(protected))
        # Wine uses Unix sockets and Steam uses loopback TCP/UDP. Block external
        # IP connections while permitting those local transports.
        self.profile = ('(version 1)(allow default)(deny network-outbound '
                        '(require-all (remote tcp "*:*") (require-not (remote ip "localhost:*"))) '
                        '(require-all (remote udp "*:*") (require-not (remote ip "localhost:*"))))'
                        '(deny file-write* ' + paths + ')')

    @classmethod
    def prepare(cls, bottle, system, parent=None):
        bottle, system = Path(bottle).resolve(strict=True), Path(system).resolve(strict=True)
        original = system.parent / "save" / "SaveGames"
        baseline = _snapshot(original)
        root = Path(tempfile.mkdtemp(prefix="t3render-isolated-", dir=parent or "/private/tmp"))
        os.chmod(root, 0o700)
        try:
            prefix, scratch = root / "prefix", root / "save"
            prefix.mkdir(mode=0o700)
            (prefix / "dosdevices").mkdir()
            (prefix / "drive_c").symlink_to(bottle / "drive_c", target_is_directory=True)
            (prefix / "dosdevices" / "c:").symlink_to("../drive_c")
            (prefix / "dosdevices" / "z:").symlink_to("/")
            scratch.mkdir(mode=0o700)
            (root / "wine-temp").mkdir(mode=0o700)
            shutil.copytree(original, scratch / "SaveGames")
            if _snapshot(scratch / "SaveGames") != baseline or _snapshot(original) != baseline:
                raise SandboxError("save source changed while copying")
            _copy_system_registry(bottle / "system.reg", prefix / "system.reg", _windows_path(scratch))
            _copy_user_registry(bottle / "user.reg", prefix / "user.reg", _windows_path(root / "wine-temp"))
            for name in ("userdef.reg", ".update-timestamp"):
                shutil.copyfile(bottle / name, prefix / name)
            for name in ("system.reg", "user.reg", "userdef.reg"):
                os.chmod(prefix / name, 0o600)
            return cls(root, bottle, original, baseline)
        except BaseException:
            shutil.rmtree(root)
            raise

    def command(self, command, env):
        paths = [f"{name}={env[name]}" for name in
                 ("DYLD_FALLBACK_LIBRARY_PATH", "DYLD_FALLBACK_FRAMEWORK_PATH")]
        return ["/usr/bin/sandbox-exec", "-p", self.profile, "/usr/bin/env", *paths, *command]

    def environment(self, env):
        blocked = {"http_proxy", "https_proxy", "all_proxy", "no_proxy", "temp", "tmp"}
        return {**{key: value for key, value in env.items() if key.casefold() not in blocked},
                "WINEPREFIX": str(self.prefix), "TEMP": _windows_path(self.wine_temp),
                "TMP": _windows_path(self.wine_temp)}

    def close(self, engine, env):
        if self.started:
            for option in ("-k", "-w"):
                try:
                    result = subprocess.run(self.command([str(engine / "engine/bin/wineserver"), option], env),
                                            env=self.environment(env), cwd=self.root, stdout=subprocess.DEVNULL,
                                            stderr=subprocess.DEVNULL, timeout=10, check=False)
                except (OSError, subprocess.SubprocessError) as exc:
                    raise SandboxError(f"sandbox server cleanup failed; retained {self.root}: {exc}") from exc
                # `-k` returns 1 when the server has already exited. The
                # following `-w` must still confirm that the prefix is drained.
                if option == "-w" and result.returncode:
                    raise SandboxError(f"sandbox server cleanup failed; retained {self.root}")
        info = self.root.lstat()
        if self.root.is_symlink() or (info.st_dev, info.st_ino) != self.identity:
            raise SandboxError(f"sandbox directory changed; retained {self.root}")
        try:
            unchanged = _snapshot(self.original) == self.baseline
            self.saves_unchanged = unchanged
        finally:
            shutil.rmtree(self.root)
            self.removed = True
        if not unchanged:
            raise SandboxError("original save files changed during the sandbox run")
