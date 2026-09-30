#!/bin/bash
# Hical 压测报告 A/B 对比脚本
#
# 读两份 vm_bench.sh 生成的压测报告 md，把「QPS 汇总」表并排摆出来，
# 算 B 相对 A 的差异百分比，再按噪声 / 可疑 / 值得追给个初步判断。
#
# 用法:
#   bash benchmark/hical/compare_reports.sh <报告A> <报告B> [-o <输出路径>] [--label-a <名称>] [--label-b <名称>]
#
# 示例:
#   bash benchmark/hical/compare_reports.sh result-a.md result-b.md
#   bash benchmark/hical/compare_reports.sh a.md b.md -o /tmp/cmp.md --label-a v2.6.8 --label-b v2.7.0
#
# 说明:
#   只认报告里「## QPS 汇总」那张表，抠不出来直接报错退出，不会给你一张空表。
#   跳过 / N/A 的场景照样列出来，但判断写「不可比」，不参与差异计算。

set -euo pipefail

# ==================== 默认参数 ====================
outputFile=""
labelA="A"
labelB="B"

reportA=""
reportB=""
generatedAt=""

# 两边轮数对不上的场景先攒着，等表格打完再统一提示
mismatchNotes=()

declare -A aQps aLat aRounds aConns
declare -A bQps bLat bRounds bConns
aOrder=()
bOrder=()

# ==================== 用法 ====================

showUsage() {
    cat <<'EOF'
Hical 压测报告 A/B 对比

用法:
  bash benchmark/hical/compare_reports.sh <报告A> <报告B> [-o <输出路径>] [--label-a <名称>] [--label-b <名称>]

参数:
  <报告A> <报告B>        两份 vm_bench.sh 生成的报告 md，必填
  -o, --output <路径>    对比结果输出路径，默认 <报告A所在目录>/result-compare.md
  --label-a <名称>       A 侧表头名字，默认 A（比如写成 v2.6.8）
  --label-b <名称>       B 侧表头名字，默认 B
  -h, --help             打印本帮助

示例:
  bash benchmark/hical/compare_reports.sh result-a.md result-b.md
  bash benchmark/hical/compare_reports.sh a.md b.md --label-a v2.6.8 --label-b v2.7.0

判断口径:
  |差异| <= 5%          噪声
  5% < |差异| <= 10%    可疑
  |差异| > 10%          值得追
  任一侧跳过 / N/A      不可比
EOF
}

# ==================== 参数解析 ====================

positional=()
while [ $# -gt 0 ]; do
    case "$1" in
        -o|--output)
            outputFile="${2:-}"
            if [ -z "$outputFile" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        --label-a)
            labelA="${2:-}"
            if [ -z "$labelA" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        --label-b)
            labelB="${2:-}"
            if [ -z "$labelB" ]; then
                echo "[错误] $1 需要一个参数"
                exit 1
            fi
            shift 2
            ;;
        -h|--help)
            showUsage
            exit 0
            ;;
        -*)
            echo "[错误] 未知参数: $1"
            echo ""
            showUsage
            exit 1
            ;;
        *)
            positional+=("$1")
            shift
            ;;
    esac
done

if [ "${#positional[@]}" -ne 2 ]; then
    if [ "${#positional[@]}" -eq 0 ]; then
        echo "[错误] 必须给两份报告：<报告A> <报告B>"
    else
        echo "[错误] 位置参数要正好两个（报告A 和 报告B），现在给了 ${#positional[@]} 个"
    fi
    echo ""
    showUsage
    exit 1
fi

reportA="${positional[0]}"
reportB="${positional[1]}"

if [ ! -f "$reportA" ]; then
    echo "[错误] 找不到报告 A: $reportA"
    exit 1
fi
if [ ! -f "$reportB" ]; then
    echo "[错误] 找不到报告 B: $reportB"
    exit 1
fi

