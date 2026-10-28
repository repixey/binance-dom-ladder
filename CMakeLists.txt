cmake_minimum_required(VERSION 3.20)
project(orderbook_dom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# System packages: Boost (header-only use: Asio + Beast >= 1.75), OpenSSL, OpenGL
find_package(Boost 1.75 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)

# Fetched: GLFW + Dear ImGui (pinned)
include(FetchContent)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw  GIT_REPOSITORY https://github.com/glfw/glfw.git      GIT_TAG 3.4     GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git  GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

add_executable(dom src/main.cpp)
target_link_libraries(dom PRIVATE imgui Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
if(MSVC)
  target_compile_options(dom PRIVATE /O2 /W3)
else()
  target_compile_options(dom PRIVATE -O2 -Wall -Wextra)
endif()
if(WIN32)
  target_compile_definitions(dom PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(dom PRIVATE ws2_32 mswsock)
endif()

# Dependency-free unit tests for the parser / book / sync logic
enable_testing()
add_executable(test_core tests/test_core.cpp)
add_test(NAME core COMMAND test_core)
