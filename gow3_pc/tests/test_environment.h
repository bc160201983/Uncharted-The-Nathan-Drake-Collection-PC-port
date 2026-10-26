#pragma once
#include <cassert>
#include <cstdlib>

inline void SetTestEnv(const char* name, const char* value) {
#ifdef _WIN32
    const int result = _putenv_s(name, value ? value : "");
#else
    const int result = value ? setenv(name, value, 1) : unsetenv(name);
#endif
    assert(result == 0);
    (void)result;
}
