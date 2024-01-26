#!/usr/bin/env python3

from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout


class SimfilRecipe(ConanFile):
    name = "simfil"
    version = "1.0"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "fPIC": [True, False],
        "with_json": [True, False],
    }
    default_options = {
        "fPIC": True,
        "with_json": True,
    }

    # Sources are located in the same place as this recipe, copy them to the recipe
    exports_sources = "CMakeLists.txt", "include/*", "src/*"

    # Requirements
    def requirements(self):
        self.requires("bitsery/5.2.3")
        if self.options.with_json:
            self.requires("nlohmann_json/3.11.2")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        variables = {
            "SIMFIL_WITH_REPL": "FALSE",
            "SIMFIL_WITH_COVERAGE": "FALSE",
            "SIMFIL_WITH_EXAMPLES": "FALSE",
            "SIMFIL_WITH_TESTS": "FALSE",
        }
        if self.options.with_json:
            variables["SIMFIL_WITH_MODEL_JSON"] = "TRUE"

        cmake = CMake(self)
        cmake.configure(variables=variables)
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["simfil"]
        self.cpp_info.set_property("cmake_target_name", "simfil::simfil")
