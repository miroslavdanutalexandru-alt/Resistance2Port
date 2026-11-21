# Offline tests for the options screen patch (r2_swfpatch.cpp: AVM1 decode / encode / insert, the
# avoptions.swf PC pages). Standalone: does not need the lifted game code.
#   cmake -S project/tests/swfpatch -B build_swftest -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl
#   cmake --build build_swftest && ctest --test-dir build_swftest --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(swfpatch_test C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(PORT_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(PORT_ROOT "${PORT_PROJECT_DIR}/.." ABSOLUTE)
set(MINIZ_DIR "${PORT_ROOT}/ps3recomp/third_party/fossilize/miniz")
# the game's archive with avoptions.swf (the real-data test is skipped when absent)
file(GLOB _global "${PORT_ROOT}/gamefiles/game/*/USRDIR/packed/game/global_cached.psarc")
list(FILTER _global EXCLUDE REGEX "\\.mine/")
set(_global0 "none")
if(_global)
    list(GET _global 0 _global0)
endif()
set(SWF_TEST_ARCHIVE "${_global0}" CACHE FILEPATH "global_cached.psarc for the real-data test")

add_executable(swfpatch_test
    swfpatch_test.cpp
    "${PORT_PROJECT_DIR}/r2_swfpatch.cpp"
    "${PORT_PROJECT_DIR}/mods_psarc.cpp"
    "${MINIZ_DIR}/miniz.c")
target_include_directories(swfpatch_test PRIVATE "${PORT_PROJECT_DIR}" "${MINIZ_DIR}")
target_compile_definitions(swfpatch_test PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX R2_PROJECT_DIR="${PORT_PROJECT_DIR}")
set_source_files_properties("${MINIZ_DIR}/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")

add_test(NAME swf_unit COMMAND swfpatch_test unit)
add_test(NAME swf_real COMMAND swfpatch_test real "${SWF_TEST_ARCHIVE}" WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(swf_real PROPERTIES SKIP_RETURN_CODE 77)
