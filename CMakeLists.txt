cmake_minimum_required(VERSION 3.20)
project(Legionfall LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Get Vulkan SDK path from environment variable
if (NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "VULKAN_SDK environment variable is not set. Install Vulkan SDK or set VULKAN_SDK.")
endif()

set(VULKAN_SDK $ENV{VULKAN_SDK})

message(STATUS "Using Vulkan SDK at: ${VULKAN_SDK}")

include_directories("${VULKAN_SDK}/Include")
link_directories("${VULKAN_SDK}/Lib")

add_executable(Legionfall
    src/platform/main.cpp
)

# Link against vulkan-1.lib on Windows
if (WIN32)
    target_link_libraries(Legionfall PRIVATE vulkan-1 user32 gdi32)
else()
    target_link_libraries(Legionfall PRIVATE vulkan)
endif()
