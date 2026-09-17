#pragma once

#if defined(_WIN32) && defined(ADBCPP_SHARED)
#    if defined(ADBCPP_EXPORTS)
#        define ADBCPP_API __declspec(dllexport)
#    else
#        define ADBCPP_API __declspec(dllimport)
#    endif
#elif defined(__GNUC__) || defined(__clang__)
#    define ADBCPP_API __attribute__((visibility("default")))
#else
#    define ADBCPP_API
#endif
