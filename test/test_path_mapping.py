"""Host CLI path conversions; these do not run a Windows fixture."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


WIBO = str(Path(sys.argv.pop(1)).resolve())


class PathMappingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve() / "drive c"
        self.root.mkdir()

    def convert(self, direction, value, root=None):
        env = os.environ.copy()
        env.pop("WIBO_DRIVE_C", None)
        if root is not None:
            env["WIBO_DRIVE_C"] = str(root)
        return subprocess.check_output(
            [WIBO, "path", direction, str(value)], env=env, text=True
        ).rstrip("\n")

    def test_default_mapping(self):
        for setting in (None, ""):
            with self.subTest(setting=setting):
                host = str(self.root / "output.pdb")
                guest = "Z:" + host.replace("/", "\\")
                self.assertEqual(self.convert("--windows", host, setting), guest)
                self.assertEqual(self.convert("--unix", guest, setting), host)
                self.assertEqual(self.convert("--unix", "C:" + guest[2:], setting), host)

    def test_mapped_round_trip(self):
        host = self.root / "WCB" / "wc2" / "Debug" / "wc2.pdb"
        guest = r"C:\WCB\wc2\Debug\wc2.pdb"
        self.assertEqual(self.convert("--windows", host, self.root), guest)
        self.assertEqual(self.convert("--unix", guest, self.root), str(host))
        self.assertEqual(self.convert("--unix", guest.replace("C:", "c:"), self.root), str(host))
        self.assertEqual(self.convert("--unix", "\\\\?\\" + guest, self.root), str(host))

    def test_root_and_parent_normalization(self):
        self.assertEqual(self.convert("--windows", self.root, self.root), "C:\\")
        for guest in ("C:\\", r"C:\..\..", r"C:\folder\.."):
            with self.subTest(guest=guest):
                self.assertEqual(Path(self.convert("--unix", guest, self.root)), self.root)
        self.assertEqual(self.convert("--unix", r"C:\..\new.txt", self.root), str(self.root / "new.txt"))

    def test_outside_tree_and_relative_paths(self):
        outside = self.root.with_name(self.root.name + "-sibling") / "output.pdb"
        self.assertEqual(
            self.convert("--windows", outside, self.root), "Z:" + str(outside).replace("/", "\\")
        )
        host = str(self.root / "output.pdb")
        self.assertEqual(self.convert("--unix", "Z:" + host.replace("/", "\\"), self.root), host)
        self.assertEqual(self.convert("--windows", "relative/output.pdb", self.root), r"relative\output.pdb")
        self.assertEqual(Path(self.convert("--unix", r"relative\output.pdb", self.root)), Path("relative/output.pdb"))

    def test_case_insensitive_lookup(self):
        directory = self.root / "MixedCase"
        directory.mkdir()
        target = directory / "Output.PDB"
        target.touch()
        self.assertTrue(os.path.samefile(self.convert("--unix", r"c:\mixedcase\output.pdb", self.root), target))


if __name__ == "__main__":
    unittest.main()
