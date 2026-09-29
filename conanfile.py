from typing import ClassVar

from conan import ConanFile


class FrameConan(ConanFile):
    package_type = "application"
    settings = "os", "arch", "compiler", "build_type"

    requires = (
        "benchmark/1.9.5",
        "gtest/1.17.0",
        "lua/5.4.8",
        "libpng/1.6.58",
        "zstd/1.5.7",
    )

    default_options: ClassVar[dict[str, bool]] = {
        "lua/*:shared": False,
        "zstd/*:build_programs": False,
        "zstd/*:shared": False,
    }

    generators = "CMakeDeps", "CMakeToolchain"
