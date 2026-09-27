# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# The test executables.
#
# No test carries a timeout, and no test is wrapped in one. A hang is a defect
# to diagnose, never something to terminate.

set(DCR_TEST_SOURCES
  test_main.cpp
  test_support.cpp
  test_types.cpp
  test_registry.cpp
  test_lifecycle_states.cpp
  test_persistence.cpp
  test_corruption.cpp
  test_adversarial.cpp
  test_property.cpp
  test_concurrency.cpp)

add_executable(dcr_tests ${DCR_TEST_SOURCES})
# The tests reach into src/ on purpose: the canonical codec, the store and the
# seal around them are part of what is being proved, and testing them only
# through a public wrapper would test the wrapper instead.
target_include_directories(dcr_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dcr_tests PRIVATE data_center_registry dcr_warnings)

add_test(NAME dcr_unit COMMAND dcr_tests)

# A second run of the whole suite with repeats proves that the runtime starts
# and stops deterministically and that no state leaks between instances.
add_test(NAME dcr_unit_repeat COMMAND dcr_tests --repeat 2)

# The independent-process suite starts real processes, so it needs the helper
# binary and its own main.
add_executable(dcr_test_child proc_child_main.cpp)
target_link_libraries(dcr_test_child PRIVATE data_center_registry dcr_warnings)

add_executable(dcr_multiprocess_tests test_multiprocess.cpp test_main.cpp test_support.cpp)
target_include_directories(dcr_multiprocess_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(dcr_multiprocess_tests PRIVATE data_center_registry dcr_warnings)

add_test(NAME dcr_multiprocess COMMAND dcr_multiprocess_tests)
set_tests_properties(dcr_multiprocess PROPERTIES
  ENVIRONMENT "DCR_TEST_CHILD=$<TARGET_FILE:dcr_test_child>")

# Independent processes are started, killed and restarted, so the suite is run
# repeatedly to show the behaviour is not a one-off.
add_test(NAME dcr_multiprocess_repeat COMMAND dcr_multiprocess_tests --repeat 3)
set_tests_properties(dcr_multiprocess_repeat PROPERTIES
  ENVIRONMENT "DCR_TEST_CHILD=$<TARGET_FILE:dcr_test_child>")

# The inspection tool, driven as a real process through its command line. It is
# a consumer of the same public API, so it faces the same preconditions.
if(TARGET dcr_cli)
  add_executable(dcr_cli_tests test_cli.cpp test_main.cpp test_support.cpp)
  target_include_directories(dcr_cli_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(dcr_cli_tests PRIVATE data_center_registry dcr_warnings)

  add_test(NAME dcr_cli COMMAND dcr_cli_tests)
  set_tests_properties(dcr_cli PROPERTIES
    ENVIRONMENT "DCR_TEST_CLI=$<TARGET_FILE:dcr_cli>")
endif()
