#!/usr/bin/env python3
"""用 PowerShell、cmd 和子进程测试安装后的 hical.exe。"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from check_hical_cli import movePrefix


EXPECTED_FILES = {
    "CMakeLists.txt", "README.md", ".gitignore", "src/app.h", "src/app.cpp",
    "src/main.cpp", "tests/test_app.cpp",
}
LAUNCHER = os.environ.get("HICAL_TEST_CLI_LAUNCHER")
POWERSHELL_CALL = r"""$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$OutputEncoding = [Console]::OutputEncoding
$data = ConvertFrom-Json -InputObject $env:HICAL_WINDOWS_CLI_CASE
$arguments = @($data.arguments | ForEach-Object { [string]$_ })
$launcher = [string]$data.launcher
& $launcher @arguments
exit $LASTEXITCODE
"""
POWERSHELL_BARE_CALL = POWERSHELL_CALL.replace(
    "$launcher = [string]$data.launcher\n& $launcher @arguments",
    "$resolved = (Get-Command hical -CommandType Application | Select-Object -First 1).Source\n"
    "if ($resolved -ne [string]$data.launcher) { throw ('PowerShell 找到的是另一个 hical：' + $resolved) }\n"
    "hical @arguments",
)


@unittest.skipUnless(os.name == "nt", "仅验证 Windows 原生入口")
class WindowsCliLauncherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        """测试需要已安装的 hical.exe 和 Windows PowerShell。"""
        if not LAUNCHER:
            raise RuntimeError("请用 --launcher 或 HICAL_TEST_CLI_LAUNCHER 提供已安装的 hical.exe 路径")
        cls.launcher = Path(LAUNCHER).resolve(strict=True)
        cls.powershell = shutil.which("powershell.exe")
        if not cls.powershell:
            raise RuntimeError("找不到 Windows PowerShell（powershell.exe）")

    def setUp(self) -> None:
        """给每个用例一个临时目录，跑完自动清理。"""
        self.temporary = tempfile.TemporaryDirectory(
            prefix="hical-win-entry-", dir=os.environ.get("HICAL_TEST_CLI_TMPDIR"))
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()

    def runPowerShell(self, *arguments: str, cwd: Path | None = None,
                      launcher: Path | None = None, bare: bool = False,
                      environment: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        """从 PowerShell 用 & 或直接用 hical 调起程序，返回进程结果。"""
        childEnvironment = dict(environment or os.environ)
        childEnvironment["PYTHONUTF8"] = "1"
        childEnvironment["HICAL_WINDOWS_CLI_CASE"] = json.dumps({
            "launcher": str(launcher or self.launcher), "arguments": list(arguments),
        }, ensure_ascii=False)
        return subprocess.run(
            [self.powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-Command",
             POWERSHELL_BARE_CALL if bare else POWERSHELL_CALL],
            cwd=cwd or self.directory, env=childEnvironment, capture_output=True,
            text=True, encoding="utf-8", timeout=30, check=False)

    def fileContents(self, directory: Path) -> dict[str, bytes]:
        """按相对路径读取文件内容，方便对比执行前后有哪些变化。"""
        return {path.relative_to(directory).as_posix(): path.read_bytes()
                for path in directory.rglob("*") if path.is_file()}

    def test_powershell_preserves_special_parent_paths(self) -> None:
        for index, name in enumerate(("R&D", "R&ver", "%PATH%", "bang!", "caret^", "(round)", "中文 空格")):
            with self.subTest(parent=name):
                working = self.directory / f"case-{index}"
                working.mkdir()
                parent = working / name
                parent.mkdir()
                guard = working / "R"
                guard.mkdir()
                (guard / "marker.txt").write_text("保持原样", encoding="utf-8")
                before = self.fileContents(guard)
                # 用相对路径，临时目录里有空格也不会掩盖旧入口截断 R&D 的问题。
                result = self.runPowerShell("new", "demo", "--directory", name, cwd=working)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(set(self.fileContents(parent / "demo")), EXPECTED_FILES)
                self.assertEqual(self.fileContents(guard), before)
                self.assertEqual({path.name for path in working.iterdir()}, {name, "R"})

    def test_powershell_propagates_help_duplicate_and_invalid_exit_codes(self) -> None:
        parent = self.directory / "R&ver"
        parent.mkdir()
        guard = self.directory / "R"
        guard.mkdir()
        result = self.runPowerShell("--help")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.runPowerShell("new", "demo", "--directory", parent.name)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(set(self.fileContents(parent / "demo")), EXPECTED_FILES)
        before = self.fileContents(parent)
        for arguments, expectedCode in ((("new", "demo", "--directory", parent.name), 1), (("gen",), 2)):
            with self.subTest(arguments=arguments):
                result = self.runPowerShell(*arguments)
                self.assertEqual(result.returncode, expectedCode, result.stdout + result.stderr)
                self.assertEqual(self.fileContents(parent), before)
                self.assertEqual(list(guard.iterdir()), [])

    def test_moved_prefix_with_special_characters_and_bare_path_command(self) -> None:
        self.assertEqual(self.launcher.suffix.lower(), ".exe")
        prefix = self.directory / "安装 & %PATH% ! ^ (中文 空格)"
        (prefix / "bin").mkdir(parents=True)
        shutil.copy2(self.launcher, prefix / "bin" / "hical.exe")
        shutil.copytree(self.launcher.parent.parent / "share" / "hical" / "cli",
                        prefix / "share" / "hical" / "cli")
        moved = self.directory / "搬移 R&D %PATH% ! ^ (中文 空格)"
        self.assertTrue(prefix.resolve(strict=True).is_relative_to(self.directory))
        self.assertTrue(moved.resolve().is_relative_to(self.directory))
        movePrefix(prefix, moved)
        launcher = moved / "bin" / "hical.exe"
        parent = self.directory / "R&ver"
        parent.mkdir()
        guard = self.directory / "R"
        guard.mkdir()
        environment = os.environ.copy()
        environment["PATH"] = str(launcher.parent) + os.pathsep + environment.get("PATH", "")
        result = self.runPowerShell("new", "demo", "--directory", parent.name,
                                   launcher=launcher, bare=True, environment=environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(set(self.fileContents(parent / "demo")), EXPECTED_FILES)
        self.assertEqual(list(guard.iterdir()), [])

    def test_native_argv_and_child_exit_code_are_preserved(self) -> None:
        prefix = self.directory / "native & 中文"
        (prefix / "bin").mkdir(parents=True)
        launcher = prefix / "bin" / "hical.exe"
        shutil.copy2(self.launcher, launcher)
        script = prefix / "share" / "hical" / "cli" / "hical.py"
        script.parent.mkdir(parents=True)
        script.write_text(
            "import json, os, pathlib, sys\n"
            "pathlib.Path(os.environ['HICAL_WINDOWS_ARGV_RESULT']).write_text(\n"
            "    json.dumps(sys.argv[1:], ensure_ascii=False), encoding='utf-8')\n"
            "sys.exit(37)\n", encoding="utf-8")
        resultPath = self.directory / "argv.json"
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        environment["HICAL_WINDOWS_ARGV_RESULT"] = str(resultPath)
        arguments = ["", "plain", 'embedded"quote', "trailing\\", 'slash\\"quote',
                     "中文 %PATH% & ^ ! ()", "空 格\\", "\\\\"]
        result = subprocess.run([str(launcher), *arguments], cwd=self.directory, env=environment,
                                capture_output=True, text=True, encoding="utf-8", timeout=15, check=False)
        self.assertEqual(result.returncode, 37, result.stdout + result.stderr)
        self.assertEqual(json.loads(resultPath.read_text(encoding="utf-8")), arguments)

    def test_plain_cmd_invocation_preserves_special_parent_and_exit_code(self) -> None:
        parent = self.directory / "R&D %PATH% ! ^ (中文 空格)"
        parent.mkdir()
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        environment["HICAL_WINDOWS_CLI_LAUNCHER"] = str(self.launcher)
        environment["HICAL_WINDOWS_CLI_PARENT"] = str(parent)
        # 直接把命令送进 cmd 的标准输入，模拟在 cmd 窗口里敲命令。
        command = ('"%HICAL_WINDOWS_CLI_LAUNCHER%" new demo '
                   '--directory "%HICAL_WINDOWS_CLI_PARENT%"\nexit /b %errorlevel%\n')
        for expectedCode in (0, 1):
            result = subprocess.run(
                [str(Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"), "/d", "/q", "/v:off"],
                input=command, cwd=self.directory, env=environment, capture_output=True,
                text=True, encoding="utf-8", errors="replace", timeout=15, check=False)
            self.assertEqual(result.returncode, expectedCode, result.stdout + result.stderr)
            self.assertEqual(set(self.fileContents(parent / "demo")), EXPECTED_FILES)

    def test_missing_script_reports_utf8_error(self) -> None:
        launcher = self.directory / "hical.exe"
        shutil.copy2(self.launcher, launcher)
        result = subprocess.run([str(launcher), "--help"], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=15, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("错误：找不到", result.stderr)
        self.assertIn("hical.py", result.stderr)

    def test_python_resolution_uses_only_path(self) -> None:
        emptyPath = self.directory / "empty-path"
        emptyPath.mkdir()
        (self.directory / "python.exe").write_bytes(b"not an executable")
        environment = os.environ.copy()
        environment["PATH"] = str(emptyPath)
        result = subprocess.run([str(self.launcher), "--help"], cwd=self.directory, env=environment,
                                capture_output=True, text=True, encoding="utf-8", timeout=15, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("错误：找不到 Python", result.stderr)

    def test_extended_launcher_path_finds_adjacent_script(self) -> None:
        normalPath = str(self.launcher)
        if normalPath.startswith("\\\\?\\"):
            extendedPath = normalPath
        elif normalPath.startswith("\\\\"):
            extendedPath = "\\\\?\\UNC\\" + normalPath[2:]
        else:
            extendedPath = "\\\\?\\" + normalPath
        environment = os.environ.copy()
        environment["PYTHONUTF8"] = "1"
        result = subprocess.run([extendedPath, "--help"], cwd=self.directory, env=environment,
                                capture_output=True, text=True, encoding="utf-8", timeout=15, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("用法", result.stdout)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_quoted_python_directory_in_path_is_supported(self) -> None:
        emptyPath = self.directory / "without-python"
        emptyPath.mkdir()
        pythonDirectory = Path(sys.executable).resolve().parent
        for searchPath in (f'"{pythonDirectory}"', f'{emptyPath};"{pythonDirectory}";{emptyPath}'):
            with self.subTest(path=searchPath):
                environment = os.environ.copy()
                environment["PYTHONUTF8"] = "1"
                environment["PATH"] = searchPath
                result = subprocess.run([str(self.launcher), "--help"], cwd=self.directory, env=environment,
                                        capture_output=True, text=True, encoding="utf-8", timeout=15, check=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("用法", result.stdout)
                self.assertEqual(list(emptyPath.iterdir()), [])

    def test_expanded_command_line_limit_fails_without_writes(self) -> None:
        # 这行能启动 hical.exe。等它给 Python 拼上脚本路径并转义反斜杠，命令行才会超长。
        result = subprocess.run([str(self.launcher), "\\" * 16500], cwd=self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=15, check=False)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("错误：命令行超过 Windows 长度上限", result.stderr)
        self.assertEqual(list(self.directory.iterdir()), [])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--launcher", help="已安装的 hical.exe 路径")
    options, remaining = parser.parse_known_args()
    if options.launcher:
        LAUNCHER = options.launcher
    unittest.main(argv=[sys.argv[0], *remaining])
