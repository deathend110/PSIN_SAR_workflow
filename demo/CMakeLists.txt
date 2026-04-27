cmake_minimum_required(VERSION 3.16)

project(DemoSimpleInfer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    message(FATAL_ERROR "DemoSimpleInfer requires the same Linux/ZG330 toolchain style as the main workflow. Please build demo on a Linux host.")
endif()

set(DEMO_DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps)
set(DEMO_LOG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/log)

file(MAKE_DIRECTORY ${DEMO_LOG_DIR})
message(STATUS "Demo log dir: ${DEMO_LOG_DIR}")

set(DEMO_THIRDPARTY_INCLUDES
    ${DEMO_DEPS_DIR}/thirdparty/include
    ${DEMO_DEPS_DIR}/thirdparty/include/Eigen-3.4.0
    ${DEMO_DEPS_DIR}/thirdparty/include/Eigen-3.4.0/Eigen
    ${DEMO_DEPS_DIR}/thirdparty/include/fmt
    ${DEMO_DEPS_DIR}/thirdparty/include/opencv2
)

set(DEMO_INCLUDE_DIRS
    ${DEMO_DEPS_DIR}/modelzoo_utils/include
    ${DEMO_THIRDPARTY_INCLUDES}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set(DEMO_OPENCV_STATIC_LIBS
    opencv_imgcodecs opencv_imgproc opencv_core
    tegra_hal libjpeg-turbo ittnotify libpng libwebp libtiff libopenjp2
    IlmImf-2_3_s Iex-2_3_s Half-2_3_s IexMath-2_3_s
    IlmImfUtil-2_3_s IlmThread-2_3_s Imath-2_3_s
)

option(DEMO_BUILD_TESTS "Build demo host-side tests" ON)

link_directories(${DEMO_DEPS_DIR}/thirdparty/a)
find_package(Icraft-HostBackend REQUIRED)
find_package(icraft-zg330backend REQUIRED)

add_library(demo_support STATIC
    src/demo_config.cpp
    src/image_collector.cpp
    src/result_writer.cpp
)
target_include_directories(demo_support PUBLIC ${DEMO_INCLUDE_DIRS})
target_link_libraries(demo_support
    PUBLIC
    ${DEMO_OPENCV_STATIC_LIBS}
    fmt pthread dl zlib ssl crypto atomic
    $<$<CXX_COMPILER_ID:GNU>:stdc++fs>
)

add_executable(demo_simple_infer
    src/demo_main.cpp
    src/model_runner.cpp
)
target_include_directories(demo_simple_infer PRIVATE ${DEMO_INCLUDE_DIRS})
target_link_libraries(demo_simple_infer
    PRIVATE
    demo_support
    Icraft::HostBackend Icraft::ZG330Backend
    avformat avdevice avfilter avcodec avutil swscale swresample
    ${DEMO_OPENCV_STATIC_LIBS}
    fmt pthread dl zlib ssl crypto atomic
    $<$<CXX_COMPILER_ID:GNU>:stdc++fs>
)

target_compile_definitions(demo_simple_infer PRIVATE WISE_ENUM_OPTIONAL_TYPE=std::optional)

if(DEMO_BUILD_TESTS)
    enable_testing()

    add_executable(demo_config_test tests/demo_config_test.cpp)
    target_include_directories(demo_config_test PRIVATE ${DEMO_INCLUDE_DIRS})
    target_link_libraries(demo_config_test PRIVATE demo_support)
    add_test(NAME demo_config_test COMMAND demo_config_test)

    add_executable(image_collector_test tests/image_collector_test.cpp)
    target_include_directories(image_collector_test PRIVATE ${DEMO_INCLUDE_DIRS})
    target_link_libraries(image_collector_test PRIVATE demo_support)
    add_test(NAME image_collector_test COMMAND image_collector_test)

    add_executable(result_writer_test tests/result_writer_test.cpp)
    target_include_directories(result_writer_test PRIVATE ${DEMO_INCLUDE_DIRS})
    target_link_libraries(result_writer_test PRIVATE demo_support)
    add_test(NAME result_writer_test COMMAND result_writer_test)
endif()
