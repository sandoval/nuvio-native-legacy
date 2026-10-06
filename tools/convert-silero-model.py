#!/usr/bin/env python3
"""Release-only offline conversion. Never install generated weights in package."""
import hashlib, json, pathlib, subprocess, sys
import onnxruntime
SOURCE_SHA = '7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49'
assert onnxruntime.__version__ == '1.20.1', 'Use converter onnxruntime==1.20.1'
source = pathlib.Path(sys.argv[1]).resolve()
assert hashlib.sha256(source.read_bytes()).hexdigest() == SOURCE_SHA
subprocess.run([sys.executable, '-m', 'onnxruntime.tools.convert_onnx_models_to_ort', str(source),
                '--optimization_style', 'Fixed', '--enable_type_reduction'], check=True)
artifact = source.with_suffix('.ort')
manifest = {'source_sha256': SOURCE_SHA, 'runtime': '1.20.1', 'format': 'ORT',
            'bytes': artifact.stat().st_size,
            'sha256': hashlib.sha256(artifact.read_bytes()).hexdigest(),
            'url': None, 'release_status': 'unpublished'}
artifact.with_suffix('.manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
