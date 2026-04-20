#!/usr/bin/env python3
"""修复 clang-format 在 NamespaceIndentation: All 下多行注释的两个问题：

1. 缩进丢失：/** 开头行有缩进，但续行 * 只做列对齐不继承缩进
2. 空 * 行：删除注释中只有 * 没有内容的行（如 @brief 后的空行）

用法:
    python fix-comment-indent.py file1.h file2.cpp ...
    python fix-comment-indent.py --check file1.h   # 仅检查，不修改
"""

import sys
import re


def fix_comment_indent(content: str) -> str:
    lines = content.split('\n')
    result = []
    in_comment = False
    indent = ''

    for line in lines:
        if not in_comment:
            # 匹配 /* 或 /** 开头的多行注释（同行无 */）
            match = re.match(r'^(\s*)/\*', line)
            if match and '*/' not in line[line.index('/*') + 2:]:
                indent = match.group(1)
                in_comment = True
                result.append(line)
            else:
                result.append(line)
        else:
            stripped = line.lstrip()
            if stripped.startswith('*'):
                # 跳过空 * 行（只有 * 没有内容）
                if stripped == '*':
                    continue
                # 续行 * 或闭合 */，对齐到 /** 的第一个 * 后一列
                result.append(indent + ' ' + stripped)
                if '*/' in stripped:
                    in_comment = False
            else:
                # 非标准注释续行，原样保留
                result.append(line)
                if '*/' in line:
                    in_comment = False

    return '\n'.join(result)


def main():
    check_only = '--check' in sys.argv
    files = [f for f in sys.argv[1:] if f != '--check']

    if not files:
        print(__doc__)
        sys.exit(1)

    has_diff = False
    for filepath in files:
        with open(filepath, 'r', encoding='utf-8') as f:
            original = f.read()

        fixed = fix_comment_indent(original)

        if original != fixed:
            has_diff = True
            if check_only:
                print(f"[需修复] {filepath}")
            else:
                with open(filepath, 'w', encoding='utf-8', newline='') as f:
                    f.write(fixed)
                print(f"[已修复] {filepath}")

    if check_only and has_diff:
        sys.exit(1)


if __name__ == '__main__':
    main()
