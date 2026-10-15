cmake_minimum_required(VERSION 3.14.0)
project(Emu68 VERSION 1.1.0)
set(PROJECT_VERSION 1.1.0-beta.1)

#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
include(ExternalProject)

set(PPC_ROM_HEADER ${CMAKE_BINARY_DIR}/include/ppc_rom.h)

# ppc_rom needs the ppc cross compiler and only ppc-enable ever uses it, so
# let the build skip it and drop in an empty stub ROM instead.
option(EMU68_SKIP_PPC_ROM "Do not build the PowerPC ROM (use an empty stub)" OFF)
option(EMU68_SKIP_FIRMWARE "Do not download Raspberry Pi firmware files" OFF)
set(AGA_PISTORM "" CACHE PATH "Path to the AGA-PISTORM repository (enables the virtual AGA chipset)")
option(FRF_VIRTUAL_AGA "Standalone virtual AGA Amiga: no physical PiStorm/Amiga bus" OFF)
option(FRF_USB_HOST "FRF standalone Pi3 DWC2 USB host/hub/HID on CPU1 (required for usable Pi3A+ builds)" ON)
option(FRF_VIRTUAL_GAYLE_ABSENT "A1200 Gayle/IDE absent-device model; standalone regression test" OFF)

if(NOT EMU68_SKIP_PPC_ROM)
ExternalProject_Add(
    ppc_rom
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/ppc
    CMAKE_ARGS 
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/toolchains/ppc-linux-gnu.cmake 
        -DCMAKE_INSTALL_PREFIX=${CMAKE_BINARY_DIR}/include
    BUILD_IN_SOURCE 0
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND "" 
    BYPRODUCTS ${PPC_ROM_HEADER}
)
else()
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include)
    file(WRITE ${PPC_ROM_HEADER} "unsigned char ppc_rom_img[] = { 0 };
unsigned int ppc_rom_img_len = 0;
")
endif()

include(cmake/verstring.cmake)
include(cmake/firmware_download.cmake)
include(cmake/devicetree.cmake)

find_program(GZIP gzip REQUIRED)

get_verstring(VERSTRING)
get_git_sha(GIT_SHA)

set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 23)

set(CONTEXT_RESERVE_FLAGS 
        "-ffixed-q19 -ffixed-v19 -ffixed-d19 -ffixed-q20 -ffixed-v20 \
         -ffixed-d20 -ffixed-q21 -ffixed-v21 -ffixed-d21 -ffixed-v22 \
         -ffixed-d22 -ffixed-v23 -ffixed-d23 -ffixed-v24 -ffixed-d24 \
         -ffixed-v25 -ffixed-d25 -ffixed-v26 -ffixed-d26")

set(M68K_FIXED_REGS 
        "-ffixed-x12 -ffixed-x18 -ffixed-x19 -ffixed-x20 -ffixed-x21 \
         -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 \
         -ffixed-x13 -ffixed-x14 -ffixed-x15 -ffixed-x16 -ffixed-x17 \
         -ffixed-x27 -ffixed-x28 -ffixed-x29")

set(M68K_SAVED_TEMPS
        "-fcall-saved-x4 -fcall-saved-x5 -fcall-saved-x6 -fcall-saved-x7 \
         -fcall-saved-x8 -fcall-saved-x9 -fcall-saved-x10 -fcall-saved-x11"
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CONTEXT_RESERVE_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CONTEXT_RESERVE_FLAGS}")
    
    # Optional: Add to all subdirectories
#add_compile_options(${CONTEXT_RESERVE_FLAGS})

set(SUPPORTED_TARGETS "raspi64" "pbpro" "rockpro64" "virt")
set(TARGET "raspi64" CACHE STRING "One of target machines: ${SUPPORTED_TARGETS}")
set_property(CACHE TARGET PROPERTY STRINGS ${SUPPORTED_TARGETS})

set(SUPPORTED_VARIANTS "none" "pistorm" "pistorm-classic")
set(VARIANT "none" CACHE STRING "Compilation variant: ${SUPPORTED_VARIANTS}")
set_property(CACHE VARIANT PROPERTY STRINGS ${SUPPORTED_VARIANTS})

