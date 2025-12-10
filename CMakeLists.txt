cmake_minimum_required(VERSION 3.20)
project(Legionfall VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/MP /W4 /fp:fast)
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Vulkan found: ${Vulkan_INCLUDE_DIRS}")

set(SOURCES
    src/platform/Win32VulkanApp.cpp
    src/render/Renderer.cpp
    src/core/JobSystem.cpp
    src/core/Game.cpp
)

set(HEADERS
    src/render/Renderer.h
    src/core/JobSystem.h
    src/core/Game.h
)

add_executable(Legionfall WIN32 ${SOURCES} ${HEADERS})

target_include_directories(Legionfall PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${Vulkan_INCLUDE_DIRS}
)

target_link_libraries(Legionfall PRIVATE ${Vulkan_LIBRARIES})

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
        ${SHADER_DIR}/instanced.vert.spv 
        ${SHADER_DIR}/instanced.frag.spv)
    add_dependencies(Legionfall Shaders)
    
    add_custom_command(TARGET Legionfall POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SHADER_DIR} $<TARGET_FILE_DIR:Legionfall>/shaders)
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT Legionfall)
