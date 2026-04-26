cmake_minimum_required(VERSION 3.16)

project(PSINWorkflow LANGUAGES CXX)

message(STATUS "Project name: ${PROJECT_NAME}")
message(STATUS "Project source dir: ${PROJECT_SOURCE_DIR}")
message(STATUS "Project binary dir: ${PROJECT_BINARY_DIR}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
file(MAKE_DIRECTORY ${PROJECT_SOURCE_DIR}/log)

set(PSIN_BUILD_PROFILE "zg330" CACHE STRING "Build profile: zg330 or host-tests")
set_property(CACHE PSIN_BUILD_PROFILE PROPERTY STRINGS zg330 host-tests)

function(psin_add_host_regression_test target_name)
    add_executable(${target_name} ${ARGN})
    target_include_directories(${target_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(${target_name} PRIVATE cxx_std_17)
    add_test(NAME ${target_name} COMMAND ${target_name})
endfunction()

if(PSIN_BUILD_PROFILE STREQUAL "host-tests")
    enable_testing()

    psin_add_host_regression_test(
        main_menu_regression_test
        tests/main_menu_regression_test.cpp
        src/config_utils.cpp
    )

    psin_add_host_regression_test(
        hdmi_stopped_regression_test
        tests/hdmi_stopped_regression_test.cpp
    )
    return()
endif()

if(NOT PSIN_BUILD_PROFILE STREQUAL "zg330")
    message(FATAL_ERROR "Unsupported PSIN_BUILD_PROFILE='${PSIN_BUILD_PROFILE}'. Expected 'zg330' or 'host-tests'.")
endif()

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "PSIN_BUILD_PROFILE=zg330 requires a Linux build host and ZG330 toolchain.")
endif()

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
    src/infer/ui_render.cpp
    src/infer_workflow.cpp
    src/manual_flight_runtime.cpp
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
