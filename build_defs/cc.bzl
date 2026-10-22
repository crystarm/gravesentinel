"""Project-local C++ rules with a uniform C++23 warning policy."""

_PROJECT_COPTS = [
    "-std=c++23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-Wconversion",
    "-Wsign-conversion",
    "-Wshadow",
    "-Wnon-virtual-dtor",
    "-Wold-style-cast",
    "-Woverloaded-virtual",
    "-Wnull-dereference",
    "-Wdouble-promotion",
    "-Wformat=2",
]

_PROJECT_DEFINES = ["GRAVESENTINEL_CXX_STANDARD=23"]


def gravesentinel_cc_library(name, copts = [], defines = [], **kwargs):
    """Declares a project C++ library without affecting external repositories."""
    native.cc_library(
        name = name,
        copts = _PROJECT_COPTS + copts,
        defines = _PROJECT_DEFINES + defines,
        **kwargs
    )


def gravesentinel_cc_binary(name, copts = [], defines = [], **kwargs):
    """Declares a project C++ binary with the common compile policy."""
    native.cc_binary(
        name = name,
        copts = _PROJECT_COPTS + copts,
        defines = _PROJECT_DEFINES + defines,
        **kwargs
    )


def gravesentinel_cc_test(name, copts = [], defines = [], **kwargs):
    """Declares a project C++ test with the common compile policy."""
    native.cc_test(
        name = name,
        copts = _PROJECT_COPTS + copts,
        defines = _PROJECT_DEFINES + defines,
        **kwargs
    )
