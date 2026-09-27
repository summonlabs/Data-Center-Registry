# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# A standalone consumer of the installed Data Center Registry package.
#
# It is deliberately not part of the main build: it exists to prove that the
# installed package is usable from outside the source tree, by a project that
# finds it with find_package and links the exported target.
#
# Usage, from a build directory of your own:
#
#   cmake -S examples/downstream_consumer -B <build-dir> \
#         -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <build-dir>
#   <build-dir>/dcr_downstream_consumer

cmake_minimum_required(VERSION 3.20)

project(DataCenterRegistryDownstreamConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent consumer of the installed Data Center Registry package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(data_center_registry 1.0 REQUIRED)

add_executable(dcr_downstream_consumer main.cpp)
target_link_libraries(dcr_downstream_consumer PRIVATE data_center_registry::data_center_registry)

message(STATUS "data_center_registry ${DATA_CENTER_REGISTRY_VERSION} "
               "(${DATA_CENTER_REGISTRY_LICENSE}) found at ${data_center_registry_DIR}")
