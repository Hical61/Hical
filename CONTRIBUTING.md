# Contributing to hical / 贡献指南

[English](#english) | [简体中文](#简体中文)

---

## English

Thank you for your interest in contributing to hical! Please read this guide before submitting issues or pull requests.

### Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code.

### Development Setup

See [docs/build_and_test_guide.md](docs/build_and_test_guide.md) for full details.

**Requirements:**

| Dependency | Version |
|-----------|---------|
| C++ Standard | C++20 |
| Boost | >= 1.70 (Asio, Beast, System, JSON) |
| CMake | >= 3.20 |
| OpenSSL | Required |
| Google Test | Required |
| Compiler | GCC 14+ / Clang 18+ / MSVC 2022+ |

**Quick build:**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Coding Standards

This project enforces style via `.clang-format` and `.clang-tidy`. Run before committing:

```bash
clang-format -i <files>
```

> **Note:** CI will automatically fix formatting issues (including multi-line comment indentation via `scripts/fix-comment-indent.py`) and commit the fix to your PR branch. You don't need to run this script locally.

**Naming conventions** (from `.clang-tidy`):

| Element | Convention | Example |
|---------|-----------|---------|
| Class | `C` + PascalCase | `CHttpServer` |
| Struct | `S` + PascalCase | `SRequestData` |
| Enum | `E` + PascalCase | `EHttpMethod` |
| Interface (abstract) | `I` + PascalCase | `IEventLoop` |
| Function / Method | camelBack | `handleRequest` |
| Member variable | `m_` + camelBack | `m_bufferSize` |
| Global variable | `g_` + camelBack | `g_instanceCount` |
| Constant | `h` + PascalCase | `hDefaultSize` |
| Macro | UPPER_CASE | `HICAL_LOG_INFO` |

**Style:** 4-space indentation, 120-column limit, Allman brace style.

### PR Process

1. Fork the repository
2. Create a feature branch from `main`
3. Make your changes, ensure all tests pass
4. Fill out the PR template
5. Wait for CI to pass and maintainer review

### Commit Message Format

```
[type] short description

type:
  feat     - New feature
  fix      - Bug fix
  perf     - Performance improvement
  refactor - Code refactoring
  docs     - Documentation
  test     - Tests
  chore    - Build/tooling
```

Example: `[feat] add WebSocket broadcast support`

### Releasing (Maintainers Only)

Hical uses a tag-based release workflow. Pushing a tag triggers CI to build, test, and create a GitHub Release automatically.

**Steps:**

1. Update the version in `CMakeLists.txt`: `project(hical VERSION x.y.z ...)`
2. Commit: `[chore] bump version to vx.y.z`
3. Tag and push:
   ```bash
   git tag -a vx.y.z -m "Release vx.y.z"
   git push origin vx.y.z
   ```
4. GitHub Actions will: build → test → create Release with auto-generated notes
5. Verify the [Releases page](https://github.com/Hical61/Hical/releases)

**Pre-release:** Use tags like `v1.1.0-alpha.1`, `v1.1.0-beta.1`, or `v1.1.0-rc.1` — they will be marked as pre-release automatically.

### Reporting Issues

Use the [issue templates](https://github.com/Hical61/Hical/issues/new/choose). Please include your compiler version, Boost version, and platform.

---

## 简体中文

感谢你对 hical 的贡献兴趣！提交 Issue 或 Pull Request 前请阅读本指南。

### 行为准则

本项目遵循 [Contributor Covenant](CODE_OF_CONDUCT.md)。参与即表示你同意遵守该准则。

### 开发环境配置

详见 [docs/build_and_test_guide.md](docs/build_and_test_guide.md)。

**依赖项：**

| 依赖项 | 版本要求 |
|--------|---------|
| C++ 标准 | C++20 |
| Boost | >= 1.70（Asio、Beast、System、JSON） |
| CMake | >= 3.20 |
| OpenSSL | 必需 |
| Google Test | 必需 |
| 编译器 | GCC 14+ / Clang 18+ / MSVC 2022+ |

**快速构建：**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 编码规范

项目通过 `.clang-format` 和 `.clang-tidy` 强制执行代码风格。提交前请运行：

```bash
clang-format -i <files>
```

> **提示：** CI 会自动修复格式问题（包括通过 `scripts/fix-comment-indent.py` 修复多行注释缩进），并将修复 commit 推送到你的 PR 分支。你无需在本地运行此脚本。

**命名约定**（来自 `.clang-tidy`）：

| 元素 | 约定 | 示例 |
|------|------|------|
| 类 | `C` + PascalCase | `CHttpServer` |
| 结构体 | `S` + PascalCase | `SRequestData` |
| 枚举 | `E` + PascalCase | `EHttpMethod` |
| 接口（抽象类） | `I` + PascalCase | `IEventLoop` |
| 函数 / 方法 | camelBack | `handleRequest` |
| 成员变量 | `m_` + camelBack | `m_bufferSize` |
| 全局变量 | `g_` + camelBack | `g_instanceCount` |
| 常量 | `h` + PascalCase | `hDefaultSize` |
| 宏 | UPPER_CASE | `HICAL_LOG_INFO` |

**风格：** 4 空格缩进，120 列限制，Allman 大括号风格。

### PR 流程

1. Fork 本仓库
2. 从 `main` 创建功能分支
3. 完成修改，确保所有测试通过
4. 按照 PR 模板填写信息
5. 等待 CI 通过和维护者审查

### 提交信息格式

```
[类型] 简短描述

类型：
  feat     - 新功能
  fix      - 缺陷修复
  perf     - 性能优化
  refactor - 代码重构
  docs     - 文档更新
  test     - 测试相关
  chore    - 构建/工具变更
```

示例：`[feat] 添加 WebSocket 广播支持`

### 版本发布（仅限维护者）

Hical 使用基于 tag 的发布流程。推送 tag 后，CI 会自动构建、测试并创建 GitHub Release。

**步骤：**

1. 更新 `CMakeLists.txt` 中的版本号：`project(hical VERSION x.y.z ...)`
2. 提交：`[chore] bump version to vx.y.z`
3. 打标签并推送：
   ```bash
   git tag -a vx.y.z -m "Release vx.y.z"
   git push origin vx.y.z
   ```
4. GitHub Actions 自动执行：构建 → 测试 → 创建 Release（附自动生成的变更日志）
5. 在 [Releases 页面](https://github.com/Hical61/Hical/releases) 确认发布结果

**预发布版本：** 使用 `v1.1.0-alpha.1`、`v1.1.0-beta.1` 或 `v1.1.0-rc.1` 格式的 tag，会自动标记为预发布。

### 提交 Issue

请使用 [Issue 模板](https://github.com/Hical61/Hical/issues/new/choose)，并包含编译器版本、Boost 版本和平台信息。
