"""Offline ANR package import: integrity, completeness, paths and preservation, without game/provider binaries."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace
import stat
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("anr_runtime", ROOT / "scripts/anr_runtime.py")
anr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(anr)

class ImportTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / "Gioco è 日本"
        self.game.mkdir()
        (self.game / "hookshot.exe").write_bytes(b"fixture, not executable")
        self.files = {name: b"fixture" for name in (
            "adapters/pack.nrx", "kernels/pack.nrx", "kernels-portable/pack.nrx",
            "programs/fg-lite-any-portable-srgb8/pack.nrx", "path_table.json", "size_table.json")}
        self.files["nr_fg.ini"] = b"; upstream fixture\npath = auto\nmodel_size = large\n"
        self.files["nvngx_dlssg.dll"] = b"fixture, not a provider"

    def package(self, files=None):
        package = self.root / "package.zip"
        with zipfile.ZipFile(package, "w") as archive:
            for name, content in (files or self.files).items():
                archive.writestr("Apex Neural Rendering FG/dlssg/" + name, content)
            archive.writestr("game/dxgi.dll", b"must not be installed")
            archive.writestr("install.cmd", b"must not run")
        return package

    def install(self, package, *, portable=False):
        # Trust only this generated fixture for unit tests. The CLI has no integrity bypass.
        with patch.object(anr, "PACKAGE_SHA256", {anr.digest(package): "fixture"}), \
             patch.object(anr, "PROVIDER_SHA256", __import__("hashlib").sha256(self.files["nvngx_dlssg.dll"]).hexdigest()):
            return anr.install(package, self.game, portable=portable)

    def test_complete_package_and_unicode_game_path(self):
        target = self.install(self.package())
        self.assertEqual(target, self.game / "anr-fg")
        self.assertEqual((target / "nr_fg.ini").read_bytes(), self.files["nr_fg.ini"])
        self.assertFalse((target / "nr_fg.ini.upstream").exists())
        self.assertFalse((self.game / "dxgi.dll").exists())
        self.assertFalse((self.game / "install.cmd").exists())
        self.assertEqual(json.loads((target / "hookshot_anr.json").read_text())["kernel_path"], "upstream")

    def test_portable_path_is_an_explicit_choice_with_backup(self):
        target = self.install(self.package(), portable=True)
        self.assertIn("path = portable", (target / "nr_fg.ini").read_text())
        self.assertEqual((target / "nr_fg.ini.upstream").read_bytes(), self.files["nr_fg.ini"])
        self.assertEqual(json.loads((target / "hookshot_anr.json").read_text())["kernel_path"], "portable")

    def test_unknown_package_is_refused_before_installation(self):
        with self.assertRaisesRegex(ValueError, "unmodified"):
            anr.install(self.package(), self.game)
        self.assertFalse((self.game / "anr-fg").exists())

    def test_existing_install_is_preserved(self):
        package = self.package()
        target = self.install(package)
        (target / "nr_fg.ini").write_text("user settings")
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.install(package)
        self.assertEqual((target / "nr_fg.ini").read_text(), "user settings")

    def test_missing_model_never_publishes_partial_install(self):
        del self.files["programs/fg-lite-any-portable-srgb8/pack.nrx"]
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            self.install(self.package())
        self.assertFalse((self.game / "anr-fg").exists())
        self.assertEqual([p.name for p in self.game.iterdir()], ["hookshot.exe"])

    def test_traversal_and_windows_paths_are_rejected(self):
        for name in ("../escape", "C:/escape", "..\\escape"):
            with self.subTest(name=name):
                files = dict(self.files, **{name: b"bad"})
                with self.assertRaisesRegex(ValueError, "Unsafe"):
                    self.install(self.package(files))
                self.assertFalse((self.game / "anr-fg").exists())

    def test_case_insensitive_duplicates_are_rejected(self):
        files = dict(self.files, **{"NR_FG.INI": b"duplicate"})
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.install(self.package(files))

    def test_windows_reparse_parent_is_refused_without_creating_files(self):
        package = self.package()
        info = SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400)
        with patch.object(Path, "lstat", return_value=info):
            with self.assertRaisesRegex(ValueError, "must not be a link"):
                self.install(package)
        self.assertFalse((self.game / "anr-fg").exists())

if __name__ == "__main__":
    unittest.main()
