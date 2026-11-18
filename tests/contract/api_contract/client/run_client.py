#!/usr/bin/env python3
"""用指定的 OpenAPI 文档生成 SDK，再编译并运行房间接口测试。"""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys


GENERATOR_SHA256 = "25d6bd8273dd2be99979d544b62ea43f0ce1975f1aa582678b5093d1e7fcfce8"
SETUP_FAILURE, GENERATION_FAILURE, TYPESCRIPT_FAILURE, CALL_FAILURE = 10, 11, 12, 13
CLIENT_FILES = ("package.json", "package-lock.json", "tsconfig.json", "generator-config.json", "smoke.ts")


class StageFailure(Exception):
    """记录失败阶段的退出码和错误说明。"""

    def __init__(self, code: int, message: str):
        super().__init__(message)
        self.code = code


def executable(value: str) -> Path:
    """查找可执行文件并返回它的绝对路径。"""
    path = shutil.which(value)
    if path is None:
        raise ValueError(f"找不到工具 {value}")
    return Path(path).resolve()


def npmCli(node: Path) -> Path:
    """在 Windows 或 Linux 的官方 Node 发行包里找 npm CLI。"""
    override = os.environ.get("HICAL_NPM_CLI")
    if override:
        return Path(override).resolve(strict=True)
    candidates = (
        node.parent / "node_modules/npm/bin/npm-cli.js",
        node.parent.parent / "lib/node_modules/npm/bin/npm-cli.js",
    )
    for path in candidates:
        if path.is_file():
            return path
    raise ValueError("找不到 Node 发行包内的 npm-cli.js，请设置 HICAL_NPM_CLI")


def sha256(path: Path) -> str:
    """分块读取文件，计算它的 SHA-256。"""
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def runLogged(command, output: Path, logName: str, timeout: int, failureCode: int) -> None:
    """在输出目录执行命令，保存日志，失败时返回对应阶段的退出码。"""
    log = output / logName
    try:
        with log.open("wb") as stream:
            result = subprocess.run(command, cwd=output, stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise StageFailure(failureCode, f"{logName} 执行失败（{error}），请查看 {log}") from None
    if result.returncode:
        raise StageFailure(failureCode, f"{logName} 的退出码为 {result.returncode}，请查看 {log}")


def checkVersion(command, expected: str, output: Path) -> None:
    """核对工具版本，并把工具输出的版本信息写进日志。"""
    result = subprocess.run(command, cwd=output, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    with (output / "versions.log").open("ab") as stream:
        stream.write(result.stdout)
    if result.returncode or result.stdout.strip() != expected.encode("ascii"):
        raise ValueError(f"工具版本应为 {expected}，请查看 {output / 'versions.log'}")


def runClient(args) -> Path:
    """在单独的目录里安装依赖、生成 SDK，再编译并运行客户端测试。"""
    if args.generator_jar is None:
        raise ValueError("请用 --generator-jar 或 OPENAPI_GENERATOR_JAR 指定生成器 JAR")
    jar = args.generator_jar.resolve(strict=True)
    if sha256(jar) != GENERATOR_SHA256:
        raise ValueError("OpenAPI Generator 7.17.0 的 JAR 文件 SHA-256 与预期不符")
    node = executable(os.environ.get("HICAL_NODE", "node"))
    java = executable(os.environ.get("HICAL_JAVA", "java"))
    npm = npmCli(node)
    spec = args.openapi.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise ValueError(f"输出目录已有文件，请换一个空目录。原路径为 {output}")
    client = Path(__file__).resolve().parent
    for name in CLIENT_FILES:
        shutil.copyfile(client / name, output / name)
    frozenSpec = output / "openapi.json"
    shutil.copyfile(spec, frozenSpec)
    (output / "versions.log").write_text(
        f"Generator SHA256: {GENERATOR_SHA256}\nOpenAPI SHA256: {sha256(frozenSpec)}\n", encoding="utf-8")
    checkVersion([node, "--version"], "v20.16.0", output)
    checkVersion([node, npm, "--version"], "10.8.1", output)
    checkVersion([java, "-jar", jar, "version"], "7.17.0", output)
    runLogged([node, npm, "ci", "--ignore-scripts", "--no-audit", "--no-fund", "--engine-strict"],
              output, "npm-ci.log", 300, SETUP_FAILURE)
    tsc = output / "node_modules/typescript/lib/tsc.js"
    checkVersion([node, tsc, "--version"], "Version 5.9.3", output)

    runLogged([java, "-jar", jar, "generate", "-c", "generator-config.json", "-i", "openapi.json", "-o", "generated",
               "--global-property", "apiDocs=false,modelDocs=false,apiTests=false,modelTests=false"],
              output, "generate.log", 120, GENERATION_FAILURE)
    if not (output / "generated/index.ts").is_file():
        raise StageFailure(GENERATION_FAILURE, f"没有生成 TypeScript 入口，请查看 {output / 'generate.log'}")
    runLogged([node, tsc, "-p", "tsconfig.json", "--pretty", "false", "--locale", "en"],
              output, "tsc.log", 120, TYPESCRIPT_FAILURE)
    runLogged([node, "dist/smoke.js", args.base_url], output, "call.log", 30, CALL_FAILURE)
    return output / "call.log"


def main() -> int:
    """读取命令行参数，按失败阶段返回对应的退出码。"""
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--openapi", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--generator-jar", type=Path, default=os.environ.get("OPENAPI_GENERATOR_JAR"))
    try:
        log = runClient(parser.parse_args())
    except StageFailure as error:
        print(error, file=sys.stderr)
        return error.code
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"客户端工具准备失败：{error}", file=sys.stderr)
        return SETUP_FAILURE
    print(log.read_text(encoding="utf-8"), end="")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    raise SystemExit(main())
