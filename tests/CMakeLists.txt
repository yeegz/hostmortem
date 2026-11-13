find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(hm_unit_tests
  unit/test_event_queue.cpp
  unit/test_store.cpp
  unit/test_json_codec.cpp
  unit/test_config.cpp
  unit/test_siphash.cpp
  unit/test_procfs_parse.cpp
  unit/test_kmsg_parse.cpp
  unit/test_dns_wire.cpp
  unit/test_detector.cpp
  unit/test_correlator.cpp
  unit/test_report.cpp
)
target_link_libraries(hm_unit_tests PRIVATE hm_core GTest::gtest_main)
target_compile_definitions(hm_unit_tests PRIVATE HM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
gtest_discover_tests(hm_unit_tests DISCOVERY_TIMEOUT 30)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(hm_linux_tests
    unit/test_netlink_parse.cpp
    integration/test_writer.cpp
    integration/test_collectors.cpp
  )
  target_link_libraries(hm_linux_tests PRIVATE hm_linux GTest::gtest_main)
  target_compile_definitions(hm_linux_tests PRIVATE HM_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  gtest_discover_tests(hm_linux_tests DISCOVERY_TIMEOUT 30)

  # Lab scenario fixtures replayed through hm ingest + hm explain; checks the
  # expected diagnosis for each (tests/integration/scenarios.sh).
  add_test(NAME scenario_fixtures
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/scenarios.sh $<TARGET_FILE:hm> ${PROJECT_SOURCE_DIR}/lab/fixtures)
endif()
