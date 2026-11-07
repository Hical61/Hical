#!/usr/bin/env python3
"""测试 hical new 的参数、生成的文件和写入失败后的清理。"""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from contextlib import redirect_stderr, redirect_stdout
import errno
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock


CLI_PATH = Path(__file__).with_name("hical.py").resolve()
MODULE_SPEC = importlib.util.spec_from_file_location("hical_scaffold", CLI_PATH)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
HICAL = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(HICAL)
EXPECTED_FILES = {
    "CMakeLists.txt", "README.md", ".gitignore", "src/app.h", "src/app.cpp",
    "src/main.cpp", "tests/test_app.cpp",
}


class ScaffoldCliTest(unittest.TestCase):
    def setUp(self) -> None:
        """给每个用例一个临时目录，跑完自动清理。"""
        self.temporary = tempfile.TemporaryDirectory(prefix="hical-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()

    def runCli(self, *arguments: str, cwd: Path | None = None) -> subprocess.CompletedProcess:
        """从源码运行 CLI，返回退出码和 UTF-8 输出。"""
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        return subprocess.run([sys.executable, str(CLI_PATH), *map(str, arguments)],
                              cwd=cwd or self.directory, env=environment, capture_output=True,
                              text=True, encoding="utf-8", timeout=10, check=False)

    def callMain(self, *arguments: str) -> tuple[int, str, str]:
        """在当前进程调用 main，便于模拟写入失败。返回退出码、标准输出和标准错误。"""
        output, error = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(error):
            code = HICAL.main(list(map(str, arguments)))
        return code, output.getvalue(), error.getvalue()

    def fileContents(self, directory: Path) -> dict[str, bytes]:
        """读取目录里的文件，以相对路径为键，原始字节为值。"""
        return {path.relative_to(directory).as_posix(): path.read_bytes()
                for path in directory.rglob("*") if path.is_file()}

    def createLink(self, link: Path, target: Path) -> None:
        """创建目录符号链接。系统不支持或没有权限时跳过测试。"""
        try:
            link.symlink_to(target, target_is_directory=True)
        except NotImplementedError:
            self.skipTest("当前系统未实现符号链接")
        except OSError as error:
            if error.errno in (errno.EPERM, errno.EACCES, errno.ENOTSUP) or getattr(error, "winerror", None) == 1314:
                self.skipTest("当前账户无创建符号链接的权限")
            raise

    def test_help_succeeds_without_creating_files(self) -> None:
        for arguments in (("--help",), ("new", "--help")):
            with self.subTest(arguments=arguments):
                result = self.runCli(*arguments)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn("用法", result.stdout)
                self.assertEqual(list(self.directory.iterdir()), [])

    def test_invalid_arguments_return_two_without_writes(self) -> None:
        for arguments in ((), ("new",), ("gen",), ("dev",), ("new", "demo", "--unknown"),
                          ("new", "demo", "--directory")):
            with self.subTest(arguments=arguments):
                result = self.runCli(*arguments)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("参数错误", result.stderr)
                self.assertEqual(list(self.directory.iterdir()), [])

    def test_rejects_invalid_names_and_windows_device_names(self) -> None:
        invalidNames = ("", "_demo", "9demo", ".", "..", "a/b", "a\\b", "a.b", "a b",
                        "中文", "a\n", "a" * 65, "CON", "prn", "AuX", "nul", "COM1", "com9", "LPT1", "lpt9")
        for name in invalidNames:
            with self.subTest(name=name):
                result = self.runCli("new", name)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(list(self.directory.iterdir()), [])

    def test_accepts_name_boundaries_and_non_reserved_prefixes(self) -> None:
        for name in ("A", "a" * 64, "demo-api_2", "CONSOLE", "COM10", "LPT10"):
            with self.subTest(name=name):
                result = self.runCli("new", name)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(set(self.fileContents(self.directory / name)), EXPECTED_FILES)

    def test_relative_parent_with_unicode_and_spaces_from_another_cwd(self) -> None:
        parent = self.directory / "中文 空格"
        working = self.directory / "working"
        parent.mkdir()
        working.mkdir()
        result = self.runCli("new", "demo-api", "--directory", "../中文 空格", cwd=working)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(set(self.fileContents(parent / "demo-api")), EXPECTED_FILES)
        self.assertEqual(list(working.iterdir()), [])

    def test_gbk_stdout_escapes_unencodable_paths_without_changing_encoding(self) -> None:
        parent = self.directory / "扩展𠀀 😀"
        parent.mkdir()
        reference = HICAL.generateProject("demo", self.directory)
        outputBytes, errorBytes = io.BytesIO(), io.BytesIO()
        output = io.TextIOWrapper(outputBytes, encoding="gbk", errors="strict")
        error = io.TextIOWrapper(errorBytes, encoding="gbk", errors="strict")
        with redirect_stdout(output), redirect_stderr(error):
            code = HICAL.main(["new", "demo", "--directory", str(parent)])
        output.flush()
        error.flush()
        self.assertEqual(code, 0)
        self.assertEqual(output.encoding, "gbk")
        self.assertEqual(error.encoding, "gbk")
        text = outputBytes.getvalue().decode("gbk")
        self.assertIn("\\U00020000", text)
        self.assertIn("\\U0001f600", text)
        self.assertNotIn(b"Traceback", errorBytes.getvalue())
        self.assertEqual(self.fileContents(parent / "demo"), self.fileContents(reference))

    def test_gbk_stderr_preserves_io_and_argument_exit_codes(self) -> None:
        parent = self.directory / "扩展𠀀 😀"
        parent.mkdir()
        target = HICAL.generateProject("demo", parent)
        before = self.fileContents(target)
        outputBytes, errorBytes = io.BytesIO(), io.BytesIO()
        output = io.TextIOWrapper(outputBytes, encoding="gbk", errors="strict")
        error = io.TextIOWrapper(errorBytes, encoding="gbk", errors="strict")
        with redirect_stdout(output), redirect_stderr(error):
            code = HICAL.main(["new", "demo", "--directory", str(parent)])
            with self.assertRaises(SystemExit) as stopped:
                HICAL.main(["new", "demo", "--未知𠀀😀"])
        output.flush()
        error.flush()
        self.assertEqual(code, 1)
        self.assertEqual(stopped.exception.code, 2)
        self.assertEqual(error.encoding, "gbk")
        self.assertIn("\\U00020000", errorBytes.getvalue().decode("gbk"))
        self.assertNotIn(b"Traceback", errorBytes.getvalue())
        self.assertEqual(self.fileContents(target), before)

    @unittest.skipUnless(os.name == "nt", "需要 Windows 的默认输出编码")
    def test_windows_default_encoding_succeeds_without_utf8_environment_overrides(self) -> None:
        parent = self.directory / "扩展𠀀 😀"
        parent.mkdir()
        environment = os.environ.copy()
        environment.pop("PYTHONUTF8", None)
        environment.pop("PYTHONIOENCODING", None)
        result = subprocess.run([sys.executable, str(CLI_PATH), "new", "demo", "--directory", str(parent)],
                                cwd=self.directory, env=environment, capture_output=True, timeout=10, check=False)
        self.assertEqual(result.returncode, 0, result.stderr.decode("ascii", errors="backslashreplace"))
        self.assertNotIn(b"Traceback", result.stderr)
        self.assertEqual(set(self.fileContents(parent / "demo")), EXPECTED_FILES)

    def test_output_is_deterministic_utf8_with_lf(self) -> None:
        first, second = self.directory / "first", self.directory / "second"
        first.mkdir()
        second.mkdir()
        for parent in (first, second):
            result = self.runCli("new", "demo-api", "--directory", parent)
            self.assertEqual(result.returncode, 0, result.stderr)
        generated = self.fileContents(first / "demo-api")
        self.assertEqual(set(generated), EXPECTED_FILES)
        self.assertEqual(generated, self.fileContents(second / "demo-api"))
        for relative, content in generated.items():
            with self.subTest(relative=relative):
                decoded = content.decode("utf-8")
                self.assertNotIn("\r", decoded)
                self.assertNotIn("@PROJECT_NAME@", decoded)
                self.assertNotIn("@TARGET_NAME@", decoded)

    def test_existing_directory_empty_directory_and_file_are_preserved(self) -> None:
        for kind in ("directory", "empty", "file"):
            with self.subTest(kind=kind):
                parent = self.directory / kind
                parent.mkdir()
                target = parent / "demo"
                if kind == "file":
                    target.write_bytes(b"existing-file")
                else:
                    target.mkdir()
                    if kind == "directory":
                        (target / "user.txt").write_bytes(b"existing-user-content")
                before = target.read_bytes() if target.is_file() else self.fileContents(target)
                result = self.runCli("new", "demo", "--directory", parent)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("目标已存在", result.stderr)
                after = target.read_bytes() if target.is_file() else self.fileContents(target)
                self.assertEqual(before, after)

    def test_existing_directory_symlink_is_preserved(self) -> None:
        real = self.directory / "real"
        real.mkdir()
        (real / "user.txt").write_bytes(b"untouched")
        link = self.directory / "demo"
        self.createLink(link, real)
        result = self.runCli("new", "demo")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.readlink(), real)
        self.assertEqual(self.fileContents(real), {"user.txt": b"untouched"})

    def test_existing_dangling_symlink_is_preserved(self) -> None:
        missing = self.directory / "missing"
        link = self.directory / "demo"
        self.createLink(link, missing)
        result = self.runCli("new", "demo")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertTrue(link.is_symlink())
        self.assertEqual(link.readlink(), missing)
        self.assertFalse(missing.exists())

    def test_symlink_loop_parent_returns_one_without_traceback(self) -> None:
        link = self.directory / "loop"
        self.createLink(link, link)
        result = self.runCli("new", "demo", "--directory", link)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertTrue(link.is_symlink())
        self.assertFalse((self.directory / "demo").exists())

    def test_nonexistent_and_file_parent_return_one_without_creating_ancestors(self) -> None:
        parentFile = self.directory / "parent.txt"
        parentFile.write_bytes(b"keep-parent")
        for parent in (self.directory / "missing" / "nested", parentFile):
            with self.subTest(parent=parent):
                result = self.runCli("new", "demo", "--directory", parent)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertNotIn("Traceback", result.stderr)
        self.assertFalse((self.directory / "missing").exists())
        self.assertEqual(parentFile.read_bytes(), b"keep-parent")

    def test_repeated_generation_preserves_the_first_project(self) -> None:
        first = self.runCli("new", "demo")
        self.assertEqual(first.returncode, 0, first.stderr)
        target = self.directory / "demo"
        (target / "src/app.cpp").write_bytes(b"user-edited-content")
        before = self.fileContents(target)
        result = self.runCli("new", "demo")
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(self.fileContents(target), before)

    def test_missing_template_is_reported_before_creating_target(self) -> None:
        incomplete = self.directory / "templates"
        incomplete.mkdir()
        (incomplete / "CMakeLists.txt.in").write_text("project(@PROJECT_NAME@)\n", encoding="utf-8")
        with mock.patch.object(HICAL, "TEMPLATE_ROOT", incomplete):
            code, _, error = self.callMain("new", "demo", "--directory", self.directory)
        self.assertEqual(code, 1)
        self.assertIn("生成失败", error)
        self.assertFalse((self.directory / "demo").exists())

    def test_second_file_open_failure_rolls_back_only_created_content(self) -> None:
        originalOpen = Path.open
        opened = []
        neighbor = self.directory / "neighbor.txt"
        neighbor.write_bytes(b"keep-neighbor")

        def failSecond(path, mode="r", *args, **kwargs):
            if mode == "x":
                opened.append(path)
                if len(opened) == 2:
                    raise OSError("第二个文件打不开")
            return originalOpen(path, mode, *args, **kwargs)

        with mock.patch.object(Path, "open", new=failSecond):
            code, _, error = self.callMain("new", "demo", "--directory", self.directory)
        self.assertEqual(code, 1)
        self.assertIn("第二个文件打不开", error)
        self.assertFalse((self.directory / "demo").exists())
        self.assertEqual(neighbor.read_bytes(), b"keep-neighbor")

    def test_partial_write_failure_removes_the_partially_written_file(self) -> None:
        originalOpen = Path.open
        opened = []

        class PartialWriter:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                return self

            def __exit__(self, *unused):
                self.stream.close()

            def write(self, content):
                self.stream.write(content[:5])
                raise OSError("文件写到一半出错")

        def failSecondWrite(path, mode="r", *args, **kwargs):
            stream = originalOpen(path, mode, *args, **kwargs)
            if mode == "x":
                opened.append(path)
                if len(opened) == 2:
                    return PartialWriter(stream)
            return stream

        with mock.patch.object(Path, "open", new=failSecondWrite):
            code, _, error = self.callMain("new", "demo", "--directory", self.directory)
        self.assertEqual(code, 1)
        self.assertIn("文件写到一半出错", error)
        self.assertFalse((self.directory / "demo").exists())

    def test_rollback_preserves_foreign_file_and_reports_the_residual_directory(self) -> None:
        originalOpen = Path.open
        target = self.directory / "demo"
        opened = []

        def failWithForeignFile(path, mode="r", *args, **kwargs):
            if mode == "x":
                opened.append(path)
                if len(opened) == 2:
                    with originalOpen(target / "user.txt", "x", encoding="utf-8") as foreign:
                        foreign.write("保留外来文件")
                    raise OSError("写文件失败")
            return originalOpen(path, mode, *args, **kwargs)

        with mock.patch.object(Path, "open", new=failWithForeignFile):
            code, _, error = self.callMain("new", "demo", "--directory", self.directory)
        self.assertEqual(code, 1)
        self.assertIn("无法清理", error)
        self.assertIn(str(target), error)
        self.assertEqual(self.fileContents(target), {"user.txt": "保留外来文件".encode("utf-8")})

    def test_concurrent_same_name_allows_exactly_one_complete_project(self) -> None:
        target = self.directory / "demo"
        originalMkdir = Path.mkdir
        barrier = threading.Barrier(2, timeout=5)

        def synchronizedMkdir(path, *args, **kwargs):
            if path == target:
                barrier.wait()
            return originalMkdir(path, *args, **kwargs)

        def generate():
            try:
                return HICAL.generateProject("demo", self.directory)
            except HICAL.ScaffoldError as error:
                return error

        with mock.patch.object(Path, "mkdir", new=synchronizedMkdir), ThreadPoolExecutor(max_workers=2) as executor:
            first, second = executor.submit(generate), executor.submit(generate)
            results = [first.result(timeout=10), second.result(timeout=10)]
        self.assertEqual(sum(isinstance(result, Path) for result in results), 1)
        failures = [result for result in results if isinstance(result, HICAL.ScaffoldError)]
        self.assertEqual(len(failures), 1)
        self.assertIn("目标已存在", str(failures[0]))
        self.assertEqual(set(self.fileContents(target)), EXPECTED_FILES)


if __name__ == "__main__":
    unittest.main()
