// SPDX-License-Identifier: MIT
#include "Platform.h"

#include <algorithm>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#endif

namespace dlss5 {

static void normalizeSlashes(std::string& s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
}

std::string moduleDir(const void* addr_in_module) {
#ifdef _WIN32
    HMODULE hMod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr_in_module, &hMod)) {
        return std::string();
    }
    char modPath[MAX_PATH] = {0};
    if (!GetModuleFileNameA(hMod, modPath, MAX_PATH)) return std::string();
    std::string p(modPath);
#else
    Dl_info info;
    if (dladdr(const_cast<void*>(addr_in_module), &info) == 0 || !info.dli_fname) {
        return std::string();
    }
    std::string p(info.dli_fname);
    // dladdr can hand back a relative path if the .so was loaded by one.
    p = absolutePath(p);
#endif
    normalizeSlashes(p);
    size_t slash = p.rfind('/');
    if (slash == std::string::npos) return std::string();
    return p.substr(0, slash);
}

std::string homeDir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    if (!h || !*h) return std::string();
    std::string s(h);
    normalizeSlashes(s);
    return s;
}

std::string absolutePath(const std::string& path) {
    if (path.empty()) return path;
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    if (GetFullPathNameA(path.c_str(), MAX_PATH, buf, nullptr)) {
        std::string s(buf);
        normalizeSlashes(s);
        return s;
    }
    return path;
#else
    char buf[PATH_MAX] = {0};
    if (::realpath(path.c_str(), buf)) {
        std::string s(buf);
        normalizeSlashes(s);
        return s;
    }
    return path;
#endif
}

} // namespace dlss5
