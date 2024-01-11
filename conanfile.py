#!/usr/bin/env python3

from conan import ConanFile
from conan.tools.cmake import CMakeToolchain, CMakeDeps, CMake, cmake_layout


class SimfilRecipe(ConanFile):
    name = "simfil"
    version = "1.0"

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {"shared": [True, False], "fPIC": [True, False], "with_json": [True, False], "with_tests": [True, False], "with_coverage": [True, False]}
    default_options = {"shared": False, "fPIC": True, "with_json": True, "with_tests": False, "with_coverage": False}

    # Sources are located in the same place as this recipe, copy them to the recipe
    exports_sources = "CMakeLists.txt", "src/*", "include/*", "repl/*"

    # Requirements
    def requirements(self):
        self.requires("bitsery/5.2.3")
        if self.options.with_json:
            self.requires("nlohmann_json/3.11.2")
        if self.options.with_tests:
            self.requires("catch2/3.5.1")

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
        variables = {}
        if self.options.with_json:
            variables["SIMFIL_WITH_MODEL_JSON"] = "TRUE"
        if self.options.with_tests:
            variables["SIMFIL_WITH_TESTS"] = "TRUE"
        if self.options.with_coverage:
            variables["SIMFIL_WITH_COVERAGE"] = "TRUE"

        cmake = CMake(self)
        cmake.configure(variables=variables)
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
