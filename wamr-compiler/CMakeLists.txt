# Copyright (C) 2019 Intel Corporation. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required (VERSION 2.8)

string (TOLOWER ${CMAKE_HOST_SYSTEM_NAME} WAMR_BUILD_PLATFORM)

if (NOT WAMR_BUILD_PLATFORM STREQUAL "windows")
  project (aot-compiler)
else()
  project (aot-compiler C ASM CXX)
  enable_language (ASM_MASM)
  add_definitions(-DCOMPILING_WASM_RUNTIME_API=1)
endif()

set (CMAKE_CXX_STANDARD 14)

if (NOT DEFINED WAMR_BUILD_PLATFORM)
  set (WAMR_BUILD_PLATFORM "linux")
endif()

# Reset default linker flags
set (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
set (CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")

add_definitions(-DWASM_ENABLE_INTERP=1)
add_definitions(-DWASM_ENABLE_WAMR_COMPILER=1)
add_definitions(-DWASM_ENABLE_BULK_MEMORY=1)
add_definitions(-DWASM_DISABLE_HW_BOUND_CHECK=1)
add_definitions(-DWASM_ENABLE_SHARED_MEMORY=1)
add_definitions(-DWASM_ENABLE_THREAD_MGR=1)
add_definitions(-DWASM_ENABLE_TAIL_CALL=1)
add_definitions(-DWASM_ENABLE_SIMD=1)
add_definitions(-DWASM_ENABLE_REF_TYPES=1)
add_definitions(-DWASM_ENABLE_CUSTOM_NAME_SECTION=1)
add_definitions(-DWASM_ENABLE_DUMP_CALL_STACK=1)
add_definitions(-DWASM_ENABLE_PERF_PROFILING=1)

# Set WAMR_BUILD_TARGET, currently values supported:
# "X86_64", "AMD_64", "X86_32", "ARM_32", "MIPS_32", "XTENSA_32"
if (NOT WAMR_BUILD_TARGET)
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Build as X86_64 by default in 64-bit platform
    set (WAMR_BUILD_TARGET "X86_64")
  else ()
    # Build as X86_32 by default in 32-bit platform
    set (WAMR_BUILD_TARGET "X86_32")
  endif ()
  if (WAMR_BUILD_PLATFORM STREQUAL "windows")
    if (("${CMAKE_GENERATOR_PLATFORM}" STREQUAL "Win32"))
      set (WAMR_BUILD_TARGET "X86_32")
    endif()
  endif()
endif ()

string(TOUPPER ${WAMR_BUILD_TARGET} WAMR_BUILD_TARGET)

# Add definitions for the build target
if (WAMR_BUILD_TARGET STREQUAL "X86_64")
  add_definitions(-DBUILD_TARGET_X86_64)
elseif (WAMR_BUILD_TARGET STREQUAL "AMD_64")
  add_definitions(-DBUILD_TARGET_AMD_64)
elseif (WAMR_BUILD_TARGET STREQUAL "X86_32")
  add_definitions(-DBUILD_TARGET_X86_32)
elseif (WAMR_BUILD_TARGET MATCHES "AARCH64.*")
  add_definitions(-DBUILD_TARGET_AARCH64)
  add_definitions(-DBUILD_TARGET="${WAMR_BUILD_TARGET}")
elseif (WAMR_BUILD_TARGET MATCHES "ARM.*")
  add_definitions(-DBUILD_TARGET_ARM)
  add_definitions(-DBUILD_TARGET="${WAMR_BUILD_TARGET}")
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV64" OR WAMR_BUILD_TARGET STREQUAL "RISCV64_LP64D")
  add_definitions(-DBUILD_TARGET_RISCV64_LP64D)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV64_LP64")
  add_definitions(-DBUILD_TARGET_RISCV64_LP64)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV32" OR WAMR_BUILD_TARGET STREQUAL "RISCV32_ILP32D")
  add_definitions(-DBUILD_TARGET_RISCV32_ILP32D)
elseif (WAMR_BUILD_TARGET STREQUAL "RISCV32_ILP32")
  add_definitions(-DBUILD_TARGET_RISCV32_ILP32)
else ()
  message (FATAL_ERROR "-- Build target isn't set")
endif ()

message ("-- Build as target ${WAMR_BUILD_TARGET}")

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    if (WAMR_BUILD_TARGET STREQUAL "X86_64" OR WAMR_BUILD_TARGET STREQUAL "AMD_64"
        OR WAMR_BUILD_TARGET MATCHES "AARCH64.*" OR WAMR_BUILD_TARGET MATCHES "RISCV64.*")
    if (NOT WAMR_BUILD_PLATFORM STREQUAL "windows")
      # Add -fPIC flag if build as 64-bit
      set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIC")
      set (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "${CMAKE_SHARED_LIBRARY_LINK_C_FLAGS} -fPIC")
    endif ()
  else ()
    add_definitions (-m32)
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m32")
    set (CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
  endif ()
endif ()

if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif (NOT CMAKE_BUILD_TYPE)
message ("-- CMAKE_BUILD_TYPE = " ${CMAKE_BUILD_TYPE})

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DBH_DEBUG=1)
endif ()

# Enable LLVM
set (LLVM_SRC_ROOT "${PROJECT_SOURCE_DIR}/../core/deps/llvm")
if (WAMR_BUILD_PLATFORM STREQUAL "windows")
  if (NOT EXISTS "${LLVM_SRC_ROOT}/win32build")
    message (FATAL_ERROR "Cannot find LLVM dir: ${LLVM_SRC_ROOT}/win32build")
  endif ()
  set (CMAKE_PREFIX_PATH "${LLVM_SRC_ROOT}/win32build;${CMAKE_PREFIX_PATH}")
else()
  if (NOT EXISTS "${LLVM_SRC_ROOT}/build")
    message (FATAL_ERROR "Cannot find LLVM dir: ${LLVM_SRC_ROOT}/build")
  endif ()
  set (CMAKE_PREFIX_PATH "${LLVM_SRC_ROOT}/build;${CMAKE_PREFIX_PATH}")
endif ()

find_package(LLVM REQUIRED CONFIG)
include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

if (NOT (CMAKE_C_COMPILER MATCHES ".*clang.*" OR CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
  if(NOT MSVC)
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
  else()
    set (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
  endif()
endif()

if (NOT MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wformat -Wformat-security \
                                       -ffunction-sections -fdata-sections \
                                       -Wno-unused-parameter -Wno-pedantic")
  # Remove the extra spaces for better make log
  string (REGEX REPLACE "  *" " " CMAKE_C_FLAGS ${CMAKE_C_FLAGS})
endif()

set (SHARED_DIR ../core/shared)
set (IWASM_DIR ../core/iwasm)
set (APP_FRAMEWORK_DIR ../core/app-framework)

include_directories (${SHARED_DIR}/include
                     ${IWASM_DIR}/include)

enable_language (ASM)

include (${SHARED_DIR}/platform/${WAMR_BUILD_PLATFORM}/shared_platform.cmake)
include (${SHARED_DIR}/mem-alloc/mem_alloc.cmake)
include (${SHARED_DIR}/utils/shared_utils.cmake)
include (${SHARED_DIR}/utils/uncommon/shared_uncommon.cmake)
include (${IWASM_DIR}/libraries/thread-mgr/thread_mgr.cmake)
include (${IWASM_DIR}/libraries/libc-builtin/libc_builtin.cmake)
include (${IWASM_DIR}/common/iwasm_common.cmake)
include (${IWASM_DIR}/interpreter/iwasm_interp.cmake)
include (${IWASM_DIR}/aot/iwasm_aot.cmake)
include (${IWASM_DIR}/compilation/iwasm_compl.cmake)

# set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wconversion -Wsign-conversion")
if (WAMR_BUILD_TARGET MATCHES "X86_.*" OR WAMR_BUILD_TARGET STREQUAL "AMD_64")
  if (NOT (CMAKE_C_COMPILER MATCHES ".*clang.*" OR CMAKE_C_COMPILER_ID MATCHES ".*Clang" OR MSVC))
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mindirect-branch-register")
  endif()
endif ()

if (NOT MSVC)
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fstack-protector-strong --param ssp-buffer-size=4")
endif()
if (NOT (MSVC OR CMAKE_C_COMPILER MATCHES ".*clang.*" OR CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
  set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-z,noexecstack,-z,relro,-z,now")
endif()

# We disable these flags by default to stay the same with wasm runtime
# set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mindirect-branch=thunk -mfunction-return=thunk")

if (NOT MSVC)
  if (NOT (CMAKE_C_COMPILER MATCHES ".*clang.*" OR CMAKE_C_COMPILER_ID MATCHES ".*Clang"))
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pie -fPIE -ftrapv -D_FORTIFY_SOURCE=2")
  else()
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fPIE -ftrapv -D_FORTIFY_SOURCE=2")
  endif()
endif()

# message ("-- CMAKE_C_FLAGS: ${CMAKE_C_FLAGS}")

add_library (vmlib
             ${PLATFORM_SHARED_SOURCE}
             ${MEM_ALLOC_SHARED_SOURCE}
             ${UTILS_SHARED_SOURCE}
             ${UNCOMMON_SHARED_SOURCE}
             ${THREAD_MGR_SOURCE}
             ${LIBC_BUILTIN_SOURCE}
             ${IWASM_COMMON_SOURCE}
             ${IWASM_INTERP_SOURCE}
             ${IWASM_AOT_SOURCE})

add_library (aotclib ${IWASM_COMPL_SOURCE})

add_executable (wamrc main.c)

if (NOT MSVC)
  target_link_libraries (wamrc aotclib vmlib LLVMDemangle ${LLVM_AVAILABLE_LIBS} -lm -ldl -lpthread)
else()
  target_link_libraries (wamrc aotclib vmlib  ${LLVM_AVAILABLE_LIBS})
endif()
