# AGA-PISTORM: Arm GNU Toolchain for big-endian AArch64 (glibc), 15.3.rel2.
# GCC 14.2 - Ubuntu's gcc-14 and Arm's 14.2.rel1 alike - gets big-endian
# vector code wrong; see the AGA block in CMakeLists.txt. ARM_GNU_TOOLCHAIN
# points elsewhere if yours is unpacked somewhere else; the default is where
# tools/build-wsl.sh puts it.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{ARM_GNU_TOOLCHAIN})
    set(ARM_GNU $ENV{ARM_GNU_TOOLCHAIN})
else()
    set(ARM_GNU $ENV{HOME}/toolchains/armtc-15.3.rel2/arm-gnu-toolchain-15.3.rel2-x86_64-aarch64_be-none-linux-gnu)
endif()
set(CROSS_COMPILE ${ARM_GNU}/bin/aarch64_be-none-linux-gnu)

set(CMAKE_C_COMPILER ${CROSS_COMPILE}-gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}-g++)
set(CMAKE_AR ${CROSS_COMPILE}-ar)
set(CMAKE_RANLIB ${CROSS_COMPILE}-ranlib)
set(CMAKE_OBJCOPY ${CROSS_COMPILE}-objcopy)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Ubuntu's gcc-14, which upstream builds with, turns _FORTIFY_SOURCE on by
# itself: glibc's headers then call printf __printf_chk and sprintf
# __sprintf_chk - the two names Emu68 supplies (support_rpi.c, support.c).
# Arm's GCC does not, and plain printf (capstone's Mapping.c) is undefined.
set(CMAKE_C_FLAGS_INIT "-D_FORTIFY_SOURCE=3")
set(CMAKE_CXX_FLAGS_INIT "-D_FORTIFY_SOURCE=3")
