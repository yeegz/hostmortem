cmake_minimum_required(VERSION 3.25)
project(hostmortem VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(HM_BUILD_TESTS "Build unit and integration tests" ON)
option(HM_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(HM_WERROR "Treat warnings as errors" ON)
option(HM_STATIC "Link hmd and hm statically (for the initramfs used by the hard-reset VM test)" OFF)
set(HM_SANITIZE "" CACHE STRING "Sanitizer set: '', 'address,undefined' or 'thread'")

include(cmake/Warnings.cmake)
# One version, from project(): binaries, package and reports agree.
add_compile_definitions(HM_VERSION="${PROJECT_VERSION}")

if(HM_STATIC)
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
  add_link_options(-static)
endif()
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)

if(HM_SANITIZE)
  add_compile_options(-fsanitize=${HM_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${HM_SANITIZE})
endif()

# Portable core: builds on any POSIX host. No Linux-only headers here.
add_library(hm_core STATIC
  src/core/event.cpp
  src/core/json_codec.cpp
  src/core/config.cpp
  src/core/siphash.cpp
  src/core/log.cpp
  src/store/store.cpp
  src/analysis/detector.cpp
  src/analysis/playbooks.cpp
  src/analysis/correlator.cpp
  src/analysis/report.cpp
  src/collectors/procfs_parse.cpp
  src/collectors/kmsg_parse.cpp
  src/collectors/dns_wire.cpp
)
target_include_directories(hm_core PUBLIC src)
target_link_libraries(hm_core PUBLIC SQLite::SQLite3 nlohmann_json::nlohmann_json Threads::Threads)
hm_set_warnings(hm_core)

# Linux collectors and agent runtime.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(hm_linux STATIC
    src/core/clock_linux.cpp
    src/collectors/netlink_parse.cpp
    src/collectors/procfs_sampler.cpp
    src/collectors/netlink_collector.cpp
    src/collectors/kmsg_collector.cpp
    src/collectors/prober.cpp
    src/collectors/process_watch.cpp
    src/agent/agent.cpp
    src/agent/writer.cpp
  )
  target_link_libraries(hm_linux PUBLIC hm_core)
  hm_set_warnings(hm_linux)

  add_executable(hmd src/agent/main.cpp)
  target_link_libraries(hmd PRIVATE hm_linux)
  hm_set_warnings(hmd)

  add_executable(hm src/cli/main.cpp src/cli/doctor.cpp)
  target_link_libraries(hm PRIVATE hm_linux)
  hm_set_warnings(hm)
  install(TARGETS hmd hm RUNTIME DESTINATION bin)
  install(FILES packaging/systemd/hmd.service DESTINATION lib/systemd/system)
  install(FILES packaging/hmd.conf.example DESTINATION /etc/hostmortem RENAME hmd.conf
          PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
  install(FILES README.md LICENSE DESTINATION share/doc/hostmortem)

  # Debian package: cmake --build --preset release --target package
  set(CPACK_GENERATOR DEB)
  set(CPACK_PACKAGE_NAME hostmortem)
  set(CPACK_PACKAGE_VENDOR "Yousof Selim")
  set(CPACK_PACKAGE_CONTACT "Yousof Selim <165985093+yeegz@users.noreply.github.com>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Flight recorder for Linux edge boxes: evidence that survives the reboot")
  set(CPACK_DEBIAN_PACKAGE_SECTION admin)
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "adduser")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
  set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
      "${PROJECT_SOURCE_DIR}/packaging/deb/postinst;${PROJECT_SOURCE_DIR}/packaging/deb/prerm;${PROJECT_SOURCE_DIR}/packaging/deb/postrm;${PROJECT_SOURCE_DIR}/packaging/deb/conffiles")
  set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION ON)
  set(CPACK_STRIP_FILES ON)
  include(CPack)

  add_executable(bench_pipeline bench/bench_pipeline.cpp)
  target_link_libraries(bench_pipeline PRIVATE hm_linux)
  hm_set_warnings(bench_pipeline)
endif()

if(HM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HM_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()
