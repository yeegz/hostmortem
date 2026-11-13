# libFuzzer targets (clang only): cmake --preset clang-fuzz && cmake --build --preset clang-fuzz
# Run: build/clang-fuzz/tests/fuzz/fuzz_<name> -max_total_time=60 tests/fuzz/corpus/<name>
foreach(name json_event config kmsg procfs netlink dns)
  add_executable(fuzz_${name} fuzz_${name}.cpp)
  target_link_libraries(fuzz_${name} PRIVATE hm_linux)
  target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
