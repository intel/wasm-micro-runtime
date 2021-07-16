#!/bin/sh

# Copyright (C) 2020 Intel Corporation. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

DEPS_DIR=${PWD}/../core/deps

cd ${DEPS_DIR}
if [ ! -d "llvm" ]; then
  echo "Clone llvm to core/deps/ .."
  git clone --depth 1 --branch release/11.x https://github.com/llvm/llvm-project.git llvm
fi

cd llvm
mkdir -p build
cd build

if [ ! -f bin/llvm-lto ]; then

  CORE_NUM=$(nproc --all)
  if [ -z "${CORE_NUM}" ]; then
    CORE_NUM=1
  fi

  echo "Build llvm with" ${CORE_NUM} "cores"

  cmake ../llvm \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          -DCMAKE_BUILD_TYPE:STRING="Release" \
          -DLLVM_TARGETS_TO_BUILD:STRING="X86;ARM;AArch64;Mips;RISCV" \
          -DLLVM_BUILD_LLVM_DYLIB:BOOL=OFF \
          -DLLVM_OPTIMIZED_TABLEGEN:BOOL=ON \
          -DLLVM_ENABLE_ZLIB:BOOL=OFF \
          -DLLVM_INCLUDE_DOCS:BOOL=OFF \
          -DLLVM_INCLUDE_EXAMPLES:BOOL=OFF \
          -DLLVM_INCLUDE_TESTS:BOOL=OFF \
          -DLLVM_INCLUDE_BENCHMARKS:BOOL=OFF \
          -DLLVM_APPEND_VC_REV:BOOL=OFF
  make -j ${CORE_NUM}

else
  echo "llvm has already been built"
fi

cd ${PWD}

