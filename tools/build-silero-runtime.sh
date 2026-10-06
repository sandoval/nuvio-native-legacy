#!/bin/sh
# Offline converted .ort + required_operators_and_types.config required.
# Build outside repository. Exceptions remain enabled: errors must not abort TV.
set -eu
COMMIT=5c1b7ccbff7e5141c1da7a9d963d660e5741c319
BUILD=${NUVIO_SILERO_BUILD:?external build directory required}
CONFIG=${NUVIO_SILERO_OPS:?converted model operator/type config required}
PYTHON=${PYTHON:-python3}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$BUILD"
if [ ! -d "$BUILD/source/.git" ]; then
  git clone --branch v1.20.1 --depth 1 https://github.com/microsoft/onnxruntime.git "$BUILD/source"
fi
[ "$(git -C "$BUILD/source" rev-parse HEAD)" = "$COMMIT" ] || { echo 'Unexpected runtime revision' >&2; exit 1; }
git -C "$BUILD/source" submodule update --init --recursive
set -- --cmake_extra_defines onnxruntime_BUILD_UNIT_TESTS=OFF
if [ "${NUVIO_SILERO_HOST:-0}" != 1 ]; then
  "$PYTHON" "$SCRIPT_DIR/patch-silero-runtime.py" "$BUILD/source"
  : "${CC:=arm-webos-linux-gnueabi-gcc}" "${CXX:=arm-webos-linux-gnueabi-g++}"
  set -- "$@" CMAKE_SYSTEM_NAME=Linux CMAKE_SYSTEM_PROCESSOR=arm \
    "CMAKE_C_COMPILER=$CC" "CMAKE_CXX_COMPILER=$CXX" \
    onnxruntime_ENABLE_CPUINFO=OFF CMAKE_C_FLAGS=-Os CMAKE_CXX_FLAGS=-Os \
    "CMAKE_SHARED_LINKER_FLAGS=-static-libstdc++ -static-libgcc"
fi
if [ -n "${NUVIO_SILERO_EIGEN:-}" ]; then
  set -- "$@" --use_preinstalled_eigen --eigen_path "$NUVIO_SILERO_EIGEN"
fi
if [ "$(id -u)" = 0 ]; then set -- "$@" --allow_running_as_root; fi
"$PYTHON" "$BUILD/source/tools/ci_build/build.py" --build_dir "$BUILD/runtime" \
  --config MinSizeRel --update --build --parallel "${NUVIO_SILERO_JOBS:-2}" \
  --skip_tests --build_shared_lib --minimal_build --disable_ml_ops \
  --include_ops_by_config "$CONFIG" --enable_reduced_operator_type_support "$@"
# Do not disable contrib ops: converted model includes com.microsoft.FusedConv.
# Install/strip only after parity, loader ABI and combined 5 MB budget pass.
PREFIX=${NUVIO_SILERO_ROOT:-$BUILD/install}
mkdir -p "$PREFIX/include" "$PREFIX/lib" "$PREFIX/licenses"
cp "$BUILD/source/include/onnxruntime/core/session/onnxruntime_c_api.h" "$PREFIX/include/"
cp "$BUILD/runtime/MinSizeRel/libonnxruntime.so.1.20.1" "$PREFIX/lib/"
if [ "${NUVIO_SILERO_HOST:-0}" = 1 ]; then strip_tool=${STRIP:-strip};
else strip_tool=${STRIP:-${CC%-gcc}-strip}; fi
"$strip_tool" --strip-unneeded "$PREFIX/lib/libonnxruntime.so.1.20.1"
ln -sf libonnxruntime.so.1.20.1 "$PREFIX/lib/libonnxruntime.so.1"
ln -sf libonnxruntime.so.1 "$PREFIX/lib/libonnxruntime.so"
cp "$BUILD/source/LICENSE" "$BUILD/source/ThirdPartyNotices.txt" "$PREFIX/licenses/"
printf 'ONNX Runtime 1.20.1\ncommit %s\nCPU minimal ORT; exceptions enabled; generated operator/type config\n' "$COMMIT" > "$PREFIX/licenses/SOURCE.txt"
