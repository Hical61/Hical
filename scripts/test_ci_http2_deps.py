#!/usr/bin/env python3
"""验证 HTTP/2 CI 依赖解包的权限、缓存和路径边界，不下载或构建依赖。"""

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


if __name__ == "__main__":
    unittest.main()