if [ -z "$outputFile" ]; then
    # 报告 A 带目录就用它那个目录，光给个文件名就落当前目录
    case "$reportA" in
        */*) reportDirA="${reportA%/*}" ;;
        *) reportDirA="." ;;
    esac
    outputFile="${reportDirA}/result-compare.md"
fi

# ==================== 报告解析 ====================

# 抠出「## QPS 汇总」表，每行整理成:
#   场景<TAB>连接数<TAB>轮数<TAB>平均QPS<TAB>平均Latency<TAB>Transfer<TAB>Socket错误<TAB>Non2xx
# 表格行按 | 切开后首尾各有一个空字段，所以先把首尾的 | 削掉再切；
# 场景名里理论上可能混进 |，所以第 1 个字段不直接当名字用，
# 而是把「最后 7 个字段之外的部分」都并回名字。
parseSummary() {
    local file="$1"
    awk '
        function trim(s) { gsub(/^[[:space:]]+/, "", s); gsub(/[[:space:]]+$/, "", s); return s }
        /^##[[:space:]]+QPS/ { inTable = 1; next }
        inTable && /^##[[:space:]]/ { inTable = 0 }
        inTable && /^[[:space:]]*\|/ {
            cnt++
            # 前两行固定是表头和 --- 分隔行，直接丢
            if (cnt <= 2) next
            line = $0
            sub(/^[[:space:]]*\|/, "", line)
            sub(/[[:space:]]*\|[[:space:]]*$/, "", line)
            n = split(line, f, "|")
            if (n < 8) next
            for (i = 1; i <= n; i++) f[i] = trim(f[i])
            name = f[1]
            for (i = 2; i <= n - 7; i++) name = name "|" f[i]
            if (name == "") next
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", name, f[n-6], f[n-5], f[n-4], f[n-3], f[n-2], f[n-1], f[n]
        }
    ' "$file"
}

# 从「## 被测版本」表里取某个键的值，没有就吐空串
versionField() {
    local file="$1" key="$2"
    awk -v key="$key" '
        function trim(s) { gsub(/^[[:space:]]+/, "", s); gsub(/[[:space:]]+$/, "", s); return s }
        /^##[[:space:]]+被测版本/ { inT = 1; next }
        inT && /^##[[:space:]]/ { inT = 0 }
        inT && /^[[:space:]]*\|/ {
            line = $0
            sub(/^[[:space:]]*\|/, "", line)
            sub(/[[:space:]]*\|[[:space:]]*$/, "", line)
            n = split(line, f, "|")
            if (n < 2) next
            for (i = 1; i <= n; i++) f[i] = trim(f[i])
            if (f[1] != key) next
            v = f[2]
            for (i = 3; i <= n; i++) v = v "|" f[i]
            print v
            exit
        }
    ' "$file"
}

# 解析结果灌进对应侧的关联数组，side 取 a / b
loadSummaryInto() {
    local side="$1" name conns rounds qps lat transfer sock non2xx
    while IFS=$'\t' read -r name conns rounds qps lat transfer sock non2xx; do
        if [ -z "$name" ]; then
            continue
        fi
        case "$side" in
            a)
                aQps["$name"]="$qps"
                aLat["$name"]="$lat"
                aRounds["$name"]="$rounds"
                aConns["$name"]="$conns"
                aOrder+=("$name")
                ;;
            b)
                bQps["$name"]="$qps"
                bLat["$name"]="$lat"
                bRounds["$name"]="$rounds"
                bConns["$name"]="$conns"
                bOrder+=("$name")
                ;;
        esac
    done
}

# 是能算差异的数字吗？跳过 / N/A / 空 一律不算
isComparable() {
    local v="$1"
    if [ -z "$v" ]; then
        return 1
    fi
    case "$v" in
        *[!0-9.]*) return 1 ;;
    esac
    return 0
}

# B 相对 A 的百分比，带正负号
diffPct() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        if (a + 0 == 0) { print "-"; exit }
        printf "%+.1f%%", (b - a) / a * 100
    }'
}

# 按差异绝对值判噪声 / 可疑 / 值得追
verdictOf() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        if (a + 0 == 0) { print "不可比"; exit }
        d = (b - a) / a * 100
        if (d < 0) d = -d
        if (d <= 5) print "噪声"
        else if (d <= 10) print "可疑"
        else print "值得追"
    }'
}

# 轮数对不上的记一笔，等会儿提示
collectMismatch() {
    local name="$1" ra rb
    ra="${aRounds[$name]:-}"
    rb="${bRounds[$name]:-}"
    case "$ra" in ''|*[!0-9]*) return 0 ;; esac
    case "$rb" in ''|*[!0-9]*) return 0 ;; esac
    if [ "$ra" != "$rb" ]; then
        mismatchNotes+=("${name}（${labelA} ${ra} 轮 / ${labelB} ${rb} 轮）")
    fi
    return 0
}

# ==================== 报告输出 ====================

echoTitle() {
    echo "# Hical 压测报告 A/B 对比"
    echo ""
    echo "> 由 \`benchmark/hical/compare_reports.sh\` 自动生成"
    echo ""
    echo "- 生成时间：${generatedAt}"
    echo "- 报告 A（${labelA}）：\`${reportA##*/}\`"
    echo "- 报告 B（${labelB}）：\`${reportB##*/}\`"
    echo ""
}

echoVersionCompare() {
    local branchA shaA tagA binA branchB shaB tagB binB
    branchA=$(versionField "$reportA" "git 分支")
    shaA=$(versionField "$reportA" "commit")
    tagA=$(versionField "$reportA" "最近 tag")
    binA=$(versionField "$reportA" "被测二进制")
    branchB=$(versionField "$reportB" "git 分支")
    shaB=$(versionField "$reportB" "commit")
    tagB=$(versionField "$reportB" "最近 tag")
    binB=$(versionField "$reportB" "被测二进制")

    echo "## 版本信息"
    echo ""
    echo "| 项目 | ${labelA} | ${labelB} |"
    echo "| --- | --- | --- |"
    echo "| git 分支 | ${branchA:-未知} | ${branchB:-未知} |"
    echo "| commit | ${shaA:-未知} | ${shaB:-未知} |"
    echo "| 最近 tag | ${tagA:-无} | ${tagB:-无} |"
    echo "| 被测二进制 | ${binA:-（报告里没写）} | ${binB:-（报告里没写）} |"
    echo ""

    # 先别急着看 QPS，两边 commit 一样基本就是比错对象了
    if [ -n "$shaA" ] && [ "$shaA" = "$shaB" ] && [ "$shaA" != "未知" ]; then
        echo "> ⚠️ 两边 commit 都是 ${shaA}，先确认下是不是拿同一份二进制（或同一份报告）在比。"
        echo ""
    fi
}

echoCompareTable() {
    echo "## 场景对比"
    echo ""
    echo "| 场景 | ${labelA} QPS | ${labelB} QPS | B 相对 A | ${labelA} Latency | ${labelB} Latency | 判断 |"
    echo "| --- | ---: | ---: | ---: | ---: | ---: | --- |"

    local name aq bq pct verdict
    for name in "${aOrder[@]}"; do
        # B 里压根没这个场景的，挪到「只在单侧出现」那段去说
        if [ -z "${bQps[$name]+x}" ]; then
            continue
        fi
        aq="${aQps[$name]}"
        bq="${bQps[$name]}"
        if isComparable "$aq" && isComparable "$bq"; then
            pct=$(diffPct "$aq" "$bq")
            verdict=$(verdictOf "$aq" "$bq")
        else
            pct="-"
            verdict="不可比"
        fi
        collectMismatch "$name"
        echo "| ${name} | ${aq:-N/A} | ${bq:-N/A} | ${pct} | ${aLat[$name]:--} | ${bLat[$name]:--} | ${verdict} |"
    done
    echo ""
}

echoRoundsNote() {
    if [ "${#mismatchNotes[@]}" -eq 0 ]; then
        return 0
    fi
    echo "> ⚠️ 下面这些场景两边轮数不一样，轮数少的那侧可信度低，别拿它下结论："
    echo ">"
    local note
    for note in "${mismatchNotes[@]}"; do
        echo "> - ${note}"
    done
    echo ""
}

echoSingleSide() {
    local onlyA=() onlyB=() name
    for name in "${aOrder[@]}"; do
        if [ -z "${bQps[$name]+x}" ]; then
            onlyA+=("$name")
        fi
    done
    for name in "${bOrder[@]}"; do
        if [ -z "${aQps[$name]+x}" ]; then
            onlyB+=("$name")
        fi
    done

    if [ "${#onlyA[@]}" -eq 0 ] && [ "${#onlyB[@]}" -eq 0 ]; then
        return 0
    fi

    echo "## 只在单侧出现的场景"
    echo ""
    echo "这些场景两边没对齐（多半是端点预检没过被跳过，或者两次跑的场景集合不一样），"
    echo "没法算差异，单独列出来看看就行，别硬塞进对比表。"
    echo ""
    echo "| 场景 | 出现在 | QPS | 轮数 |"
    echo "| --- | --- | ---: | ---: |"
    if [ "${#onlyA[@]}" -gt 0 ]; then
        for name in "${onlyA[@]}"; do
            echo "| ${name} | 只有 ${labelA} | ${aQps[$name]:-N/A} | ${aRounds[$name]:--} |"
        done
    fi
    if [ "${#onlyB[@]}" -gt 0 ]; then
        for name in "${onlyB[@]}"; do
            echo "| ${name} | 只有 ${labelB} | ${bQps[$name]:-N/A} | ${bRounds[$name]:--} |"
        done
    fi
    echo ""
}

echoVerdictTable() {
    echo "## 判断标准"
    echo ""
    echo "| B 相对 A 的差异 | 判断 | 说明 |"
    echo "| --- | --- | --- |"
    echo "| \|差异\| ≤ 5% | 噪声 | 跑两遍都能差出这几个点，别下结论 |"
    echo "| 5% < \|差异\| ≤ 10% | 可疑 | 处在中间地带，多跑几轮再看稳不稳 |"
    echo "| \|差异\| > 10% | 值得追 | 到这个量级基本能认为有真实变化 |"
    echo "| 任一侧跳过 / N/A / 空 | 不可比 | 没数据，谈不上差异 |"
    echo ""
    echo "口径跟文档里既有的一致：只看差异的绝对值，涨 8% 和掉 8% 一样算「可疑」。"
    echo ""
}

echoHowToRead() {
    echo "## 怎么读这张表"
    echo ""
    echo "- **±5% 以内当噪声**：同一份代码连跑两遍都能差出几个点，机器一忙一闲、CPU 频率一飘就这数，"
    echo "  拿 2% 的差异下结论纯属自己骗自己。"
    echo "- **超 10% 才值得追**：到这个量级基本能认为有真实变化，值得去翻是哪次改动引进来的。"
    echo "- **5% ~ 10% 先别急**：可疑区间，多跑几轮看它稳不稳再决定。"
    echo "- **最要紧的一条**：A、B 各只跑一次的数据混着机器漂移，不算数。想把机器因素压下去，"
    echo "  得让 A/B 交替多轮跑（这轮压 A 下轮压 B），最后取每侧的平均值再比。"
    echo "- 单侧跳过的场景是「没数据」，不是「没差异」，别当成绩读。"
    echo ""
}

# ==================== 主流程 ====================

generatedAt=$(date '+%Y-%m-%d %H:%M:%S')

summaryA=$(parseSummary "$reportA")
if [ -z "$summaryA" ]; then
    echo "[错误] 在 ${reportA} 里没找到「## QPS 汇总」表，没法对比"
    echo "        确认下这份 md 是不是 benchmark/hical/vm_bench.sh 生成的完整报告"
    exit 1
fi

summaryB=$(parseSummary "$reportB")
if [ -z "$summaryB" ]; then
    echo "[错误] 在 ${reportB} 里没找到「## QPS 汇总」表，没法对比"
    echo "        确认下这份 md 是不是 benchmark/hical/vm_bench.sh 生成的完整报告"
    exit 1
fi

loadSummaryInto a <<< "$summaryA"
loadSummaryInto b <<< "$summaryB"

mkdir -p "$(dirname "$outputFile")"

{
    echoTitle
    echoVersionCompare
    echoCompareTable
    echoRoundsNote
    echoSingleSide
    echoVerdictTable
    echoHowToRead
} | tee "$outputFile"

echo "[*] 对比结果已写入: ${outputFile}"
