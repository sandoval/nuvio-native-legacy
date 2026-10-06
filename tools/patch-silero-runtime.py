#!/usr/bin/env python3
"""Apply explicit webOS glibc 2.12 auxv compatibility to pinned ORT source."""
import pathlib, shutil, sys
source = pathlib.Path(sys.argv[1])
repo = pathlib.Path(__file__).resolve().parent.parent
common = source / 'onnxruntime/core/common'
shutil.copyfile(repo / 'src/audplatform.h', common / 'nuvio_audplatform.h')
shutil.copyfile(repo / 'tools/silero-runtime-compat.h', common / 'nuvio_auxv_compat.h')
for relative in ['onnxruntime/core/common/cpuid_info.cc', 'onnxruntime/core/mlas/lib/platform.cpp']:
    path = source / relative
    text = path.read_text()
    original, replacement = '#include <sys/auxv.h>', '#include "core/common/nuvio_auxv_compat.h"'
    assert original in text or replacement in text, relative
    path.write_text(text.replace(original, replacement))
