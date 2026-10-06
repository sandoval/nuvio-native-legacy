#!/usr/bin/env python3
"""Audit unpacked shipped native bytes and actual IPK payloads, never compression.

Compare identical build variants (compiler, optimization, stripping and existing
optional components) with the DTS base, and report the upstream-master total.
All ELF files are counted regardless of their name. Outputs are JSON evidence.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess
import tarfile
import tempfile

LIMIT = 5_000_000
MODEL_HASH = "7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49"


def inspect(root):
    files, forbidden = {}, []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        name = path.relative_to(root).as_posix()
        with path.open("rb") as stream:
            prefix = stream.read(8)
        suffixes = {s.lower() for s in path.suffixes}
        if (suffixes & {".onnx", ".ort", ".pt", ".pth", ".safetensors", ".partial", ".a"}
                or "subtitle-autosync/" in name or prefix[4:8] == b"ORTM"):
            forbidden.append(name)
        if path.stat().st_size == 1289603 and hashlib.sha256(path.read_bytes()).hexdigest() == MODEL_HASH:
            forbidden.append(name)
        if prefix[:4] == b"\x7fELF":
            # ELF architecture/e_flags are useful when comparing build variants.
            byteorder = "little" if prefix[5] == 1 else "big"
            with path.open("rb") as stream:
                header = stream.read(52)
            files[name] = {"bytes": path.stat().st_size,
                           "machine": int.from_bytes(header[18:20], byteorder),
                           "class": header[4]}
    return {"native_bytes": sum(item["bytes"] for item in files.values()),
            "files": files, "forbidden_artifacts": sorted(set(forbidden))}


def inspect_ipk(path):
    members = subprocess.check_output(["ar", "t", str(path)], text=True).splitlines()
    payloads = [name for name in members if name.startswith("data.tar")]
    if len(payloads) != 1:
        raise ValueError("IPK must contain exactly one data archive")
    payload = subprocess.check_output(["ar", "p", str(path), payloads[0]])
    with tempfile.TemporaryDirectory(prefix="nuvio-package-audit-") as directory:
        root = pathlib.Path(directory)
        archive = root / "payload.tar"
        archive.write_bytes(payload)
        dest = root / "unpacked"
        dest.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(dest, filter="data")
        result = inspect(dest)
    result["package_bytes"] = path.stat().st_size
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-dir", type=pathlib.Path, required=True)
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--master-dir", type=pathlib.Path)
    parser.add_argument("--baseline-ipk", type=pathlib.Path)
    parser.add_argument("--package-ipk", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    for directory in [args.baseline_dir, args.package_dir, args.master_dir]:
        if directory is not None and not directory.is_dir():
            parser.error(f"Missing distributable directory: {directory}")
    base, package = inspect(args.baseline_dir), inspect(args.package_dir)
    errors = []
    if not base["files"] or not package["files"]:
        errors.append("Both distributables must contain native ELF binaries")
    if package["forbidden_artifacts"]:
        errors.append("Distributable contains model weights, partial downloads or static libraries")
    architectures = lambda report: {(f["machine"], f["class"]) for f in report["files"].values()}
    if architectures(base) != architectures(package):
        errors.append("Baseline and package native architectures differ")
    delta = package["native_bytes"] - base["native_bytes"]
    if delta > LIMIT:
        errors.append("AutoSync added shipped native bytes exceed 5,000,000")
    report = {"limit_bytes": LIMIT, "dts_baseline": base, "package": package,
              "autosync_native_delta": delta, "errors": errors}
    if args.master_dir:
        master = inspect(args.master_dir)
        report.update(upstream_master=master,
                      total_native_delta_from_master=package["native_bytes"]-master["native_bytes"])
    if bool(args.baseline_ipk) != bool(args.package_ipk):
        parser.error("Supply both IPKs to measure the compressed package delta")
    if args.package_ipk:
        bi, pi = inspect_ipk(args.baseline_ipk), inspect_ipk(args.package_ipk)
        report.update(baseline_ipk=bi, package_ipk=pi,
                      compressed_package_delta=pi["package_bytes"]-bi["package_bytes"])
        if pi["forbidden_artifacts"]:
            errors.append("Actual IPK contains forbidden artifacts")
        if pi["native_bytes"] != package["native_bytes"]:
            errors.append("IPK native bytes do not match the inspected distributable")
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(rendered)
    print(rendered, end="")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
