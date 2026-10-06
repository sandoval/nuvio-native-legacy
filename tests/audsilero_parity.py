#!/usr/bin/env python3
"""Replay external mono 16 kHz WAV against pinned ONNX and native C adapter.
Usage: python tests/audsilero_parity.py BINARY ONNX NATIVE_MODEL WAV
Native model may be the converted ORT model. No media enters the repository.
"""
import os, subprocess, sys, wave
import numpy as np
import onnxruntime as ort
binary, onnx, native, wav = sys.argv[1:]
# Different CPU kernels have different rounding; ARM NEON gate is 1e-4.
tolerance = float(os.environ.get('NUVIO_SILERO_PARITY_TOLERANCE', '5e-5'))
assert 0 < tolerance <= 1e-4, 'Parity tolerance must not exceed ARM acceptance limit 1e-4'
with wave.open(wav) as w:
    assert (w.getnchannels(), w.getsampwidth(), w.getframerate()) == (1, 2, 16000)
    # Include a deliberately partial final frame.
    seconds = int(os.environ.get('NUVIO_SILERO_PARITY_SECONDS', '30'))
    pcm = np.frombuffer(w.readframes(min(w.getnframes(), 16000 * seconds + 137)), dtype='<i2')
    if len(pcm) >= 512 and len(pcm) % 512 == 0:
        pcm = pcm[:-137]
opts = ort.SessionOptions()
opts.intra_op_num_threads = opts.inter_op_num_threads = 1
session = ort.InferenceSession(onnx, sess_options=opts, providers=['CPUExecutionProvider'])
state, context = np.zeros((2, 1, 128), np.float32), np.zeros((1, 64), np.float32)
reference = []
for pos in range(0, len(pcm), 512):
    frame = np.zeros((1, 512), np.float32)
    part = pcm[pos:pos+512]
    frame[0, :len(part)] = part.astype(np.float32) / 32768
    data = np.concatenate([context, frame], axis=1)
    probability, state = session.run(None, {'input': data, 'state': state, 'sr': np.array(16000, np.int64)})
    context = data[:, -64:]
    reference.append(float(probability.flat[0]))
# Construct benchmark-default segments independently from frame probabilities.
raw, begin = [], None
for i, p in enumerate(reference):
    start = i * 512 / 16000
    if p >= .5 and begin is None:
        begin = start
    elif p < .5 and begin is not None:
        raw.append([begin, start]); begin = None
if begin is not None:
    raw.append([begin, len(pcm) / 16000])
merged = []
for begin, end in raw:
    if merged and begin - merged[-1][1] <= .300000001:
        merged[-1][1] = end
    else:
        merged.append([begin, end])
expected = [(a + 7.312345, b + 7.312345) for a, b in merged if b - a >= .2 - 1e-9]
baseline = None
for chunk in [1, 137, 512, 4096]:
    result = subprocess.run([binary, native, str(chunk), '7312345'], input=pcm.tobytes(), capture_output=True, check=True)
    lines = result.stdout.decode().splitlines()
    probs = [float(line.split()[3]) for line in lines if line.startswith('P ')]
    assert len(probs) == len(reference)
    error = float(np.max(np.abs(np.array(probs) - reference)))
    assert error <= tolerance, (error, tolerance)
    assert baseline is None or lines == baseline, 'chunk-dependent output'
    segments = [tuple(map(float, line.split()[1:])) for line in lines if line.startswith('S ')]
    assert len(segments) == len(expected), (segments, expected)
    assert not segments or np.max(np.abs(np.array(segments) - expected)) <= .032, (segments, expected)
    baseline = lines
    frames = [line.split() for line in lines if line.startswith('P ')]
    assert int(frames[0][1]) == 7312345
    assert int(frames[-1][2]) == len(pcm) % 512 or len(pcm) % 512 == 0
    print(f'chunk {chunk}: {len(probs)} probabilities, max error {error:.3g}')

failed = subprocess.run([binary, native + '.missing', '512', '0'], input=b'', capture_output=True)
assert failed.returncode == 1 and failed.stderr, 'missing model did not fail recoverably'
