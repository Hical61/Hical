#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""版本号一键传播脚本。

唯一版本源头：CMakeLists.txt 中的 project(hical VERSION X.Y.Z)
本脚本将版本号同步到所有需要硬编码版本的文件。

已自动化（无需本脚本管理）：
  - src/core/Version.h.in  → CMake configure_file 生成
  - conanfile.py           → set_version() 从 CMakeLists.txt 正则提取

无版本号（已移除）：
  - CLAUDE.md / README.md / README_CN.md / docs/TESTING.md

用法:
    python scripts/bump_version.py 2.2.0            # 执行更新
    python scripts/bump_version.py 2.2.0 --dry-run  # 仅预览，不写入
"""

import argparse
import json
import os
import re
import sys
from datetime import date

# ──────────────────────────────────────────────
# 路径配置
# ──────────────────────────────────────────────

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = {
    "cmake": os.path.join(PROJECT_ROOT, "CMakeLists.txt"),
    "vcpkg": os.path.join(PROJECT_ROOT, "vcpkg.json"),
    "ports_vcpkg": os.path.join(
        PROJECT_ROOT, "ports", "hical61-hical", "vcpkg.json"
    ),
    "versions_template": os.path.join(
        PROJECT_ROOT, "ports", "hical61-hical", "versions_template.json"
    ),
    "changelog": os.path.join(PROJECT_ROOT, "CHANGELOG.md"),
}

VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


# ──────────────────────────────────────────────
# 工具函数
# ──────────────────────────────────────────────


def read_file(path):
    if not os.path.exists(path):
        print(f"  [警告] 文件不存在，跳过：{path}")
        return None
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write_file(path, content):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)


def report(label, changed, dry_run):
    if changed:
        tag = "[预览]" if dry_run else "[已更新]"
    else:
        tag = "[跳过] "
    suffix = "" if changed else "（无需更改）"
    print(f"  {tag} {label}{suffix}")


# ──────────────────────────────────────────────
# 提取当前版本
# ──────────────────────────────────────────────


def extract_current_version():
    content = read_file(FILES["cmake"])
    if content is None:
        print("[错误] 无法读取 CMakeLists.txt")
        sys.exit(1)
    m = re.search(
        r"project\s*\(\s*hical\s+VERSION\s+(\d+\.\d+\.\d+)", content
    )
    if not m:
        print("[错误] CMakeLists.txt 中未找到 project(hical VERSION X.Y.Z)")
        sys.exit(1)
    return m.group(1)


# ──────────────────────────────────────────────
# 各文件更新逻辑
# ──────────────────────────────────────────────


def update_cmake(content, new_ver):
    return re.sub(
        r"(project\s*\(\s*hical\s+VERSION\s+)\d+\.\d+\.\d+",
        rf"\g<1>{new_ver}",
        content,
    )


def update_vcpkg_json(content, new_ver):
    return re.sub(
        r'("version"\s*:\s*")\d+\.\d+\.\d+(")',
        rf"\g<1>{new_ver}\2",
        content,
    )


def update_versions_template(content, new_ver):
    """在 versions 数组顶部插入新条目（幂等：已存在则跳过）。"""
    data = json.loads(content)
    versions = data.get("versions", [])
    if versions and versions[0].get("version") == new_ver:
        return content  # 已是最新
    versions.insert(
        0,
        {"version": new_ver, "git-tree": "PLACEHOLDER_RUN_STEP_5_TO_FILL"},
    )
    data["versions"] = versions
    return json.dumps(data, ensure_ascii=False, indent=2) + "\n"


def update_changelog(content, old_ver, new_ver):
    # 1. 更新 [Unreleased] 对比链接
    content = re.sub(
        r"(\[Unreleased\]:\s*https://github\.com/Hical61/Hical/compare/)"
        r"v[\d.]+(\.\.\.)HEAD",
        rf"\g<1>v{new_ver}\2HEAD",
        content,
    )
    # 2. 插入新版本对比链接（幂等）
    new_link = (
        f"[{new_ver}]: https://github.com/Hical61/Hical/"
        f"compare/v{old_ver}...v{new_ver}"
    )
    if new_link not in content:
        content = re.sub(
            r"(\[Unreleased\]:\s*https://github\.com/Hical61/Hical/"
            r"compare/v[\d.]+\.\.\.HEAD)",
            rf"\1\n{new_link}",
            content,
        )
    return content


# ──────────────────────────────────────────────
# 处理单个文件
# ──────────────────────────────────────────────

UPDATERS = {
    "cmake": lambda c, _o, n: update_cmake(c, n),
    "vcpkg": lambda c, _o, n: update_vcpkg_json(c, n),
    "ports_vcpkg": lambda c, _o, n: update_vcpkg_json(c, n),
    "versions_template": lambda c, _o, n: update_versions_template(c, n),
    "changelog": lambda c, o, n: update_changelog(c, o, n),
}


def process_file(key, old_ver, new_ver, dry_run):
    path = FILES[key]
    content = read_file(path)
    if content is None:
        return

    updated = UPDATERS[key](content, old_ver, new_ver)
    changed = updated != content
    report(os.path.relpath(path, PROJECT_ROOT), changed, dry_run)

    if changed and not dry_run:
        write_file(path, updated)


# ──────────────────────────────────────────────
# 入口
# ──────────────────────────────────────────────


def main():
    parser = argparse.ArgumentParser(
        description="Hical 版本号一键传播脚本",
    )
    parser.add_argument("version", help="新版本号，格式 X.Y.Z")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="仅预览，不写入文件",
    )
    args = parser.parse_args()

    new_ver = args.version
    if not VERSION_RE.match(new_ver):
        print(f"[错误] 版本格式不合法：'{new_ver}'，应为 X.Y.Z（纯数字）")
        sys.exit(1)

    old_ver = extract_current_version()

    if old_ver == new_ver:
        print(f"当前版本已是 v{new_ver}，检查各文件一致性...\n")
    else:
        print(f"版本升级：v{old_ver}  →  v{new_ver}\n")

    if args.dry_run:
        print("[预览模式] 不会写入任何文件\n")

    for key in FILES:
        process_file(key, old_ver, new_ver, args.dry_run)

    print()
    if args.dry_run:
        print("预览完成。去掉 --dry-run 重新执行即可写入。")
    else:
        today = date.today().strftime("%Y-%m-%d")
        print("所有文件已更新。后续步骤：")
        print(f"  1. CHANGELOG.md — 在 [Unreleased] 下添加 ## [{new_ver}] - {today} 条目")
        print(f"  2. git diff")
        print(f'  3. git add -u && git commit -m "[chore] bump version to v{new_ver}"')
        print(f"  4. git tag v{new_ver}")
        print(f"  5. git push origin main v{new_ver}")


if __name__ == "__main__":
    main()
