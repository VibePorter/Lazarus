# Mass Effect 2 (Xbox 360): static recompilation host application with Renut's Direct3D-level renderer (me2-nx-deko,
# from mgr-nx-deko v0.2.2).
#
# Run tools/codegen.ps1 first: it writes generated/ (including generated/rexglue.cmake) from your own default.xex.
# Build (from PowerShell, through tools\win\run.ps1):
#   cmake --preset win-pc -S app && cmake --build out/pc
#
# The SA2 port's full CMake file (native renderer, Switch build, PGO, ...) is kept as CMakeLists.sa2.txt: the Switch
# and renderer parts come back from there, one at a time, after the first boot.

cmake_minimum_required(VERSION 3.25)
project(sa2 LANGUAGES CXX)   # "sa2" is the prefix of the generated files; renamed to "mgr" in one step later

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../sdk" CACHE PATH "Path to the ReXGlue SDK source tree")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR "app/generated/rexglue.cmake is missing: run tools/codegen.ps1 first")
endif()
include(generated/rexglue.cmake)

option(MGR_RENUT_RENDERER "Build the Direct3D-level reNut renderer (deko3d on Switch)" ${REXGLUE_PLATFORM_SWITCH})
if(REXGLUE_PLATFORM_SWITCH AND NOT MGR_RENUT_RENDERER)
    message(FATAL_ERROR "This Switch product requires MGR_RENUT_RENDERER=ON")
endif()
if(REXGLUE_PLATFORM_SWITCH AND NOT REXGLUE_SWITCH_NO_NVK)
    message(FATAL_ERROR "This deko3d product requires REXGLUE_SWITCH_NO_NVK=ON")
endif()

# The build re-runs the generator when an input in codegen.d changes; the flavour's post-codegen steps follow it.
if(TARGET sa2_codegen)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/codegen.build.stamp" APPEND
        COMMAND "${CMAKE_COMMAND}" -E env "REXSDK_DIR=${REXSDK_DIR}"
                "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/post_codegen.py")
endif()

set(MGR_SOURCES
    me2_src/me2_main.cpp
    me2_src/me2_switch_save.cpp
)
# Diagnostic: counting hooks on every function of the Direct3D range (tools/mgr_d3d_census.py writes the source).
option(MGR_D3D_CENSUS "Count the calls of every Direct3D-range function per frame (diagnostic build)" OFF)
if(MGR_D3D_CENSUS)
    list(APPEND MGR_SOURCES mgr_src/mgr_d3d_census.cpp)
endif()

if(WIN32)
    add_executable(sa2 WIN32 ${MGR_SOURCES})
else()
    add_executable(sa2 ${MGR_SOURCES})
endif()

# Stage 1 draws with ReXGlue's Xenos GPU emulation.
if(REXGLUE_PLATFORM_SWITCH)
    # The native renderer owns Vulkan on Horizon; the emulated GPU is only a PC reference.
    rexglue_setup_target(sa2)
else()
    rexglue_setup_target(sa2 GPU_PLUGINS xenos)
endif()
# me2-nx-deko (2026-11-03): diagnostic write census of UE3 render-thread phases (app/me2_src/me2_write_census.cpp):
# every guest store also checks a flag. Never for a release build. 2026-11-04: also on PC (me2_census_device, the
# device census for tools/watch_marks.py).
option(ME2_WRITE_CENSUS "Diagnostic: write census of guest stores (slower guest stores)" OFF)
if(ME2_WRITE_CENSUS)
    target_compile_definitions(sa2_recomp PRIVATE ME2_WRITE_CENSUS)
    target_compile_definitions(sa2 PRIVATE ME2_WRITE_CENSUS)
endif()

# The app includes xxhash.h (and later the renderer renderdoc_app.h) directly; when the SDK is built from source they
# are private to its libraries (the same block as CMakeLists.sa2.txt).
foreach(_dep xxhash renderdoc)
    if(TARGET ${_dep})
        target_include_directories(sa2 PRIVATE $<TARGET_PROPERTY:${_dep},INTERFACE_INCLUDE_DIRECTORIES>)
    endif()
