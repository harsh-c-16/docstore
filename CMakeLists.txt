cmake_minimum_required(VERSION 3.20)
project(docstore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Put every binary at the top of the build tree (./build/release/docstore_bench).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(DOCSTORE_WERROR "Treat compiler warnings as errors" OFF)
set(DOCSTORE_SANITIZE "" CACHE STRING "Sanitizer build: empty, 'address' (ASan+UBSan) or 'thread'")

# Sanitizers apply to everything, including GoogleTest, so instrumentation is consistent.
if(DOCSTORE_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
elseif(DOCSTORE_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT DOCSTORE_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Unknown DOCSTORE_SANITIZE value: ${DOCSTORE_SANITIZE}")
endif()

# Re-extracted archives get fresh timestamps so dependents rebuild (CMake >= 3.24).
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
include(FetchContent)
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json googletest)

function(docstore_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  if(DOCSTORE_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# ---- library ----
file(GLOB_RECURSE DOCSTORE_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/*.cpp)
add_library(docstore STATIC ${DOCSTORE_SOURCES})
target_include_directories(docstore PUBLIC ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(docstore PUBLIC nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(docstore PUBLIC Threads::Threads)
docstore_warnings(docstore)

# ---- tools ----
foreach(tool crash_writer docstore_cli docstore_server docstore_bench)
  add_executable(${tool} tools/${tool}.cpp)
  target_link_libraries(${tool} PRIVATE docstore)
  docstore_warnings(${tool})
endforeach()

# ---- tests ----
enable_testing()
include(GoogleTest)
file(GLOB_RECURSE DOCSTORE_TEST_SOURCES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/*_test.cpp)
add_executable(docstore_tests ${DOCSTORE_TEST_SOURCES})
target_include_directories(docstore_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests ${PROJECT_SOURCE_DIR}/tools)
target_compile_definitions(docstore_tests PRIVATE
  CRASH_WRITER_PATH="$<TARGET_FILE:crash_writer>" DOCSTORE_CLI_PATH="$<TARGET_FILE:docstore_cli>")
add_dependencies(docstore_tests crash_writer docstore_cli)
target_link_libraries(docstore_tests PRIVATE docstore GTest::gtest_main)
docstore_warnings(docstore_tests)

# Tests put their scratch directories inside the build tree, not in /tmp.
set(DOCSTORE_TEST_TMP ${CMAKE_BINARY_DIR}/test_tmp)
file(MAKE_DIRECTORY ${DOCSTORE_TEST_TMP})
gtest_discover_tests(docstore_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES LABELS fast ENVIRONMENT "TEST_TMPDIR=${DOCSTORE_TEST_TMP}/")

# The long suite re-runs the scalable tests (model, crash, power-loss, stress) with more
# iterations; each test reads DOCSTORE_LONG=1 through test::LongMode().
set(DOCSTORE_LONG_FILTER "EngineModel*:DocModel*:PowerLoss*:ProcessCrash*:Stress*")
add_test(NAME long_suite COMMAND docstore_tests --gtest_filter=${DOCSTORE_LONG_FILTER})
set_tests_properties(long_suite PROPERTIES
  LABELS long TIMEOUT 7200
  ENVIRONMENT "DOCSTORE_LONG=1;TEST_TMPDIR=${DOCSTORE_TEST_TMP}/")
