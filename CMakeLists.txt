cmake_minimum_required(VERSION 3.16)
project(SkilletNTSC VERSION 1.0 LANGUAGES CXX)

# Skillet NTSC — the videoskillet signal path as a Resolume FFGL effect.
#   Windows (Resolume): builds SkilletNTSC.dll
#   Linux:              builds the headless test harnesses (Mesa/EGL)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(FFGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl)
set(GLEW_DIR ${FFGL_DIR}/deps/glew-2.1.0)

# ── the engine: translated shaders + ported CPU signal state, no host deps ──
add_library(skillet_engine STATIC
  engine/glapi.cpp
  engine/signal.cpp
  engine/chain.cpp
  engine/engine.cpp
)
target_include_directories(skillet_engine PUBLIC engine)
if(MSVC)
  target_compile_options(skillet_engine PRIVATE /W3 /utf-8 /bigobj)
  target_compile_definitions(skillet_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(skillet_engine PRIVATE -Wall -O2)
endif()
set_target_properties(skillet_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── the FFGL plugin ──
if(WIN32)
  add_library(SkilletNTSC SHARED
    plugin/Skillet.cpp
    ${FFGL_DIR}/source/lib/FFGLSDK.cpp
    ${FFGL_DIR}/build/windows/FFGLPlugins.def
  )
  target_include_directories(SkilletNTSC PRIVATE
    ${FFGL_DIR}/source/lib
    ${FFGL_DIR}/source/lib/ffgl
    ${GLEW_DIR}/include
  )
  target_compile_definitions(SkilletNTSC PRIVATE GLEW_STATIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_compile_options(SkilletNTSC PRIVATE /W3 /utf-8)
  target_link_libraries(SkilletNTSC PRIVATE
    skillet_engine
    ${GLEW_DIR}/lib/Release/x64/glew32s.lib
    opengl32
  )
  set_target_properties(SkilletNTSC PROPERTIES PREFIX "" OUTPUT_NAME "SkilletNTSC")
elseif(UNIX AND NOT APPLE)
  add_executable(skillet_dump_shaders test/dump_shaders.cpp)
  # Headless checks on Mesa: the engine harness and a minimal FFGL host that
  # loads the real plugin code through plugMain.
  find_library(EGL_LIB NAMES EGL libEGL.so.1 PATHS /usr/lib/x86_64-linux-gnu)
  find_library(GL_LIB NAMES GL libGL.so.1 PATHS /usr/lib/x86_64-linux-gnu)
  if(EGL_LIB AND GL_LIB)
    add_executable(skillet_render test/render.cpp)
    target_link_libraries(skillet_render PRIVATE skillet_engine ${EGL_LIB})
    add_executable(skillet_ffgl_host
      test/ffgl_host.cpp
      test/glew_shim.cpp
      plugin/Skillet.cpp
      ${FFGL_DIR}/source/lib/FFGLSDK.cpp
    )
    target_include_directories(skillet_ffgl_host PRIVATE
      ${FFGL_DIR}/source/lib ${FFGL_DIR}/source/lib/ffgl ${GLEW_DIR}/include)
    target_compile_definitions(skillet_ffgl_host PRIVATE GLEW_NO_GLU)
    target_link_libraries(skillet_ffgl_host PRIVATE skillet_engine ${EGL_LIB} ${GL_LIB} dl)
  endif()
endif()