set(ARCH_FILES "")
set(TARGET_FILES "")
set(BASE_FILES
    src/support.c
    src/tlsf.c
    src/devicetree.c
    src/cpp_support.cpp
    src/HunkLoader.c
    src/ElfLoader.c
    src/md5.c
    src/disasm.c
    src/findtoken.c
    src/cache.c
)

set(EMU68_FILES
    src/M68k_Translator.c
    src/M68k_SR.c
    src/M68k_MULDIV.c
    src/M68k_MOVE.c
    src/M68k_EA.c
    src/M68k_LINE0.c
    src/M68k_LINE4.c
    src/M68k_LINE5.c
    src/M68k_LINE6.c
    src/M68k_LINE8.c
    src/M68k_LINE9.c
    src/M68k_LINEB.c
    src/M68k_LINEC.c
    src/M68k_LINED.c
    src/M68k_LINEE.c
    src/M68k_LINEF.c
    src/M68k_Exception.c
    src/M68k_ExceptionEntry.c
    src/M68k_CC.c
    src/ExecutionLoop.c
    src/TranslatorContext.cpp
    src/PPC_Translator.cpp
    src/PPC_ExecutionLoop.cpp
    src/PPC_TranslatorContext.cpp
    src/PPC_LoadStore.cpp
    src/PPC_FPU.cpp
    src/PPC_SystemInstructions.cpp
    src/PPC_Arithmetic.cpp
    src/LRUCache.cpp
    src/ReturnStack.cpp
    
    src/math/__rem_pio2.c
    src/math/__rem_pio2_large.c
    src/math/__tan.c
    src/math/__sin.c
    src/math/__cos.c
    src/math/__expo2.c
    src/math/atan.c
    src/math/atanh.c
    src/math/acos.c
    src/math/asin.c
    src/math/tan.c
    src/math/tanh.c
    src/math/scalbn.c
    src/math/floor.c
    src/math/cosh.c
    src/math/exp.c
    src/math/exp10.c
    src/math/exp2.c
    src/math/expm1.c
    src/math/log.c
    src/math/log1p.c
    src/math/log10.c
    src/math/log2.c
    src/math/sinh.c
    src/math/pow.c
    src/math/modf.c
    src/math/sincos.c
    src/math/sin.c
    src/math/cos.c
    src/math/remquo.c
    src/math/96bit.c
)

set(OVERLAY_FILES
    src/overlays/emu68.dts
    src/overlays/diagnostic.dts
    src/overlays/unicam.dts
    src/overlays/emmc.dts
    src/overlays/sdhc.dts
    src/overlays/z2ram.dts
    src/overlays/pal.dts
    src/overlays/ntsc.dts
    src/overlays/noscsi.dts
)

build_devicetree(OVERLAY_FILES)

set_source_files_properties(src/math/96bit.c PROPERTIES COMPILE_FLAGS 
    "${M68K_SAVED_TEMPS} ${M68K_FIXED_REGS} ${CONTEXT_RESERVE_FLAGS}")
set_source_files_properties(src/math/__rem_pio2_large.c PROPERTIES COMPILE_FLAGS
    "-Wno-error=maybe-uninitialized ${M68K_SAVED_TEMPS} ${M68K_FIXED_REGS} ${CONTEXT_RESERVE_FLAGS}")
set_source_files_properties(src/math/__rem_pio2.c PROPERTIES COMPILE_FLAGS
    "-Wno-error=maybe-uninitialized ${M68K_SAVED_TEMPS} ${M68K_FIXED_REGS} ${CONTEXT_RESERVE_FLAGS}")

# ExecutionLoop breaks the C ABI for aarch64. All m68k registers are fixed (x13-x29)
# The temporary arm code and PC are fixed too (x12, x18)
set_source_files_properties(src/ExecutionLoop.c PROPERTIES COMPILE_FLAGS 
    "${M68K_FIXED_REGS} ${CONTEXT_RESERVE_FLAGS}")


