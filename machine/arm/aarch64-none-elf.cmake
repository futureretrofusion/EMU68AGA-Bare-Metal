# CMake toolchain file for building Emu68 with the Arm GNU aarch64-none-elf
# toolchain installed at C:/Tools/aarch64 (Windows host).
# SPDX-License-Identifier: GPL-2.0-only
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(TOOLCHAIN_DIR "C:/Tools/aarch64/bin")
set(CROSS_COMPILE ${TOOLCHAIN_DIR}/aarch64-none-elf)

set(CMAKE_C_COMPILER ${CROSS_COMPILE}-gcc.exe)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}-g++.exe)
set(CMAKE_AR ${CROSS_COMPILE}-ar.exe)
set(CMAKE_RANLIB ${CROSS_COMPILE}-ranlib.exe)
set(CMAKE_OBJCOPY ${CROSS_COMPILE}-objcopy.exe)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
