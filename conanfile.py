#!/usr/bin/env python3

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout


required_conan_version = ">=1.62.0"


# NOTE: The recipe used for conan-center-index is a copy of this
#       file with the version removed! Make sure to copy changes
#       in this file over to the conan-center-index recipe.
class SimfilRecipe(ConanFile):
    name = "simfil"
    version = "dev"
    url = "https://github.com/conan-io/conan-center-index"
    homepage = "https://github.com/Klebert-Engineering/simfil"
    license = "BSD 3-Clause"
    topics = ["query language"]

    # Binary configuration
    settings = "os", "compiler", "build_type", "arch"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
        "with_json": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
        "with_json": True,
    }

    exports_sources = "CMakeLists.txt", "*.cmake", "include/*", "src/*"

    def validate(self):
        check_min_cppstd(self, "20")

    def requirements(self):
        self.requires("sfl/1.2.4", transitive_headers=True)
        self.requires("fmt/10.2.1", transitive_headers=True)
        self.requires("bitsery/5.2.3", transitive_headers=True)
        if self.options.with_json:
            self.requires("nlohmann_json/3.11.2")

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def layout(self):
        cmake_layout(self)

    def generate(self):
        tc = CMakeToolchain(self)
        tc.cache_variables["SIMFIL_CONAN"] = True
        tc.cache_variables["SIMFIL_SHARED"] = bool(self.options.get_safe("shared"))
        tc.cache_variables["SIMFIL_WITH_REPL"] = False
        tc.cache_variables["SIMFIL_WITH_COVERAGE"] = False
        tc.cache_variables["SIMFIL_WITH_TESTS"] = False
        tc.cache_variables["SIMFIL_WITH_EXAMPLES"] = False
        tc.cache_variables["SIMFIL_WITH_MODEL_JSON"] = bool(self.options.with_json)
        tc.generate()
        deps = CMakeDeps(self)
        deps.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()

    def package_info(self):
        self.cpp_info.libs = ["simfil"]
        self.cpp_info.set_property("cmake_target_name", "simfil::simfil")
