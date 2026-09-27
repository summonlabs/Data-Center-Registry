# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Data Center Registry build.
#
# The project is standalone: it requires no other Summon Software Labs
# repository to build its core, and it has no third-party dependencies.

cmake_minimum_required(VERSION 3.20)

project(
  DataCenterRegistry
  VERSION 1.0.0
  DESCRIPTION "Canonical registry for data-center identities, site membership, control-plane generations, lifecycle state and authoritative facility metadata"
  HOMEPAGE_URL "https://github.com/summonlabs/Data-Center-Registry"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(DCR_IS_TOP_LEVEL ON)
else()
  set(DCR_IS_TOP_LEVEL OFF)
endif()

option(DCR_BUILD_LIBRARY "Build the dcr library" ON)
option(DCR_BUILD_TOOLS "Build the dcr inspection and administration tool" ON)
option(DCR_BUILD_TESTS "Build the Data Center Registry test suites" ON)
option(DCR_BUILD_EXAMPLES "Build the examples" ON)
option(DCR_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(DCR_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(DCR_ENABLE_ASAN "Build with AddressSanitizer where the toolchain supports it" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Declared after GNUInstallDirs: CMAKE_INSTALL_LIBDIR is only defined once that
# module has run, and an empty component would turn the destination into an
# absolute path outside the install prefix.
set(DCR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/data_center_registry"
    CACHE STRING "Directory for the installed CMake package configuration")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Warning configuration: strict for first-party code only.
# ---------------------------------------------------------------------------

add_library(dcr_warnings INTERFACE)
if(MSVC)
  target_compile_options(dcr_warnings INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
  if(DCR_WARNINGS_AS_ERRORS)
    target_compile_options(dcr_warnings INTERFACE /WX)
  endif()
  set(DCR_ASAN_AVAILABLE OFF)
  if(DCR_ENABLE_ASAN)
    target_compile_options(dcr_warnings INTERFACE /fsanitize=address)
    target_link_options(dcr_warnings INTERFACE /fsanitize=address)
    set(DCR_ASAN_AVAILABLE ON)
  endif()
  target_compile_definitions(dcr_warnings INTERFACE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
else()
  target_compile_options(dcr_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor)
  if(DCR_WARNINGS_AS_ERRORS)
    target_compile_options(dcr_warnings INTERFACE -Werror)
  endif()
  set(DCR_ASAN_AVAILABLE OFF)
  if(DCR_ENABLE_ASAN)
    target_compile_options(dcr_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(dcr_warnings INTERFACE -fsanitize=address)
    set(DCR_ASAN_AVAILABLE ON)
  endif()
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

set(DCR_SOURCES
  src/core/codec.cpp
  src/core/commands.cpp
  src/core/compatibility.cpp
  src/core/digest.cpp
  src/core/error.cpp
  src/core/generation.cpp
  src/core/identity.cpp
  src/core/lifecycle.cpp
  src/core/limits.cpp
  src/core/membership.cpp
  src/core/metadata.cpp
  src/core/mutation.cpp
  src/core/outcome.cpp
  src/core/provenance.cpp
  src/core/query.cpp
  src/core/record.cpp
  src/core/registry.cpp
  src/core/sha256.cpp
  src/core/snapshot.cpp
  src/core/state.cpp
  src/core/time.cpp
  src/persistence/store.cpp
  src/internal/files.cpp
  src/internal/text.cpp)

add_library(data_center_registry STATIC ${DCR_SOURCES})
add_library(data_center_registry::data_center_registry ALIAS data_center_registry)
# Short alias used inside this build tree only.
add_library(dcr ALIAS data_center_registry)

target_include_directories(data_center_registry
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# BUILD_INTERFACE keeps the warning interface out of the installed export set:
# consumers of the package never inherit first-party build flags.
target_link_libraries(data_center_registry PRIVATE $<BUILD_INTERFACE:dcr_warnings>)
target_compile_features(data_center_registry PUBLIC cxx_std_20)

set_target_properties(data_center_registry PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  EXPORT_NAME data_center_registry)

if(WIN32)
  target_link_libraries(data_center_registry PRIVATE advapi32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(data_center_registry PRIVATE Threads::Threads)
endif()

target_compile_definitions(data_center_registry PRIVATE DCR_BUILDING_LIBRARY=1)

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------

if(DCR_BUILD_TOOLS)
  add_executable(dcr_cli tools/dcr_main.cpp)
  set_target_properties(dcr_cli PROPERTIES OUTPUT_NAME dcr)
  target_link_libraries(dcr_cli PRIVATE data_center_registry dcr_warnings)

  install(TARGETS dcr_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests, examples, benchmarks
# ---------------------------------------------------------------------------

if(DCR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DCR_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(DCR_BUILD_BENCHMARKS)
  add_executable(dcr_benchmarks benchmarks/registry_benchmarks.cpp)
  target_link_libraries(dcr_benchmarks PRIVATE data_center_registry dcr_warnings)
endif()

# ---------------------------------------------------------------------------
# Install and package export
# ---------------------------------------------------------------------------

install(TARGETS data_center_registry
        EXPORT data_center_registryTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT data_center_registryTargets
        FILE data_center_registryTargets.cmake
        NAMESPACE data_center_registry::
        DESTINATION ${DCR_INSTALL_CMAKEDIR})

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/data_center_registryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/data_center_registryConfig.cmake
  INSTALL_DESTINATION ${DCR_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/data_center_registryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/data_center_registryConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/data_center_registryConfigVersion.cmake
        DESTINATION ${DCR_INSTALL_CMAKEDIR})

install(FILES LICENSE README.md NOTICE DESTINATION ${CMAKE_INSTALL_DOCDIR})
