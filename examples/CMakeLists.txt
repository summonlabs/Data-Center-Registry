# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# The examples. Each one is a real consumer of the installed public API and is
# run as part of validation, so an example that stops working fails the build.

add_executable(dcr_example_quick_start quick_start.cpp)
target_link_libraries(dcr_example_quick_start PRIVATE data_center_registry dcr_warnings)

add_executable(dcr_example_lifecycle_and_recovery lifecycle_and_recovery.cpp)
target_link_libraries(dcr_example_lifecycle_and_recovery PRIVATE data_center_registry dcr_warnings)

if(DCR_BUILD_TESTS)
  add_test(NAME dcr_example_quick_start COMMAND dcr_example_quick_start)
  add_test(NAME dcr_example_lifecycle_and_recovery COMMAND dcr_example_lifecycle_and_recovery)
endif()
