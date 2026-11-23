cmake_minimum_required(VERSION 3.20)
project(ScarlettX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# Core emulator static library
add_library(scarlettx_core STATIC
    src/core/logger.cpp
    src/core/hle_trace.cpp
    src/core/status.cpp
    src/core/iat_symbols.cpp
    src/loader/game_config.cpp
    src/core/pe_import_resolver.cpp
    src/loader/pe_loader.cpp
    src/loader/pe_mapper.cpp
    src/loader/guest_modules.cpp
    src/loader/package_vfs.cpp
    src/loader/xvd_container.cpp
    src/core/kernelx_memory.cpp
    src/core/kernelx_threading.cpp
    src/core/veh_interceptor.cpp
    src/kernel/xmemory.cpp
    src/kernel/xruntime.cpp
    src/kernel/xstorage.cpp
    src/gpu/d3d12_backend.cpp
    src/gpu/rt_bvh.cpp
    src/gpu/shader_ir.cpp
    src/gpu/rdna2_lifter.cpp
    src/gpu/dxil_backend.cpp
    src/gpu/xg_layout.cpp
    src/runtime/emulator.cpp
    src/runtime/virtual_drive.cpp
)

target_include_directories(scarlettx_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

find_package(ZLIB REQUIRED)
target_link_libraries(scarlettx_core PUBLIC ZLIB::ZLIB)
target_compile_definitions(scarlettx_core PUBLIC SCARLETTX_HAS_ZLIB=1)

find_path(DXC_INCLUDE_DIR
    NAMES dxcapi.h
    HINTS "$ENV{VULKAN_SDK}/Include/dxc"
)
if(WIN32 AND DXC_INCLUDE_DIR)
    target_include_directories(scarlettx_core PRIVATE ${DXC_INCLUDE_DIR})
    target_compile_definitions(scarlettx_core PUBLIC SCARLETTX_HAS_DXC=1)
else()
    target_compile_definitions(scarlettx_core PUBLIC SCARLETTX_HAS_DXC=0)
endif()

if(WIN32)
    target_compile_definitions(scarlettx_core PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        SCARLETTX_PLATFORM_WINDOWS=1
    )
    if(MSVC)
        target_link_libraries(scarlettx_core PUBLIC
            d3d12
            dxgi
            d3dcompiler
            ole32
            uuid
        )
    else()
        target_link_libraries(scarlettx_core PUBLIC
            d3d12
            dxgi
            d3dcompiler_47
            ole32
            uuid
        )
        # Also the thread runtime (libwinpthread): a shim that needs a DLL from
        # the toolchain only loads while the toolchain is on PATH.
        add_link_options(-static -static-libgcc -static-libstdc++)
    endif()
endif()

# ---------------------------------------------------------------------------
# Scarlett HLE System DLL Shims
# ---------------------------------------------------------------------------
if(WIN32)
    add_library(vicewin_core SHARED src/launcher/core.cpp)
    set_target_properties(vicewin_core PROPERTIES
        PREFIX ""
        OUTPUT_NAME "vicewin_core"
    )
    target_include_directories(vicewin_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_compile_definitions(vicewin_core PRIVATE
        SCARLETTX_BUILDING_VICEWIN_CORE=1
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_link_libraries(vicewin_core PRIVATE scarlettx_core)

    add_executable(ViceWin WIN32
        src/launcher/main.cpp
        src/launcher/process_launcher.cpp
        src/core/logger.cpp
    )
    target_include_directories(ViceWin PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/launcher
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(ViceWin PRIVATE UNICODE _UNICODE)
    target_link_libraries(ViceWin PRIVATE comdlg32 dwmapi user32 gdi32)
    set_target_properties(ViceWin PROPERTIES OUTPUT_NAME "ViceWin")

    # Stage the DXC pair beside the launcher when a Windows SDK provides it, so
    # a release is self-contained. The launcher also works without it: the
    # shader-compiler cascade searches the game directory, the host directory,
    # System32 and every SDK bin\<version>\x64 at runtime.
    set(_dxc_search_dirs "")
    if(DEFINED ENV{WindowsSdkDir})
        list(APPEND _dxc_search_dirs "$ENV{WindowsSdkDir}/bin")
    endif()
    if(DEFINED ENV{ProgramW6432})
        list(APPEND _dxc_search_dirs "$ENV{ProgramW6432}/Windows Kits/10/bin")
    endif()
    if(DEFINED ENV{SYSTEMDRIVE})
        list(APPEND _dxc_search_dirs
            "$ENV{SYSTEMDRIVE}/Program Files/Windows Kits/10/bin")
    endif()
    list(APPEND _dxc_search_dirs "C:/Program Files (x86)/Windows Kits/10/bin")

    set(_dxcompiler_dll "")
    foreach(_dxc_dir IN LISTS _dxc_search_dirs)
        if(_dxcompiler_dll STREQUAL "" AND IS_DIRECTORY "${_dxc_dir}")
            file(GLOB _dxc_candidates "${_dxc_dir}/*/x64/dxcompiler.dll")
            if(_dxc_candidates)
                list(SORT _dxc_candidates)
                list(GET _dxc_candidates -1 _dxcompiler_dll)
            endif()
        endif()
    endforeach()

    if(NOT _dxcompiler_dll STREQUAL "")
        get_filename_component(_dxc_bin_dir "${_dxcompiler_dll}" DIRECTORY)
        add_custom_command(TARGET ViceWin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dxcompiler_dll}" "$<TARGET_FILE_DIR:ViceWin>/dxcompiler.dll"
            COMMENT "Staging dxcompiler.dll next to ViceWin.exe")
        if(EXISTS "${_dxc_bin_dir}/dxil.dll")
            add_custom_command(TARGET ViceWin POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_dxc_bin_dir}/dxil.dll" "$<TARGET_FILE_DIR:ViceWin>/dxil.dll"
                COMMENT "Staging dxil.dll next to ViceWin.exe")
        else()
            message(STATUS "dxil.dll is absent next to dxcompiler.dll; validation relies on the runtime cascade")
        endif()
        message(STATUS "DXC staged from ${_dxc_bin_dir}")
    else()
        message(STATUS "No dxcompiler.dll found in a Windows SDK; ViceWin will use the runtime cascade")
    endif()

    add_executable(vicewin_dump_tester
        src/tools/shader_dump_tester.cpp
    )
    target_link_libraries(vicewin_dump_tester PRIVATE scarlettx_core)
    target_include_directories(vicewin_dump_tester PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    # Dev bench: selectable GTA VI-class shader/physics workloads.
    add_executable(vicewindev
        src/dev/main.cpp
        src/dev/tests.cpp
        src/dev/gta6_shaders.cpp
        src/dev/physics_kernel.cpp
        src/dev/physics_reference.cpp
        src/dev/compute_harness.cpp
        src/dev/log_scan.cpp
    )
    set_target_properties(vicewindev PROPERTIES OUTPUT_NAME "vicewindev")
    target_link_libraries(vicewindev PRIVATE scarlettx_core)
    target_include_directories(vicewindev PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_definitions(vicewindev PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

add_library(d3d12_xs SHARED
    src/shims/d3d12_xs_shim.cpp
    src/shims/d3d12_xs_exports.cpp
    exports/d3d12_xs.def)
set_target_properties(d3d12_xs PROPERTIES PREFIX "" OUTPUT_NAME "d3d12_xs")
target_link_libraries(d3d12_xs PRIVATE scarlettx_core)
if(WIN32)
    target_link_libraries(d3d12_xs PRIVATE vicewin_core)
endif()
target_compile_definitions(d3d12_xs PRIVATE SCARLETTX_BUILDING_D3D12_XS=1)

add_library(xg_xs SHARED src/shims/xg_xs_shim.cpp)
set_target_properties(xg_xs PROPERTIES PREFIX "" OUTPUT_NAME "xg_xs")
target_link_libraries(xg_xs PRIVATE scarlettx_core)
target_compile_definitions(xg_xs PRIVATE SCARLETTX_BUILDING_XG_XS=1)

add_library(AcpHal SHARED src/shims/acphal_shim.cpp)
set_target_properties(AcpHal PROPERTIES PREFIX "" OUTPUT_NAME "AcpHal")
target_link_libraries(AcpHal PRIVATE scarlettx_core)
target_compile_definitions(AcpHal PRIVATE SCARLETTX_BUILDING_ACPHAL=1)

add_library(xgameruntime SHARED
    src/shims/xgameruntime_shim.cpp
    src/shims/xgr_api_tables.cpp
    src/shims/xgr_task_queue.cpp
    exports/xgameruntime.def)
set_target_properties(xgameruntime PROPERTIES PREFIX "" OUTPUT_NAME "xgameruntime")
target_link_libraries(xgameruntime PRIVATE scarlettx_core)
target_compile_definitions(xgameruntime PRIVATE SCARLETTX_BUILDING_XGAMERUNTIME=1)

add_library(XboxBvhBuilder SHARED src/shims/xbox_bvh_builder_shim.cpp
    src/shims/xbox_bvh_builder_class.cpp)
set_target_properties(XboxBvhBuilder PROPERTIES PREFIX "" OUTPUT_NAME "XboxBvhBuilder")
target_link_libraries(XboxBvhBuilder PRIVATE scarlettx_core)
target_compile_definitions(XboxBvhBuilder PRIVATE SCARLETTX_BUILDING_XBOX_BVH=1)

add_library(dstorage SHARED src/shims/dstorage_shim.cpp)
set_target_properties(dstorage PROPERTIES PREFIX "" OUTPUT_NAME "dstorage")
target_link_libraries(dstorage PRIVATE scarlettx_core)
target_compile_definitions(dstorage PRIVATE SCARLETTX_BUILDING_DSTORAGE=1)

add_library(dstorage_xs SHARED
    src/core/dstorage_xs_shim.cpp
    exports/dstorage_xs.def)
set_target_properties(dstorage_xs PROPERTIES PREFIX "" OUTPUT_NAME "dstorage_xs")
target_link_libraries(dstorage_xs PRIVATE scarlettx_core)
target_compile_definitions(dstorage_xs PRIVATE SCARLETTX_BUILDING_DSTORAGE=1)

add_library(xcrdapi SHARED
    src/core/dstorage_xs_shim.cpp
    src/core/xcrdapi_shim.cpp
    exports/xcrdapi.def)
set_target_properties(xcrdapi PROPERTIES PREFIX "" OUTPUT_NAME "xcrdapi")
target_link_libraries(xcrdapi PRIVATE scarlettx_core ZLIB::ZLIB)
target_compile_definitions(xcrdapi PRIVATE
    SCARLETTX_BUILDING_DSTORAGE=1
    SCARLETTX_BUILDING_XCRDAPI=1)

add_library(xaudio2_9 SHARED src/shims/xaudio2_shim.cpp src/shims/xaudio2_guest.cpp src/shims/xaudio2_null_output.cpp src/shims/xaudio2_9.def)
set_target_properties(xaudio2_9 PROPERTIES PREFIX "" OUTPUT_NAME "xaudio2_9")
target_link_libraries(xaudio2_9 PRIVATE scarlettx_core)
target_compile_definitions(xaudio2_9 PRIVATE SCARLETTX_BUILDING_XAUDIO2=1)

add_library(chad SHARED src/shims/xdsp_shim.cpp exports/chad.def)
set_target_properties(chad PROPERTIES PREFIX "" OUTPUT_NAME "chad")
target_link_libraries(chad PRIVATE scarlettx_core)
target_compile_definitions(chad PRIVATE SCARLETTX_BUILDING_CHAD=1)

add_library(GameInput SHARED src/shims/gameinput_shim.cpp)
set_target_properties(GameInput PROPERTIES PREFIX "" OUTPUT_NAME "GameInput")
target_link_libraries(GameInput PRIVATE scarlettx_core)
target_compile_definitions(GameInput PRIVATE SCARLETTX_BUILDING_GAMEINPUT=1)

add_library(Bink2Scarlett SHARED src/shims/bink2_scarlett_shim.cpp)
set_target_properties(Bink2Scarlett PROPERTIES PREFIX "" OUTPUT_NAME "Bink2Scarlett")
target_link_libraries(Bink2Scarlett PRIVATE scarlettx_core)
target_compile_definitions(Bink2Scarlett PRIVATE SCARLETTX_BUILDING_BINK2=1)

add_library(amd_fidelityfx_upscaler_xbox SHARED src/shims/ffx_upscaler_shim.cpp)
set_target_properties(amd_fidelityfx_upscaler_xbox PROPERTIES PREFIX "" OUTPUT_NAME "amd_fidelityfx_upscaler_xbox")
target_link_libraries(amd_fidelityfx_upscaler_xbox PRIVATE scarlettx_core)
target_compile_definitions(amd_fidelityfx_upscaler_xbox PRIVATE SCARLETTX_BUILDING_FFX_UPSCALER=1)

add_library(amd_fidelityfx_loader_xbox SHARED src/shims/ffx_loader_shim.cpp)
set_target_properties(amd_fidelityfx_loader_xbox PROPERTIES PREFIX "" OUTPUT_NAME "amd_fidelityfx_loader_xbox")
target_link_libraries(amd_fidelityfx_loader_xbox PRIVATE scarlettx_core amd_fidelityfx_upscaler_xbox)
target_compile_definitions(amd_fidelityfx_loader_xbox PRIVATE SCARLETTX_BUILDING_FFX_LOADER=1)

add_library(libHttpClient_GDK SHARED src/shims/libhttpclient_shim.cpp)
set_target_properties(libHttpClient_GDK PROPERTIES PREFIX "" OUTPUT_NAME "libHttpClient.GDK")
target_link_libraries(libHttpClient_GDK PRIVATE scarlettx_core xgameruntime)
target_compile_definitions(libHttpClient_GDK PRIVATE SCARLETTX_BUILDING_LIBHTTPCLIENT=1)

add_library(XCurl SHARED src/shims/xcurl_shim.cpp)
set_target_properties(XCurl PROPERTIES PREFIX "" OUTPUT_NAME "XCurl")
target_link_libraries(XCurl PRIVATE scarlettx_core)
target_compile_definitions(XCurl PRIVATE SCARLETTX_BUILDING_XCURL=1)

# ---------------------------------------------------------------------------
# ScarlettX CLI Emulator Runner
# ---------------------------------------------------------------------------
add_executable(scarlettx src/main.cpp)
target_link_libraries(scarlettx PRIVATE scarlettx_core)
add_dependencies(scarlettx
    d3d12_xs
    xg_xs
    AcpHal
    xgameruntime
    XboxBvhBuilder
    dstorage
    dstorage_xs
    xcrdapi
    xaudio2_9
    GameInput
    Bink2Scarlett
    amd_fidelityfx_upscaler_xbox
    amd_fidelityfx_loader_xbox
    libHttpClient_GDK
    XCurl
)
if(WIN32)
    add_dependencies(ViceWin scarlettx)
endif()

# ---------------------------------------------------------------------------
# Synthetic Scarlett GDK Test Games (compile as real x86-64 PEs linking HLE DLLs)
# ---------------------------------------------------------------------------
add_executable(hello_scarlett tests/synthetic_games/hello_scarlett/main.cpp)
target_link_libraries(hello_scarlett PRIVATE d3d12_xs xgameruntime XboxBvhBuilder)
target_include_directories(hello_scarlett PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(hello_scarlett PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/packages/hello_scarlett>"
)
add_custom_command(TARGET hello_scarlett POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/hello_scarlett/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:hello_scarlett>/MicrosoftGame.Config"
)

add_executable(rt_showcase tests/synthetic_games/rt_showcase/main.cpp)
target_link_libraries(rt_showcase PRIVATE d3d12_xs xgameruntime XboxBvhBuilder)
target_include_directories(rt_showcase PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(rt_showcase PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/packages/rt_showcase>"
)
add_custom_command(TARGET rt_showcase POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/rt_showcase/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:rt_showcase>/MicrosoftGame.Config"
)

# Synthetic Scarlett Game 3: IAT resolution and explicit missing-import failure.
if(WIN32)
    function(scarlettx_add_dummy_import_lib target_name c_src def_file)
        add_library(${target_name} SHARED ${c_src} ${def_file})
        set_target_properties(${target_name} PROPERTIES
            PREFIX ""
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dummy_import_dlls"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dummy_import_libs"
        )
        # Remove the dummy runtime DLL so it cannot be loaded by Windows LoadLibrary;
        # only its import library (.lib/.dll.a) is used at link time.
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E remove -f "$<TARGET_FILE:${target_name}>"
        )
    endfunction()

    scarlettx_add_dummy_import_lib(xordinal_hle
        tests/synthetic_games/iat_probe/xordinal_hle_stub.c
        tests/synthetic_games/iat_probe/xordinal_hle.def
    )
    scarlettx_add_dummy_import_lib(kernelx_missing
        tests/synthetic_games/iat_probe/kernelx_missing_stub.c
        tests/synthetic_games/iat_probe/kernelx_missing.def
    )

    add_library(iat_probe SHARED
        tests/synthetic_games/iat_probe/iat_probe.cpp
    )
    target_link_libraries(iat_probe PRIVATE
        xgameruntime
        xordinal_hle
    )
    set_target_properties(iat_probe PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/packages/iat_probe>"
    )
    add_custom_command(TARGET iat_probe POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/iat_probe/MicrosoftGame.Config"
            "${CMAKE_BINARY_DIR}/packages/iat_probe/MicrosoftGame.Config"
    )

    add_library(iat_missing_probe SHARED
        tests/synthetic_games/iat_probe/missing_import_probe.cpp
    )
    target_link_libraries(iat_missing_probe PRIVATE
        kernelx_missing
    )
    set_target_properties(iat_missing_probe PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/packages/iat_missing_probe>"
    )

    add_library(iat_missing_ordinal_probe SHARED
        tests/synthetic_games/iat_probe/missing_ordinal_probe.cpp
    )
    target_link_libraries(iat_missing_ordinal_probe PRIVATE kernelx_missing)
    set_target_properties(iat_missing_ordinal_probe PROPERTIES
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/packages/iat_missing_ordinal_probe>"
    )
endif()

# Synthetic Scarlett Game 4: scarlett_arena (full-stack Gen9 E2E demo)
add_executable(scarlett_arena tests/synthetic_games/scarlett_arena/main.cpp)
target_link_libraries(scarlett_arena PRIVATE
    xgameruntime
    d3d12_xs
    XboxBvhBuilder
    dstorage
    xaudio2_9
    GameInput
)
target_include_directories(scarlett_arena PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(MINGW)
    target_link_options(scarlett_arena PRIVATE -Wl,--dynamicbase)
endif()
set_target_properties(scarlett_arena PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/packages/scarlett_arena>"
)
add_custom_command(TARGET scarlett_arena POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/synthetic_games/scarlett_arena/MicrosoftGame.Config"
        "$<TARGET_FILE_DIR:scarlett_arena>/MicrosoftGame.Config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:xgameruntime>"
        "$<TARGET_FILE:d3d12_xs>"
        "$<TARGET_FILE:XboxBvhBuilder>"
        "$<TARGET_FILE:dstorage>"
        "$<TARGET_FILE:xaudio2_9>"
        "$<TARGET_FILE:GameInput>"
        "$<TARGET_FILE_DIR:scarlett_arena>"
)
if(WIN32)
    add_custom_command(TARGET scarlett_arena POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:vicewin_core>"
            "$<TARGET_FILE_DIR:scarlett_arena>"
    )
endif()

# ---------------------------------------------------------------------------
# Autonomous Test Suite
# ---------------------------------------------------------------------------
function(scarlettx_add_test name src)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE scarlettx_core)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_compile_definitions(${name} PRIVATE
        SCARLETTX_BINARY_DIR="${CMAKE_BINARY_DIR}"
        SCARLETTX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

scarlettx_add_test(test_game_config tests/test_game_config.cpp)
scarlettx_add_test(test_pe_loader tests/test_pe_loader.cpp)
add_dependencies(test_pe_loader hello_scarlett rt_showcase)

if(WIN32)
    scarlettx_add_test(test_inproc_loader tests/test_inproc_loader.cpp)
    add_dependencies(test_inproc_loader
        iat_probe
        iat_missing_probe
        iat_missing_ordinal_probe
        xgameruntime)
    scarlettx_add_test(test_guest_loader tests/test_guest_loader.cpp)
    scarlettx_add_test(test_veh_interceptor tests/test_veh_interceptor.cpp)
endif()

scarlettx_add_test(test_kernel_hle tests/test_kernel_hle.cpp)
scarlettx_add_test(test_xgameruntime_abi tests/test_xgameruntime_abi.cpp)
target_link_libraries(test_xgameruntime_abi PRIVATE xgameruntime)
scarlettx_add_test(test_acphal tests/test_acphal.cpp)
target_link_libraries(test_acphal PRIVATE AcpHal)
scarlettx_add_test(test_query_api_impl tests/test_query_api_impl.cpp)
target_link_libraries(test_query_api_impl PRIVATE xgameruntime)
scarlettx_add_test(test_xtaskqueue_xasync tests/test_xtaskqueue_xasync.cpp)
target_link_libraries(test_xtaskqueue_xasync PRIVATE xgameruntime)
scarlettx_add_test(test_d3d12_warp tests/test_d3d12_warp.cpp)
scarlettx_add_test(test_xg_layout tests/test_xg_layout.cpp)
target_link_libraries(test_xg_layout PRIVATE xg_xs)
scarlettx_add_test(test_d3d12_guest_texture tests/test_d3d12_guest_texture.cpp)
target_link_libraries(test_d3d12_guest_texture PRIVATE d3d12_xs)
set_tests_properties(test_d3d12_guest_texture PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_list_layout tests/test_d3d12_list_layout.cpp)
target_link_libraries(test_d3d12_list_layout PRIVATE d3d12_xs)
set_tests_properties(test_d3d12_list_layout PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_present_window tests/test_d3d12_present_window.cpp)
target_link_libraries(test_d3d12_present_window PRIVATE d3d12_xs)
set_tests_properties(test_d3d12_present_window PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_frame_pacing tests/test_d3d12_frame_pacing.cpp)
target_link_libraries(test_d3d12_frame_pacing PRIVATE d3d12_xs)
set_tests_properties(test_d3d12_frame_pacing PROPERTIES ENVIRONMENT "SCARLETTX_REAL_TITLE=1")
scarlettx_add_test(test_d3d12_xs_abi tests/test_d3d12_xs_abi.cpp)
target_link_libraries(test_d3d12_xs_abi PRIVATE d3d12_xs XboxBvhBuilder)
scarlettx_add_test(test_rt_pipeline tests/test_rt_pipeline.cpp)
scarlettx_add_test(test_dstorage_xvd tests/test_dstorage_xvd.cpp)
target_link_libraries(test_dstorage_xvd PRIVATE dstorage)
scarlettx_add_test(test_xaudio2 tests/test_xaudio2.cpp)
target_link_libraries(test_xaudio2 PRIVATE xaudio2_9)
scarlettx_add_test(test_xaudio2_guest tests/test_xaudio2_guest.cpp)
target_link_libraries(test_xaudio2_guest PRIVATE xaudio2_9)
scarlettx_add_test(test_dstorage_guest tests/test_dstorage_guest.cpp)
target_link_libraries(test_dstorage_guest PRIVATE xcrdapi)
scarlettx_add_test(test_xcrdapi tests/test_xcrdapi.cpp)
target_link_libraries(test_xcrdapi PRIVATE xcrdapi ZLIB::ZLIB)
scarlettx_add_test(test_gameinput tests/test_gameinput.cpp)
target_link_libraries(test_gameinput PRIVATE GameInput)

scarlettx_add_test(test_shader_pipeline tests/test_shader_pipeline.cpp)
target_link_libraries(test_shader_pipeline PRIVATE d3d12_xs)

scarlettx_add_test(test_logger_spam tests/test_logger_spam.cpp)

if(WIN32)
    # Smoke run of the dev bench: toolchain probe plus a small physics dispatch.
    add_test(
        NAME test_vicewindev_smoke
        COMMAND vicewindev --run toolchain --run physics --quick
    )
endif()

scarlettx_add_test(test_middleware_net tests/test_middleware_net.cpp)
target_link_libraries(test_middleware_net PRIVATE
    Bink2Scarlett
    amd_fidelityfx_loader_xbox
    amd_fidelityfx_upscaler_xbox
    libHttpClient_GDK
    XCurl
    xgameruntime
)

scarlettx_add_test(test_scarlett_arena_e2e tests/test_scarlett_arena_e2e.cpp)
target_link_libraries(test_scarlett_arena_e2e PRIVATE dstorage)
add_dependencies(test_scarlett_arena_e2e
    scarlett_arena
    xgameruntime
    d3d12_xs
    XboxBvhBuilder
    dstorage
    xaudio2_9
    GameInput
)

add_test(
    NAME test_boot_hello_scarlett
    COMMAND $<TARGET_FILE:scarlettx> boot "${CMAKE_BINARY_DIR}/packages/hello_scarlett" --headless --warp --frames 2
)
add_test(
    NAME test_boot_rt_showcase
    COMMAND $<TARGET_FILE:scarlettx> boot "${CMAKE_BINARY_DIR}/packages/rt_showcase" --headless --warp --frames 2 --rt-dump "${CMAKE_BINARY_DIR}/rt_showcase_frame.ppm"
)
