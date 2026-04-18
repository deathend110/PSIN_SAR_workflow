#pragma once

// 这个头文件只是一个很小的兼容层：
// - Windows 下系统没有 unistd.h，于是这里补一个 usleep
// - Linux 下则继续使用系统原生的 unistd.h
#ifdef _WIN32
#include <windows.h>

inline int usleep(unsigned int usec)
{
    // Windows 的 Sleep 单位是毫秒，因此这里做一次向上取整转换。
    ::Sleep((usec + 999U) / 1000U);
    return 0;
}

#else
#include_next <unistd.h>
#endif
