#!/usr/bin/env python3
"""Download labelled audio and compare Nuvio, WebRTC/libfvad and Silero VAD.

Run: python3 tools/benchmark-vad.py --setup
Requires Python 3.12-3.14, a C compiler and FFmpeg. All dependencies, audio,
models, builds and reports live outside the checkout. No PyTorch is needed.
See docs/features/subtitle-autosync/benchmark.md for scoring and limitations.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import csv
import ctypes
import hashlib
import io
import importlib.util
import json
import math
import os
from pathlib import Path
import platform
import resource
import shlex
import statistics
import subprocess
import sys
import tarfile
import time
import urllib.request
import venv
import wave

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "tools/vad-benchmark/manifest.json"
METHODS = ("nuvio", "webrtc", "silero")
AVA_CLASSES = ("NO_SPEECH", "CLEAN_SPEECH", "SPEECH_WITH_MUSIC", "SPEECH_WITH_NOISE")
RATE = 16000
GRID = 0.01


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def outside_checkout(path):
    path = path.expanduser().resolve()
    if path == ROOT or ROOT in path.parents:
        raise ValueError(f"Downloads/results must stay outside the checkout: {path}")
    return path


def download(entry, directory):
    path = directory / entry["id"]
    if path.is_file() and digest(path) == entry["sha256"]:
        return path
    print(f"Downloading {entry['id']}", flush=True)
    request = urllib.request.Request(entry["url"], headers={"User-Agent": "Nuvio-VAD-benchmark/1"})
    partial = path.with_suffix(path.suffix + ".partial")
    try:
        with urllib.request.urlopen(request, timeout=90) as response, partial.open("wb") as output:
            size = 0
            while chunk := response.read(1024 * 1024):
                size += len(chunk)
                if size > entry["bytes"]:
                    raise ValueError(f"Download size mismatch: {entry['id']}")
                output.write(chunk)
        if size != entry["bytes"] or digest(partial) != entry["sha256"]:
            raise ValueError(f"Download size/checksum mismatch: {entry['id']}")
    except Exception:
        partial.unlink(missing_ok=True)
        raise
    partial.replace(path)
    return path


def setup_python(args, manifest):
    env = args.cache / "venv"
    python = env / "bin/python"
    if not python.exists():
        venv.EnvBuilder(with_pip=True).create(env)
    deps = manifest["dependencies"]
    subprocess.run([str(python), "-m", "pip", "install", "--disable-pip-version-check",
                    f"numpy=={deps['numpy']}", f"onnxruntime=={deps['onnxruntime']}"], check=True)
    argv = [a for a in sys.argv[1:] if a != "--setup"]
    os.execv(str(python), [str(python), str(Path(__file__).resolve()), *argv])


def build_native(cache, manifest):
    archive = cache / "downloads/libfvad.tar.gz"
    source = cache / "source" / ("libfvad-" + manifest["dependencies"]["libfvad_commit"])
    if not source.exists():
        source.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(archive, "r:gz") as tar:
            # This is a pinned, checksummed source archive. Still reject links
            # and paths escaping its extraction directory.
            for member in tar.getmembers():
                target = (source.parent / member.name).resolve()
                if source.parent.resolve() not in target.parents or member.issym() or member.islnk():
                    raise ValueError("Unsafe libfvad archive entry")
            tar.extractall(source.parent, filter="data")
    build = cache / "build"
    build.mkdir(exist_ok=True)
    wrapper = ROOT / "tools/vad-benchmark/native.c"
    inputs = [ROOT / "src/audvad.c", ROOT / "src/audvad.h", wrapper,
              *sorted((source / "src").rglob("*.c")),
              *sorted((source / "src").rglob("*.h")), source / "include/fvad.h"]
    compiler = shlex.split(os.environ.get("CC", "cc"))
    fingerprint = hashlib.sha256((json.dumps(compiler) + "-O2 -shared -fPIC" +
                                  "".join(digest(p) for p in inputs)).encode()).hexdigest()
    library = build / "vad.so"
    stamp = build / "fingerprint.txt"
    command = [*compiler, "-O2", "-fPIC", "-shared", "-I" + str(ROOT / "src"),
               "-I" + str(source / "include"), "-I" + str(source / "src"),
               str(wrapper), str(ROOT / "src/audvad.c"),
               *map(str, sorted((source / "src").rglob("*.c"))), "-lm", "-o", str(library)]
    if not library.exists() or not stamp.exists() or stamp.read_text() != fingerprint:
        subprocess.run(command, check=True)
        stamp.write_text(fingerprint)
    return {"command": command, "fingerprint": fingerprint,
            "combined_native_library_bytes": library.stat().st_size}


def merge_intervals(intervals, gap=0.0, minimum=0.0):
    merged = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if merged and start - merged[-1][1] <= gap + 1e-9:
            merged[-1][1] = max(end, merged[-1][1])
        else:
            merged.append([start, end])
    return [p for p in merged if p[1] - p[0] + 1e-9 >= minimum]


def annotations(path, fmt, duration):
    if fmt == "rttm":
        speech = []
        for line in path.read_text().splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            fields = line.split()
            if fields[0] != "SPEAKER" or len(fields) < 8:
                raise ValueError(f"Invalid RTTM in {path}")
            start, length = float(fields[3]), float(fields[4])
            if start < 0 or length <= 0 or start + length > duration + 0.05:
                raise ValueError(f"RTTM outside audio in {path}")
            speech.append([start, min(start + length, duration)])
        return merge_intervals(speech), [[0.0, duration]]
    if fmt != "ten-scv":
        raise ValueError(f"Unknown annotation format: {fmt}")
    fields = next(csv.reader(io.StringIO(path.read_text().strip())))[1:]
    if len(fields) % 3:
        raise ValueError(f"Invalid SCV in {path}")
    speech, valid, previous = [], [], 0.0
    for i in range(0, len(fields), 3):
        start, end, label = float(fields[i]), float(fields[i+1]), int(fields[i+2])
        if start < previous - 1e-6 or end <= start or label not in (0, 1) or end > duration + 0.05:
            raise ValueError(f"Invalid annotation interval in {path}: {fields[i:i+3]}")
        end = min(end, duration)
        valid.append([start, end])
        if label:
            speech.append([start, end])
        previous = end
    # Unannotated gaps/tails are excluded, not silently treated as silence.
    return merge_intervals(speech), merge_intervals(valid)


def ava_annotations(path, video_id, origin, duration):
    """AVA CSV timestamps refer to the original movie, not the extracted WAV."""
    conditions = {label: [] for label in AVA_CLASSES}
    previous = None
    for vid, start, end, label in csv.reader(io.StringIO(path.read_text())):
        if vid != video_id:
            continue
        start, end = float(start), float(end)
        if label not in conditions or end <= start or (previous is not None and start < previous - 1e-6):
            raise ValueError(f"Invalid/overlapping AVA annotation for {video_id}")
        previous = end
        a, b = max(0.0, start-origin), min(duration, end-origin)
        if b > a:
            conditions[label].append([a, b])
    conditions = {k: merge_intervals(v) for k, v in conditions.items()}
    valid = merge_intervals([p for v in conditions.values() for p in v])
    if not valid:
        raise ValueError(f"No AVA annotations for {video_id} at {origin}")
    speech = merge_intervals([p for k, v in conditions.items() if k != "NO_SPEECH" for p in v])
    return speech, valid, conditions


def prepare_clips(args, manifest):
    wavdir = args.cache / "wav16k"
    wavdir.mkdir(exist_ok=True)
    clips = []
    selected = manifest["clips"][:args.limit] if args.limit else manifest["clips"]
    for item in selected:
        source = args.cache / "downloads" / item["audio"]
        target = wavdir / (item["id"] + ".wav")
        stamp = target.with_suffix(".sha256")
        recipe = target.with_suffix(".recipe.json")
        audio_filter = f"aresample={RATE}:first_pts=0"
        if "start_seconds" in item:
            start = item["start_seconds"]
            end = start + item["duration_seconds"]
            # Decode from the beginning and trim the normalized media timeline.
            # Input seeking can shift Opus by its preroll/container start time.
            audio_filter = f"atrim=start={start}:end={end},asetpts=PTS-{start}/TB," + audio_filter
        if "duration_seconds" in item:
            audio_filter += f",atrim=end_sample={round(item['duration_seconds']*RATE)}"
        extraction = {"source_sha256": digest(source), "start_seconds": item.get("start_seconds", 0),
                      "duration_seconds": item.get("duration_seconds"),
                      "rate": RATE, "channels": 1, "audio_filter": audio_filter}
        if (not target.exists() or not stamp.exists() or digest(target) != stamp.read_text().strip()
                or not recipe.exists() or json.loads(recipe.read_text()) != extraction):
            partial = target.with_suffix(".partial.wav")
            trim = ["-t", str(item["duration_seconds"])] if "duration_seconds" in item else []
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                            "-i", str(source), *trim, "-map", "0:a:0", "-ac", "1", "-af", audio_filter, "-ar", str(RATE),
                            "-c:a", "pcm_s16le", str(partial)], check=True)
            partial.replace(target)
            stamp.write_text(digest(target)+"\n")
            recipe.write_text(json.dumps(extraction))
        with wave.open(str(target)) as wav:
            if wav.getnchannels() != 1 or wav.getframerate() != RATE or wav.getsampwidth() != 2:
                raise ValueError(f"Expected mono 16 kHz PCM16: {target}")
            duration = wav.getnframes() / RATE
        if "duration_seconds" in item and abs(duration-item["duration_seconds"]) > 1/RATE:
            raise ValueError(f"Extracted WAV has wrong duration: {target}: {duration}")
        label_path = args.cache / "downloads" / item["annotation"]
        conditions = {}
        if item["format"] == "ava-csv":
            ref, valid, conditions = ava_annotations(label_path, item["video_id"], item["annotation_origin_seconds"], duration)
        else:
            ref, valid = annotations(label_path, item["format"], duration)
        clips.append({**item, "path": str(target), "duration": duration,
                      "reference": ref, "valid": valid, "conditions": conditions, "normalized_sha256": digest(target)})
    return clips


def mask(intervals, duration):
    # Sample all approaches and labels on the same 10 ms midpoint grid.
    result = [False] * math.ceil(duration / GRID)
    for start, end in intervals:
        for i in range(max(0, math.ceil(start / GRID - 0.5)),
                       min(len(result), math.ceil(end / GRID - 0.5))):
            result[i] = True
    return result


def confusion(reference, predicted, valid, duration):
    truth, guess, known = (mask(x, duration) for x in (reference, predicted, valid))
    counts = dict(tp=0, fp=0, fn=0, tn=0)
    for t, p, v in zip(truth, guess, known):
        if v:
            counts["tp" if t and p else "fn" if t else "fp" if p else "tn"] += 1
    return counts


def rates(counts):
    tp, fp, fn, tn = (counts[k] for k in ("tp", "fp", "fn", "tn"))
    ratio = lambda a, b: a / b if b else 0.0
    return {"precision": ratio(tp, tp+fp), "recall": ratio(tp, tp+fn),
            "f1": ratio(2*tp, 2*tp+fp+fn), "false_positive_rate": ratio(fp, fp+tn),
            "miss_rate": ratio(fn, tp+fn), "labelled_seconds": (tp+fp+fn+tn)*GRID}


def boundary_scores(reference, predicted, valid, duration, tolerance=0.250):
    # Score onset and offset independently. Exclude file edges and boundaries
    # with unlabelled material on either side. Chronological one-to-one match.
    counts = dict(tp=0, fp=0, fn=0, tn=0)
    errors = []
    for edge in (0, 1):
        def eligible(t):
            return any(a + 1e-6 < t < b - 1e-6 for a, b in valid) and 0 < t < duration
        truth = sorted(p[edge] for p in reference if eligible(p[edge]))
        guess = sorted(p[edge] for p in predicted if eligible(p[edge]))
        i = j = 0
        while i < len(truth) and j < len(guess):
            delta = guess[j] - truth[i]
            if abs(delta) <= tolerance + 1e-9:
                counts["tp"] += 1
                errors.append(abs(delta)*1000)
                i += 1; j += 1
            elif delta < 0:
                counts["fp"] += 1; j += 1
            else:
                counts["fn"] += 1; i += 1
        counts["fn"] += len(truth)-i
        counts["fp"] += len(guess)-j
    return {"counts": counts, "matched_errors_ms": errors}


class Native:
    def __init__(self, cache, method, mode):
        import numpy as np
        self.np, self.method, self.mode = np, method, mode
        self.library = ctypes.CDLL(str(cache / "build/vad.so"))
        self.function = getattr(self.library, "bench_" + method)
        self.function.argtypes = [ctypes.POINTER(ctypes.c_int16), ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_double), ctypes.c_int,
                                  ctypes.POINTER(ctypes.c_double)]
        if method == "webrtc":
            self.function.argtypes += [ctypes.c_int]
        self.function.restype = ctypes.c_int

    def run(self, pcm):
        pairs = self.np.empty((4096, 2), dtype=self.np.float64)
        seconds = ctypes.c_double()
        args = [pcm.ctypes.data_as(ctypes.POINTER(ctypes.c_int16)), len(pcm),
                pairs.ctypes.data_as(ctypes.POINTER(ctypes.c_double)), len(pairs), ctypes.byref(seconds)]
        if self.method == "webrtc":
            args.append(self.mode)
        n = self.function(*args)
        if n < 0:
            raise RuntimeError(f"{self.method}: native processing failed/segment capacity exceeded")
        return pairs[:n].tolist(), seconds.value


class Silero:
    def __init__(self, cache, threshold):
        import numpy as np
        import onnxruntime as ort
        self.np, self.threshold = np, threshold
        opts = ort.SessionOptions()
        opts.intra_op_num_threads = opts.inter_op_num_threads = 1
        self.session = ort.InferenceSession(str(cache / "downloads/silero-16k.onnx"),
                                           sess_options=opts, providers=["CPUExecutionProvider"])

    def run(self, pcm):
        np = self.np
        state = np.zeros((2, 1, 128), dtype=np.float32)
        context = np.zeros((1, 64), dtype=np.float32)
        sr = np.array(RATE, dtype=np.int64)
        audio = pcm.astype(np.float32) / 32768.0
        intervals, begin, kernel_seconds = [], None, 0.0
        for pos in range(0, len(audio), 512):
            chunk = np.zeros((1, 512), dtype=np.float32)
            part = audio[pos:pos+512]
            chunk[0, :len(part)] = part
            data = np.concatenate((context, chunk), axis=1)
            start = time.perf_counter()
            prob, state = self.session.run(None, {"input": data, "state": state, "sr": sr})
            kernel_seconds += time.perf_counter() - start
            context = data[:, -64:]
            speaking = float(prob.reshape(-1)[0]) >= self.threshold
            if speaking and begin is None:
                begin = pos / RATE
            elif not speaking and begin is not None:
                intervals.append([begin, pos / RATE]); begin = None
        if begin is not None:
            intervals.append([begin, len(audio) / RATE])
        return intervals, kernel_seconds


def rss_mb():
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return value / (1024*1024 if sys.platform == "darwin" else 1024)


def worker(args):
    import numpy as np
    baseline = rss_mb()
    start = time.perf_counter()
    detector = (Silero(args.cache, args.silero_threshold) if args.worker == "silero"
                else Native(args.cache, args.worker, args.webrtc_mode))
    init_seconds = time.perf_counter() - start
    clips = json.loads((args.cache / "prepared.json").read_text())
    results = []
    for clip in clips:
        with wave.open(clip["path"]) as wav:
            pcm = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.int16)
        detector.run(pcm)  # unmeasured warm-up; state is reset on every run
        elapsed, kernels, cpu = [], [], []
        for _ in range(args.repeats):
            start_wall, start_cpu = time.perf_counter(), time.process_time()
            raw, kernel = detector.run(pcm)
            cpu.append(time.process_time() - start_cpu)
            elapsed.append(time.perf_counter() - start_wall)
            kernels.append(kernel)
        # Same extra segment cleanup for every detector. Nuvio also retains
        # its native onset/release logic; this is its shipping behavior.
        segments = merge_intervals(raw, gap=args.merge_gap, minimum=args.min_speech)
        scores = confusion(clip["reference"], segments, clip["valid"], clip["duration"])
        conditions = {label: confusion([] if label == "NO_SPEECH" else intervals,
                                       segments, intervals, clip["duration"])
                      for label, intervals in clip.get("conditions", {}).items()}
        results.append({"clip": clip["id"], "dataset": clip["dataset"],
                        "duration": clip["duration"], "segments": segments,
                        "counts": scores, "metrics": rates(scores),
                        "condition_counts": conditions,
                        "boundaries": boundary_scores(clip["reference"], segments, clip["valid"], clip["duration"]),
                        "wall_seconds": statistics.median(elapsed),
                        "cpu_seconds": statistics.median(cpu),
                        "kernel_seconds": statistics.median(kernels)})
    versions = {"python": platform.python_version(), "numpy": np.__version__}
    if args.worker == "silero":
        import onnxruntime
        versions["onnxruntime"] = onnxruntime.__version__
    report = {"method": args.worker, "init_seconds": init_seconds,
              "peak_rss_mb": rss_mb(), "baseline_peak_rss_mb": baseline,
              "versions": versions, "clips": results}
    (args.cache / (args.worker + "-worker.json")).write_text(json.dumps(report, indent=2)+"\n")


def aggregate(clips):
    counts = {k: sum(c["counts"][k] for c in clips) for k in ("tp", "fp", "fn", "tn")}
    boundary_counts = {k: sum(c["boundaries"]["counts"][k] for c in clips) for k in counts}
    errors = [e for c in clips for e in c["boundaries"]["matched_errors_ms"]]
    duration = sum(c["duration"] for c in clips)
    wall = sum(c["wall_seconds"] for c in clips)
    return {**rates(counts), "counts": counts, "duration_seconds": duration,
            "conditions": {label: {**rates(total), "counts": total}
                           for label in sorted({k for c in clips for k in c.get("condition_counts", {})})
                           for total in [{k: sum(c.get("condition_counts", {}).get(label, {}).get(k, 0)
                                               for c in clips) for k in counts}]},
            "macro_clip_f1": statistics.mean(c["metrics"]["f1"] for c in clips),
            "wall_rtf": wall / duration, "kernel_rtf": sum(c["kernel_seconds"] for c in clips) / duration,
            "cpu_rtf": sum(c["cpu_seconds"] for c in clips) / duration,
            "boundary_f1_250ms": rates(boundary_counts)["f1"],
            "boundary_counts": boundary_counts,
            "matched_boundary_mae_ms": statistics.mean(errors) if errors else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--corpus", choices=("public-smoke", "ava-speech"), default="public-smoke")
    parser.add_argument("--cache", type=Path, help="External cache (default: ~/.cache/nuvio-vad-benchmark, separate ava-speech subdirectory)")
    parser.add_argument("--setup", action="store_true", help="Create external venv, install pinned numpy/ONNX Runtime, then run")
    parser.add_argument("--limit", type=int, default=0, help="First N clips; 0 runs all clips in the selected corpus")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--webrtc-mode", type=int, choices=range(4), default=2)
    parser.add_argument("--silero-threshold", type=float, default=0.5)
    parser.add_argument("--merge-gap", type=float, default=0.3)
    parser.add_argument("--min-speech", type=float, default=0.2)
    parser.add_argument("--worker", choices=METHODS, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.repeats < 1 or args.limit < 0 or not 0 < args.silero_threshold < 1 or args.merge_gap < 0 or args.min_speech < 0:
        parser.error("Invalid repeat/limit/threshold/segment settings")
    default_cache = Path.home()/".cache/nuvio-vad-benchmark"
    if args.corpus == "ava-speech":
        default_cache /= "ava-speech"
    args.cache = outside_checkout(args.cache or default_cache)
    args.cache.mkdir(parents=True, exist_ok=True)
    manifest_path = MANIFEST if args.corpus == "public-smoke" else MANIFEST.with_name("ava-speech.json")
    manifest = json.loads(manifest_path.read_text())
    if args.setup:
        setup_python(args, manifest)
    if args.worker:
        worker(args)
        return
    # Do not import ONNX Runtime in the coordinator: native workers must not
    # inherit its loaded memory when fork/exec establishes their RSS baseline.
    if any(importlib.util.find_spec(name) is None for name in ("numpy", "onnxruntime")):
        parser.error("numpy and onnxruntime are required. Run with --setup to install them in an external venv.")
    downloads = args.cache / "downloads"
    downloads.mkdir(exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda e: download(e, downloads), manifest["files"]))
    build = build_native(args.cache, manifest)
    clips = prepare_clips(args, manifest)
    (args.cache / "prepared.json").write_text(json.dumps(clips, indent=2)+"\n")
    print(f"Comparing {len(clips)} clips, {sum(c['duration'] for c in clips):.1f} seconds. CPU only; one ONNX thread.", flush=True)
    reports = {}
    for method in METHODS:
        print(f"Running {method}...", flush=True)
        subprocess.run([sys.executable, str(Path(__file__).resolve()), "--cache", str(args.cache),
                        "--corpus", args.corpus,
                        "--worker", method, "--repeats", str(args.repeats),
                        "--webrtc-mode", str(args.webrtc_mode), "--silero-threshold", str(args.silero_threshold),
                        "--merge-gap", str(args.merge_gap), "--min-speech", str(args.min_speech)], check=True)
        report = json.loads((args.cache / (method+"-worker.json")).read_text())
        report["overall"] = aggregate(report["clips"])
        report["datasets"] = {d: aggregate([c for c in report["clips"] if c["dataset"] == d])
                              for d in sorted({c["dataset"] for c in report["clips"]})}
        reports[method] = report
    metadata = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "host": platform.platform(), "machine": platform.machine(),
                "compiler_version": subprocess.check_output([*shlex.split(os.environ.get("CC", "cc")), "--version"], text=True).splitlines()[0],
                "ffmpeg_version": subprocess.check_output(["ffmpeg", "-version"], text=True).splitlines()[0],
                "manifest_sha256": digest(manifest_path), "settings": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                "sources": manifest["sources"], "dependencies": manifest["dependencies"],
                "build": build, "silero_model_bytes": (downloads/"silero-16k.onnx").stat().st_size,
                "normalized_files": {c["id"]: c["normalized_sha256"] for c in clips},
                "limitations": [manifest.get("scope", "Short public test clips; no movie-domain ranking or ARM performance claim."),
                                "Same segment cleanup, but detector-native onset/hangover differ.",
                                "No timing collar on frame metrics; 10 ms midpoint grid.",
                                "Boundary MAE is only for one-to-one matches within 250 ms; read with boundary F1.",
                                "Wall RTF includes wrapper/preparation of input tensors; excludes file IO, scoring and model load.",
                                "Native kernel RTF includes detector init/flush; Silero kernel RTF includes Python-to-ORT calls.",
                                "Peak RSS is absolute per worker, including Python/NumPy/runtime; not detector-only RAM.",
                                "Combined host library/model sizes are not incremental webOS binary size.",
                                "Accuracy parameters are fixed beforehand; do not tune on this evaluation set."]}
    results = args.cache / "results"
    results.mkdir(exist_ok=True)
    (results/"report.json").write_text(json.dumps({"metadata": metadata, "methods": reports}, indent=2)+"\n")
    columns = ["method", "precision", "recall", "f1", "false_positive_rate", "boundary_f1_250ms", "matched_boundary_mae_ms", "wall_rtf", "kernel_rtf", "peak_rss_mb"]
    with (results/"summary.csv").open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for method, report in reports.items():
            row = {"method": method, "peak_rss_mb": report["peak_rss_mb"], **report["overall"]}
            writer.writerow({c: row[c] for c in columns})
    lines = ["# VAD host benchmark", "", f"{len(clips)} clips; {sum(c['duration'] for c in clips):.1f} seconds. {platform.machine()}.", "",
             "| Method | Precision | Recall | F1 | Non-speech FPR | Boundary F1 ±250 ms | Wall RTF | Peak RSS MB |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for method, report in reports.items():
        m = report["overall"]
        lines.append(f"| {method} | {m['precision']:.3f} | {m['recall']:.3f} | {m['f1']:.3f} | {m['false_positive_rate']:.3f} | {m['boundary_f1_250ms']:.3f} | {m['wall_rtf']:.5f} | {report['peak_rss_mb']:.1f} |")
    if args.corpus == "ava-speech":
        lines += ["", "Speech recall by AVA condition (singing counts as speech):", "",
                  "| Method | Clean | With music | With noise |",
                  "|---|---:|---:|---:|"]
        for method, report in reports.items():
            cond = report["overall"]["conditions"]
            lines.append("| " + method + " | " + " | ".join(
                f"{cond[k]['recall']:.3f}" for k in AVA_CLASSES[1:]) + " |")
        lines += ["", "Labelled seconds: " + ", ".join(
            f"{k}={reports['nuvio']['overall']['conditions'][k]['labelled_seconds']:.2f}"
            for k in AVA_CLASSES) + "."]
    lines += ["", "RTF is processing seconds/audio seconds; lower is faster. RSS includes the host interpreter/runtime.", "", *["- "+s for s in metadata["limitations"]], ""]
    (results/"summary.md").write_text("\n".join(lines))
    print("\n".join(lines))
    print(f"Reports: {results}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"benchmark-vad: {error}", file=sys.stderr)
        sys.exit(1)
