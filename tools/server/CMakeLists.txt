# ixian-server: an llama-server compatible HTTP server on the Ixian Hexagon SDK.
#
#   # on the IQ9 (or cross-compiling), against a real SDK tarball
#   cmake -S tools/server -B build-server -DIXIAN_SDK_DIR=/path/to/ixian-hexagon-<ver>-linux-arm64-qcom-iq9
#   cmake --build build-server -j
#
#   # on a development host without an NPU: the mock engine implements the SDK headers
#   cmake -S tools/server -B build-mock -DIXIAN_SERVER_MOCK_ENGINE=ON -DIXIAN_SDK_DIR=...
#
# The SDK directory holds include/ (the public headers), lib/libzetic_ixian_hexagon.so, lib/libhexrt_v73_skel.so and
# lib/cmake/IxianHexagon/. When IXIAN_SDK_DIR is not given, the newest tools/server/.sdk/ixian-hexagon-* is used
# (scripts/fetch-sdk.sh puts the release tarball there), and as a last resort find_package(IxianHexagon) searches
# CMAKE_PREFIX_PATH.
cmake_minimum_required(VERSION 3.18)
project(ixian_hexagon_server VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(IXIAN_SERVER_MOCK_ENGINE "Build against the mock engine (development host, no NPU, no SDK library)" OFF)
option(IXIAN_SERVER_OPENSSL     "Enable HTTPS (--ssl-key-file / --ssl-cert-file) with OpenSSL when it is found" OFF)
option(IXIAN_SERVER_BUILD_TESTS "Build the C++ self-tests (mock engine, media helpers)" ON)
option(IXIAN_SERVER_CDSPRPC_STUB "When cross-compiling, link against a stub libcdsprpc.so.1 (the real one lives on the board)" ON)
option(IXIAN_SERVER_WERROR      "Treat warnings as errors in the server sources" OFF)
set(IXIAN_SDK_DIR "" CACHE PATH "Ixian Hexagon SDK directory (include/, lib/)")

find_package(Threads REQUIRED)

#
# SDK location
#
if(NOT IXIAN_SDK_DIR)
    file(GLOB _ixsrv_sdk_candidates LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/.sdk/ixian-hexagon-*")
    if(_ixsrv_sdk_candidates)
        list(SORT _ixsrv_sdk_candidates COMPARE NATURAL ORDER DESCENDING)
        list(GET _ixsrv_sdk_candidates 0 IXIAN_SDK_DIR)
        message(STATUS "ixian-server: using SDK from ${IXIAN_SDK_DIR} (tools/server/.sdk)")
    endif()
endif()
if(IXIAN_SDK_DIR)
    get_filename_component(IXIAN_SDK_DIR "${IXIAN_SDK_DIR}" REALPATH)
    if(NOT EXISTS "${IXIAN_SDK_DIR}/include/zetic_llm.h")
        message(FATAL_ERROR "ixian-server: ${IXIAN_SDK_DIR}/include/zetic_llm.h not found; IXIAN_SDK_DIR must point at an extracted SDK tarball")
    endif()
    set(IXIAN_SDK_INCLUDE_DIR "${IXIAN_SDK_DIR}/include")
    # the SDK version, from the header
    file(STRINGS "${IXIAN_SDK_INCLUDE_DIR}/zetic_types.h" _ixsrv_sdk_version_line REGEX "#define ZETIC_IXIAN_HEXAGON_VERSION \"")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" IXIAN_SDK_VERSION "${_ixsrv_sdk_version_line}")
    message(STATUS "ixian-server: SDK version ${IXIAN_SDK_VERSION}")
else()
    set(IXIAN_SDK_VERSION "unknown")
endif()

#
# build info (git of the IxianHexagon repository)
#
set(IXSRV_BUILD_NUMBER 0)
set(IXSRV_BUILD_COMMIT "unknown")
find_package(Git QUIET)
if(Git_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _ixsrv_count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc1)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT_VARIABLE _ixsrv_commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _rc2)
    if(_rc1 EQUAL 0 AND _rc2 EQUAL 0)
        set(IXSRV_BUILD_NUMBER "${_ixsrv_count}")
        set(IXSRV_BUILD_COMMIT "${_ixsrv_commit}")
    endif()
endif()
message(STATUS "ixian-server: build b${IXSRV_BUILD_NUMBER}-${IXSRV_BUILD_COMMIT}, version ${PROJECT_VERSION}")

#
# vendored third-party code (warnings off)
#
add_library(ixsrv_httplib STATIC vendor/cpp-httplib/httplib.cpp)
target_include_directories(ixsrv_httplib SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
target_compile_options(ixsrv_httplib PRIVATE -w)
target_compile_features(ixsrv_httplib PUBLIC cxx_std_17)
target_compile_definitions(ixsrv_httplib PUBLIC
    CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=1048576
    CPPHTTPLIB_LISTEN_BACKLOG=512
    CPPHTTPLIB_REQUEST_URI_MAX_LENGTH=32768
    CPPHTTPLIB_TCP_NODELAY=1)
target_link_libraries(ixsrv_httplib PUBLIC Threads::Threads)
if(IXIAN_SERVER_OPENSSL)
    find_package(OpenSSL)
    if(OpenSSL_FOUND)
        message(STATUS "ixian-server: HTTPS enabled (OpenSSL ${OPENSSL_VERSION})")
        target_compile_definitions(ixsrv_httplib PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
        target_link_libraries(ixsrv_httplib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
    else()
        message(WARNING "ixian-server: OpenSSL not found, HTTPS disabled")
    endif()
endif()

add_library(ixsrv_vendor INTERFACE)
target_include_directories(ixsrv_vendor SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nlohmann"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniaudio")

#
# the engine: the SDK, or the mock
#
if(IXIAN_SERVER_MOCK_ENGINE)
    if(NOT IXIAN_SDK_INCLUDE_DIR)
        message(FATAL_ERROR "ixian-server: the mock engine needs the SDK headers; set IXIAN_SDK_DIR or run scripts/fetch-sdk.sh")
    endif()
    add_library(ixian_mock_engine STATIC mock/ixian_mock.cpp)
    target_include_directories(ixian_mock_engine PUBLIC "${IXIAN_SDK_INCLUDE_DIR}")
    target_link_libraries(ixian_mock_engine PUBLIC Threads::Threads PRIVATE ixsrv_vendor)
    target_compile_options(ixian_mock_engine PRIVATE -Wall -Wextra)
    add_library(zetic::ixian_hexagon ALIAS ixian_mock_engine)
    set(IXSRV_ENGINE_NAME "mock")
else()
    if(IXIAN_SDK_DIR)
        find_package(IxianHexagon 0.2 CONFIG REQUIRED PATHS "${IXIAN_SDK_DIR}/lib/cmake" NO_DEFAULT_PATH)
    else()
        find_package(IxianHexagon 0.2 CONFIG REQUIRED)
    endif()
    set(IXSRV_ENGINE_NAME "hexagon")
    # cross-compiling on a host without libcdsprpc.so.1: the SDK library NEEDs it, so the linker must find a copy;
    # a stub with the FastRPC entry points satisfies the link, the board supplies the real library at run time
    if(CMAKE_CROSSCOMPILING AND IXIAN_SERVER_CDSPRPC_STUB)
        find_library(_ixsrv_cdsprpc NAMES cdsprpc libcdsprpc.so.1)
        if(NOT _ixsrv_cdsprpc)
            add_library(cdsprpc SHARED cmake/cdsprpc_stub.c)
            set_target_properties(cdsprpc PROPERTIES
                OUTPUT_NAME cdsprpc SOVERSION 1
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/cdsprpc-stub")
            message(STATUS "ixian-server: cross build, linking against a stub libcdsprpc.so.1 (not installed)")
            set(IXSRV_CDSPRPC_STUB_TARGET cdsprpc)
        endif()
    endif()
endif()

#
# the server
#
set(IXSRV_SOURCES
    src/server-log.cpp
    src/server-utils.cpp
    src/server-media.cpp
    src/server-args.cpp
    src/server-engine.cpp
    src/server-chat.cpp
    src/server-task.cpp
    src/server-decision.cpp
    src/server-routes.cpp
    src/server-http.cpp
)

# the fallback web page and the LFM3 template, embedded as C++ string literals
set(IXSRV_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${IXSRV_GENERATED_DIR}")
function(ixsrv_embed_file input symbol output)
    file(READ "${input}" _hex HEX)
    string(LENGTH "${_hex}" _len)
    math(EXPR _bytes "${_len} / 2")
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes_list "${_hex}")
    file(WRITE "${output}"
        "// generated from ${input} - do not edit\n"
        "#include <cstddef>\n"
        "extern const unsigned char ${symbol}[];\n"
        "extern const size_t ${symbol}_size;\n"
        "const unsigned char ${symbol}[] = {${_bytes_list} 0x00};\n"
        "const size_t ${symbol}_size = ${_bytes};\n")
endfunction()
ixsrv_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"          ixsrv_asset_index_html  "${IXSRV_GENERATED_DIR}/asset_index_html.cpp")
ixsrv_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/templates/LFM3.jinja" ixsrv_asset_lfm3_jinja  "${IXSRV_GENERATED_DIR}/asset_lfm3_jinja.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html" "${CMAKE_CURRENT_SOURCE_DIR}/src/templates/LFM3.jinja")

add_library(ixsrv_core STATIC ${IXSRV_SOURCES}
    "${IXSRV_GENERATED_DIR}/asset_index_html.cpp"
    "${IXSRV_GENERATED_DIR}/asset_lfm3_jinja.cpp")
target_include_directories(ixsrv_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ixsrv_core PUBLIC zetic::ixian_hexagon ixsrv_httplib ixsrv_vendor Threads::Threads)
target_compile_definitions(ixsrv_core PUBLIC
    IXSRV_VERSION="${PROJECT_VERSION}"
    IXSRV_BUILD_NUMBER=${IXSRV_BUILD_NUMBER}
    IXSRV_BUILD_COMMIT="${IXSRV_BUILD_COMMIT}"
    IXSRV_SDK_VERSION="${IXIAN_SDK_VERSION}"
    IXSRV_ENGINE_NAME="${IXSRV_ENGINE_NAME}")
if(IXIAN_SERVER_MOCK_ENGINE)
    target_compile_definitions(ixsrv_core PUBLIC IXSRV_MOCK_ENGINE=1)
endif()
target_compile_options(ixsrv_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 12 reports false positives inside nlohmann/json 3.12 (instantiated from our code, so SYSTEM does not silence them)
    target_compile_options(ixsrv_core PRIVATE -Wno-maybe-uninitialized)
endif()
if(IXIAN_SERVER_WERROR)
    target_compile_options(ixsrv_core PRIVATE -Werror)
endif()
# stb_image / miniaudio implementations live in server-media.cpp, keep their warnings quiet
set_source_files_properties(src/server-media.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function;-Wno-unused-but-set-variable;-Wno-missing-field-initializers")

add_executable(ixian-server src/main.cpp)
target_link_libraries(ixian-server PRIVATE ixsrv_core)
if(IXSRV_CDSPRPC_STUB_TARGET)
    # the SDK library NEEDs libcdsprpc.so.1: a cross linker finds such dependencies through -rpath-link, not -L
    add_dependencies(ixian-server ${IXSRV_CDSPRPC_STUB_TARGET})
    target_link_directories(ixian-server PRIVATE "${CMAKE_BINARY_DIR}/cdsprpc-stub")
    target_link_options(ixian-server PRIVATE "-Wl,-rpath-link,${CMAKE_BINARY_DIR}/cdsprpc-stub")
    if(TARGET ixsrv-media-selftest)
        add_dependencies(ixsrv-media-selftest ${IXSRV_CDSPRPC_STUB_TARGET})
        target_link_options(ixsrv-media-selftest PRIVATE "-Wl,-rpath-link,${CMAKE_BINARY_DIR}/cdsprpc-stub")
    endif()
endif()
# the binary finds libzetic_ixian_hexagon.so next to itself or in ../lib, like the SDK's own tools
set_target_properties(ixian-server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    BUILD_RPATH "$ORIGIN;$ORIGIN/../lib"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
    BUILD_RPATH_USE_ORIGIN ON
    BUILD_RPATH_USE_LINK_PATH OFF
    INSTALL_RPATH_USE_LINK_PATH OFF)

#
# self-tests
#
if(IXIAN_SERVER_BUILD_TESTS)
    add_executable(ixsrv-media-selftest tests/cpp/media_selftest.cpp)
    target_link_libraries(ixsrv-media-selftest PRIVATE ixsrv_core)
    if(IXIAN_SERVER_MOCK_ENGINE)
        add_executable(ixsrv-mock-selftest mock/mock_selftest.cpp)
        target_link_libraries(ixsrv-mock-selftest PRIVATE ixian_mock_engine ixsrv_vendor)
    endif()
endif()

if(IXSRV_CDSPRPC_STUB_TARGET AND TARGET ixsrv-media-selftest)
    add_dependencies(ixsrv-media-selftest ${IXSRV_CDSPRPC_STUB_TARGET})
    target_link_options(ixsrv-media-selftest PRIVATE "-Wl,-rpath-link,${CMAKE_BINARY_DIR}/cdsprpc-stub")
endif()

install(TARGETS ixian-server RUNTIME DESTINATION bin)
install(FILES README.md DESTINATION .)
install(DIRECTORY LICENSES/ DESTINATION LICENSES)
install(FILES vendor/cpp-httplib/LICENSE DESTINATION LICENSES RENAME cpp-httplib-LICENSE)
install(FILES vendor/nlohmann/LICENSE.MIT DESTINATION LICENSES RENAME nlohmann-json-LICENSE.MIT)
install(FILES vendor/stb/LICENSE.txt DESTINATION LICENSES RENAME stb_image-LICENSE.txt)
install(FILES vendor/miniaudio/LICENSE DESTINATION LICENSES RENAME miniaudio-LICENSE)
install(PROGRAMS scripts/smoke-test.sh DESTINATION bin)
