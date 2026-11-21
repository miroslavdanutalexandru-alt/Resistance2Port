# Offline tests for the HD UI pack (project/hd_ui_pack.cpp = HDUIPack.exe, rpcs3/Emu/RSX/Common/r2_hd_ui.h = the
# renderer's key / DDS / pack-folder code). Nothing here starts the game.
#   cmake -S project/tests/hd_ui -B build_hduitest -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl
#   cmake --build build_hduitest && ctest --test-dir build_hduitest --output-on-failure
# The key test compiles RPCS3's own texture layout code (TextureUtils.cpp get_subresources_layout_impl,
# RSXTexture.cpp get_exact_mipmap_count, VKR2Gfx.cpp tp_hash), cut out of the sources at configure time, so it checks
# the code the game runs, not a copy of it.
cmake_minimum_required(VERSION 3.20)
project(hd_ui_test C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

get_filename_component(PORT_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(PORT_ROOT "${PORT_PROJECT_DIR}/.." ABSOLUTE)
set(RPCS3_RSX "${PORT_ROOT}/ps3recomp/third_party/rpcs3/rpcs3/Emu/RSX")
set(MINIZ_DIR "${PORT_ROOT}/ps3recomp/third_party/fossilize/miniz")
set(STB_DIR "${PORT_ROOT}/ps3recomp/third_party/rpcs3/3rdparty/stblib/stb")

# The game files for the real-data test (skipped when absent)
set(_usr "none")
foreach(_c gamefiles/PS3_GAME/USRDIR game/PS3_GAME/USRDIR PS3_GAME/USRDIR game/USRDIR)
    if(_usr STREQUAL "none" AND IS_DIRECTORY "${PORT_ROOT}/${_c}/packed")
        set(_usr "${PORT_ROOT}/${_c}")
    endif()
endforeach()
set(HDUI_TEST_USRDIR "${_usr}" CACHE PATH "USRDIR with packed/ for the real-data test")

# Cuts the text from `start` to the "}" that follows `last` (the end of a function) out of `file`
function(hdui_extract out file start last)
    file(READ "${file}" _c)
    string(FIND "${_c}" "${start}" _s)
    if(_s EQUAL -1)
        message(FATAL_ERROR "hd_ui test: '${start}' not found in ${file} (RPCS3 changed: update the test)")
    endif()
    string(SUBSTRING "${_c}" ${_s} -1 _rest)
    string(FIND "${_rest}" "${last}" _e)
    if(_e EQUAL -1)
        message(FATAL_ERROR "hd_ui test: '${last}' not found in ${file}")
    endif()
    string(LENGTH "${last}" _l)
    math(EXPR _e "${_e} + ${_l}")
    string(SUBSTRING "${_rest}" ${_e} -1 _tail)
    string(FIND "${_tail}" "}" _b)
    math(EXPR _n "${_e} + ${_b} + 1")
    string(SUBSTRING "${_rest}" 0 ${_n} _piece)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gen/${out}" "// cut from ${file} at configure time\n${_piece}\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
endfunction()
hdui_extract(rpx_layout_impl.inc "${RPCS3_RSX}/Common/TextureUtils.cpp"
    "template <u8 block_edge_in_texel, typename SRC_TYPE>" "return result;")
hdui_extract(rpx_layout.inc "${RPCS3_RSX}/Common/TextureUtils.cpp"
    "// Common implementation for packing texture definitions into subresource layout arrays"
    "fmt::throw_exception(\"Wrong format 0x%x\", format);")
hdui_extract(rpx_packed_pitch.inc "${RPCS3_RSX}/Common/TextureUtils.cpp"
    "u32 get_format_packed_pitch(u32 format, u16 width, bool border, bool swizzled)" "return width_in_block * bytes_per_block;")
hdui_extract(rpx_mips.inc "${RPCS3_RSX}/RSXTexture.cpp"
    "u16 fragment_texture::get_exact_mipmap_count() const" "return std::min(ensure(mipmap()), max_mipmap_count);")
hdui_extract(rpx_tp_hash.inc "${RPCS3_RSX}/VK/VKR2Gfx.cpp"
    "u64 tp_hash(const std::vector<rsx::subresource_layout>& layout" "return hdui::hash(spans, n, w, h, fmt, mips);")

add_executable(hd_ui_test
    hd_ui_test.cpp
    "${PORT_PROJECT_DIR}/mods_psarc.cpp"
    "${MINIZ_DIR}/miniz.c")
target_include_directories(hd_ui_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gen" "${PORT_PROJECT_DIR}" "${RPCS3_RSX}/Common"
    "${MINIZ_DIR}" "${STB_DIR}")
target_compile_definitions(hd_ui_test PRIVATE _CRT_SECURE_NO_WARNINGS HDUI_NO_MAIN "HDUI_RPCS3_RSX=\"${RPCS3_RSX}\"" "HDUI_REPO=\"${PORT_ROOT}\"")
target_link_libraries(hd_ui_test PRIVATE urlmon bcrypt)
if(MSVC)
    target_compile_options(hd_ui_test PRIVATE /EHsc)
endif()
set_source_files_properties("${MINIZ_DIR}/miniz.c" PROPERTIES COMPILE_OPTIONS "-w")

set(_dir "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME hdui_dds COMMAND hd_ui_test dds WORKING_DIRECTORY "${_dir}")
add_test(NAME hdui_pack_folder COMMAND hd_ui_test folder "${_dir}/folder" WORKING_DIRECTORY "${_dir}")
add_test(NAME hdui_blit COMMAND hd_ui_test blit WORKING_DIRECTORY "${_dir}")
add_test(NAME hdui_loose COMMAND hd_ui_test loose WORKING_DIRECTORY "${_dir}")
add_test(NAME hdui_upscaler_install COMMAND hd_ui_test upscaler "${_dir}/upscaler" WORKING_DIRECTORY "${_dir}")
# GPU safety without the GPU: the upscaler's command line, the watchdog (on a stand-in upscaler) and the settings
# after a crashed / failed run
add_test(NAME hdui_gpu_safety COMMAND hd_ui_test gpu_safety "${_dir}/gpu_safety" WORKING_DIRECTORY "${_dir}")
set_tests_properties(hdui_gpu_safety PROPERTIES TIMEOUT 120)
# The pack and tools\upscaler on different drives (skipped when LOCALAPPDATA is on the repo's drive)
set(_xvol "none")
if(DEFINED ENV{LOCALAPPDATA})
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" _la)
    string(SUBSTRING "${_la}" 0 2 _la_drive)
    string(SUBSTRING "${PORT_ROOT}" 0 2 _repo_drive)
    string(TOUPPER "${_la_drive}" _la_drive)
    string(TOUPPER "${_repo_drive}" _repo_drive)
    if(NOT _la_drive STREQUAL _repo_drive)
        set(_xvol "${_la}/hd_ui_xvolume_test")
    endif()
endif()
add_test(NAME hdui_pack_xvolume COMMAND hd_ui_test xvolume "${_dir}/xvolume" "${_xvol}" WORKING_DIRECTORY "${_dir}")
set_tests_properties(hdui_pack_xvolume PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME hdui_real_keys COMMAND hd_ui_test real "${HDUI_TEST_USRDIR}" WORKING_DIRECTORY "${_dir}")
set_tests_properties(hdui_real_keys PROPERTIES SKIP_RETURN_CODE 77)
