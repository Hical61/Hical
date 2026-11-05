#!/usr/bin/env python3
"""验证 HTTP/2 CI 依赖的解包和构建参数，不下载或编译依赖。"""

from __future__ import annotations

import importlib.util
import io
import os
from pathlib import Path
import stat
import subprocess
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


MODULE_SPEC = importlib.util.spec_from_file_location(
    "hical_ci_http2_deps", Path(__file__).with_name("ci_http2_deps.py")
)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
DEPS = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(DEPS)


class ArchiveExtractionTest(unittest.TestCase):
    def setUp(self) -> None:
        """为每个用例创建隔离目录；不触碰已配置的依赖树。"""
        self.temporary = tempfile.TemporaryDirectory(prefix="hical-http2-deps-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.args = SimpleNamespace(root=self.directory / "workspace")
        self.archive = self.directory / "source.tar"
        self.source = {"directory": "boost_fixture", "sha256": "fixture-sha256"}

    def makeArchive(self, entries: list[tuple[tarfile.TarInfo, bytes]]) -> None:
        """把给定成员和内容写入本地归档，供真实 unpack 路径读取。"""
        with tarfile.open(self.archive, "w") as packed:
            for item, content in entries:
                item.size = len(content) if item.isfile() else 0
                packed.addfile(item, io.BytesIO(content) if item.isfile() else None)

    def regularFile(self, name: str, content: bytes, mode: int = 0o644) -> tuple[tarfile.TarInfo, bytes]:
        """构造源码目录内普通成员，返回成员元数据和正文。"""
        item = tarfile.TarInfo(self.source["directory"] + "/" + name)
        item.mode = mode
        return item, content

    def unpack(self) -> Path:
        """调用实际解包实现，只把下载步骤替换为本地归档。"""
        with mock.patch.object(DEPS, "archive", return_value=self.archive):
            return DEPS.unpack(self.args, self.source)

    def test_preserves_execute_bits_and_strips_special_permission_bits(self) -> None:
        script = b"#!/bin/sh\nprintf 'ready\\n'\n"
        data = b"ordinary data\n"
        self.makeArchive([
            self.regularFile("tools/build.sh", script, 0o6755),
            self.regularFile("include/data.txt", data, 0o7644),
        ])
        appliedModes: dict[Path, int] = {}
        originalChmod = os.chmod

        def recordChmod(path, mode, *args, **kwargs):
            # Windows 不表达 POSIX 执行位，但仍验证传给系统的完整权限值。
            appliedModes[Path(path).resolve()] = mode
            return originalChmod(path, mode, *args, **kwargs)

        with mock.patch.object(DEPS.os, "chmod", side_effect=recordChmod):
            root = self.unpack()

        self.assertEqual((root / "tools/build.sh").read_bytes(), script)
        self.assertEqual((root / "include/data.txt").read_bytes(), data)
        self.assertEqual(appliedModes.get((root / "tools/build.sh").resolve()), 0o755)
        self.assertEqual(appliedModes.get((root / "include/data.txt").resolve()), 0o644)
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE((root / "tools/build.sh").stat().st_mode), 0o755)
            self.assertEqual(stat.S_IMODE((root / "include/data.txt").stat().st_mode), 0o644)

    @unittest.skipUnless(os.name == "posix", "Windows 没有 POSIX 文件执行权限语义")
    def test_bootstrap_can_execute_its_extracted_child_script(self) -> None:
        self.makeArchive([
            self.regularFile("bootstrap.sh", b"#!/bin/sh\nexec ./tools/build/src/engine/build.sh\n", 0o755),
            self.regularFile("tools/build/src/engine/build.sh", b"#!/bin/sh\nprintf 'engine-ready\\n'\n", 0o755),
        ])
        root = self.unpack()

        # 和 Boost bootstrap 一样，入口交给 shell，内部脚本直接执行。
        result = subprocess.run(["sh", "./bootstrap.sh"], cwd=root, capture_output=True,
                                text=True, timeout=5, check=False)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "engine-ready\n")

    def test_rejects_members_outside_the_frozen_source_directory(self) -> None:
        for name in ("../escape.txt", "/escape.txt", "other/escape.txt", "boost_fixture/../escape.txt"):
            with self.subTest(name=name):
                item = tarfile.TarInfo(name)
                self.makeArchive([(item, b"must not escape")])

                with self.assertRaisesRegex(ValueError, "归档路径超出固定源码目录"):
                    self.unpack()

                self.assertFalse((self.directory / "escape.txt").exists())
                self.assertFalse((self.args.root / "escape.txt").exists())
                self.assertFalse((self.args.root / "src/escape.txt").exists())

    def test_does_not_extract_links_or_special_files(self) -> None:
        entries = [self.regularFile("include/data.txt", b"valid")]
        blockedNames = []
        for index, kind in enumerate((tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE,
                                     tarfile.BLKTYPE, tarfile.FIFOTYPE)):
            name = "blocked-" + str(index)
            item = tarfile.TarInfo(self.source["directory"] + "/" + name)
            item.type = kind
            item.linkname = "../../escape.txt"
            entries.append((item, b""))
            blockedNames.append(name)
        self.makeArchive(entries)

        root = self.unpack()

        self.assertEqual((root / "include/data.txt").read_bytes(), b"valid")
        for name in blockedNames:
            self.assertFalse((root / name).exists())
            self.assertFalse((root / name).is_symlink())

    def test_reuses_a_cache_created_with_the_current_extraction_rules(self) -> None:
        self.makeArchive([self.regularFile("include/data.txt", b"valid")])
        root = self.unpack()

        with mock.patch.object(DEPS.tarfile, "open", side_effect=AssertionError("缓存不应重新解包")):
            reused = self.unpack()

        self.assertEqual(reused, root)
        self.assertEqual((reused / "include/data.txt").read_bytes(), b"valid")

    def test_rejects_an_old_sha_only_cache_without_changing_it(self) -> None:
        root = self.args.root / "src" / self.source["directory"]
        root.mkdir(parents=True)
        marker = root / ".hical-archive-sha256"
        marker.write_text(self.source["sha256"] + "\n", encoding="utf-8")
        oldScript = root / "bootstrap.sh"
        oldScript.write_bytes(b"old cached script")

        with self.assertRaisesRegex(ValueError, r"解包规则.*--root"):
            self.unpack()

        self.assertEqual(oldScript.read_bytes(), b"old cached script")
        self.assertEqual(marker.read_text(encoding="utf-8"), self.source["sha256"] + "\n")