endforeach()

# Stage 4, milestone 2: the native Vulkan renderer of the SA2 port (src/me, from masseffect-nx through banjo-nx), with
# MGR's own hooks (mgr_src/mgr_me_hooks.cpp). Left out on purpose: SA2's hooks and everything that names SA2's guest
# functions (src/me/sa2_me_hooks.cpp, sa2_d3d_api.cpp, sa2_api_*.cpp: the native Direct3D layer; src/cpu/sa2_hot_guest.cpp,
# sa2_gpu_wait.cpp; src/sa2_d3d_trace.cpp, sa2_debug_hooks.cpp). What the kept code still needs from them is stubbed in
# mgr_src/mgr_sa2_stubs.cpp. The renderer runs only with sa2_renderer_native = true; the emulated GPU stays the default.
set(_mgr_native_default ON)
if(MGR_RENUT_RENDERER)
    set(_mgr_native_default OFF)
endif()
option(MGR_NATIVE_RENDERER "Build the PM4 Vulkan reference renderer (src/me)" ${_mgr_native_default})
if(MGR_NATIVE_RENDERER)
    target_sources(sa2 PRIVATE
        mgr_src/mgr_me_hooks.cpp
        mgr_src/mgr_sa2_stubs.cpp
        src/sa2_capture.cpp
        src/profiler/sa2_thread_cycles_pc.cpp
        src/me/me_native_system.cpp
        src/me/me_ring_wait.cpp
        src/me/me_renderdoc.cpp
        src/me/native/sa2_native_draws.cpp
        src/me/native/sa2_deferred_recording.cpp
        src/me/native/sa2_native_targets.cpp
        src/me/native/sa2_native_shaders.cpp
        src/me/native/sa2_native_textures_pool.cpp
        src/me/native/sa2_shader_library.cpp
        src/me/native/sa2_native_glue.cpp
        src/me/sa2_render_mode.cpp
        src/cpu/sa2_enhancements.cpp
        src/cpu/sa2_codegen_checks.cpp
        src/cpu/sa2_forms_selfcheck.cpp)
    if(NOT REXGLUE_PLATFORM_SWITCH)
        target_sources(sa2 PRIVATE src/me/me_pc_stubs.cpp)
    endif()
    target_compile_definitions(sa2 PRIVATE MGR_NATIVE_RENDERER=1)
    set(_mgr_codegen_label "classic codegen")
    set(_mgr_codegen_label_file "${CMAKE_CURRENT_SOURCE_DIR}/generated/codegen_flavor.txt")
    if(EXISTS "${_mgr_codegen_label_file}")
        file(READ "${_mgr_codegen_label_file}" _mgr_codegen_label)
        string(STRIP "${_mgr_codegen_label}" _mgr_codegen_label)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mgr_codegen_label_file}")
    endif()
    set_source_files_properties(src/cpu/sa2_enhancements.cpp PROPERTIES
        COMPILE_DEFINITIONS "SA2_BUILD_FLAVOR=\"${_mgr_codegen_label}\"")

# SPIR-V initializers of the native renderer's EDRAM transfer / utility shaders (src/me/native/shaders), compiled
# on the host with glslangValidator (tools/win/build_glslang.ps1 builds it from the SDK's glslang). The Switch build
# uses the same host tool: SPIR-V is platform independent.
set(_MGR_HOST_EXE_SUFFIX "")
if(CMAKE_HOST_WIN32)
    set(_MGR_HOST_EXE_SUFFIX ".exe")
endif()
set(SA2_GLSLANG_VALIDATOR "${CMAKE_CURRENT_SOURCE_DIR}/../out/glslang/StandAlone/glslangValidator${_MGR_HOST_EXE_SUFFIX}"
    CACHE FILEPATH "Host glslangValidator")
