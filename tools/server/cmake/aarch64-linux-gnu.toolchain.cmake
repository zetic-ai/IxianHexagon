# Cross-compile ixian-server for the Dragonwing IQ9 (aarch64 Ubuntu 24.04) from an x86-64 Debian / Ubuntu host:
#
#   sudo apt install g++-aarch64-linux-gnu
#   cmake -S tools/server -B build-aarch64 \
#       -DCMAKE_TOOLCHAIN_FILE=tools/server/cmake/aarch64-linux-gnu.toolchain.cmake \
#       -DIXIAN_SDK_DIR=/path/to/ixian-hexagon-<ver>-linux-arm64-qcom-iq9
#   cmake --build build-aarch64 -j
#
# The host toolchain's glibc / libstdc++ must not be newer than the board's (Debian 12: glibc 2.36, GCC 12; the IQ9's
# Ubuntu 24.04 ships glibc 2.39 and GCC 13's libstdc++, so this works). libcdsprpc.so.1 only exists on the board; the
# build links a stub (IXIAN_SERVER_CDSPRPC_STUB) that is not installed.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
