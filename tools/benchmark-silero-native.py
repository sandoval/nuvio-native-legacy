#!/usr/bin/env python3
"""Score actual native Silero segments using the existing external corpus labels."""
import argparse
import importlib.util
import json
import pathlib
import subprocess
import time
import wave

SPEC = importlib.util.spec_from_file_location("vad", pathlib.Path(__file__).with_name("benchmark-vad.py"))
VAD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VAD)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True, help="tests/audsilero_native.c executable")
    parser.add_argument("--model", type=pathlib.Path, required=True)
    parser.add_argument("--cache", type=pathlib.Path, required=True)
    args = parser.parse_args()
    results = []
    for clip in json.loads((args.cache / "prepared.json").read_text()):
        with wave.open(clip["path"]) as audio:
            assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate()) == (1, 2, 16000)
            pcm = audio.readframes(audio.getnframes())
        began = time.monotonic()
        completed = subprocess.run([str(args.binary.resolve()), str(args.model.resolve()), "4096", "0"], input=pcm, capture_output=True, check=True)
        elapsed = time.monotonic() - began
        segments = [list(map(float, line.split()[1:])) for line in completed.stdout.decode().splitlines() if line.startswith("S ")]
        counts = VAD.confusion(clip["reference"], segments, clip["valid"], clip["duration"])
        conditions = {label: VAD.confusion([] if label == "NO_SPEECH" else intervals, segments, intervals, clip["duration"])
                      for label, intervals in clip.get("conditions", {}).items()}
        results.append({"clip": clip["id"], "dataset": clip["dataset"], "duration": clip["duration"],
                        "segments": segments, "counts": counts, "metrics": VAD.rates(counts),
                        "condition_counts": conditions,
                        "boundaries": VAD.boundary_scores(clip["reference"], segments, clip["valid"], clip["duration"]),
                        "wall_seconds": elapsed, "cpu_seconds": 0, "kernel_seconds": 0})
    report = {"method": "native-silero", "runtime": "1.20.1", "format": args.model.suffix,
              "threshold": 0.5, "merge_gap_seconds": 0.3, "minimum_speech_seconds": 0.2,
              "timing_note": "Wall time includes subprocess startup; CPU/kernel fields are unmeasured",
              "overall": VAD.aggregate(results), "clips": results}
    (args.cache / "native-silero-worker.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["overall"], indent=2))


if __name__ == "__main__":
    main()
