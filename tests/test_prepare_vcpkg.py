import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "prepare_vcpkg", Path(__file__).resolve().parents[1] / "tools" / "prepare_vcpkg.py"
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class OverlayTests(unittest.TestCase):
    def test_overlay_is_repeatable_and_does_not_modify_pinned_port(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "vcpkg" / "ports" / "ffmpeg"
            source.mkdir(parents=True)
            (source / "vcpkg.json").write_text(json.dumps({"version": "9.0.2"}))
            original = "vcpkg_from_github(\n    PATCHES\n        upstream.patch\n)\n"
            (source / "portfile.cmake").write_text(original)
            (source / "upstream.patch").write_text("upstream patch")
            for _ in range(2):
                target = module.prepare(root / "vcpkg", root / "overlay")
                self.assertEqual((source / "portfile.cmake").read_text(), original)
                self.assertEqual((target / "portfile.cmake").read_text().count("nco-amf-timeout.patch"), 1)
                self.assertEqual((target / "upstream.patch").read_text(), "upstream patch")
                self.assertIn("AVERROR(ETIMEDOUT)", (target / "nco-amf-timeout.patch").read_text())
            with self.assertRaises(ValueError):
                module.prepare(root / "vcpkg", source.parent)
            self.assertEqual((source / "portfile.cmake").read_text(), original)
            (source / "vcpkg.json").write_text(json.dumps({"version": "10.0"}))
            with self.assertRaises(ValueError):
                module.prepare(root / "vcpkg", root / "overlay")


if __name__ == "__main__":
    unittest.main()
