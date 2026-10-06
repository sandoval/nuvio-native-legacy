#!/usr/bin/env python3
"""Verify a published immutable ORT model and generate its app build header.

The conversion manifest must contain the actual published HTTPS URL. This tool
never publishes artifacts or installs model weights into an application tree.
"""
import argparse
import hashlib
import json
import pathlib
import urllib.parse
import urllib.request

SHA256 = "c211d5f612376c9d7307a3569271f6ee9742d9a83597ad5a960758d5c62584fb"
SOURCE = "7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49"
SIZE = 1852896


class HttpsRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, target):
        if urllib.parse.urlsplit(target).scheme != "https":
            raise ValueError("Model release redirected away from HTTPS")
        return super().redirect_request(request, fp, code, message, headers, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=pathlib.Path)
    parser.add_argument("--header", type=pathlib.Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    for key, value in {"source_sha256": SOURCE, "sha256": SHA256, "bytes": SIZE,
                       "runtime": "1.20.1", "format": "ORT", "release_status": "published"}.items():
        if manifest.get(key) != value:
            parser.error(f"Incompatible or unpublished release metadata: {key}")
    url = manifest.get("url")
    parsed = urllib.parse.urlsplit(url or "")
    if parsed.scheme != "https" or not parsed.hostname or parsed.username or parsed.password or parsed.fragment:
        parser.error("Release must use a real HTTPS URL without credentials")
    opener = urllib.request.build_opener(HttpsRedirects)
    with opener.open(url, timeout=120) as response:
        if response.status != 200:
            parser.error("Model release download failed")
        artifact = response.read(SIZE + 1)
    if len(artifact) != SIZE or hashlib.sha256(artifact).hexdigest() != SHA256:
        parser.error("Published release size or SHA-256 differs from the pinned artifact")
    args.header.write_text("/* Verified external ORT artifact; no model weights. */\n"
                           "#define AUDMODEL_ORT_URL " + json.dumps(url) + "\n")
    print("Verified published ORT model; release header written.")


if __name__ == "__main__":
    main()