class BoostBuildSetupTest(unittest.TestCase):
    def setUp(self) -> None:
        """准备安装结果和工具链文件，只替换下载与编译的执行边界。"""
        self.temporary = tempfile.TemporaryDirectory(prefix="hical-boost-setup-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.source = self.directory / "boost-source"
        self.source.mkdir()
        self.args = SimpleNamespace(root=self.directory / "workspace", compiler="msvc", cxx="cl",
                                    configuration="Release", sanitizer="none", jobs=2)
        self.args.root.mkdir()
        self.args.prefix = self.args.root / "prefix"
        header = self.args.prefix / "include/boost/version.hpp"
        header.parent.mkdir(parents=True)
        header.write_text("#define BOOST_VERSION 109000\n", encoding="utf-8")
        package = self.args.prefix / "lib/cmake/Boost-1.90.0/BoostConfig.cmake"
        package.parent.mkdir(parents=True)
        package.write_text("# fixture\n", encoding="utf-8")
        self.compiler = self.directory / "active toolchain/bin/cl.exe"
        self.compiler.parent.mkdir(parents=True)
        self.compiler.touch()
        self.vcRoot = self.directory / "Visual Studio/VC"
        self.setupScript = self.vcRoot / "Auxiliary/Build/vcvarsall.bat"
        self.setupScript.parent.mkdir(parents=True)
        self.setupScript.write_text("@exit /b 0\n", encoding="utf-8")
        self.environment = {"COMSPEC": "fixture-cmd.exe", "VCToolsVersion": "14.51.36231",
                            "VCINSTALLDIR": str(self.vcRoot)}

    def runBuild(self, platform: str):
        """调用实际构建入口，返回记录下来的外部命令。"""
        simulatedOs = SimpleNamespace(name=platform, environ=self.environment)
        with mock.patch.object(DEPS, "os", simulatedOs), \
             mock.patch.object(DEPS, "unpack", return_value=self.source), \
             mock.patch.object(DEPS.shutil, "which", return_value=str(self.compiler)), \
             mock.patch.object(DEPS, "execute", return_value="") as execute:
            DEPS.build_boost(self.args)
        return execute

    def test_windows_gcc_bootstraps_without_starting_a_bare_bash(self) -> None:
        self.args.compiler = "gcc"
        self.args.cxx = "g++"
        commands = self.runBuild("nt").call_args_list

        bootstrap = commands[0].args[1]
        self.assertEqual(bootstrap[0], self.environment["COMSPEC"])
        self.assertEqual(bootstrap[-2:], ["bootstrap.bat", "gcc"])
        self.assertNotIn("bash", bootstrap)
        self.assertEqual(commands[0].kwargs["cwd"], self.source)
        self.assertEqual(Path(commands[1].args[1][0]).name, "b2.exe")

    def test_windows_msvc_uses_the_active_compiler_version_and_setup_script(self) -> None:
        commands = self.runBuild("nt").call_args_list

        self.assertEqual(commands[0].args[1][-2:], ["bootstrap.bat", "msvc"])
        configuration = (self.args.root / "user-config.jam").read_text(encoding="utf-8")
        # VS 18 不在旧 Boost 的探测表里，版本和 setup 都得来自当前开发环境。
        self.assertRegex(configuration, r"using msvc\s*:\s*14\.51\s*:")
        self.assertIn('"' + self.compiler.as_posix() + '"', configuration)
        self.assertIn('<setup>"' + self.setupScript.as_posix() + '"', configuration)
        self.assertIn("toolset=msvc-14.51", commands[1].args[1])

    def test_unix_clang_keeps_its_shell_and_sanitizer_flags(self) -> None:
        self.args.compiler = "clang"
        self.args.cxx = "clang++-20"
        self.args.sanitizer = "address,undefined"
        commands = self.runBuild("posix").call_args_list

        self.assertEqual(commands[0].args[1], ["bash", "./bootstrap.sh", "--with-toolset=clang"])
        self.assertEqual(Path(commands[1].args[1][0]).name, "b2")
        self.assertIn("toolset=clang-hical", commands[1].args[1])
        self.assertIn("cxxflags=-fsanitize=address,undefined -fno-omit-frame-pointer", commands[1].args[1])
        self.assertIn("linkflags=-fsanitize=address,undefined -fno-omit-frame-pointer", commands[1].args[1])

    def test_msvc_rejects_missing_or_invalid_toolset_version(self) -> None:
        for version in (None, "", "VS18", "14.51 invalid"):
            with self.subTest(version=version):
                if version is None:
                    self.environment.pop("VCToolsVersion", None)
                else:
                    self.environment["VCToolsVersion"] = version
                with self.assertRaisesRegex(ValueError, "VCToolsVersion"):
                    self.runBuild("nt")

    def test_msvc_rejects_a_missing_developer_environment_or_setup_script(self) -> None:
        self.environment.pop("VCINSTALLDIR")
        with self.assertRaisesRegex(ValueError, "VCINSTALLDIR"):
            self.runBuild("nt")

        self.environment["VCINSTALLDIR"] = str(self.vcRoot)
        self.setupScript.unlink()
        with self.assertRaisesRegex(ValueError, "vcvarsall"):
            self.runBuild("nt")


class PrefixReuseTest(unittest.TestCase):
    def setUp(self) -> None:
        """搭一个完整符号的 prefix，再按用例逐个破坏。"""
        self.temporary = tempfile.TemporaryDirectory(prefix="hical-prefix-reuse-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.args = SimpleNamespace(compiler="gcc")
        self.args.prefix = self.directory / "prefix"
        self.boostHeader = self.args.prefix / "include/boost/version.hpp"
        self.boostHeader.parent.mkdir(parents=True)
        self.boostHeader.write_text("#define BOOST_VERSION 109000\n", encoding="utf-8")
        self.nghttp2Header = self.args.prefix / "include/nghttp2/nghttp2ver.h"
        self.nghttp2Header.parent.mkdir(parents=True)
        self.nghttp2Header.write_text('#define NGHTTP2_VERSION "1.70.0"\n', encoding="utf-8")
        package = self.args.prefix / "lib/cmake/Boost-1.90.0/BoostConfig.cmake"
        package.parent.mkdir(parents=True)
        package.write_text("# fixture\n", encoding="utf-8")
        for name in ("libboost_json.a", "libnghttp2.a"):
            (self.args.prefix / "lib" / name).write_text("", encoding="utf-8")

    def test_complete_prefix_is_reused(self) -> None:
        self.assertTrue(DEPS.prefix_is_ready(self.args, with_interop=False))

    def test_missing_static_library_forces_a_rebuild(self) -> None:
        """只有头文件不能说明链接得上，静态库缺一个就得重编。"""
        (self.args.prefix / "lib/libnghttp2.a").unlink()
        self.assertFalse(DEPS.prefix_is_ready(self.args, with_interop=False))

    def test_msvc_prefix_looks_for_dot_lib(self) -> None:
        self.args.compiler = "msvc"
        for name in ("libboost_json.lib", "libnghttp2.lib"):
            (self.args.prefix / "lib" / name).write_text("", encoding="utf-8")
        for name in ("libboost_json.a", "libnghttp2.a"):
            (self.args.prefix / "lib" / name).unlink()
        self.assertTrue(DEPS.prefix_is_ready(self.args, with_interop=False))
        (self.args.prefix / "lib/libnghttp2.lib").unlink()
        self.assertFalse(DEPS.prefix_is_ready(self.args, with_interop=False))

    def test_wrong_installed_version_is_not_reused(self) -> None:
        self.boostHeader.write_text("#define BOOST_VERSION 108300\n", encoding="utf-8")
        self.assertFalse(DEPS.prefix_is_ready(self.args, with_interop=False))
        self.boostHeader.write_text("#define BOOST_VERSION 109000\n", encoding="utf-8")
        self.nghttp2Header.write_text('#define NGHTTP2_VERSION "1.64.0"\n', encoding="utf-8")
        self.assertFalse(DEPS.prefix_is_ready(self.args, with_interop=False))

    def test_interop_prefix_also_needs_every_tool(self) -> None:
        self.assertTrue(DEPS.prefix_is_ready(self.args, with_interop=False))
        self.assertFalse(DEPS.prefix_is_ready(self.args, with_interop=True))
        binary = self.args.prefix / "bin/h2spec"
        binary.parent.mkdir(parents=True)
        binary.write_text("", encoding="utf-8")
        # 只有 h2spec 不够，interop 还要 nghttp / nghttpd / h2load。
        self.assertFalse(DEPS.prefix_is_ready(self.args, with_interop=True))
        for name in ("nghttp", "nghttpd", "h2load"):
            (self.args.prefix / "bin" / name).write_text("", encoding="utf-8")
        self.assertTrue(DEPS.prefix_is_ready(self.args, with_interop=True))


if __name__ == "__main__":
    unittest.main()
