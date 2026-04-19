cmake_minimum_required(VERSION 3.16)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(1_input_ai_HDMI)

message(STATUS "Project name: ${PROJECT_NAME}")
message(STATUS "Project source dir: ${PROJECT_SOURCE_DIR}")
message(STATUS "Project binary dir: ${PROJECT_BINARY_DIR}")

set(TARGET_CHIP "ZG" CACHE STRING "Select the target chip backend: BY (Buyi) or ZG (ZG330)")
set_property(CACHE TARGET_CHIP PROPERTY STRINGS "BY" "ZG")
string(TOUPPER "${TARGET_CHIP}" TARGET_CHIP_UPPER)

if(TARGET_CHIP_UPPER STREQUAL "BY")
    add_compile_definitions(USE_BUYI_BACKEND)
    message(STATUS "Configuring for Buyi (BY) Backend.")
elseif(TARGET_CHIP_UPPER STREQUAL "ZG")
    add_compile_definitions(USE_ZG330_BACKEND)
    message(STATUS "Configuring for ZG330 (ZG) Backend.")
else()
    message(FATAL_ERROR "Invalid TARGET_CHIP value: ${TARGET_CHIP}. Please choose 'BY' or 'ZG'.")
endif()

set(TARGET_NAME imgpatch)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIBRARY_OUTPUT_PATH ${CMAKE_BINARY_DIR})
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_BINARY_DIR})

add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>")
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/MT>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/MT>")
add_compile_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/MTd>")
add_compile_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:/MTd>")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/Zi>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/Zi>")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/permissive->")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/permissive->")
add_compile_options("$<$<C_COMPILER_ID:MSVC>:/wd4244>")
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/wd4244>")
add_link_options("$<$<C_COMPILER_ID:MSVC>:/INCREMENTAL:NO>")
add_link_options("$<$<CXX_COMPILER_ID:MSVC>:/INCREMENTAL:NO>")
add_link_options("$<$<C_COMPILER_ID:MSVC>:/DEBUG>")
add_link_options("$<$<CXX_COMPILER_ID:MSVC>:/DEBUG>")
add_link_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/OPT:REF>")
add_link_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/OPT:REF>")
add_link_options("$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/OPT:ICF>")
add_link_options("$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:/OPT:ICF>")

set(DEPS_DIR ../../deps)

set(THIRDPARTY_INCLUDES
    ${DEPS_DIR}/thirdparty/include
    ${DEPS_DIR}/thirdparty/include/Eigen-3.4.0
    ${DEPS_DIR}/thirdparty/include/Eigen-3.4.0/Eigen
    ${DEPS_DIR}/thirdparty/include/fmt
    ${DEPS_DIR}/thirdparty/include/spdlog
    ${DEPS_DIR}/thirdparty/include/opencv2
    ${DEPS_DIR}/thirdparty/include/onnxruntime
    ${DEPS_DIR}/thirdparty/include/OpenEXR
    ${DEPS_DIR}/thirdparty/include/ffmpeg
)

if(WIN32)
    set_property(GLOBAL PROPERTY USE_FOLDERS OFF)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT ${TARGET_NAME})
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

    link_directories(${DEPS_DIR}/thirdparty/lib)
    find_package(Icraft-HostBackend REQUIRED)
    if(TARGET_CHIP_UPPER STREQUAL "BY")
        find_package(Icraft-BuyiBackend REQUIRED)
    elseif(TARGET_CHIP_UPPER STREQUAL "ZG")
        find_package(icraft-zg330backend REQUIRED)
    endif()

    link_libraries(
        opencv_world455 opencv_img_hash455 ade IlmImf ippicvmt ippiw
        ittnotify libjpeg-turbo libopenjp2 libpng libprotobuf libtiff libwebp quirc
        fmt jpeg libpng16 tiff turbojpeg zlib webp webpdecoder webpdemux lzma
    )
elseif(UNIX)
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
    set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)
    set(CMAKE_BUILD_TYPE Release)

    link_directories(${DEPS_DIR}/thirdparty/a)
    link_libraries(
        avformat avdevice avfilter avcodec avutil swscale swresample
        opencv_imgcodecs opencv_imgproc opencv_core tegra_hal libjpeg-turbo ittnotify libpng libwebp libtiff libopenjp2
        IlmImf-2_3_s Iex-2_3_s Half-2_3_s IexMath-2_3_s IlmImfUtil-2_3_s IlmThread-2_3_s Imath-2_3_s
        fmt pthread dl zlib ssl crypto atomic
    )
    find_package(Icraft-HostBackend REQUIRED)
    if(TARGET_CHIP_UPPER STREQUAL "BY")
        find_package(Icraft-BuyiBackend REQUIRED)
    elseif(TARGET_CHIP_UPPER STREQUAL "ZG")
        find_package(icraft-zg330backend REQUIRED)
    endif()
endif()

add_executable(${TARGET_NAME} src/${TARGET_NAME}.cpp)

if(MSVC)
    set_property(TARGET ${TARGET_NAME} PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_include_directories(${TARGET_NAME}
    PUBLIC
    ${DEPS_DIR}/modelzoo_utils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${THIRDPARTY_INCLUDES}
)

if(TARGET_CHIP_UPPER STREQUAL "BY")
    target_link_libraries(${TARGET_NAME}
        PRIVATE
        Icraft::HostBackend Icraft::BuyiBackend
        $<$<CXX_COMPILER_ID:GNU>:stdc++fs>
    )
elseif(TARGET_CHIP_UPPER STREQUAL "ZG")
    target_link_libraries(${TARGET_NAME}
        PRIVATE
        Icraft::HostBackend Icraft::ZG330Backend
        $<$<CXX_COMPILER_ID:GNU>:stdc++fs>
    )
endif()
