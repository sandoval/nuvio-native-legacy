#!/usr/bin/env python3
"""Package audit fixtures: native byte accounting and forbidden model payloads."""
import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("check_autosync_package", ROOT / "tools/check-autosync-package.py")
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


def elf(size=128):
    header = bytearray(max(size, 52))
    header[:4] = b"\x7fELF"
    header[4] = 2
    header[5] = 1
    header[18:20] = (62).to_bytes(2, "little")
    return bytes(header)


class PackageAuditFixtures(unittest.TestCase):
    def test_counts_every_elf_and_flags_known_model_forms(self):
        with tempfile.TemporaryDirectory(prefix="nv-package-audit-") as temporary:
            root = pathlib.Path(temporary)
            (root / "app-with-unusual-name").write_bytes(elf(128))
            (root / "libsomething.so.7").write_bytes(elf(256))
            (root / "model.onnx").write_bytes(b"weights")
            (root / "converted.ort").write_bytes(b"weights")
            (root / "private.a").write_bytes(b"archive")
            (root / "format.bin").write_bytes(b"xxxxORTMmodel")
            (root / "subtitle-autosync").mkdir()
            (root / "subtitle-autosync" / "weights.bin").write_bytes(b"weights")
            report = AUDIT.inspect(root)
            self.assertEqual(report["native_bytes"], 384)
            self.assertEqual(set(report["files"]), {"app-with-unusual-name", "libsomething.so.7"})
            self.assertEqual(
                report["forbidden_artifacts"],
                ["converted.ort", "format.bin", "model.onnx", "private.a",
                 "subtitle-autosync/weights.bin"],
            )

    def test_main_enforces_unpacked_five_megabyte_delta(self):
        with tempfile.TemporaryDirectory(prefix="nv-package-size-") as temporary:
            base = pathlib.Path(temporary) / "base"
            package = pathlib.Path(temporary) / "package"
            base.mkdir(); package.mkdir()
            (base / "app").write_bytes(elf())
            (package / "app").write_bytes(elf(AUDIT.LIMIT + 129))
            argv = ["check-autosync-package.py", "--baseline-dir", str(base), "--package-dir", str(package)]
            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(sys, "argv", argv):
                self.assertEqual(AUDIT.main(), 1)
            self.assertIn("AutoSync added shipped native bytes exceed 5,000,000", output.getvalue())

    def test_ipk_payload_is_inspected_after_unpacking(self):
        with tempfile.TemporaryDirectory(prefix="nv-ipk-audit-") as temporary:
            work = pathlib.Path(temporary)
            payload = work / "payload"
            payload.mkdir()
            (payload / "app").write_bytes(elf())
            (payload / "model.ort").write_bytes(b"uncompressed model bytes")
            data_tar = work / "data.tar.gz"
            with tarfile.open(data_tar, "w:gz") as archive:
                archive.add(payload, arcname=".")
            control_tar = work / "control.tar.gz"
            with tarfile.open(control_tar, "w:gz"):
                pass
            (work / "debian-binary").write_text("2.0\n")
            package = work / "fixture.ipk"
            subprocess.run(
                ["ar", "rcs", str(package), "debian-binary", str(control_tar), str(data_tar)],
                cwd=work, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            )
            report = AUDIT.inspect_ipk(package)
            self.assertIn("model.ort", report["forbidden_artifacts"])
            self.assertEqual(report["native_bytes"], len(elf()))
            self.assertEqual(report["package_bytes"], package.stat().st_size)


if __name__ == "__main__":
    unittest.main()