# TODO: PPC need a check which regs need really to be saved here...
set_source_files_properties(src/PPC_Translator.cpp PROPERTIES COMPILE_FLAGS 
    "-fcall-used-x4 -fcall-used-x5 -fcall-used-x6 -fcall-used-x7 -fcall-used-x8 \
     -fcall-used-x9 -fcall-used-x10 -fcall-used-x11 -ffixed-x12 -ffixed-x18  \
     -ffixed-x19 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 \
     -ffixed-x27 -ffixed-x28 -ffixed-x29 ${CONTEXT_RESERVE_FLAGS}")

set_source_files_properties(src/PPC_ExecutionLoop.cpp PROPERTIES COMPILE_FLAGS 
    "-ffixed-x12 -ffixed-x18  \
     -ffixed-x19 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 \
     -ffixed-x27 -ffixed-x28 -ffixed-x29 ${CONTEXT_RESERVE_FLAGS}")


set_source_files_properties(src/LRUCache.cpp PROPERTIES COMPILE_FLAGS 
    "-ffixed-x12 -ffixed-x18  \
     -ffixed-x19 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 \
     -ffixed-x27 -ffixed-x28 -ffixed-x29 ${CONTEXT_RESERVE_FLAGS}")

# Exception entry may be called from translated code directly so it breaks the C ABI for aarch64. 
# All m68k registers are fixed (x13-x29)
# The temporary arm code and PC are fixed too (x12, x18)
set_source_files_properties(src/M68k_ExceptionEntry.c PROPERTIES COMPILE_FLAGS 
    "${M68K_SAVED_TEMPS} ${M68K_FIXED_REGS} ${CONTEXT_RESERVE_FLAGS}")

# ExecutionLoop breaks the C ABI for aarch64. All m68k registers are fixed (x13-x29)
# The temporary arm code and PC are fixed too (x12, x18)
set_source_files_properties(src/aarch64/vectors.c PROPERTIES COMPILE_FLAGS 
     "-ffixed-x19 -ffixed-x20 -ffixed-x21 -ffixed-x22 -ffixed-x23 -ffixed-x24 -ffixed-x25 -ffixed-x26 \
      -ffixed-x27 -ffixed-x28 -ffixed-x29 ${CONTEXT_RESERVE_FLAGS}")

if (${TARGET} IN_LIST SUPPORTED_TARGETS)
    message("-- Selected target machine: ${TARGET}")
    if(${TARGET} STREQUAL "raspi64")
        list(APPEND TARGET_FILES src/raspi/start_rpi64.c)
        list(APPEND TARGET_FILES src/raspi/support_rpi.c)
        list(APPEND TARGET_FILES src/raspi/topaz.c)
        if(NOT EMU68_SKIP_FIRMWARE)
            download_raspi_firmware()
        endif()
        if(${VARIANT} STREQUAL "pistorm" OR ${VARIANT} STREQUAL "pistorm-classic")
            configure_file(scripts/config_pistorm.txt scripts/config.txt @ONLY)
            install(FILES ${CMAKE_BINARY_DIR}/Emu68.img.gz DESTINATION . RENAME Emu68-${VARIANT}.gz)
            install(FILES ${CMAKE_BINARY_DIR}/scripts/config.txt DESTINATION .)
            install(FILES ${CMAKE_SOURCE_DIR}/documentation/overlays.md DESTINATION overlays/)
        else()
            install(FILES ${CMAKE_BINARY_DIR}/Emu68.img.gz DESTINATION .)
            install(FILES ${CMAKE_SOURCE_DIR}/scripts/config.txt DESTINATION .)
        endif()
        add_compile_definitions(RASPI)
    elseif(${TARGET} STREQUAL "pbpro")
        list(APPEND TARGET_FILES
            src/pine64/start_pbpro.c
            src/pine64/support_pbpro.c
        )
        install(FILES ${CMAKE_SOURCE_DIR}/blobs/rk3399-pinebookpro.dtb DESTINATION .)
        install(FILES ${CMAKE_BINARY_DIR}/Emu68.img.gz DESTINATION .)
        install(FILES ${CMAKE_SOURCE_DIR}/scripts/extlinux.conf DESTINATION extlinux/)
    elseif(${TARGET} STREQUAL "virt")
        list(APPEND TARGET_FILES
            src/virt/start_virt.c
            src/virt/support_virt.c
        )
        install(FILES ${CMAKE_BINARY_DIR}/Emu68.img.gz DESTINATION .)
    endif()

    set(TARGET_ARCH "aarch64")
