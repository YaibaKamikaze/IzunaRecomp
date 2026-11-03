# [IzunaRecomp] The Windows build, made on Linux (tools/windows/README.md):
# Clang 19 targeting x86_64-pc-windows-msvc, LLVM's linker, resource compiler,
# librarian and manifest tool, against a copy of MSVC's headers and libraries
# and the Windows SDK (the player's own Build Tools, copied by
# copy-winsdk.ps1; made case-insensitive by case_links.py).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(IZUNA_WINSDK "$ENV{HOME}/ng2-ps5-deps/winsdk" CACHE PATH "The copied msvc/ and sdk/")
set(IZUNA_WINTC_BIN "$ENV{HOME}/ng2-ps5-deps/tools/wintc/bin" CACHE PATH "lld-link, llvm-rc, llvm-lib, llvm-mt")
file(GLOB _izuna_sdk_versions LIST_DIRECTORIES true "${IZUNA_WINSDK}/sdk/Include/10.*")
list(SORT _izuna_sdk_versions)
list(GET _izuna_sdk_versions -1 _izuna_sdk)
get_filename_component(IZUNA_WINSDK_VERSION "${_izuna_sdk}" NAME)

set(CMAKE_C_COMPILER clang-19)
set(CMAKE_CXX_COMPILER clang++-19)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_RC_COMPILER "${IZUNA_WINTC_BIN}/llvm-rc")
set(CMAKE_AR "${IZUNA_WINTC_BIN}/llvm-ar")  # (CMake drives it as ar; it writes COFF .lib archives)
set(CMAKE_LINKER "${IZUNA_WINTC_BIN}/lld-link")
set(CMAKE_MT "${IZUNA_WINTC_BIN}/llvm-mt")

set(_izuna_ms "-Xmicrosoft-visualc-tools-root ${IZUNA_WINSDK}/msvc -Xmicrosoft-windows-sdk-root ${IZUNA_WINSDK}/sdk -Xmicrosoft-windows-sdk-version ${IZUNA_WINSDK_VERSION}")
# The CPU level (IZUNA_MARCH in the environment, build.sh): x86-64-v3 for
# ng2.exe (AVX2, FMA, BMI1/2, MOVBE: Intel since 2013, every AMD Ryzen - the
# recompiled game's fused multiply-adds become instructions, as on the PS5),
# x86-64-v2 for ng2-compat.exe (SSE4.2, POPCNT: every PC CPU since about 2009;
# the SDK's SSSE3 / SSE4.1 paths need at least that). ng2.exe hands an older
# CPU to ng2-compat.exe before any of its code runs (izuna_cpu_redirect.cpp).
set(IZUNA_MARCH "$ENV{IZUNA_MARCH}")
if(NOT IZUNA_MARCH)
  set(IZUNA_MARCH x86-64-v3)
endif()
set(CMAKE_C_FLAGS_INIT "${_izuna_ms} -march=${IZUNA_MARCH}")
set(CMAKE_CXX_FLAGS_INIT "${_izuna_ms} -march=${IZUNA_MARCH}")
# (CMake passes -fuse-ld=lld-link: build.sh puts IZUNA_WINTC_BIN on PATH)
foreach(kind EXE SHARED MODULE)
  set(CMAKE_${kind}_LINKER_FLAGS_INIT "${_izuna_ms}")
endforeach()
set(_izuna_inc "${IZUNA_WINSDK}/sdk/Include/${IZUNA_WINSDK_VERSION}")
set(CMAKE_RC_FLAGS_INIT "-I ${_izuna_inc}/um -I ${_izuna_inc}/shared -I ${_izuna_inc}/ucrt -I ${IZUNA_WINSDK}/msvc/include")

# The C and C++ runtime linked into ng2.exe (libcmt, libcpmt, libucrt): the
# game runs without the Visual C++ Redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
