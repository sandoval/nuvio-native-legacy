#!/usr/bin/env python3
"""Prepare opt-in external real audio/subtitle fixtures; nothing enters Git."""
import argparse
import hashlib
import pathlib
import subprocess
import urllib.request

FILES = {
    "ED-captions.vtt": ("https://cdn.theoplayer.com/video/elephants-dream/", 4476, "a599bb2ffc9c9e88ab1e3bd68c9867b8a986e2a74394b80c4993774df98073b7"),
    "sintel-master-st.flac": ("sintel/", 71533398, "49b279400b6fcad23f6a8862ae65a2a22f21f249cd7576b09991880da221412b"),
    "sintel-m+e-st.flac": ("sintel/", 74266467, "777f238aed789a0e3a25f8ba156fae182c11a61568c1aa87b7a8bc3b15807785"),
    "ED-CM-St-16bit.flac": ("ED/", 59166432, "fc25f3658365529c599097954fa5342c3e20333cf55648e4cbbcaea95b360f18"),
    "sintel_en.srt": ("sintel/subtitles/", 1514, "4ed7e1f1bc5ff69fe33606960e4c048ba6766237bf33b132fef76e1a92cb64b2"),
    "README.txt": ("sintel/subtitles/", 390, "c709d4d7ae435f88bc00a65233e73a0010721c67ebb68d9a012b0a498964162b"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=pathlib.Path, required=True)
    parser.add_argument("--download", action="store_true", help="explicitly download missing fixtures")
    args = parser.parse_args()
    root = args.cache.resolve()
    if root.is_relative_to(pathlib.Path(__file__).resolve().parents[1]):
        parser.error("Media cache must be outside the repository")
    root.mkdir(parents=True, exist_ok=True)
    for name, (folder, size, checksum) in FILES.items():
        path = root / name
        if not path.exists():
            if not args.download:
                parser.error(f"Missing {name}; use --download explicitly")
            partial = path.with_suffix(path.suffix + ".partial")
            try:
                with urllib.request.urlopen((folder + "captions.en.vtt") if folder.startswith("https://") else ("https://media.xiph.org/" + folder + name), timeout=60) as response, partial.open("wb") as out:
                    received = 0
                    while block := response.read(min(1024 * 1024, size + 1 - received)):
                        received += len(block)
                        if received > size:
                            raise ValueError("Fixture exceeds pinned size")
                        out.write(block)
                if partial.stat().st_size != size or hashlib.sha256(partial.read_bytes()).hexdigest() != checksum:
                    raise ValueError("Fixture size/checksum mismatch")
                partial.rename(path)
            finally:
                partial.unlink(missing_ok=True)
        if path.stat().st_size != size or hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
            raise ValueError(f"Unverified fixture: {name}")
        if path.suffix == ".flac":
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(path), "-ac", "1", "-ar", "16000", "-f", "s16le", str(path.with_suffix(".raw"))], check=True)
    print("Verified external Blender/Xiph fixtures and prepared mono 16 kHz PCM.")


if __name__ == "__main__":
    main()