else()
    message(FATAL_ERROR "Wrong target machine specified: ${TARGET}")
endif()

if (${VARIANT} IN_LIST SUPPORTED_VARIANTS)
    message("-- Selected variant: ${VARIANT}")
    if(${VARIANT} STREQUAL "pistorm" OR ${VARIANT} STREQUAL "pistorm-classic")
        if(${TARGET} STREQUAL "raspi64")
            add_compile_definitions(PISTORM_ANY_MODEL)
            if(${VARIANT} STREQUAL "pistorm")
                add_compile_definitions(PISTORM)
                list(APPEND BASE_FILES
                    src/pistorm/ps_protocol.c
                )
            else()
                add_compile_definitions(PISTORM_CLASSIC)
                list(APPEND BASE_FILES
                    src/pistorm/ps_classic_protocol.c
                    src/pistorm/cpld.c
                    src/pistorm/libxsvf/xsvf.c
                    src/pistorm/libxsvf/tap.c
                    src/pistorm/libxsvf/svf.c
                    src/pistorm/libxsvf/statename.c
                    src/pistorm/libxsvf/scan.c
                    src/pistorm/libxsvf/play.c
                )
            endif()
            include_directories(src/pistorm)
            list(APPEND BASE_FILES
                src/boards/z2ram.c
                src/boards/emu68rom.c
            )
        else()
            message(FATAL_ERROR "PiStorm variants are supported on raspi targets, only.")
        endif()
    endif()
else()
    message(FATAL_ERROR "Wrong variant selected: ${VARIANT}")
endif()

add_link_options(-Wl,--build-id -nostdlib -nostartfiles -static)
add_compile_options(-mbig-endian -fno-exceptions -fno-unwind-tables -fno-stack-protector -fno-asynchronous-unwind-tables -fno-pic -fno-pie -no-pie -ffreestanding -Wall -Wextra -Werror)

list(APPEND ARCH_FILES
    src/aarch64/start.c
    src/aarch64/mmu.c
    src/aarch64/RegisterAllocator64.c
    src/aarch64/vectors.c
    src/aarch64/intc.c
)

set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/scripts/ldscript-be64.lds)
add_link_options(-Wl,-EB -Wl,-Map -Wl,${CMAKE_BINARY_DIR}/Emu68.map -Wl,--format=elf64-bigaarch64 -T ${LINKER_SCRIPT})
add_compile_options(-falign-functions=32 -march=armv8-a+crc -mtune=cortex-a76 -fomit-frame-pointer -O3 -ffixed-x12)

if (CMAKE_COMPILER_IS_GNUCC AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 10.0)
    add_compile_options(-mno-outline-atomics)
endif()

add_executable(Emu68.elf
    ${ARCH_FILES}
    ${TARGET_FILES}
    ${BASE_FILES}
    ${EMU68_FILES}
)

if(FRF_VIRTUAL_GAYLE_ABSENT)
    if(NOT FRF_VIRTUAL_AGA OR NOT "${TARGET}" STREQUAL "raspi64")
        message(FATAL_ERROR "Virtual Gayle requires standalone Pi AArch64")
    endif()
    target_sources(Emu68.elf PRIVATE ${CMAKE_SOURCE_DIR}/src/frf_native_hw/frf_native_bus.c)
    target_include_directories(Emu68.elf PRIVATE ${CMAKE_SOURCE_DIR}/src/frf_native_hw)
    target_compile_definitions(Emu68.elf PRIVATE FRF_VIRTUAL_GAYLE_ABSENT=1)
endif()

target_compile_options(Emu68.elf
  PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti -fno-exceptions>
)

