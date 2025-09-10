if (SIMFIL_SHARED)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif ()

# sfl
CPMAddPackage("gh:slavenf/sfl-library#1.10.1")

# fmt (library target required, so ensure header-only is OFF)
CPMAddPackage(
    URI "gh:fmtlib/fmt#11.1.4"
    OPTIONS "FMT_HEADER_ONLY OFF")

# bitsery
CPMAddPackage("gh:fraillt/bitsery@5.2.4")

# tl::expected
CPMAddPackage(
    URI "gh:TartanLlama/expected@1.1.0"
    OPTIONS
        "EXPECTED_BUILD_TESTS OFF"
        "EXPECTED_BUILD_PACKAGE_DEB OFF")

# nlohmann/json
if (SIMFIL_WITH_MODEL_JSON)
    CPMAddPackage("gh:nlohmann/json@3.11.3")
endif ()

# Catch2
if (SIMFIL_WITH_TESTS)
    CPMAddPackage(
        URI "gh:catchorg/Catch2@3.5.2"
        OPTIONS
            "CATCH_INSTALL_DOCS OFF"
            "CATCH_INSTALL_EXTRAS OFF")
endif ()
