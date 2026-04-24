cmake_minimum_required(VERSION 3.16)

if(WIN32)
    message(FATAL_ERROR "main/ psin_workflow is Linux/ZG only. Windows builds are intentionally unsupported for this stage.")
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)
set(CMAKE_BUILD_TYPE Release)

project(PSINWorkflow LANGUAGES CXX)

message(STATUS "Project name: ${PROJECT_NAME}")
message(STATUS "Project source dir: ${PROJECT_SOURCE_DIR}")
message(STATUS "Project binary dir: ${PROJECT_BINARY_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
file(MAKE_DIRECTORY ${PROJECT_SOURCE_DIR}/log)

add_compile_definitions(USE_ZG330_BACKEND)

set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps)

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

set(COMMON_INCLUDE_DIRS
    ${DEPS_DIR}/modelzoo_utils/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${THIRDPARTY_INCLUDES}
)

set(OPENCV_STATIC_LIBS
    opencv_imgcodecs opencv_imgproc opencv_core
    tegra_hal libjpeg-turbo ittnotify libpng libwebp libtiff libopenjp2
    IlmImf-2_3_s Iex-2_3_s Half-2_3_s IexMath-2_3_s
    IlmImfUtil-2_3_s IlmThread-2_3_s Imath-2_3_s
)

link_directories(${DEPS_DIR}/thirdparty/a)
find_package(Icraft-HostBackend REQUIRED)
find_package(icraft-zg330backend REQUIRED)

add_executable(psin_workflow
    src/main.cpp
    src/config_utils.cpp
    src/rd_config.cpp
    src/rd_imaging_stream.cpp
    src/infer_config.cpp
    src/infer_workflow.cpp
    src/web_console.cpp
    src/web_console_config.cpp
    src/web_console_protocol.cpp
    src/web_console_controller.cpp
    src/web_console_server.cpp
    src/web_console_assets.cpp
)
target_include_directories(psin_workflow PRIVATE ${COMMON_INCLUDE_DIRS})
target_link_libraries(psin_workflow
    PRIVATE
    Icraft::HostBackend Icraft::ZG330Backend
    avformat avdevice avfilter avcodec avutil swscale swresample
    ${OPENCV_STATIC_LIBS}
    fmt pthread dl zlib ssl crypto atomic
    $<$<CXX_COMPILER_ID:GNU>:stdc++fs>
)
