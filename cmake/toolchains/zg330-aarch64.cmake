set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER /usr/bin/aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /usr/bin/aarch64-linux-gnu-g++)

# Avoid try-run checks on the build host during cross compilation.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
