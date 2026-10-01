cmake_minimum_required(VERSION 3.16)
project(HZRDSkillet VERSION 1.0 LANGUAGES CXX)

# HZRD//Skillet — the videoskillet signal path as a Resolume FFGL effect.
#   Windows (Resolume): builds HZRD-Skillet.dll and HZRD-Skillet-Mixer.dll
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
  engine/extras.cpp
)
target_include_directories(skillet_engine PUBLIC engine)
if(MSVC)
  target_compile_options(skillet_engine PRIVATE /W3 /utf-8 /bigobj)
  target_compile_definitions(skillet_engine PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(skillet_engine PRIVATE -Wall -O2)
endif()
set_target_properties(skillet_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ── the FFGL plugins ──
# Two DLLs from one source: the effect (one input) and the mixer (two inputs,
# so Resolume hands it the layers below as well as the layer).
set(PLUGIN_SOURCES plugin/Skillet.cpp plugin/docs_folder.cpp plugin/audio.cpp plugin/audio_win.cpp)
if(WIN32)
  foreach(variant IN ITEMS effect mixer)
    if(variant STREQUAL "mixer")
      set(tgt HZRD-Skillet-Mixer)
    else()
      set(tgt HZRD-Skillet)
    endif()
    add_library(${tgt} SHARED
      ${PLUGIN_SOURCES}
      ${FFGL_DIR}/source/lib/FFGLSDK.cpp
      ${FFGL_DIR}/build/windows/FFGLPlugins.def
    )
    target_include_directories(${tgt} PRIVATE
      ${FFGL_DIR}/source/lib
      ${FFGL_DIR}/source/lib/ffgl
      ${GLEW_DIR}/include
    )
    target_compile_definitions(${tgt} PRIVATE GLEW_STATIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(variant STREQUAL "mixer")
      target_compile_definitions(${tgt} PRIVATE SKILLET_MIXER=1)
    endif()
    target_compile_options(${tgt} PRIVATE /W3 /utf-8)
    target_link_libraries(${tgt} PRIVATE
      skillet_engine
      ${GLEW_DIR}/lib/Release/x64/glew32s.lib
      opengl32
    )
    set_target_properties(${tgt} PROPERTIES PREFIX "" OUTPUT_NAME "${tgt}")
  endforeach()
elseif(UNIX AND NOT APPLE)
  add_executable(skillet_dump_shaders test/dump_shaders.cpp)
  # Headless checks on Mesa: the engine harness and a minimal FFGL host that
  # loads the real plugin code through plugMain.
  find_library(EGL_LIB NAMES EGL libEGL.so.1 PATHS /usr/lib/x86_64-linux-gnu)
  find_library(GL_LIB NAMES GL libGL.so.1 PATHS /usr/lib/x86_64-linux-gnu)
  if(EGL_LIB AND GL_LIB)
    add_executable(skillet_render test/render.cpp)
    target_link_libraries(skillet_render PRIVATE skillet_engine ${EGL_LIB})
    foreach(variant IN ITEMS effect mixer)
      if(variant STREQUAL "mixer")
        set(tgt skillet_ffgl_host_mixer)
      else()
        set(tgt skillet_ffgl_host)
      endif()
      add_executable(${tgt}
        test/ffgl_host.cpp
        test/glew_shim.cpp
        ${PLUGIN_SOURCES}
        ${FFGL_DIR}/source/lib/FFGLSDK.cpp
      )
      target_include_directories(${tgt} PRIVATE
        ${FFGL_DIR}/source/lib ${FFGL_DIR}/source/lib/ffgl ${GLEW_DIR}/include)
      target_compile_definitions(${tgt} PRIVATE GLEW_NO_GLU)
      if(variant STREQUAL "mixer")
        target_compile_definitions(${tgt} PRIVATE SKILLET_MIXER=1)
      endif()
      target_link_libraries(${tgt} PRIVATE skillet_engine ${EGL_LIB} ${GL_LIB} dl pthread)
    endforeach()
  endif()
endif()
