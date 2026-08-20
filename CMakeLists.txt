cmake_minimum_required(VERSION 3.20)
project(Legionfall VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(LF_BUILD_TESTS  "Build the scheduler tests"     ON)
option(LF_BUILD_BENCH  "Build the scheduler benchmark" ON)
option(LF_ASAN         "AddressSanitizer + UBSan"      OFF)
option(LF_TSAN         "ThreadSanitizer"               OFF)

# Downgrades the seq_cst fence in ChaseLevDeque::pop() to relaxed, which is the
# bug the 2013 paper exists to fix. Build with this ON and run
# deque_steal_no_duplication a few times - on ARM it starts handing the same job
# to two threads. It is here so the claim is reproducible instead of folklore.
option(LF_BREAK_POP_FENCE "Deliberately break the pop() fence (demo)" OFF)

if(MSVC)
    add_compile_options(/MP /W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ASan and TSan can't be linked together.
if(LF_ASAN AND LF_TSAN)
    message(FATAL_ERROR "LF_ASAN and LF_TSAN are mutually exclusive")
endif()
if(LF_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()
if(LF_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

if(LF_BREAK_POP_FENCE)
    add_compile_definitions(LF_BREAK_POP_FENCE)
    message(WARNING "LF_BREAK_POP_FENCE is ON - the deque is intentionally broken")
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# lf_core - the scheduler on its own. No Vulkan, no Win32, builds anywhere.
# Splitting this out is what lets the tests and benchmark run on macOS and in
# CI without a graphics stack.
# ---------------------------------------------------------------------------
add_library(lf_core STATIC
    src/core/JobSystem.cpp
    src/core/MutexJobSystem.cpp
    src/core/Game.cpp          # no Vulkan in here, so CI compiles it too
)
target_include_directories(lf_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(lf_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LF_BUILD_TESTS)
    enable_testing()
    add_executable(lf_tests tests/tests.cpp)
    target_link_libraries(lf_tests PRIVATE lf_core)

    # Registered individually so a red build names the test that broke.
    set(LF_TEST_NAMES
        deque_push_pop_is_lifo
        deque_steal_is_fifo
        deque_grows_beyond_initial_capacity
        deque_empty_pop_and_steal
        deque_steal_no_duplication
        jobsystem_runs_every_job_exactly_once
        jobsystem_wait_is_a_real_barrier
        jobsystem_handles_uneven_job_costs
        jobsystem_nested_scheduling
        jobsystem_parallel_for_covers_range_exactly
        jobsystem_parallel_for_edge_cases
        jobsystem_single_worker_still_works
        jobsystem_submitter_actually_helps
        jobsystem_ring_wraps_safely
        jobsystem_stealing_actually_happens
        mutex_jobsystem_baseline_is_correct
    )
    foreach(t ${LF_TEST_NAMES})
        add_test(NAME ${t} COMMAND lf_tests ${t})
        # A missed wakeup shows up as a hang, so every test needs a timeout or
        # CI sits there for an hour instead of failing.
        set_tests_properties(${t} PROPERTIES TIMEOUT 120)
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Benchmark
# ---------------------------------------------------------------------------
if(LF_BUILD_BENCH)
    add_executable(lf_bench bench/bench.cpp)
    target_link_libraries(lf_bench PRIVATE lf_core)
    if(NOT MSVC)
        target_compile_options(lf_bench PRIVATE -O2)
    endif()
endif()

# ---------------------------------------------------------------------------
# Terminal playtest harness. Runs the real Game::update and the real JobSystem,
# draws with ANSI instead of Vulkan. POSIX only - it needs termios.
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE OR APPLE)
    add_executable(lf_playtest tools/playtest.cpp)
    target_link_libraries(lf_playtest PRIVATE lf_core)
    if(NOT MSVC)
        target_compile_options(lf_playtest PRIVATE -O2)
    endif()
endif()

# ---------------------------------------------------------------------------
# The game itself - Windows + Vulkan only. Skipped elsewhere so the scheduler
# work above still builds and tests on macOS and Linux.
# ---------------------------------------------------------------------------
find_package(Vulkan QUIET)

if(WIN32 AND Vulkan_FOUND)
    message(STATUS "Vulkan found - building the Legionfall game target")

    add_executable(Legionfall WIN32
        src/platform/Win32VulkanApp.cpp
        src/render/Renderer.cpp
        src/render/Renderer.h
        src/core/Game.h
    )
    target_link_libraries(Legionfall PRIVATE lf_core ${Vulkan_LIBRARIES})
    target_include_directories(Legionfall PRIVATE ${Vulkan_INCLUDE_DIRS})
    if(MSVC)
        target_compile_options(Legionfall PRIVATE /fp:fast)
    endif()

    find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/Bin")
    if(GLSLC)
        set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
        file(MAKE_DIRECTORY ${SHADER_DIR})
        add_custom_command(OUTPUT ${SHADER_DIR}/instanced.vert.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/shaders/instanced.vert -o ${SHADER_DIR}/instanced.vert.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/shaders/instanced.vert)
        add_custom_command(OUTPUT ${SHADER_DIR}/instanced.frag.spv
            COMMAND ${GLSLC} ${CMAKE_SOURCE_DIR}/shaders/instanced.frag -o ${SHADER_DIR}/instanced.frag.spv
            DEPENDS ${CMAKE_SOURCE_DIR}/shaders/instanced.frag)
        add_custom_target(Shaders ALL DEPENDS
            ${SHADER_DIR}/instanced.vert.spv ${SHADER_DIR}/instanced.frag.spv)
        add_dependencies(Legionfall Shaders)
        add_custom_command(TARGET Legionfall POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${SHADER_DIR} $<TARGET_FILE_DIR:Legionfall>/shaders)
    endif()

    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Legionfall)
else()
    message(STATUS "Not Windows+Vulkan - building lf_core, tests and benchmark only")
endif()