add_custom_command(
    TARGET Emu68.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -v -O binary "${CMAKE_BINARY_DIR}/Emu68.elf" "${CMAKE_BINARY_DIR}/Emu68.img"
    COMMAND ${GZIP} -f "${CMAKE_BINARY_DIR}/Emu68.img"
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

if(NOT EMU68_SKIP_PPC_ROM)
    add_dependencies(Emu68.elf ppc_rom)
endif()

# FRF standalone USB: USPi DWC2 host adapted to Emu68's big-endian AArch64
# kernel, high virtual addresses, interrupt routing and TLSF allocator.
if(FRF_VIRTUAL_AGA)
    target_sources(Emu68.elf PRIVATE ${CMAKE_SOURCE_DIR}/src/boards/frf_z3ram.c)
endif()

if(FRF_USB_HOST)
    if(NOT "${TARGET}" STREQUAL "raspi64")
        message(FATAL_ERROR "FRF_USB_HOST currently supports Raspberry Pi only")
    endif()
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/uspi/include/uspi.h"
       OR NOT EXISTS "${CMAKE_SOURCE_DIR}/third_party/uspi/include/uspios.h")
        message(FATAL_ERROR
            "FRF_USB_HOST=ON (the usable Pi3A+ default), but the adapted USPi "
            "host sources are missing from third_party/uspi/. "
            "This public tree does not bundle USPi due to incompatible "
            "redistribution licences for the statically linked firmware. "
            "To reproduce your private tested build, provide the matching "
            "adapted USPi include/ and lib/ source under "
            "machine/Emu68/third_party/uspi/. "
            "FRF_USB_HOST=OFF builds only a diagnostic image WITHOUT USB input.")
    endif()
    file(GLOB FRF_USPI_SRC ${CMAKE_SOURCE_DIR}/third_party/uspi/lib/*.c)
    if(NOT FRF_USPI_SRC)
        message(FATAL_ERROR
            "FRF_USB_HOST=ON but third_party/uspi/lib contains no C sources. "
            "The build cannot provide keyboard/mouse/gamepad input.")
    endif()
    set(FRF_USB_ENV
        ${CMAKE_SOURCE_DIR}/src/frf_usb/frf_uspi_env.c
        ${CMAKE_SOURCE_DIR}/src/frf_usb/frf_usb.c
    )
    target_sources(Emu68.elf PRIVATE ${FRF_USPI_SRC} ${FRF_USB_ENV})
    target_include_directories(Emu68.elf PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/uspi/include
        ${CMAKE_SOURCE_DIR}/src/frf_usb
    )
    target_compile_definitions(Emu68.elf PRIVATE FRF_USB_HOST=1)
    set_source_files_properties(${FRF_USPI_SRC} ${CMAKE_SOURCE_DIR}/src/frf_usb/frf_uspi_env.c
        PROPERTIES
        COMPILE_DEFINITIONS "FRF_USPI_EMU68=1;AARCH64=1;RASPPI=3;NDEBUG=1"
        COMPILE_FLAGS "-Wno-error -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-type-limits"
    )
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/frf_usb/frf_usb.c
        PROPERTIES COMPILE_FLAGS "-Wno-error")
endif()

# AGA-PISTORM: virtual AGA chipset core + glue
if(AGA_PISTORM)
    file(GLOB AGA_CORE_SRC ${AGA_PISTORM}/core/src/*.c)
    target_sources(Emu68.elf PRIVATE ${AGA_CORE_SRC} ${AGA_PISTORM}/arm/aga_glue.c ${AGA_PISTORM}/arm/aga_hvs.c ${AGA_PISTORM}/arm/aga_audio.c ${AGA_PISTORM}/arm/aga_diag.c ${AGA_PISTORM}/arm/aga_rgbout.c ${AGA_PISTORM}/arm/aga_hdmi.c)
    target_include_directories(Emu68.elf PRIVATE ${AGA_PISTORM}/core/include ${AGA_PISTORM}/arm)
    target_compile_definitions(Emu68.elf PRIVATE AGA_PISTORM=1)
    if(FRF_VIRTUAL_AGA)
        target_compile_definitions(Emu68.elf PRIVATE FRF_VIRTUAL_AGA=1)
    endif()
    # AGA_DIAG: the per-trap counters. Do NOT try to pass this as
    # -DCMAKE_C_FLAGS=-DAGA_DIAG - the project sets CMAKE_C_FLAGS itself and
    # silently overwrites whatever you hand in. Bit me once: the 'diag' kernel
    # came out byte-for-byte as uninstrumented as the shipping one, and
    # traps_read/traps_write sat at 0 through every damn measurement I took
    # with it.
    option(AGA_DIAG "AGA-PISTORM: per-trap diagnostic counters" OFF)
    if(AGA_DIAG)
        target_compile_definitions(Emu68.elf PRIVATE AGA_DIAG=1)
    endif()
    # AGA_PI3: the Raspberry Pi 3 kernel - VideoCore IV display lists, chip RAM
    # through the MMU, a longer audio lead and frame skip. Without it you get
    # the Pi 4 / CM4 kernel. Both detect the Pi they run on and report it.
    option(AGA_PI3 "AGA-PISTORM: the Raspberry Pi 3 kernel" OFF)
    if(AGA_PI3)
        target_compile_definitions(Emu68.elf PRIVATE AGA_PI3=1)
        # Tuned for the Pi 3's in-order Cortex-A53, not the out-of-order A76
        # Emu68 aims at (RGB2RTG 0.75: most of Jim Power's 41 -> 50 fps on a
        # Pi 3). Last -mtune wins over add_compile_options'.
        target_compile_options(Emu68.elf PRIVATE -mtune=cortex-a53)
    endif()
    # GCC 14.2's vectorisers are fucking broken on big-endian AArch64. Proved
    # under qemu-aarch64_be with Ubuntu's gcc-14 and Arm's 14.2.rel1 on
    # 2026-10-10:
    #  - tuned for the A53, the loop and SLP vectorisers together swapped
    #    words 2/3 and 7/8 of aga_sharp.h's area kernel - the Pi 3's Sharp
    #    picture looked like hell;
    #  - at every tuning (A53, A72, A76) the loop vectoriser turned a
    #    left/right peak loop over interleaved stereo frames into one that
    #    reports the right channel's peak for both (host/tests/test_audio).
    # Arm GNU Toolchain 15.3.rel2 builds both right - host tests and
    # regression frames pass with it big-endian at both tunings, and
    # tools/build-wsl.sh uses it (toolchains/aarch64_be-arm-gnu.cmake) for
    # the classic kernels. The PiStorm16 kernel stays on Ubuntu's gcc-14 as
    # it always was. On an older GCC the Pi 3 kernel builds without the
    # vectoriser - picture right, just slower.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 15.0)
        message(WARNING "AGA-PISTORM: GCC ${CMAKE_C_COMPILER_VERSION} miscompiles big-endian vector code - use the Arm GNU Toolchain 15.3")
        if(AGA_PI3)
            target_compile_options(Emu68.elf PRIVATE -fno-tree-vectorize)
        endif()
    endif()
    # AGA_PROBES: the per-frame measurements - lock and chunk timing, PMU cache
    # counters, the memory probe, real Paula reads, audio and register
    # captures, the lag histogram, the Pi 3 display-list watch. Test kernels
    # only; the published ones are built without that crap.
    option(AGA_PROBES "AGA-PISTORM: per-frame measurements for test kernels" OFF)
    if(AGA_PROBES OR AGA_DIAG)
        target_compile_definitions(Emu68.elf PRIVATE AGA_PROBES=1)
    endif()
    set_source_files_properties(${AGA_CORE_SRC} ${AGA_PISTORM}/arm/aga_glue.c ${AGA_PISTORM}/arm/aga_hvs.c ${AGA_PISTORM}/arm/aga_audio.c ${AGA_PISTORM}/arm/aga_diag.c ${AGA_PISTORM}/arm/aga_rgbout.c ${AGA_PISTORM}/arm/aga_hdmi.c
        PROPERTIES COMPILE_FLAGS "-Wno-error -Wno-unused-parameter -Wno-unused-function")
endif()

add_subdirectory(external)

target_link_libraries(Emu68.elf capstone_static libdeflate_static)
target_include_directories(Emu68.elf PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/external/capstone/include)
target_include_directories(Emu68.elf PRIVATE ${CMAKE_BINARY_DIR}/include)
target_compile_definitions(Emu68.elf PRIVATE 
    EMU68_VERSION_MAJOR=${CMAKE_PROJECT_VERSION_MAJOR}
    EMU68_VERSION_MINOR=${CMAKE_PROJECT_VERSION_MINOR}
)

configure_file(include/version.h.in include/version.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/.git/index)