set(SA2_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-shaders")
set(SA2_EDRAM_SHADER_INCLUDES)
foreach(_shader sa2_edram_7e3_to_rgba8 sa2_edram_rgba8_to_7e3 sa2_edram_16f_to_16f me_resolve_exp_bias me_edram_depth_to_rgba8 me_edram_depth_to_16f me_edram_import me_edram_color_to_depth me_edram_depth_to_depth me_edram_depth_to_stencil me_edram_color_to_stencil me_edram_depth_to_depth_msaa2 me_edram_depth_msaa2_to_depth_1x me_edram_depth_to_stencil_msaa2 me_edram_depth_msaa2_to_stencil_1x me_edram_raw64_to_rgba8 me_edram_rgba8_to_raw64 me_edram_raw64_to_16f me_edram_16f_to_raw64 me_edram_raw64_to_raw64 me_edram_depth_to_raw64 me_edram_raw64_to_depth me_depth_resolve_guestspace me_depth_resolve_guestspace_msaa2 me_edram_stencil_to_buffer me_edram_stencil_msaa2_to_buffer me_edram_color_stencil_to_buffer me_edram_color_to_color me_edram_r64_to_r64 sa2_resolve_copy sa2_depth_resolve_ms)
    set(_extension comp)
    if(_shader STREQUAL "me_edram_import")
        set(_extension vert)
    elseif(_shader MATCHES "^me_edram_(color_to_depth|depth_to_depth|depth_to_stencil|color_to_stencil|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x|raw64_to_depth|color_to_color|r64_to_r64)$" OR _shader STREQUAL "sa2_resolve_copy" OR
           _shader STREQUAL "sa2_depth_resolve_ms")
        set(_extension frag)
    endif()
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/src/me/native/shaders/${_shader}.${_extension}")
    set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}.inc")
    set(_target_flags)
    if(_shader MATCHES "^me_edram_(stencil_to_buffer|color_stencil_to_buffer|stencil_msaa2_to_buffer|depth_to_depth_msaa2|depth_msaa2_to_depth_1x|depth_to_stencil_msaa2|depth_msaa2_to_stencil_1x)$" OR _shader STREQUAL "me_depth_resolve_guestspace_msaa2")
        set(_target_flags --target-env vulkan1.2)
        set(_output "${SA2_GENERATED_SHADER_DIR}/${_shader}_generated.inc")
    endif()
    add_custom_command(OUTPUT "${_output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SA2_GENERATED_SHADER_DIR}"
        COMMAND "${SA2_GLSLANG_VALIDATOR}" -V ${_target_flags} -x -o "${_output}" "${_source}"
        DEPENDS "${_source}" VERBATIM)
    list(APPEND SA2_EDRAM_SHADER_INCLUDES "${_output}")
endforeach()
add_custom_target(sa2_edram_shaders DEPENDS ${SA2_EDRAM_SHADER_INCLUDES})
add_dependencies(sa2 sa2_edram_shaders)
target_include_directories(sa2 PRIVATE
    "${SA2_GENERATED_SHADER_DIR}"
    src
    src/me
    src/me/native
    src/me/native/shaders
    "${REXSDK_DIR}/src/core"
    # Precompiled SPIR-V of the SDK presenter (vulkan_spirv/guest_output_*): the renderer draws into its output.
    "${REXSDK_DIR}/src/ui/shaders")

# CPU-only SDK sources the native renderer reuses (draw-extent estimation, shader microcode interpretation): the
# GPU plugin's private symbols cannot satisfy application-side use.
target_sources(sa2 PRIVATE
    "${REXSDK_DIR}/src/graphics/register_file.cpp"
    "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
    "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")

endif()

