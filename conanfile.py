import os
import re

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, CMakeDeps, cmake_layout
from conan.tools.files import copy, rmdir, load
from conan.tools.build import check_min_cppstd


class HicalConan(ConanFile):
    name = "hical"
    license = "MIT"
    url = "https://github.com/Hical61/Hical"
    homepage = "https://github.com/Hical61/Hical"
    description = "Modern C++20/C++26 high-performance web framework based on Boost.Asio"
    topics = ("web-framework", "http", "websocket", "boost-asio", "coroutine", "cpp20")
    package_type = "static-library"

    settings = "os", "compiler", "build_type", "arch"
    options = {
        "fPIC": [True, False],
        "with_reflection": [True, False],
        "with_database": [True, False],
        "with_openapi": [True, False],
        "with_mimalloc": [True, False],
    }
    default_options = {
        "fPIC": True,
        "with_reflection": False,
        "with_database": False,
        "with_openapi": True,
        "with_mimalloc": False,
    }

    exports_sources = (
        "CMakeLists.txt",
        "cmake/*",
        "src/*",
        "LICENSE",
    )

    def set_version(self):
        content = load(self, os.path.join(self.recipe_folder, "CMakeLists.txt"))
        match = re.search(r"project\s*\(\s*hical\s+VERSION\s+(\d+\.\d+\.\d+)", content)
        if match:
            self.version = match.group(1)

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def validate(self):
        check_min_cppstd(self, 20)
        if self.options.with_reflection:
            check_min_cppstd(self, 26)

    def requirements(self):
        self.requires("boost/1.90.0", transitive_headers=True, transitive_libs=True)
        self.requires("openssl/[>=1.1.0]", transitive_headers=True, transitive_libs=True)
        self.requires("zlib/[>=1.2.11]", transitive_headers=True, transitive_libs=True)
        if self.options.with_mimalloc:
            self.requires("mimalloc/[>=2.0]")

    def layout(self):
        cmake_layout(self, src_folder=".")

    def generate(self):
        deps = CMakeDeps(self)
        deps.generate()

        tc = CMakeToolchain(self)
        tc.variables["HICAL_BUILD_TESTS"] = False
        tc.variables["HICAL_BUILD_EXAMPLES"] = False
        tc.variables["HICAL_ENABLE_REFLECTION"] = bool(self.options.with_reflection)
        tc.variables["HICAL_WITH_DATABASE"] = bool(self.options.with_database)
        tc.variables["HICAL_WITH_OPENAPI"] = bool(self.options.with_openapi)
        tc.variables["HICAL_WITH_MIMALLOC"] = bool(self.options.with_mimalloc)
        tc.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        copy(self, "LICENSE", src=self.source_folder, dst=os.path.join(self.package_folder, "licenses"))
        cmake = CMake(self)
        cmake.install()
        rmdir(self, os.path.join(self.package_folder, "lib", "cmake"))

    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "hical")
        self.cpp_info.set_property("cmake_target_name", "hical::hical_core")

        self.cpp_info.libs = ["hical_core"]

        if self.settings.os == "Windows":
            self.cpp_info.system_libs.extend(["ws2_32", "mswsock"])

        if self.settings.os in ["Linux", "FreeBSD"]:
            self.cpp_info.system_libs.append("pthread")

        # OpenAPI（默认开启，可通过 with_openapi=False 关闭）
        if self.options.with_openapi:
            self.cpp_info.defines.append("HICAL_HAS_OPENAPI=1")

        if self.options.with_database:
            self.cpp_info.defines.append("HICAL_HAS_DATABASE=1")
