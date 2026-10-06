#ifndef NV_AUDMODEL_MANIFEST_H
#define NV_AUDMODEL_MANIFEST_H
/* Immutable artifact metadata; weights are external to every app package.
 * A published ORT release header can define AUDMODEL_ORT_URL after verifying
 * the remote bytes with tools/verify-silero-release.py. Empty means unavailable. */
#if defined(NUVIO_SILERO_MINIMAL)
#ifndef AUDMODEL_ORT_URL
#define AUDMODEL_ORT_URL ""
#endif
#define AUDMODEL_URL AUDMODEL_ORT_URL
#define AUDMODEL_ID "silero-16k-1e261b036686-ort-1.20.1"
#define AUDMODEL_FORMAT "ort"
#define AUDMODEL_BYTES 1852896u
#define AUDMODEL_SHA256 "c211d5f612376c9d7307a3569271f6ee9742d9a83597ad5a960758d5c62584fb"
#else
#define AUDMODEL_URL "https://raw.githubusercontent.com/snakers4/silero-vad/1e261b036686cd0017d500ee96acd1c4ba572a9d/src/silero_vad/data/silero_vad_16k_op15.onnx"
#define AUDMODEL_ID "silero-16k-op15-1e261b036686"
#define AUDMODEL_FORMAT "onnx"
#define AUDMODEL_BYTES 1289603u
#define AUDMODEL_SHA256 "7ed98ddbad84ccac4cd0aeb3099049280713df825c610a8ed34543318f1b2c49"
#endif
#ifdef AUDMODEL_TEST
#undef AUDMODEL_BYTES
#undef AUDMODEL_SHA256
#define AUDMODEL_BYTES 3u
#define AUDMODEL_SHA256 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#endif
#endif