if(MGR_RENUT_RENDERER)
    if(NOT MGR_NATIVE_RENDERER)
        target_sources(sa2 PRIVATE
            me2_src/me2_hooks.cpp
            me2_src/me2_ini_override.cpp
            me2_src/me2_physx.cpp
            me2_src/me2_prepass.cpp
            me2_src/me2_cull_distance.cpp
            me2_src/me2_material_cache.cpp
            me2_src/me2_material_params.cpp
            me2_src/me2_light_test.cpp
            me2_src/me2_write_census.cpp
            me2_src/me2_ab_cycle.cpp
            me2_src/me2_shadow_diag.cpp
            me2_src/me2_drawlist_diag.cpp
            me2_src/me2_dynshared.cpp  # me2-nx-deko (2026-11-06): skinned-mesh DrawShared diagnostic
            me2_src/me2_replay.cpp  # me2-nx-deko (2026-11-06): draw-setup replay, stage 1 (compare mode)
            me2_src/me2_vsm_light.cpp  # me2-nx-deko (2026-11-07): shadowed point light's uninitialised VSM matrix
            me2_src/me2_relink.cpp
            me2_src/me2_crowd_cpu.cpp
            me2_src/me2_anim_lod.cpp
            me2_src/me2_cerberus.cpp
            me2_src/me2_button_icons.cpp
            me2_src/me2_menu.cpp
            me2_src/me2_gyro_menu.cpp
            me2_src/me2_subtitles.cpp
            me2_src/me2_disc.cpp
            me2_src/me2_sound_diag.cpp
            me2_src/me2_startup_movies.cpp
            me2_src/me2_me1_import.cpp
            me2_src/me2_selection.cpp
            me2_src/hot/me_hot_guest.cpp
            me2_src/hot/me_hot_shadow.cpp
            me2_src/hot/me2_stream_distance.cpp
            me2_src/me2_sa2_stubs.cpp
            src/sa2_capture.cpp
            src/profiler/sa2_thread_cycles_pc.cpp
            src/me/sa2_render_mode.cpp
            src/cpu/sa2_enhancements.cpp
            src/cpu/sa2_gpu_boost.cpp
            src/cpu/sa2_codegen_checks.cpp
            src/cpu/sa2_forms_selfcheck.cpp)
    endif()
    target_include_directories(sa2 PRIVATE src src/me src/me/native "${REXSDK_DIR}/src/core")
    # ME2 (2026-10-31): the native LZX (me2_src/hot/n/n_8279B210.h) uses the SDK's libmspack lzxd (linked through rexruntime).
    target_include_directories(sa2 PRIVATE "${REXSDK_DIR}/thirdparty/libmspack/cabextract/mspack")
    # Linked directly: rexruntime has it PRIVATE, which the PC link does not carry over to sa2 (lzxd_* undefined).
    target_link_libraries(sa2 PRIVATE mspack)
    # ME2 (2026-11-02): me2_me1_import.cpp inflates the PC saves' ZIP with stb_image's zlib (static copy).
    target_include_directories(sa2 PRIVATE "${REXSDK_DIR}/thirdparty/stb")
    include(cmake/mgr_renut_gpu.cmake)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # The toolchain reserves a zero native-TLS prefix for libnx thread-name
    # compatibility with Ryujinx. Relink when its generated layout changes.
    set_property(TARGET sa2 APPEND PROPERTY LINK_DEPENDS "${CMAKE_BINARY_DIR}/switch-libnx-tls.ld")
    # Full codegen makes unhooked calls direct. Optimize those calls across the
    # generated translation units, preserving guest floating-point semantics.
    option(MGR_SWITCH_LTO "LTO for MGR game code and native renderer" ON)
    # ME2 (2026-10-26): 12, was 2. Each LTRANS job takes ~300 MB; with 2 the ~100 partitions ran two at a time on 16
    # host threads. tools/nx/deko_build.sh passes LTO_JOBS (default 12) at every build.
    set(MGR_SWITCH_LTO_JOBS "12" CACHE STRING "Parallel Switch LTO link jobs")
    target_compile_definitions(sa2 PRIVATE XXH_FORCE_MEMORY_ACCESS=0)
    if(MGR_SWITCH_LTO)
        set(_mgr_lto_targets sa2_recomp sa2)
        if(TARGET mgr_renut_gpu)
            list(APPEND _mgr_lto_targets mgr_renut_gpu)
        endif()
        foreach(_mgr_lto_target IN LISTS _mgr_lto_targets)
            target_compile_options(${_mgr_lto_target} PRIVATE -flto -fno-fat-lto-objects -fipa-pta)
        endforeach()
        target_link_options(sa2 PRIVATE -flto=${MGR_SWITCH_LTO_JOBS} -flto-partition=balanced
            -fipa-pta -ffp-contract=off -fno-strict-aliasing)
    endif()
    option(MGR_SWITCH_HOT_ORDER "Group hot functions from console samples (MGR's list: not for ME2)" OFF)
    if(MGR_SWITCH_HOT_ORDER)
        set(_mgr_hot_order "${CMAKE_CURRENT_SOURCE_DIR}/mgr_hot_functions.ld")
        if(NOT EXISTS "${_mgr_hot_order}")
            message(FATAL_ERROR "mgr_hot_functions.ld is missing; regenerate it from MGR console samples")
        endif()
        target_link_options(sa2 PRIVATE "-Wl,--section-ordering-file=${_mgr_hot_order}")
        set_property(TARGET sa2 APPEND PROPERTY LINK_DEPENDS "${_mgr_hot_order}")
    endif()
    # ME2 (2026-10-27, from SA2's CMakeLists.sa2.txt): profile-guided optimisation of the game code, the app and the
    # renderer.
    #   ME2_PGO=generate  an instrumented NRO (tools/nx/deko_build.sh PGO=generate, out/nx-pgogen, no LTO) that writes
    #                     its branch counts to pgo/sa2_pgo_<N>.stream next to the NRO every 2 minutes and on HOME
    #                     (src/sa2_pgo.cpp; counts registered in a section: libgcov's exit-time files do not fit the
    #                     Switch). Value profiling off: its shared lists are not safe without atomic updates.
    #   ME2_PGO=use       the merged counts (tools/nx/pgo_merge.sh). Code the sessions did not run is compiled as
    #                     without a profile, functions changed since the instrumented build keep none.
    # -fprofile-prefix-path = the build folder: the .gcda names are the objects' paths relative to it, so the
    # instrumented build (out/nx-pgogen) and the LTO build (out/nx) share one profile.
    set(ME2_PGO "" CACHE STRING "Profile-guided optimisation of the Switch build: empty, generate or use")
    set_property(CACHE ME2_PGO PROPERTY STRINGS "" generate use)
    set(ME2_PGO_DIR "/root/me2pgo/profile" CACHE PATH "Folder of the PGO profile (.gcda files)")
    if(ME2_PGO STREQUAL "generate")
        set(_me2_pgo "-fprofile-generate=${ME2_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                     -fno-profile-values -fprofile-update=single -fprofile-info-section=gcov_info)
        target_compile_definitions(sa2 PRIVATE SA2_PGO_GENERATE=1)
    elseif(ME2_PGO STREQUAL "use")
        if(NOT EXISTS "${ME2_PGO_DIR}")
            message(FATAL_ERROR "ME2_PGO=use: no profile in ${ME2_PGO_DIR}")
        endif()
        # -fprofile-correction: counts taken without atomic updates while several guest threads ran the same code.
        set(_me2_pgo "-fprofile-use=${ME2_PGO_DIR}" "-fprofile-prefix-path=${CMAKE_BINARY_DIR}" -fprofile-correction
                     -fno-profile-values -fprofile-partial-training -Wno-missing-profile -Wno-coverage-mismatch
                     # ME2 (2026-11-02, from naruto-nx / sa2-nx): hot functions grouped (.text.hot, L1i) and the
                     # inliner allowed to take small hot helpers of the generated code (no `inline` keywords there),
                     # with the growth of cold code capped. No -fomit-frame-pointer: the wait profiler and
                     # switch_profile_stacks walk the x29 chain.
                     -fprofile-reorder-functions --param=max-inline-insns-auto=64 --param=inline-unit-growth=12)
    elseif(ME2_PGO)
        message(FATAL_ERROR "ME2_PGO must be empty, generate or use (got ${ME2_PGO})")
    endif()
    if(ME2_PGO)
        foreach(_me2_pgo_target sa2_recomp sa2 mgr_renut_gpu)
            if(TARGET ${_me2_pgo_target})
                target_compile_options(${_me2_pgo_target} PRIVATE ${_me2_pgo})
            endif()
        endforeach()
        target_link_options(sa2 PRIVATE ${_me2_pgo})
        message(STATUS "me2: PGO ${ME2_PGO} (${ME2_PGO_DIR})")
    endif()
    target_sources(sa2 PRIVATE src/sa2_pgo.cpp)
    target_compile_options(sa2_recomp PRIVATE -ffp-contract=off -fno-strict-aliasing)
    # ME2 (2026-11-02): tools/fp_sign_ops.py and tools/single_floats.py (post_codegen.py); OFF gives the generator's
    # integer sign flips and double-then-round single arithmetic back without regenerating the code.
    option(ME2_GEN_FLOAT_FORMS "Float forms of guest sign and single-precision operations" ON)
    if(NOT ME2_GEN_FLOAT_FORMS)
        target_compile_definitions(sa2_recomp PRIVATE REX_FP_SIGN_INTEGER REX_SINGLE_FLOATS_OFF)
    endif()
    # One startup-only function registers ~85k guest functions. GCC's -O3
    # optimization of that single basic block consumed >7 GB on the host.
    # Keep gameplay code optimized and compile only the registration list at O0.
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/generated/default/sa2_register.cpp"
        APPEND PROPERTY COMPILE_OPTIONS -O0)
    set_source_files_properties(generated/default/sa2_register.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    rexglue_switch_add_nro(sa2 NAME "Mass Effect 2" AUTHOR "ME2-NX" VERSION "0.0.1"
        ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon_me2.jpg")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sa2 PRIVATE -ffp-contract=off -fno-strict-aliasing)
endif()

# ME2 (2026-10-26, from masseffect-cinnamon/masseffect-nx MASSEFFECT_FAST_MATH, part of their measured "diet"):
# the recompiled code never reads errno after a libm call and never relies on FP traps (the Xbox 360 game runs with
# exceptions masked), so libm calls such as sqrt can be inlined and FP code moved freely. -ffp-contract=off stays.
option(ME2_FAST_MATH "-fno-math-errno -fno-trapping-math on the recompiled game code" ON)
if(ME2_FAST_MATH AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND TARGET sa2_recomp)
    target_compile_options(sa2_recomp PRIVATE -fno-math-errno -fno-trapping-math)
    if(REXGLUE_PLATFORM_SWITCH AND MGR_SWITCH_LTO)
        target_link_options(sa2 PRIVATE -fno-math-errno -fno-trapping-math)
    endif()
endif()

# ME2 (2026-11-06): the link-register half of tools/frame_traffic.py. With the generator's elide_lr_stores no guest call
# sets ctx.lr, so a prologue's mflr reads a stale word, saves it to the guest stack and the epilogue writes it back:
# a ctx load, a stack store, a stack load and a ctx store per call for a value nothing uses. Measured 2026-11-06 (console
# stacks): ~0.7 % of the guest threads' time in ctx.lr traffic. SA2 passed a frame-exact capture with it; the register
# saves (REX_NV_SAVES_ELIDE) stay on, they tinted SA2's characters blue.
option(ME2_LR_MOVES_ELIDE "Leave out the guest link register's dead stack-frame moves (REX_LR_MOVES_ELIDE)" ON)
if(ME2_LR_MOVES_ELIDE AND TARGET sa2_recomp)
    target_compile_definitions(sa2_recomp PRIVATE REX_LR_MOVES_ELIDE)
endif()
