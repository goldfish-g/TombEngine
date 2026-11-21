# Builds TombEngine for the browser with Emscripten (WebAssembly and WebGL 2).
#
# Configure inside the image of Tools/WebGL/Dockerfile:
#   emcmake cmake -S Tools/WebGL -B <build dir> -G Ninja -DCMAKE_BUILD_TYPE=Release

cmake_minimum_required(VERSION 3.24)
project(TombEngineWeb C CXX)

set(TEN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(TEN_SRC "${TEN_ROOT}/TombEngine")
set(TEN_LIBS "${TEN_ROOT}/Libs")
set(EXT "/ext" CACHE PATH "Libraries built for the browser by the Dockerfile")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The sources are the ones the Visual Studio project compiles, read from it so
# the two stay the same. Only the Direct3D 11 renderer is left out. Some paths
# in the project differ in case from the files, which Windows doesn't mind, so
# they are looked up whatever their case.
file(GLOB_RECURSE ALL_CPP RELATIVE "${TEN_SRC}" "${TEN_SRC}/*.cpp")
foreach(CPP ${ALL_CPP})
    string(TOLOWER "${CPP}" CPP_LOWER)
    set("CPP_CASE_${CPP_LOWER}" "${CPP}")
endforeach()

file(READ "${TEN_SRC}/TombEngine.vcxproj" VCXPROJ)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TEN_SRC}/TombEngine.vcxproj")
string(REGEX MATCHALL "<ClCompile Include=\"[^\"]+\"" VCXPROJ_ITEMS "${VCXPROJ}")
set(SOURCES)
foreach(ITEM ${VCXPROJ_ITEMS})
    string(REGEX REPLACE "<ClCompile Include=\"([^\"]+)\"" "\\1" ITEM_PATH "${ITEM}")
    string(REPLACE "\\" "/" ITEM_PATH "${ITEM_PATH}")
    if(ITEM_PATH MATCHES "^Renderer/Native/DirectX11/")
        continue()
    endif()
    string(TOLOWER "${ITEM_PATH}" ITEM_LOWER)
    if(NOT DEFINED "CPP_CASE_${ITEM_LOWER}")
        message(FATAL_ERROR "Source file of TombEngine.vcxproj not found: ${ITEM_PATH}")
    endif()
    list(APPEND SOURCES "${TEN_SRC}/${CPP_CASE_${ITEM_LOWER}}")
endforeach()

# What the browser build adds: the WebGL 2 renderer, the platform layer and the
# browser versions of the Windows-only libraries (BASS, libVLC, Discord).
file(GLOB WEB_SOURCES CONFIGURE_DEPENDS
    "${TEN_SRC}/Renderer/Native/WebGL2/*.cpp"
    "${TEN_SRC}/Specific/Platform/Web/*.cpp")
list(APPEND SOURCES ${WEB_SOURCES})

add_executable(TombEngine ${SOURCES} "${EXT}/src/SimpleMath.cpp")
set_target_properties(TombEngine PROPERTIES OUTPUT_NAME "TombEngine" SUFFIX ".js")

# Same include order as the IncludePath of TombEngine.vcxproj.
target_include_directories(TombEngine PRIVATE
    "${TEN_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${TEN_LIBS}"
    "${TEN_LIBS}/lua"
    "${TEN_LIBS}/sol"
    "${TEN_LIBS}/lz4"
    "${EXT}/include"
    "${TEN_LIBS}/spdlog"
    "${TEN_LIBS}/vlc"
    "${TEN_LIBS}/bass"
    "${TEN_LIBS}/discordrpc"
    "${TEN_LIBS}/srtparser")

# Defines of the Release|x64 configuration that apply outside Windows.
target_compile_definitions(TombEngine PRIVATE
    NOMINMAX
    SOL_SAFE_USERTYPE
    SOL_SAFE_FUNCTION_CALLS)

target_precompile_headers(TombEngine PRIVATE "${TEN_SRC}/framework.h")
set_source_files_properties("${EXT}/src/SimpleMath.cpp" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

set(TEN_WEB_FLAGS -pthread -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
# DirectXMath takes the SSE2 code path of the Windows x64 build. Emscripten
# turns SSE2 intrinsics into WebAssembly SIMD.
target_compile_definitions(TombEngine PRIVATE _XM_SSE_INTRINSICS_)
# The engine is written against MSVC. Clang accepts its Microsoft extensions
# (extra member qualification, forward-declared enums and the like) with this.
# __unaligned is a keyword there, which Emscripten's SSE headers use as a name.
target_compile_options(TombEngine PRIVATE ${TEN_WEB_FLAGS} -msse2 -fms-extensions -D__unaligned=__tenweb_unaligned -w)

# The engine's own shaders, compiled for WebGL like a game's would be (see
# compile_shaders.py), for games that don't ship their own.
file(GLOB STOCK_SHADER_SOURCES CONFIGURE_DEPENDS "${TEN_SRC}/Shaders/*")
set(STOCK_SHADERS "${CMAKE_BINARY_DIR}/Shaders.tenshaders")
add_custom_command(
    OUTPUT "${STOCK_SHADERS}"
    COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/compile_shaders.py" "${TEN_SRC}/Shaders" "${STOCK_SHADERS}" > "${CMAKE_BINARY_DIR}/Shaders.log"
    DEPENDS ${STOCK_SHADER_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/compile_shaders.py"
    COMMENT "Compiling the engine's shaders for WebGL")
add_custom_target(StockShaders DEPENDS "${STOCK_SHADERS}")
add_dependencies(TombEngine StockShaders)
set_target_properties(TombEngine PROPERTIES LINK_DEPENDS "${STOCK_SHADERS};${CMAKE_CURRENT_SOURCE_DIR}/pre.js")

target_link_directories(TombEngine PRIVATE "${EXT}/lib")
target_link_libraries(TombEngine PRIVATE SDL3 lua lz4)
target_link_options(TombEngine PRIVATE
    ${TEN_WEB_FLAGS}
    # Function names in stack traces while the port is brought up.
    --profiling-funcs
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH
    -sMAXIMUM_MEMORY=4GB
    -sINITIAL_MEMORY=512MB
    -sSTACK_SIZE=8MB
    # The engine's threads come from workers started with the page: the parallel
    # task pool (two per core), the game thread, level loading, audio and spares.
    # Windows gives threads 1 MB of stack; WebAssembly keeps less on it.
    -sDEFAULT_PTHREAD_STACK_SIZE=2MB
    "-sPTHREAD_POOL_SIZE=navigator.hardwareConcurrency*2+8"
    -sENVIRONMENT=web,worker
    -sMODULARIZE
    -sEXPORT_NAME=TombEngine
    -sINVOKE_RUN=0
    -sFORCE_FILESYSTEM
    "-sEXPORTED_RUNTIME_METHODS=[\"FS\",\"IDBFS\",\"callMain\",\"PATH\",\"UTF8ToString\"]"
    -lidbfs.js
    "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/pre.js"
    # The engine's files the Windows build has next to or inside its executable.
    "--preload-file=${STOCK_SHADERS}@/engine/Shaders.tenshaders"
    "--preload-file=${TEN_SRC}/Resources/title.bin@/engine/title.bin")
