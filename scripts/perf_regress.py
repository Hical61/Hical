#!/usr/bin/env python3
"""
HttpArena 上的 Hical 性能回归配对压测：基线 vs 待验证。

干的事：同一次运行里把 Hical 的两个提交（基线 = 上一个版本，待验证 = 改完的版本）
各做成一个 HttpArena framework，奇偶轮交替顺序跑同一组档位，收集指标，
最后写一份中文报告，回答「这一版比上一版有没有变慢」。

两个版本平时不用手填：--baseline / --candidate 默认 auto，基线取最近的 tag
（一个 tag 都没走到就退成 HEAD~1），待验证取 HEAD；显式传 tag / 分支 / sha 就原样用。
Hical 仓库从脚本自己的上一级目录认（脚本躺在 <repo>/scripts/ 下就够了）；仓库在别处、
或者脚本没放在仓库里，用 --hical-repo 指过去。

有一点绕不过去：HttpArena 的 Dockerfile 是 `git fetch --depth 1 origin <sha>`，
只能构建远端已有的 commit。所以阶段 1 会查这两个提交推没推到 GitHub，没推直接退出；
工作区有未提交改动也会提醒一句——那些改动进不了镜像。

能测什么，三张表看全：

  1) profile 预设。--profiles <预设名>，预设名只能单独用，不能和档名混在逗号列表里。

     | 预设 | 展开成 | 用在哪 | 默认 |
     |------|--------|--------|------|
     | ws | echo-ws | 单场景复测（配合 --conns 换连接数） | 6 对 × 10s |
     | http | baseline, pipelined | 解析 / 路由 / 中间件 | 3 对 × 5s |
     | json | json-comp, json-tls | 序列化 / 压缩 / TLS | 3 对 × 5s |
     | websocket | echo-ws, echo-ws-pipeline | WebSocket | 3 对 × 5s |
     | async | async, latency-10k | 定时器 / 协程 | 3 对 × 5s |
     | db | async-db, limited-conn | 连接池 / DB 中间件 | 3 对 × 5s |
     | broad | baseline, echo-ws, json-comp | 影响面广，说不好碰了哪块 | 3 对 × 5s |
     | smoke | baseline, json-comp, echo-ws | 什么都碰了一点，先确认这台机器能测 | 3 对 × 5s |
     | scored | 10 个计分 profile（baseline, limited-conn, json-comp, json-tls, 8gbit, async, async-db, echo-ws, echo-ws-pipeline, echo-ws-limited） | 发版回归 | 6 对 × 10s |
     | all | meta.json 里全部 14 个 profile（读 frameworks/hical/meta.json 的 tests） | 全面回归 | 6 对 × 10s |

     报告里的「档」指 profile × 连接数，一个 profile 带几个连接数就出几档。

  2) 指标口径。不同档位比的东西不一样，这是最容易误读的地方。

     | 口径 | 用于 | 数字含义 | 越大越好？ |
     |------|------|----------|-----------|
     | req/s | baseline、pipelined、limited-conn、json-comp、json-tls、async、async-db | 每秒完成了多少请求 | 是 |
     | frames/s | echo-ws、echo-ws-pipeline、echo-ws-limited | 每秒完成了多少 echo 往返帧 | 是 |
     | us/req | latency-1m、latency-10k、latency-500k-8cpu、8gbit | 每个请求吃掉多少 CPU 时间 | 否，越低越好 |

     判据是 profiles.sh 最后那列 endpoint，不是档名：走 ws-echo 的比 frames/s（所以
     echo-ws-limited 名字里带 limited 也是帧）；走 zrk 的四档 rps 是压测机自己给定的，
     比它没意义，改比 us/req；其余一律 req/s。

  3) 用法。按你这次动过的部分挑一条执行——每条都是一次完整独立的运行，不是依次执行：

        # 动了 HTTP 解析 / 路由 / 中间件
        python3 scripts/perf_regress.py --profiles http

        # 动了 WebSocket
        python3 scripts/perf_regress.py --profiles websocket

        # 动了序列化
        python3 scripts/perf_regress.py --profiles json

        # 动了定时器 / 协程
        python3 scripts/perf_regress.py --profiles async

        # 说不好碰了哪块，先跑一组影响面广的
        python3 scripts/perf_regress.py --profiles broad

     发版前跑全部计分档，6 对 × 10s，够做符号检验：
        python3 scripts/perf_regress.py --profiles scored

     单场景复测，只压 echo-ws，连接数自己定：
        python3 scripts/perf_regress.py --conns 4096

     指定版本，不想用默认的「最近 tag vs HEAD」时：
        python3 scripts/perf_regress.py --profiles websocket --baseline v2.7.0 --candidate HEAD

     续跑，断了、换了机器、或者只想补没出数的档：
        python3 scripts/perf_regress.py --resume --output-dir perf-regress-20261030-101500

     只看命令不执行，先确认要跑什么，不碰 docker 也不写文件：
        python3 scripts/perf_regress.py --dry-run

     HttpArena 仓库不在默认的 ~/HttpArena 时，每条命令后面加 --httparena /你的/路径。
     这个参数只管找仓库，和测什么、测几对无关。

  时间成本：每次 benchmark.sh 调用约 25~40s（容器启停和等 ready 占大头），一档 6 对
  就是 12 次调用。全档 6 对大概 2~3 小时，--only-conns 砍掉档数是最有效的加速手段。

前提条件（脚本查不全，得人自己保证）：

  * 宿主机「HypervisorPresent」这类的检查只能丢到 VM 外面看，本脚本看不到；
    如果宿主机开着 Hyper-V，VM 里的网络会慢一大截，测出来的数没法信。
  * 发版回归请用真机。虚拟化环境下 4~7% 量级的结论不要下——脚本会读
    systemd-detect-virt 提醒，但拦不住。
  * CPU governor 得是 performance，不然频率随负载漂，直接盖掉要测的差异。
  * 网卡最好走 virtio。脚本会读 /sys/class/net/*/device/uevent 碰运气，
    读不到只警告不拦。
  * 需要 docker 能用，而且当前用户有权限；Ubuntu 记得 sudo 或加 docker 组。
  * 版本是现场解析的，所以得有 git，还得能摸到一个 Hical 仓库：脚本先看自己所在的
    仓库，摸不到就用 --hical-repo 指过去。
  * 第一次冒烟会跑 system_tune（改内核参数 + 重启 docker daemon），很慢，
    之后就靠 SKIP_TUNE=true 跳过。

产物：
    <输出目录>/
        smoke-*.log
        <profile>-<conns>-p<轮>-<pos>-<baseline|candidate>.log
        runs.jsonl              每完成一侧追加一行，续跑和报告都靠它
        report.md
"""

import argparse
import atexit
import dataclasses
import datetime
import json
import os
import pathlib
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import time
import unicodedata

try:
    import resource
except ImportError:  # Windows 上没有这个模块，dry-run 时直接跳过 ulimit 检查
    resource = None


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")

# --baseline / --candidate 的默认值，交给 git 现场解析，别在源码里钉死 sha
REF_AUTO = "auto"
CANDIDATE_AUTO_REF = "HEAD"
BASELINE_FALLBACK_REF = "HEAD~1"

# 两侧的名字。终端、报告、runs.jsonl、日志名对外只用这两个词，
# 内部变量名也跟着走，省得哪天格式化字符串里漏出单字母
SIDE_BASELINE = "baseline"
SIDE_CANDIDATE = "candidate"
SIDE_LABELS = {SIDE_BASELINE: "基线", SIDE_CANDIDATE: "待验证"}
# framework 目录名不是 side 直接加前缀拼出来的，得查表
SIDE_FRAMEWORK = {SIDE_BASELINE: "hical-base", SIDE_CANDIDATE: "hical-cand"}
# 版本行的标签宽度：按显示宽度对齐（中文占两格），「基线」占 4 格、「待验证」占 6 格，
# 补到 6 格再拼 " = "，两行的等号就齐了
REF_LABEL_WIDTH = 6
# 看着就是 sha 的 ref，展示时不再重复一遍短 sha
HEX_REF = re.compile(r"^[0-9a-fA-F]{7,40}$")

TOTAL_STAGES = 7
BEST_LINE = re.compile(r"^=== Best: (\d+)", re.MULTILINE)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
# 固定速率档的真指标在这行里：rps 是压测机自己的给定值，比它没意义
FIXED_RATE_LINE = re.compile(
    r"=== Fixed-rate: cpu/req (\S+) \| p99 (\S+) \| p99\.9 (\S+) \| rate_ratio (\S+) ===",
)
# profiles.sh 里每一项：    [名字]="pipeline|req_per_conn|cpuset|conns|endpoint"
PROFILE_LINE = re.compile(r'^\s*\[([A-Za-z0-9_.\-]+)\]="([^"]*)"', re.MULTILINE)

WS_GROUP = "ws"
SCORED_GROUP = "scored"
ALL_GROUP = "all"
ECHO_WS_PROFILE = "echo-ws"
WS_ENDPOINT = "ws-echo"
DEFAULT_CONNS = 4096
RUNS_FILE = "runs.jsonl"

# 发版回归要看的那些档：覆盖 h1 吞吐、连接数限制、JSON、TLS、大包、异步、DB 和三种 WS。
# 不含 latency-* 和 h2/h3 —— 那几档在 hical 的 meta.json 里，用 --profiles all 才跑
SCORED_PROFILES = [
    "baseline",
    "limited-conn",
    "json-comp",
    "json-tls",
    "8gbit",
    "async",
    "async-db",
    "echo-ws",
    "echo-ws-pipeline",
    "echo-ws-limited",
]

# 分类预设：按「想验哪一块」挑，比记 profile 名省事。这些档和 smoke 都按快跑规格
# （3 对 × 5s）来，看方向够用；发版回归用 scored / all，那两档是 6 对 × 10s
PROFILE_PRESETS = {
    "http": ["baseline", "pipelined"],
    "json": ["json-comp", "json-tls"],
    "websocket": ["echo-ws", "echo-ws-pipeline"],
    "async": ["async", "latency-10k"],
    "db": ["async-db", "limited-conn"],
    "broad": ["baseline", "echo-ws", "json-comp"],
    "smoke": ["baseline", "json-comp", "echo-ws"],
}
# 预设默认值。显式传 --pairs / --duration 覆盖
QUICK_PAIRS = 3
QUICK_DURATION = "5s"
DEFAULT_PAIRS = 6
DEFAULT_DURATION = "10s"

# benchmark.sh 扫 meta.json 的 tests 列表决定起不起 postgres（scripts/benchmark.sh 主循环之前）
POSTGRES_PROFILES = {"async-db", "fortunes", "gateway-64", "gateway-h3", "production-stack"}

# 走 zrk 的档。这几个档的 rps 是压测机自己给定的，基线 / 待验证 比的是 cpu/req
ZRK_ENDPOINTS = {"latency-1m", "latency-10k", "latency-500k-8cpu", "8gbit"}

# 这个档整个的存在意义就是「只给 4 个物理核」，cpuset 语义特殊，不能按前半段覆盖
LATENCY_8CPU_PROFILE = "latency-500k-8cpu"
LATENCY_8CPU_CPUS = "0-3,64-67"
LATENCY_8CPU_MIN_CORES = 68

# 一次 benchmark.sh 调用的固定开销：起停容器 + 等 ready + 清场 + 每档冷却，经验值
RUN_OVERHEAD_SECONDS = 25

PROFILE_FIELD_COUNT = 5
COL_PIPELINE = 0
COL_REQ_PER_CONN = 1
COL_CPUS = 2
COL_CONNS = 3
COL_ENDPOINT = 4

# gcannon 各档吃的 requests/ 下的模板文件（照 scripts/lib/tools/gcannon.sh 抄的）。
# 缺了这些文件 benchmark.sh 不会当场炸，而是在压测窗口里出 0，很容易被读成性能问题。
REQUESTS_FIXTURES = {
    "baseline": ["get.raw", "post_cl.raw", "post_chunked.raw"],
    "limited-conn": ["get.raw", "post_cl.raw", "post_chunked.raw"],
    "async": ["async-delay.raw"],
    "async-db": [
        "async-db-5.raw",
        "async-db-10.raw",
        "async-db-20.raw",
        "async-db-35.raw",
        "async-db-50.raw",
    ],
    "json-comp": ["json-gzip-25.raw", "json-gzip-40.raw", "json-gzip-50.raw"],
    "json-tls": ["json-tls-rotate.lua"],
    "static-tls": ["static-rotate.lua"],
}
# 8gbit 不在这儿：它现在走 zrk，请求体由 zrk 自己生成（REQUESTS_DIR/.echo-body.bin），
# requests/8gbit-rotate.lua 是 wrk 时代的遗留，查它只会在裁剪过的 checkout 上误报

DATA_FIXTURES = [
    ("data/dataset.json", "json-comp / json-tls 的响应体就是这个文件"),
    ("data/static/", "static-* 档直接从这里取静态文件"),
    ("certs/", "所有 TLS / H2 / H3 档都从这里读 server.crt / server.key"),
]


class BenchError(RuntimeError):
    """脚本主动发现的问题。带上「下一步该看什么」再往外抛，别只丢 traceback。"""


@dataclasses.dataclass(frozen=True)
class MetricKind:
    """
    基线 / 待验证 比哪个数。同一个脚本要覆盖吞吐类和固定速率类，两者「好」的方向不一样，
    所以这里把口径显式写出来，报告和判定都读它，不再各自猜。
    """

    key: str
    unit: str
    lower_is_better: bool
    label: str


METRIC_WS = MetricKind("frames", "frames/s", False, "WS 帧吞吐（越高越好）")
METRIC_THROUGHPUT = MetricKind("throughput", "req/s", False, "请求吞吐（越高越好）")
METRIC_CPU_PER_REQ = MetricKind("us_per_req", "us/req", True, "每请求 CPU 成本（越低越好）")


@dataclasses.dataclass(frozen=True)
class RefInfo:
    """
    一侧版本的来历。ref 是给人看的名字（tag / HEAD / HEAD~1 / 用户原样给的），
    sha 是喂给 Dockerfile 的完整 sha，subject 是提交说明——报告里少了它，
    过两个月就没人知道 v2.7.1 到底是哪个提交了。

    resolved=False 表示压根没解析过（找不到 Hical 仓库，用户给的什么就是什么）。
    """

    ref: str
    sha: str = ""
    short_sha: str = ""
    subject: str = ""
    resolved: bool = True


@dataclasses.dataclass(frozen=True)
class Group:
    """一档 = 一个 profile 的一个连接数。报告、续跑、时间预估都以它为单位。"""

    profile: str
    conns: int


@dataclasses.dataclass
class RunRecord:
    """runs.jsonl 的一行：一侧（一次 benchmark.sh 调用）的结果。"""

    profile: str
    conns: int
    pair: int
    side: str
    frames: int
    returncode: int
    log_file: str
    timestamp: str
    us_per_req: float = None
    rate_ratio: str = ""

    def key(self):
        return (self.profile, self.conns, self.pair, self.side)

    def to_json(self):
        return json.dumps(
            {
                "profile": self.profile,
                "conns": self.conns,
                "pair": self.pair,
                "side": self.side,
                "frames": self.frames,
                "returncode": self.returncode,
                "log_file": self.log_file,
                "timestamp": self.timestamp,
                "us_per_req": self.us_per_req,
                "rate_ratio": self.rate_ratio,
            },
            ensure_ascii=False,
        )


@dataclasses.dataclass
class GroupStats:
    """报告里一节的内容。判定结果也算好放这儿，汇总表和总判定直接读。"""

    profile: str
    conns: int
    kind: MetricKind
    rows: list
    valid: list
    diffs: list
    median: float
    spread: float
    resolution_ok: bool
    same_sign: bool
    verdict: str
    conclusion: str
    notes: list

    @property
    def is_regression(self):
        if self.median is None or self.median == 0:
            return False
        return self.median > 0 if self.kind.lower_is_better else self.median < 0

    @property
    def direction_text(self):
        if not self.diffs:
            return "—"
        if not self.same_sign:
            return "混杂"
        if self.kind.lower_is_better:
            return "待验证更省" if self.median < 0 else "待验证更费"
        return "待验证更快" if self.median > 0 else "待验证更慢"


@dataclasses.dataclass
class CommandResult:
    returncode: int
    output: str


@dataclasses.dataclass
class BenchContext:
    httparena: pathlib.Path
    dry_run: bool
    hical_repo: pathlib.Path = None
    baseline_info: RefInfo = None
    candidate_info: RefInfo = None
    cpu_count: int = 0
    docker_version: str = "未知"
    kernel_version: str = "未知"
    httparena_commit: str = "未知"
    image_baseline: str = "未知"
    image_candidate: str = "未知"
    virtualization: str = "（未检测）"
    bare_metal: bool = False
    cpu_governor: str = "（未检测）"
    server_cpus: str = ""
    gcannon_cpus: str = ""
    ws_mode: bool = True
    quick_mode: bool = False
    profile_source: str = WS_GROUP
    profile_names: list = dataclasses.field(default_factory=list)
    profile_table: dict = dataclasses.field(default_factory=dict)
    groups: list = dataclasses.field(default_factory=list)
    planned_invocations: int = 0
    estimated_seconds: float = 0.0
    resumed: dict = dataclasses.field(default_factory=dict)
    resumed_count: int = 0
    report_notes: list = dataclasses.field(default_factory=list)


# ── 输出小工具 ──────────────────────────────────────────────────────────────

def stage_header(index, title):
    print(f"\n[{index}/{TOTAL_STAGES}] {title}", flush=True)


def note(message):
    print(f"      {message}", flush=True)


def warn(message):
    print(f"      [警告] {message}", flush=True)


def tail_text(text, limit=2000):
    """把命令输出截到末尾一段、每行缩进，塞进报错里方便用户直接看原文。"""
    text = (text or "").strip()
    if not text:
        return "        （无输出）"
    if len(text) > limit:
        text = "…（前面截断）…\n" + text[-limit:]
    return "\n".join("        " + line for line in text.splitlines())


def format_duration(seconds):
    seconds = int(round(seconds))
    hours, rest = divmod(seconds, 3600)
    minutes, secs = divmod(rest, 60)
    if hours:
        return f"{hours} 小时 {minutes} 分"
    if minutes:
        return f"{minutes} 分 {secs} 秒"
    return f"{secs} 秒"


def text_width(text):
    """终端里的显示宽度：中日韩字符占两格，其余一格。只为了把版本行对齐。"""
    return sum(2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1 for ch in text)


def pad_to_width(text, width):
    return text + " " * max(width - text_width(text), 0)


def ref_identity(info):
    """
    `v2.7.1 (5f300b8)` 这种身份串。用户直接给 sha 时只留 sha 一段，
    别再写成 `abc1234 (abc1234)` 那种自己重复自己的样子。
    """
    if not info.resolved:
        return info.ref
    if HEX_REF.match(info.ref):
        return info.short_sha
    return f"{info.ref} ({info.short_sha})"


def ref_line(info):
    """终端里的版本行：`v2.7.1 (5f300b8) [chore] release v2.7.1`。"""
    identity = ref_identity(info)
    return f"{identity} {info.subject}" if info.subject else identity


def ref_report_cell(info):
    """报告环境表里的一格：完整 sha 和提交说明都得留，短 sha 只够终端扫一眼。"""
    if info is None:
        return "（未解析）"
    if not info.resolved:
        return f"`{info.ref}`（没解析，找不到 Hical 仓库）"
    subject = info.subject or "（没有提交说明）"
    return f"{ref_identity(info)} · `{info.sha}` · {subject}"


def print_ref_lines(ctx):
    """把解析好的两侧版本打出来。故意打两行——同一份提交当两边压是常见事故。"""
    for side in (SIDE_BASELINE, SIDE_CANDIDATE):
        info = ctx.baseline_info if side == SIDE_BASELINE else ctx.candidate_info
        label = pad_to_width(SIDE_LABELS[side], REF_LABEL_WIDTH)
        print(f"{label} = {ref_line(info)}", flush=True)


# ── 外部命令统一入口 ────────────────────────────────────────────────────────

def run_command(command, *, cwd=None, env=None, dry_run=False, log_path=None, quiet=False):
    """
    所有外部命令都从这儿过。
    dry-run 只把命令打出来；正式跑时把子进程输出实时怼到终端，同时写一份日志。
    quiet=True 用于检测类命令，只要 returncode 和输出，不刷屏。
    """
    argv = [str(part) for part in command]
    printable = " ".join(shlex.quote(part) for part in argv)
    if not quiet:
        print(f"      $ {printable}", flush=True)
    if dry_run:
        return CommandResult(0, "")

    run_cwd = str(cwd) if cwd is not None else None
    try:
        if quiet:
            completed = subprocess.run(
                argv,
                cwd=run_cwd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            return CommandResult(completed.returncode, completed.stdout or "")

        handle = None
        if log_path is not None:
            log_path = pathlib.Path(log_path)
            log_path.parent.mkdir(parents=True, exist_ok=True)
            handle = log_path.open("w", encoding="utf-8", errors="replace")
        collected = []
        try:
            process = subprocess.Popen(
                argv,
                cwd=run_cwd,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                bufsize=1,
            )
            assert process.stdout is not None
            for line in process.stdout:
                collected.append(line)
                print(f"      {line.rstrip()}", flush=True)
                if handle is not None:
                    handle.write(line)
            process.wait()
            return CommandResult(process.returncode, "".join(collected))
        finally:
            if handle is not None:
                handle.close()
    except FileNotFoundError as exc:
        raise BenchError(
            f"找不到命令 {argv[0]}，先确认它装了、也在 PATH 里"
        ) from exc
    except OSError as exc:
        raise BenchError(f"执行 `{printable}` 出错：{exc}") from exc


# ── profiles.sh 读写 ────────────────────────────────────────────────────────

def profiles_path(ctx):
    return ctx.httparena / "scripts" / "lib" / "profiles.sh"


def split_profile_spec(spec):
    fields = spec.split("|")
    if len(fields) != PROFILE_FIELD_COUNT:
        raise BenchError(
            f"profile 定义格式不对：'{spec}' 应该正好 5 段"
            f"（pipeline|req_per_conn|cpuset|conns|endpoint），实际 {len(fields)} 段。\n"
            "        打开 scripts/lib/profiles.sh 看看这行是不是被手改过"
        )
    return fields


def read_profile_table(path):
    """把 profiles.sh 里 PROFILES=(...) 的每一项抠成 name -> 5 段。"""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise BenchError(
            f"读不到 {path}：{exc}\n"
            "        先确认 --httparena 指的是 clone 下来的 HttpArena 根目录（里面有 scripts/lib/profiles.sh）"
        ) from exc
    table = {}
    for match in PROFILE_LINE.finditer(text):
        table[match.group(1)] = split_profile_spec(match.group(2))
    return table


def patch_profile_line(text, name, fields):
    """
    只替换 [name]= 那一行，其余行原样留着。
    返回 (新文本, 命中次数)，命中 0 次说明 profiles.sh 里没这个名字。
    """
    spec = "|".join(fields)
    pattern = re.compile(r"^(\s*)\[" + re.escape(name) + r'\]=".*"', re.MULTILINE)
    return pattern.subn(lambda m: f'{m.group(1)}[{name}]="{spec}"', text, count=1)


def apply_profile_columns(ctx, name, columns):
    """
    改 profiles.sh 里某个 profile 的若干列。columns 形如 {COL_CPUS: "0-7"}。
    每次都重新读文件再改 —— 后面还会按档改 connections，内存里缓存容易拿旧值覆盖。
    """
    if ctx.dry_run:
        return None
    path = profiles_path(ctx)
    table = read_profile_table(path)
    if name not in table:
        raise BenchError(
            f"profiles.sh 里没有 [{name}] 这一项，改不了。\n"
            f"        打开 {path} 的 PROFILES 段核对名字"
        )
    fields = list(table[name])
    for index, value in columns.items():
        fields[index] = str(value)
    text = path.read_text(encoding="utf-8")
    patched, count = patch_profile_line(text, name, fields)
    if count == 0:
        raise BenchError(
            f"profiles.sh 里没改成功 [{name}]（正则没命中）。\n"
            f"        手动打开 {path} 看这一行的格式是不是 {name}=\"...\""
        )
    path.write_text(patched, encoding="utf-8")
    return fields


def endpoint_of(ctx, profile):
    fields = ctx.profile_table.get(profile)
    if not fields:
        return ""
    return fields[COL_ENDPOINT]


def metric_for(ctx, profile):
    """这个档的报告口径。ws 模式固定只有 echo-ws，不用查表。"""
    if ctx.ws_mode:
        return METRIC_WS
    endpoint = endpoint_of(ctx, profile)
    if endpoint == WS_ENDPOINT:
        return METRIC_WS
    if endpoint in ZRK_ENDPOINTS:
        return METRIC_CPU_PER_REQ
    return METRIC_THROUGHPUT


def headline_unit(ctx, profile):
    """benchmark.sh 打出的 `=== Best:` 那个数的单位。"""
    return "frames/s" if metric_for(ctx, profile).key == METRIC_WS.key else "req/s"


# ── 档位选择 ────────────────────────────────────────────────────────────────

def read_meta_tests(ctx):
    meta_path = ctx.httparena / "frameworks" / "hical" / "meta.json"
    if not meta_path.is_file():
        raise BenchError(
            f"找不到 {meta_path}，--profiles all 要读它的 tests 列表。\n"
            "        先确认 HttpArena 仓库完整，或者改用 --profiles scored / 自定义列表"
        )
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BenchError(f"读不了 {meta_path}：{exc}\n        打开看一眼是不是 JSON 坏了") from exc
    tests = meta.get("tests") or []
    if not tests:
        raise BenchError(f"{meta_path} 里 tests 是空的，--profiles all 无从下手")
    return list(tests)


def peek_meta_tests(ctx):
    """
    能读就读，读不到就当空（调用方自己决定怎么报错）。
    hical 在 meta.json 里申报了支持哪些档，写回 hical-base/hical-cand 的 tests 只能是它的子集——
    超出的档 benchmark.sh 会按「framework 没订阅」直接跳过，白排一档还拿不到数。
    """
    meta_path = ctx.httparena / "frameworks" / "hical" / "meta.json"
    try:
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    return list(meta.get("tests") or [])


def resolve_profiles(args, ctx):
    """
    把 --profiles 解析成 ctx.profile_names 和 ctx.groups。
    ws 模式故意不碰 profiles.sh —— 那条路径已经跑出过真实数据，别为了统一而多读一次文件。
    """
    raw = (args.profiles or "").strip()
    ctx.ws_mode = raw == WS_GROUP
    if args.conns is None:
        # main() 会先归一化一次，这里再兜一层，省得直接调这个函数时拿到 None
        args.conns = DEFAULT_CONNS

    if ctx.ws_mode:
        ctx.profile_source = WS_GROUP
        ctx.profile_names = [ECHO_WS_PROFILE]
    elif raw == SCORED_GROUP:
        ctx.profile_source = SCORED_GROUP
        ctx.profile_names = list(SCORED_PROFILES)
    elif raw == ALL_GROUP:
        ctx.profile_source = ALL_GROUP
        if not ctx.httparena.is_dir():
            raise BenchError(
                f"--profiles all 要读 {ctx.httparena}/frameworks/hical/meta.json，这个目录不存在。\n"
                "        用 --httparena 指到 clone 下来的 HttpArena 根目录"
            )
        ctx.profile_names = read_meta_tests(ctx)
    elif raw in PROFILE_PRESETS:
        # 单个词优先按预设解析。注意 async 既是预设名也是 profile 名：
        # 单独写就是预设（async + latency-10k），混在逗号列表里当 profile 用
        ctx.profile_source = f"{raw}（预设）"
        ctx.quick_mode = True
        ctx.profile_names = list(PROFILE_PRESETS[raw])
    else:
        tokens = [token.strip() for token in raw.split(",") if token.strip()]
        if not tokens:
            raise BenchError("--profiles 是空的，至少给一个 profile 名")
        reserved = [t for t in tokens if t in (WS_GROUP, SCORED_GROUP, ALL_GROUP)]
        if reserved:
            raise BenchError(
                f"--profiles 里的 {', '.join(reserved)} 是分组名，只能单独用。\n"
                "        想要具体档位就直接写 profile 名，比如 --profiles baseline,echo-ws"
            )
        duplicated = sorted({t for t in tokens if tokens.count(t) > 1})
        if duplicated:
            warn(f"--profiles 里这些重复了，只跑一次：{', '.join(duplicated)}")
            tokens = list(dict.fromkeys(tokens))
        ctx.profile_source = "自定义"
        ctx.profile_names = tokens

    if not ctx.ws_mode:
        if not ctx.httparena.is_dir():
            raise BenchError(
                f"--profiles {ctx.profile_source} 要读 {ctx.httparena}/scripts/lib/profiles.sh，"
                "这个目录不存在。\n"
                "        用 --httparena 指到 clone 下来的 HttpArena 根目录"
            )
        ctx.profile_table = read_profile_table(profiles_path(ctx))
        missing = [name for name in ctx.profile_names if name not in ctx.profile_table]
        if missing:
            known = ", ".join(sorted(ctx.profile_table))
            preset_hint = [name for name in missing if name in PROFILE_PRESETS]
            hint = (
                f"\n        {', '.join(preset_hint)} 是预设名，只能单独用，"
                "不能和其他 profile 混在逗号列表里"
                if preset_hint
                else ""
            )
            raise BenchError(
                f"profiles.sh 里没有这些 profile：{', '.join(missing)}\n"
                f"        现有的是：{known}{hint}\n"
                f"        打开 {profiles_path(ctx)} 的 PROFILES 段核对名字（注意是 - 不是 _）"
            )

        # hical 没申报的档 benchmark.sh 会跳过，留在计划里只会白排一档
        subscribed = peek_meta_tests(ctx)
        dropped = [name for name in ctx.profile_names if name not in subscribed]
        if dropped:
            warn(
                f"hical 的 meta.json 没申报这些档，benchmark.sh 会跳过，先从计划里摘掉："
                f"{', '.join(dropped)}\n"
                f"        hical 申报的是：{', '.join(subscribed) or '（读不到 frameworks/hical/meta.json）'}"
            )
            ctx.profile_names = [name for name in ctx.profile_names if name in subscribed]
            if not ctx.profile_names:
                raise BenchError(
                    "摘掉没申报的档之后一个都不剩了。\n"
                    "        下一步：改用 --profiles all 看 hical 申报了哪些，或者修好 "
                    "frameworks/hical/meta.json 再跑"
                )

    groups = []
    kept_profiles = []
    for name in ctx.profile_names:
        if ctx.ws_mode:
            groups.append(Group(name, args.conns))
            kept_profiles.append(name)
            continue
        fields = ctx.profile_table[name]
        levels = []
        for token in fields[COL_CONNS].split(","):
            token = token.strip()
            if not token.isdigit():
                raise BenchError(
                    f"profile {name} 的 connections 列写的是 '{fields[COL_CONNS]}'，里面有非数字项 '{token}'。\n"
                    f"        打开 {profiles_path(ctx)} 改这一行"
                )
            levels.append(int(token))
        if args.only_conns is not None:
            if args.only_conns not in levels:
                warn(
                    f"{name} 没有 {args.only_conns} 连接这一档"
                    f"（它有 {'/'.join(str(level) for level in levels)}），跳过"
                )
                continue
            levels = [args.only_conns]
        kept_profiles.append(name)
        groups.extend(Group(name, level) for level in levels)

    if not groups:
        raise BenchError(
            "--only-conns 过滤之后一档都不剩了。\n"
            "        下一步：去掉 --only-conns，或者换成这些档里真有的连接数"
        )
    ctx.profile_names = kept_profiles
    ctx.groups = groups

    # 预设默认值在解析完之后才能定：快跑规格和发版规格不一样
    if args.pairs is None:
        args.pairs = QUICK_PAIRS if ctx.quick_mode else DEFAULT_PAIRS
    if args.duration is None:
        args.duration = QUICK_DURATION if ctx.quick_mode else DEFAULT_DURATION


# ── cpuset 派生 ─────────────────────────────────────────────────────────────

def derive_server_cpus(cpu_count):
    """
    server 拿前半段，后半段留给 gcannon（common.sh 自己按 nproc 算）。
    nproc=8 -> 0-3，nproc=64 -> 0-31。
    """
    if not cpu_count:
        # 探测不到就别乱猜，退回默认值——总比给个空 cpuset 让 docker 报错强
        return "0-3"
    if cpu_count >= 128:
        # common.sh 在 >=128 逻辑核时把 gcannon 钉在 32-63,96-127，这里如果硬切
        # 0-63 就撞上 32-63 了，两边抢核。按对方的排版让出 SMT 上半区。
        return "0-31,64-95"
    if cpu_count <= 4:
        # 核太少，server 全拿，gcannon 只能抢，结果仅供参考（阶段 1 会警告）
        return f"0-{cpu_count - 1}"
    return f"0-{cpu_count // 2 - 1}"


def derive_gcannon_cpus(cpu_count):
    """照抄 common.sh 的算法，只为了记进报告，不拿去设环境变量。"""
    if not cpu_count:
        return "（未知，由 common.sh 按 nproc 自算）"
    if cpu_count >= 128:
        return "32-63,96-127"
    if cpu_count <= 2:
        return f"0-{cpu_count - 1}"
    return f"{cpu_count // 2}-{cpu_count - 1}"


def cpuset_for(args, ctx, profile):
    """
    某个 profile 该用哪个 cpuset。默认就是 server 那半段，
    latency-500k-8cpu 例外：它测的就是「只给 4 个物理核」，按前半段覆盖等于把这个档废掉。
    """
    if profile == LATENCY_8CPU_PROFILE:
        if ctx.cpu_count and ctx.cpu_count >= LATENCY_8CPU_MIN_CORES:
            return LATENCY_8CPU_CPUS
        return "0-3"
    return ctx.server_cpus


# ── Hical 版本解析 ──────────────────────────────────────────────────────────

def script_repo_root():
    """脚本自己的上一级目录。脚本放在 Hical 的 scripts/ 里时，这个就是仓库根。"""
    return pathlib.Path(__file__).resolve().parent.parent


def is_git_repo(path):
    """
    这个目录是不是 git 仓库根。比 rev-parse --git-dir 的返回码严格一点：
    子目录也会返回 0，所以拿 --show-toplevel 和目录本身比一比，
    免得「脚本被拷到 ~/ 下」这种情况被误判成仓库。裸镜像仓库没有工作区，
    --show-toplevel 会失败，那是正常的，再问一句 is-bare-repository。
    """
    if not path.is_dir():
        return False
    toplevel = run_command(
        ["git", "-C", str(path), "rev-parse", "--show-toplevel"], quiet=True
    )
    if toplevel.returncode == 0 and toplevel.output.strip():
        return pathlib.Path(toplevel.output.strip()).resolve() == path.resolve()
    bare = run_command(
        ["git", "-C", str(path), "rev-parse", "--is-bare-repository"], quiet=True
    )
    return bare.returncode == 0 and bare.output.strip() == "true"


def resolve_hical_repo(args):
    """
    找 Hical 仓库：显式 --hical-repo，或者脚本自己所在的仓库（脚本躺在 <repo>/scripts/ 下）。
    实在找不到返回 None，调用方决定是原样透传还是报错。
    """
    if shutil.which("git") is None:
        if args.hical_repo is not None:
            raise BenchError(
                "找不到 git，解析不了 --hical-repo 里的版本。\n"
                "        装个 git，或者显式传 --baseline / --candidate（那就只按字符串用，不做检查）"
            )
        return None

    if args.hical_repo is not None:
        path = pathlib.Path(args.hical_repo).expanduser().resolve()
        if not path.is_dir():
            raise BenchError(
                f"--hical-repo 指的 {path} 不是目录。\n"
                "        指到 clone 下来的 Hical 根目录（里面该有 .git、src/、scripts/）"
            )
        if not is_git_repo(path):
            raise BenchError(
                f"--hical-repo 指的 {path} 不是 git 仓库（git rev-parse 没认出工作区根）。\n"
                "        版本要现场解析，所以必须指到真正的 clone，不能是空目录或者别处的副本"
            )
        return path

    root = script_repo_root()
    if is_git_repo(root):
        return root.resolve()
    return None


def nearest_tag(repo):
    """HEAD 能走到的最新 tag。一个 tag 都没有时返回 None。"""
    result = run_command(
        ["git", "-C", str(repo), "describe", "--tags", "--abbrev=0", "HEAD"], quiet=True
    )
    if result.returncode != 0:
        return None
    return result.output.strip() or None


def rev_parse_commit(repo, ref, label):
    """把 tag / 分支 / 短 sha 统一解析成完整 sha。解析不出来就带着原地输出报错。"""
    result = run_command(
        ["git", "-C", str(repo), "rev-parse", "--verify", f"{ref}^{{commit}}"], quiet=True
    )
    if result.returncode != 0:
        raise BenchError(
            f"{label} '{ref}' 在 {repo} 里解析不出 commit。\n"
            f"{tail_text(result.output)}\n"
            "        下一步：git log --oneline -5 看看有哪些提交，"
            "或者换一个 tag / 分支 / 完整 sha 传进来"
        )
    sha = result.output.strip()
    if not sha:
        raise BenchError(f"{label} '{ref}' 解析出空 sha，{repo} 是不是浅克隆得只剩一两条？")
    return sha


def describe_commit(repo, sha):
    """`%h %s` 拆成短 sha 和提交说明。取不到时返回 (sha 前 7 位, 空说明)。"""
    result = run_command(
        ["git", "-C", str(repo), "log", "-1", "--format=%h %s", sha], quiet=True
    )
    if result.returncode != 0:
        return sha[:7], ""
    short_sha, _, subject = result.output.strip().partition(" ")
    return short_sha or sha[:7], subject


def resolve_ref(repo, ref, label):
    sha = rev_parse_commit(repo, ref, label)
    short_sha, subject = describe_commit(repo, sha)
    return RefInfo(ref=ref, sha=sha, short_sha=short_sha, subject=subject)


def resolve_versions(args, ctx):
    """
    把 --baseline / --candidate 落到完整 sha，顺手把仓库找出来。
    用户显式传的一律原样用，不做自动推导——传 v2.7.0 就是想比 v2.7.0。
    """
    ctx.hical_repo = resolve_hical_repo(args)
    if ctx.hical_repo is None:
        if REF_AUTO in (args.baseline, args.candidate):
            raise BenchError(
                "找不到 Hical 仓库，--baseline / --candidate 又是默认的 auto，"
                "现场解析不了版本。\n"
                "        下一步：用 --hical-repo 指到 Hical 的 clone，"
                "或者显式传 --baseline <tag 或 sha> --candidate <tag 或 sha>"
            )
        # 没有仓库（VM 里只拷了脚本、或者压根没装 git）就把用户给的原样透传，
        # 只警告不拦：Docker 那边照样可能拉得到，拦下反而挡住能跑的场次
        ctx.baseline_info = RefInfo(ref=args.baseline, resolved=False)
        ctx.candidate_info = RefInfo(ref=args.candidate, resolved=False)
        print_ref_lines(ctx)
        return

    baseline_fallback = None
    baseline_ref = args.baseline
    if baseline_ref == REF_AUTO:
        baseline_ref = nearest_tag(ctx.hical_repo)
        if baseline_ref is None:
            baseline_ref = BASELINE_FALLBACK_REF
            baseline_fallback = (
                f"{ctx.hical_repo} 里一个 tag 都没走到，基线退成 {BASELINE_FALLBACK_REF}"
                "（想指定就显式传 --baseline）"
            )
    candidate_ref = args.candidate
    if candidate_ref == REF_AUTO:
        candidate_ref = CANDIDATE_AUTO_REF

    ctx.baseline_info = resolve_ref(
        ctx.hical_repo, baseline_ref, SIDE_LABELS[SIDE_BASELINE]
    )
    ctx.candidate_info = resolve_ref(
        ctx.hical_repo, candidate_ref, SIDE_LABELS[SIDE_CANDIDATE]
    )
    args.baseline = ctx.baseline_info.sha
    args.candidate = ctx.candidate_info.sha

    print_ref_lines(ctx)
    if baseline_fallback:
        warn(baseline_fallback)


# ── 阶段 1：环境自检 ────────────────────────────────────────────────────────

def ref_is_pushed(ctx, info):
    """
    这个提交在不在某个远端分支上。返回 True / False，判断不了返回 None。
    Dockerfile 只 `git fetch --depth 1 origin <sha>`，本地有不算数。
    """
    repo = ctx.hical_repo
    if repo is None or info is None or not info.resolved:
        return None

    exists = run_command(["git", "-C", str(repo), "cat-file", "-e", info.sha], quiet=True)
    if exists.returncode != 0:
        raise BenchError(
            f"提交 {info.sha} 在 {repo} 里不存在，解析出来的值和仓库对不上。\n"
            f"{tail_text(exists.output)}\n"
            "        下一步：git -C <仓库> log --oneline -5 对比一下，别是两份不同的 clone"
        )

    remotes = run_command(["git", "-C", str(repo), "remote"], quiet=True)
    if remotes.returncode != 0 or not remotes.output.strip():
        return None  # 没配远端，后面按「查不出来」处理

    contains = run_command(
        ["git", "-C", str(repo), "branch", "-r", "--contains", info.sha], quiet=True
    )
    if contains.returncode != 0:
        return None
    return bool(contains.output.strip())


def check_ref_pushed(ctx, label, info):
    """
    HttpArena 的 Dockerfile 只能从 GitHub 拉远端已有的 commit，而性能回归往往发生在
    改动刚提交、还没 push 的时候。这里提前拦住，别等 docker build 报个看不出因果的错。
    """
    pushed = ref_is_pushed(ctx, info)
    if pushed is None:
        warn(
            f"查不出{label} {ref_identity(info)} 推没推到远端（没配远端 / branch -r 跑不出来），"
            "跳过这项检查。\n"
            "        Dockerfile 是 `git fetch --depth 1 origin <sha>`，只拉得到远端已有的提交，"
            "自己确认一下"
        )
        return
    if not pushed:
        raise BenchError(
            f"{label} {ref_identity(info)} 不在任何远端分支上。\n"
            "        HttpArena 的 Dockerfile 是 `git fetch --depth 1 origin <sha>`，"
            "只能从 GitHub 拉远端已有的 commit，本地没推的提交它拉不到。\n"
            "        下一步：先 git push，或者用 --baseline / --candidate 换成已经推上去的提交。\n"
            f"        远端跟踪分支也可能是旧的：git -C {ctx.hical_repo} fetch 之后再试"
        )
    note(f"{label} {ref_identity(info)} 已经在远端分支上")


def warn_uncommitted(ctx):
    """被测代码是从 GitHub 拉的，工作区改得再多也进不了镜像——不说一句会被当成测过了。"""
    if ctx.hical_repo is None:
        return
    status = run_command(
        ["git", "-C", str(ctx.hical_repo), "status", "--porcelain"], quiet=True
    )
    if status.returncode != 0:
        return
    dirty = [line for line in status.output.splitlines() if line.strip()]
    if not dirty:
        return
    warn(
        f"工作区有 {len(dirty)} 处未提交改动，但被测版本是从 GitHub 拉的，这些改动不会进镜像。\n"
        "        要测自己刚改的代码，先 git commit && git push，等远端有了再跑"
    )


def stage_check_refs(ctx):
    """阶段 1 的第一件事：两个版本本地在不在、推没推到远端、工作区脏不脏。"""
    if ctx.hical_repo is None:
        warn(
            "--baseline / --candidate 原样使用：既不核对本地有没有这两个提交，"
            "也不检查推没推到远端。\n"
            "        Dockerfile 是 `git fetch --depth 1 origin <sha>`，拉不拉得到请自己确认；"
            "要脚本把关就加 --hical-repo"
        )
        return
    note(f"Hical 仓库：{ctx.hical_repo}")
    for side in (SIDE_BASELINE, SIDE_CANDIDATE):
        info = ctx.baseline_info if side == SIDE_BASELINE else ctx.candidate_info
        check_ref_pushed(ctx, SIDE_LABELS[side], info)
    warn_uncommitted(ctx)


def detect_cpu_count(dry_run):
    if shutil.which("nproc"):
        result = run_command(["nproc"], quiet=True, dry_run=dry_run)
        text = result.output.strip()
        if text.isdigit() and int(text) > 0:
            return int(text)
    return os.cpu_count() or 0


def detect_kernel_version(dry_run):
    """
    内核版本必须进报告。gcannon 的 io_uring provided-buffer-ring 对内核敏感，
    Ubuntu 6.8.0-147 上注册直接 EINVAL（换上 7.0 就好），同一份代码换个内核
    能测出完全不同的数，不留版本号后面根本对不上账。
    """
    result = run_command(["uname", "-r"], quiet=True, dry_run=dry_run)
    if dry_run:
        return "（dry-run 未取）"
    return result.output.strip() or "未知"


def detect_virtualization(dry_run):
    """
    返回 systemd-detect-virt 的第一行输出。注意它在「没有虚拟化」时返回码是 1，
    但 stdout 照样打 "none"，所以只看输出不看返回码。
    """
    if not shutil.which("systemd-detect-virt"):
        return None
    result = run_command(["systemd-detect-virt"], quiet=True, dry_run=dry_run)
    if dry_run:
        return None
    lines = [line.strip() for line in result.output.splitlines() if line.strip()]
    return lines[0] if lines else None


def read_cpu_governor():
    """读不到返回 None（关了 cpufreq 的机器、容器里看不到 sysfs 都会这样）。"""
    path = pathlib.Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return None


def find_virtio_interfaces():
    """
    找 virtio 网卡。两条路一起走：一是 device/driver 这个符号链接，链接名就是驱动名，
    最靠得住；二是 device/uevent 里的 DRIVER= 行——注意内核输出的是大写，早先这里按
    小写匹配，virtio 环境也会报「没找到」，纯属误伤。

    返回 None 表示「判断不了」（看不到 sysfs，或者网卡没有 driver 信息），
    空列表才是「确实不是 virtio」。
    """
    net_dir = pathlib.Path("/sys/class/net")
    if not net_dir.is_dir():
        return None  # 非 Linux 或者容器里看不到 sysfs

    found = []
    readable = False
    for iface in net_dir.iterdir():
        driver_link = iface / "device" / "driver"
        if not driver_link.exists():
            continue
        readable = True
        try:
            driver = os.path.basename(os.readlink(driver_link))
        except OSError:
            continue
        if driver.startswith("virtio"):
            found.append(iface.name)

    # 符号链接这条读不到（比如老内核、或者权限）再退回去啃 uevent
    if not readable:
        for uevent in net_dir.glob("*/device/uevent"):
            try:
                text = uevent.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            readable = True
            if re.search(r"(?im)^DRIVER=virtio_net\s*$", text):
                found.append(uevent.parent.parent.name)

    return found if readable else None


def nofile_limits():
    if resource is None:
        return None
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    except (OSError, ValueError):
        return None
    return soft, hard


def io_uring_status():
    """读 kernel.io_uring_disabled：0=开，1=只禁非特权，2=全禁。读不到返回 None。"""
    path = pathlib.Path("/proc/sys/kernel/io_uring_disabled")
    if not path.exists():
        return None
    try:
        return int(path.read_text(encoding="utf-8").strip())
    except (OSError, ValueError):
        return None


def parse_duration_seconds(text):
    match = re.fullmatch(r"\s*(\d+(?:\.\d+)?)\s*([smh]?)\s*", str(text))
    if not match:
        raise BenchError(
            f"--duration 只认 10s / 30 / 1m 这种写法，收到的是 '{text}'。\n"
            "        benchmark.sh 那边同样只吃这个格式，改不了就先按 --duration 10s 跑"
        )
    value = float(match.group(1))
    unit = match.group(2) or "s"
    return value * {"s": 1, "m": 60, "h": 3600}[unit]


def stage_environment(args, ctx):
    stage_header(1, "环境自检")

    stage_check_refs(ctx)

    ctx.cpu_count = detect_cpu_count(ctx.dry_run)
    note(f"CPU 核数：{ctx.cpu_count}")

    ctx.server_cpus = args.server_cpus or derive_server_cpus(ctx.cpu_count)
    ctx.gcannon_cpus = derive_gcannon_cpus(ctx.cpu_count)
    note(f"server cpuset：{ctx.server_cpus}（gcannon 那边 common.sh 会拿 {ctx.gcannon_cpus}）")
    if args.server_cpus:
        warn(f"用了 --server-cpus {args.server_cpus}，cpuset 不再自动派生")
    if ctx.cpu_count and ctx.cpu_count <= 4:
        warn(
            f"只有 {ctx.cpu_count} 个逻辑核，server 拿全核，gcannon 会和它抢，"
            "结果只能看方向，量级别当真"
        )
    elif ctx.cpu_count and ctx.cpu_count < 8:
        warn(
            f"只有 {ctx.cpu_count} 个逻辑核，server({ctx.server_cpus}) 和 gcannon({ctx.gcannon_cpus}) "
            "会部分重叠，建议 8 核以上"
        )

    ctx.kernel_version = detect_kernel_version(ctx.dry_run)
    note(f"内核：{ctx.kernel_version}")

    virtualization = detect_virtualization(ctx.dry_run)
    if ctx.dry_run:
        ctx.virtualization = "（dry-run 未取）"
        note("dry-run：不跑 systemd-detect-virt，裸机与否留空")
    elif virtualization is None:
        ctx.virtualization = "（未检测，没有 systemd-detect-virt）"
        note("没有 systemd-detect-virt，判断不了是不是裸机")
    elif virtualization in ("", "none"):
        ctx.bare_metal = True
        ctx.virtualization = "none（裸机）"
        note("裸机：systemd-detect-virt 报 none")
    else:
        ctx.virtualization = virtualization
        warn(
            f"当前是虚拟化环境（{virtualization}），分辨力有限，"
            "4~7% 量级的结论不要在这里下。发版回归请换真机"
        )

    governor = read_cpu_governor()
    if governor is None:
        ctx.cpu_governor = "（读不到）"
        note("读不到 scaling_governor（关了 cpufreq 或者容器里没 sysfs），频率策略没法确认")
    else:
        ctx.cpu_governor = governor
        if governor == "performance":
            note("CPU governor：performance")
        else:
            warn(
                f"CPU governor 是 {governor}，不是 performance。频率会跟着负载漂，"
                "几个百分点的差异直接淹在抖动里。\n"
                "        下一步：sudo cpupower frequency-set -g performance"
            )

    if ctx.dry_run:
        note("dry-run：跳过 docker 连通性检查")
    else:
        if not shutil.which("docker"):
            raise BenchError(
                "找不到 docker。先装 docker 并确认它在 PATH 里，"
                "再确认当前用户有权限（sudo 或加入 docker 组）"
            )
        info = run_command(["docker", "info"], quiet=True)
        if info.returncode != 0:
            raise BenchError(
                "docker info 不通。看下面几点：\n"
                "        1) docker 守护进程起了没（systemctl status docker）\n"
                "        2) 当前用户加进 docker 组了没\n"
                "        3) 磁盘/权限有没有报错\n"
                "        docker info 原文（末尾）：\n"
                f"{tail_text(info.output)}"
            )
        version = run_command(["docker", "--version"], quiet=True)
        ctx.docker_version = version.output.strip() or "未知"
        note(f"Docker：{ctx.docker_version}")

    virtio = find_virtio_interfaces()
    if virtio is None:
        warn("读不到 /sys/class/net，网卡是不是 virtio 没法判断")
    elif virtio:
        note(f"virtio 网卡：{', '.join(virtio)}")
    else:
        warn(
            "没找到 virtio 网卡，虚拟网卡驱动可能不是 virtio，会拖慢回环压测。"
            "先自己确认一下真实驱动名：ls -l /sys/class/net/*/device/driver"
        )

    # gcannon 靠 io_uring 驱动，Ubuntu 23.10 之后默认把 io_uring 关了，禁着跑
    # 会在压测阶段才爆 setup_buf_ring: Invalid argument，白等十分钟才发现。
    # 这里提前拦下来，别让后面所有阶段陪着白跑
    uring = io_uring_status()
    if uring is None:
        note("读不到 kernel.io_uring_disabled（内核 < 6.6？），跳过 io_uring 检查")
    elif uring == 0:
        note("io_uring：可用")
    else:
        raise BenchError(
            f"kernel.io_uring_disabled={uring}，io_uring 被系统禁了。\n"
            "        gcannon 全靠 io_uring 驱动，禁着会报 setup_buf_ring: Invalid argument，"
            "压测直接出 0。\n"
            "        先执行：sudo sysctl -w kernel.io_uring_disabled=0\n"
            "        （Ubuntu 23.10+ 默认禁用，重启会恢复；要长期生效写进 /etc/sysctl.d/）"
        )

    note("提醒：宿主机 HypervisorPresent 得在 VM 外面查，本脚本看不到")

    limits = nofile_limits()
    if limits is None:
        warn("读不到 fd 上限（非 Linux 环境？），跳过 ulimit 检查")
    else:
        soft, hard = limits
        infinity = getattr(resource, "RLIMIT_INFINITY", -1)
        soft_ok = soft == infinity or soft >= 65535
        hard_ok = hard == infinity or hard >= 65535
        note(f"fd 上限：soft={soft} hard={hard}")
        if not soft_ok or not hard_ok:
            warn("fd 上限低于 65535，高连接数下可能连不上，先 ulimit -n 65535")

    if shutil.which("pgrep"):
        busy = run_command(["pgrep", "-f", "bench_server"], quiet=True)
        if busy.returncode == 0 and busy.output.strip():
            warn(f"有 bench_server 在跑（pid: {busy.output.split()}），可能干扰本次压测")

    # 第一次冒烟要跑 system_tune（改内核参数 + 重启 docker daemon），用的是 sudo。
    # 密码缓存没刷的话会卡在终端等输入，而 stdout 被脚本用 PIPE 收着，看着就像挂死。
    if not shutil.which("sudo"):
        note("没有 sudo（容器里？），跳过 sudo 检查")
    elif ctx.dry_run:
        note("dry-run：跳过 sudo 免密检查")
    else:
        probe = run_command(["sudo", "-n", "true"], quiet=True)
        if probe.returncode == 0:
            note("sudo 免密可用")
        else:
            note(
                "第一次冒烟会跑 system_tune，需要 sudo；"
                "建议先在终端执行一次 `sudo true` 刷新密码缓存，免得跑一半卡在等密码"
            )

    stage_check_dependencies(args, ctx)
    stage_time_estimate(args, ctx)


# ── 阶段 1 续：依赖预检 ─────────────────────────────────────────────────────

def check_postgres_image(ctx, need_pg):
    note(f"要用数据库的档：{', '.join(need_pg)}，需要 postgres:18 镜像")
    if ctx.dry_run:
        note("dry-run：跳过 postgres:18 镜像检查")
        return
    result = run_command(["docker", "image", "inspect", "postgres:18"], quiet=True)
    if result.returncode != 0:
        raise BenchError(
            "本机没有 postgres:18 镜像，而这批档位里有要用数据库的。\n"
            "        benchmark.sh 扫 framework 的 meta.json，只要订阅了 async-db 就起 postgres "
            "sidecar，镜像不在时它会去 pull —— 离线机器直接失败，而且失败点在压测之前，"
            "报出来很像性能问题。\n"
            "        下一步：docker pull postgres:18；或者改用不含 DB 档的 --profiles 列表"
        )
    note("postgres:18 镜像：有")


def check_data_fixtures(ctx):
    missing = []
    for relative, why in DATA_FIXTURES:
        if not (ctx.httparena / relative).exists():
            missing.append(f"{relative}（{why}）")
    if missing:
        raise BenchError(
            "缺数据文件，跑起来会在压测窗口里出 0：\n"
            + "\n".join(f"        - {item}" for item in missing)
            + "\n        下一步：HttpArena 根目录下跑 scripts/generate_dataset.py 和 "
            "scripts/generate_static.py（或者 git status 看看是不是没拉全）"
        )
    note("数据文件：dataset.json / data/static/ / certs/ 都在")


def check_requests_fixtures(ctx, names):
    requests_dir = ctx.httparena / "requests"
    missing = []
    for name in names:
        for fixture in REQUESTS_FIXTURES.get(name, []):
            if not (requests_dir / fixture).is_file():
                missing.append(f"{name} 要 requests/{fixture}")
    # pipelined 走 URL 路径、不读 --raw，但它和 baseline 共用同一条 gcannon 分支，
    # 只在 requests/ 整个空掉时才可能是坏 checkout，按目录兜一层
    if "pipelined" in names:
        raws = list(requests_dir.glob("*.raw")) if requests_dir.is_dir() else []
        if not raws:
            missing.append("pipelined 要 requests/ 下有 .raw 模板（现在一个都没有）")
    if missing:
        raise BenchError(
            "requests/ 下的模板文件不全，缺了会在压测窗口里出 0：\n"
            + "\n".join(f"        - {item}" for item in missing)
            + "\n        下一步：确认 HttpArena checkout 完整（requests/ 不该是空的），"
            "或者 git checkout -- requests/"
        )


def stage_check_dependencies(args, ctx):
    """在压测之前把能确定的缺件全查出来，别跑到一半才炸。"""
    names = ctx.profile_names
    if ctx.ws_mode:
        # ws 模式只跑 echo-ws，这几样都用不上，meta.json 也精简成只有 echo-ws，不会触发 postgres
        note("依赖检查：ws 模式只跑 echo-ws，不需要 postgres / dataset / 模板文件")
        return

    # meta.json 的 tests 现在写的就是这批要跑的档（阶段 3 干的），所以判据和
    # benchmark.sh 完全一致：列表里有 DB 档它才起 postgres
    need_pg = [name for name in names if name in POSTGRES_PROFILES]
    if need_pg:
        check_postgres_image(ctx, need_pg)
    else:
        note("这批档位不涉及数据库，meta.json 里也不会写 async-db，benchmark.sh 不起 postgres")

    needs_data = ctx.profile_source == ALL_GROUP or any(
        name.startswith(("json-", "static-")) for name in names
    )
    if needs_data:
        check_data_fixtures(ctx)

    check_requests_fixtures(ctx, names)

    tools = set()
    for name in names:
        endpoint = endpoint_of(ctx, name)
        if endpoint in ZRK_ENDPOINTS:
            tools.add("zrk")
        elif endpoint.startswith(("h2", "h3", "gateway-", "grpc", "production-")) or endpoint in (
            "json-tls",
            "static-tls",
        ):
            tools.add("h2load/wrk")
    if tools:
        note(
            f"这批档位要用 {'、'.join(sorted(tools))}，benchmark.sh 发现镜像不在会现建，"
            "首次需要网络（git clone + apt）"
        )


def count_completed_invocations(args, ctx):
    """
    --resume 时数一下 runs.jsonl 里已经跑成、且属于本次计划的调用次数。只读，不动文件。
    这里只读是为了阶段 1 就能把预估差额算出来，真正的清理留给 prepare_run_state。
    """
    if not args.resume:
        return 0
    try:
        records = load_run_records(runs_path(args))
    except BenchError:
        return 0  # 坏文件留着让 prepare_run_state 报详细错
    plan = {(group.profile, group.conns) for group in ctx.groups}
    return sum(
        1
        for record in dedupe_records(records)
        if record.frames is not None and (record.profile, record.conns) in plan
    )


def stage_time_estimate(args, ctx):
    seconds_per_run = parse_duration_seconds(args.duration) + RUN_OVERHEAD_SECONDS
    planned = len(ctx.groups) * 2 * args.pairs
    completed = count_completed_invocations(args, ctx)
    remaining = max(planned - completed, 0)
    ctx.planned_invocations = planned
    ctx.estimated_seconds = remaining * seconds_per_run

    note(f"预期档数：{len(ctx.groups)} 档（profile × 连接数），共 {planned} 次 benchmark.sh 调用")
    tail = f"，已扣掉 --resume 会跳过的 {completed} 次" if completed else ""
    note(
        f"预估总时长：约 {format_duration(ctx.estimated_seconds)}"
        f"（{len(ctx.groups)} 档 × 2 侧 × {args.pairs} 对 × ({args.duration} + {RUN_OVERHEAD_SECONDS}s){tail}）"
    )
    if len(ctx.groups) > 1:
        note(
            "不含档间冷却（大连接数档 benchmark.sh 会 sleep 15s）和镜像构建，"
            f"跑完第 1 档会用实测值重算一次（现在是按每次 {RUN_OVERHEAD_SECONDS}s 估的）"
        )
    if not ctx.ws_mode and any(name in POSTGRES_PROFILES for name in ctx.profile_names):
        note("这批档位要起 postgres sidecar，每条命令会多几秒")


# ── 阶段 2：准备 HttpArena ─────────────────────────────────────────────────

def stage_prepare_httparena(args, ctx):
    stage_header(2, "准备 HttpArena")

    httparena = ctx.httparena
    if not httparena.is_dir():
        raise BenchError(
            f"HttpArena 目录不存在：{httparena}\n"
            "        用 --httparena 指到 clone 下来的仓库根目录，"
            "比如 --httparena ~/HttpArena"
        )
    driver = httparena / "scripts" / "benchmark.sh"
    if not driver.is_file():
        raise BenchError(
            f"找不到入口脚本：{driver}\n"
            "        目录可能不是 HttpArena 根目录，或者仓库不完整，先 git pull 一下"
        )
    note(f"HttpArena：{httparena}")

    log = run_command(
        ["git", "-C", str(httparena), "log", "--oneline", "-1"],
        quiet=True,
        dry_run=ctx.dry_run,
    )
    if ctx.dry_run:
        ctx.httparena_commit = "（dry-run 未取）"
    elif log.returncode != 0:
        ctx.httparena_commit = "（取不到，不是 git 仓库？）"
    else:
        ctx.httparena_commit = log.output.strip() or "（空）"
    note(f"HttpArena commit：{ctx.httparena_commit}")


# ── 阶段 3：造基线 / 待验证两份 framework ───────────────────────────────────

def framework_dir(ctx, side):
    return ctx.httparena / "frameworks" / SIDE_FRAMEWORK[side]


def build_script_text(ref, image_name):
    return (
        "#!/usr/bin/env bash\n"
        "set -euo pipefail\n"
        'cd "$(dirname "$0")"\n'
        f"docker build --build-arg HICAL_REF={ref} -t {image_name} .\n"
    )


def ensure_framework(name, ref, ctx):
    frameworks_dir = ctx.httparena / "frameworks"
    source = frameworks_dir / "hical"
    target = frameworks_dir / name
    image_name = f"httparena-{name}"

    if target.exists():
        note(f"{name} 已存在，跳过复制（不覆盖已有改动）")
    else:
        if ctx.dry_run:
            note(f"会从 {source} 复制到 {target}")
        else:
            if not source.is_dir():
                raise BenchError(
                    f"源 framework 不存在：{source}\n"
                    "        HttpArena 里没有 frameworks/hical，先确认仓库版本"
                )
            shutil.copytree(source, target)
            note(f"已复制 {source.name} -> {name}")

    build_script = target / "build.sh"
    if ctx.dry_run:
        note(f"会写 build.sh（HICAL_REF={ref}，镜像 {image_name}）")
    else:
        build_script.write_text(build_script_text(ref, image_name), encoding="utf-8")
        os.chmod(build_script, 0o755)
        note(f"build.sh 已就绪（HICAL_REF={ref}）")

    meta_path = target / "meta.json"
    # tests 写成本次真要跑的档，不原样照抄。benchmark.sh 扫这个列表决定起不起
    # postgres sidecar，照抄的话即使只跑 baseline，也会因为列表里有 async-db 去拉
    # postgres:18——离线机器直接失败，而且失败点在压测之前，报出来很像性能问题。
    tests = ["echo-ws"] if ctx.ws_mode else list(ctx.profile_names)
    if ctx.dry_run:
        if ctx.ws_mode:
            note(f"会把 meta.json 的 display_name 改成 {name}，tests 精简成只有 echo-ws")
        else:
            note(
                f"会把 meta.json 的 display_name 改成 {name}，"
                f"tests 只留本次要跑的 {len(tests)} 个：{', '.join(tests)}"
            )
        return target, image_name

    if not meta_path.is_file():
        raise BenchError(f"{name} 缺 meta.json，复制出来的是不是不完整？")
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    meta["display_name"] = name
    meta["tests"] = tests
    meta_path.write_text(
        json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    if ctx.ws_mode:
        note(f"meta.json display_name -> {name}，tests -> {tests}（不触发 postgres）")
    else:
        note(
            f"meta.json display_name -> {name}，tests -> {len(tests)} 个本次要跑的档"
            "（列表里没有 DB 档就不起 postgres）"
        )

    return target, image_name


def stage_build_frameworks(args, ctx):
    stage_header(3, "造基线 / 待验证两份 framework")

    target_baseline, image_baseline = ensure_framework(
        SIDE_FRAMEWORK[SIDE_BASELINE], args.baseline, ctx
    )
    target_candidate, image_candidate = ensure_framework(
        SIDE_FRAMEWORK[SIDE_CANDIDATE], args.candidate, ctx
    )

    if ctx.dry_run:
        note(f"会执行 {image_baseline} 的 build.sh 和 {image_candidate} 的 build.sh")
        ctx.image_baseline = "（dry-run 未取）"
        ctx.image_candidate = "（dry-run 未取）"
        return

    for script_dir in (target_baseline, target_candidate):
        script = script_dir / "build.sh"
        note(f"构建镜像（{script_dir.name}），首次会很慢……")
        result = run_command(["bash", str(script)], cwd=script_dir)
        if result.returncode != 0:
            raise BenchError(
                f"{script_dir.name} 镜像构建失败（returncode={result.returncode}）。\n"
                f"        单独跑一下 `bash {script}` 看完整报错，"
                "常见是 HICAL_REF 拉不到（网络/仓库权限）或编译失败"
            )

    ctx.image_baseline = docker_image_id(image_baseline, ctx.dry_run)
    ctx.image_candidate = docker_image_id(image_candidate, ctx.dry_run)
    note(f"基线镜像 id：{short_id(ctx.image_baseline)}")
    note(f"待验证镜像 id：{short_id(ctx.image_candidate)}")


def docker_image_id(image, dry_run):
    if dry_run:
        return "（dry-run 未取）"
    result = run_command(
        ["docker", "image", "inspect", "--format", "{{.Id}}", image], quiet=True
    )
    if result.returncode != 0:
        return "（取不到）"
    return result.output.strip() or "（空）"


def docker_image_exists(image, dry_run):
    if dry_run:
        return False
    result = run_command(["docker", "image", "inspect", image], quiet=True)
    return result.returncode == 0


def short_id(image_id):
    if image_id.startswith("sha256:"):
        return image_id[7:19]
    return image_id[:12]


# ── 阶段 4：打 profiles.sh 补丁 + 预构建 gcannon ───────────────────────────

def restore_file(backup, target):
    try:
        shutil.copy2(backup, target)
        print(f"      profiles.sh 已从备份恢复（{target.name}）", flush=True)
    except OSError as exc:
        print(f"      [警告] 恢复 profiles.sh 失败：{exc}", flush=True)


def stage_patch_profiles(args, ctx):
    stage_header(4, "打 profiles.sh 补丁 + 预构建 gcannon")

    profiles = profiles_path(ctx)
    backup = profiles.parent / ".profiles.sh.perf-regress.bak"

    if ctx.dry_run:
        if not profiles.is_file():
            raise BenchError(
                f"找不到 {profiles}，HttpArena 目录结构不对，先确认仓库完整"
            )
        note(f"会备份 {profiles} -> {backup.name}（已存在则复用，不覆盖原件）")
        table = read_profile_table(profiles)
        for name in ctx.profile_names:
            fields = list(table[name])
            fields[COL_CPUS] = cpuset_for(args, ctx, name)
            if ctx.ws_mode:
                fields[COL_CONNS] = str(args.conns)
            note(f'会把 [{name}] 改成 "{"|".join(fields)}"')
        if not ctx.ws_mode:
            note("每次调用前会把当前档的 connections 列写成单值，让一次 benchmark.sh 只跑一档")
        note("会注册 atexit，脚本退出时把 profiles.sh 恢复回去")
    else:
        if not profiles.is_file():
            raise BenchError(
                f"找不到 {profiles}，HttpArena 目录结构不对，先确认仓库完整"
            )
        if backup.exists():
            note(f"备份已存在，沿用最早的原始件：{backup.name}")
        else:
            shutil.copy2(profiles, backup)
            note(f"已备份原始 profiles.sh -> {backup.name}")
        # 只要有备份就挂上恢复钩子，不管后面哪步炸了都能还原
        atexit.register(restore_file, backup, profiles)

        for name in ctx.profile_names:
            cpus = cpuset_for(args, ctx, name)
            columns = {COL_CPUS: cpus}
            if ctx.ws_mode:
                columns[COL_CONNS] = str(args.conns)
            fields = apply_profile_columns(ctx, name, columns)
            note(f'已改 [{name}] -> "{"|".join(fields)}"')

            # 改完再读一遍核对，别信内存里的字符串
            verify = read_profile_table(profiles).get(name)
            if not verify or verify[COL_CPUS] != cpus:
                raise BenchError(
                    f"profiles.sh 补丁没生效（重读 [{name}] 的 cpuset 还是 {verify}）。\n"
                    f"        手动检查 {profiles} 里的 [{name}] 行"
                )
        note(
            f"server cpuset：{ctx.server_cpus}；gcannon 拿 {ctx.gcannon_cpus}"
            "（由 common.sh 按 nproc 自算，两边不重核）"
        )
        if any(cpuset_for(args, ctx, name) != ctx.server_cpus for name in ctx.profile_names):
            note(f"{LATENCY_8CPU_PROFILE} 保留 4 核语义（{LATENCY_8CPU_CPUS}），不按前半段覆盖")

    # gcannon 镜像先手工建好，把可能的 DNS 问题挡在压测之外
    if docker_image_exists("gcannon:latest", ctx.dry_run):
        note("gcannon:latest 已存在，跳过构建")
    else:
        note("构建 gcannon:latest ……")
        result = run_command(
            [
                "docker",
                "build",
                "-t",
                "gcannon:latest",
                "-f",
                "docker/gcannon.Dockerfile",
                "docker/",
            ],
            cwd=ctx.httparena,
            dry_run=ctx.dry_run,
        )
        if not ctx.dry_run and result.returncode != 0:
            raise BenchError(
                "gcannon 镜像构建失败（returncode="
                f"{result.returncode}）。\n"
                "        多半是 docker/ 里 git clone 或 apt 拉不到东西，"
                "先 `docker build -t gcannon:latest -f docker/gcannon.Dockerfile docker/` 单独看报错"
            )


# ── 压测执行 + 解析 ─────────────────────────────────────────────────────────

def build_bench_env(args, *, skip_tune):
    env = dict(os.environ)
    env["DURATION"] = args.duration
    env["RUNS"] = "1"
    env["THREADS"] = str(args.threads)
    env["LOADGEN_DOCKER"] = "true"
    if skip_tune:
        env["SKIP_TUNE"] = "true"
    else:
        env.pop("SKIP_TUNE", None)
    # GCANNON_CPUS 故意不设，让 common.sh 按 nproc 自动算后半段核
    return env


def parse_best_frames(output):
    if not output:
        return None
    clean = ANSI_ESCAPE.sub("", output)
    match = BEST_LINE.search(clean)
    if not match:
        return None
    value = int(match.group(1))
    return value if value > 0 else None


def parse_fixed_rate(output):
    """
    固定速率档的真指标：`=== Fixed-rate: cpu/req 1.23us | p99 ... | rate_ratio 0.99 ===`。
    这行的 rps 是压测机自己的给定值，基线 / 待验证 比它没意义，比的是每请求 CPU 成本。
    """
    if not output:
        return None, ""
    clean = ANSI_ESCAPE.sub("", output)
    match = FIXED_RATE_LINE.search(clean)
    if not match:
        return None, ""
    raw_us, rate_ratio = match.group(1), match.group(4)
    try:
        us_per_req = float(raw_us.rstrip("us"))
    except ValueError:
        us_per_req = None
    return us_per_req, "" if rate_ratio in ("n/a", "") else rate_ratio


def log_name_for(ctx, profile, conns, pair_index, position, side):
    if ctx.ws_mode:
        return f"p{pair_index}-{position}-{side}.log"
    return f"{profile}-{conns}-p{pair_index}-{position}-{side}.log"


def smoke_log_name(ctx, profile, conns, side):
    if ctx.ws_mode:
        return f"smoke-{side}.log"
    return f"smoke-{profile}-{conns}-{side}.log"


def execute_benchmark(ctx, args, side, profile, conns, *, log_path, skip_tune):
    """跑一次 benchmark.sh，只跑一个 (profile, conns)。返回 (result, frames, us_per_req, rate_ratio)。"""
    if not ctx.ws_mode:
        # benchmark.sh 会把 profile 里 connections 列的所有档一次跑完，日志里就有多个
        # `=== Best:`，解析只能拿到第一个，档位就串了。所以先把它写成单值。
        apply_profile_columns(ctx, profile, {COL_CONNS: str(conns)})
    env = build_bench_env(args, skip_tune=skip_tune)
    command = [
        "bash",
        str(ctx.httparena / "scripts" / "benchmark.sh"),
        SIDE_FRAMEWORK[side],
        profile,
    ]
    result = run_command(
        command,
        cwd=ctx.httparena,
        env=env,
        dry_run=ctx.dry_run,
        log_path=log_path,
    )
    frames = parse_best_frames(result.output)
    us_per_req, rate_ratio = parse_fixed_rate(result.output)
    return result, frames, us_per_req, rate_ratio


# ── 阶段 5：冒烟 ────────────────────────────────────────────────────────────

def stage_smoke(args, ctx):
    stage_header(5, "冒烟")

    profile, conns = ctx.groups[0].profile, ctx.groups[0].conns

    if ctx.dry_run:
        note("会给基线跑一次（带 system_tune），再给待验证跑一次（SKIP_TUNE=true）")
        if not ctx.ws_mode:
            note(f"冒烟用第一档：{profile} / {conns} 连接")
        note(f"日志写到 {args.output_dir_resolved}（dry-run 不落盘）")
        note("配对前会无条件比对两个镜像 ID，一样就地报错（防两边压的是同一份代码）")
        return

    output_dir = args.output_dir_resolved

    note("第一次跑基线，不带 SKIP_TUNE，会改内核参数 + 重启 docker，慢……")
    _, frames_baseline, _, _ = execute_benchmark(
        ctx,
        args,
        SIDE_BASELINE,
        profile,
        conns,
        log_path=output_dir / smoke_log_name(ctx, profile, conns, SIDE_BASELINE),
        skip_tune=False,
    )
    if not frames_baseline:
        raise BenchError(
            "基线冒烟没解析出非 0 的 frames/s。\n"
            f"        看 {output_dir / smoke_log_name(ctx, profile, conns, SIDE_BASELINE)} "
            "里 '=== Best:' 附近，"
            "以及 status_2xx 相关的行，确认服务端真的回了 2xx"
        )
    note(f"基线冒烟：{frames_baseline} {headline_unit(ctx, profile)}")

    note("再跑待验证，带 SKIP_TUNE=true")
    _, frames_candidate, _, _ = execute_benchmark(
        ctx,
        args,
        SIDE_CANDIDATE,
        profile,
        conns,
        log_path=output_dir / smoke_log_name(ctx, profile, conns, SIDE_CANDIDATE),
        skip_tune=True,
    )
    if not frames_candidate:
        raise BenchError(
            "待验证冒烟没解析出非 0 的 frames/s。\n"
            f"        看 {output_dir / smoke_log_name(ctx, profile, conns, SIDE_CANDIDATE)} "
            "里 '=== Best:' 附近，"
            "以及 status_2xx 相关的行，确认服务端真的回了 2xx"
        )
    note(f"待验证冒烟：{frames_candidate} {headline_unit(ctx, profile)}")


def assert_images_differ(ctx):
    """
    配对前必过的一关：两个镜像 ID 不能相同。
    两边压的是同一份代码时，后面所有数据都是自欺欺人，而且结论会完全反过来。
    所以这步单独拎出来，不管跑不跑冒烟都无条件调用。
    """
    if ctx.dry_run:
        note("dry-run：跳过镜像 ID 比对")
        return

    ctx.image_baseline = docker_image_id(
        f"httparena-{SIDE_FRAMEWORK[SIDE_BASELINE]}", ctx.dry_run
    )
    ctx.image_candidate = docker_image_id(
        f"httparena-{SIDE_FRAMEWORK[SIDE_CANDIDATE]}", ctx.dry_run
    )
    if ctx.image_baseline == ctx.image_candidate:
        raise BenchError(
            "两个镜像 ID 完全一样，说明基线 / 待验证的 HICAL_REF 没生效，"
            "压的根本是同一份代码，继续跑没意义。\n"
            f"        基线镜像 id：{ctx.image_baseline}\n"
            f"        待验证镜像 id：{ctx.image_candidate}\n"
            f"        查一下 frameworks/{SIDE_FRAMEWORK[SIDE_BASELINE]}/build.sh 和 "
            f"frameworks/{SIDE_FRAMEWORK[SIDE_CANDIDATE]}/build.sh 里的 HICAL_REF "
            "是不是分别写着两个 sha；如果 build.sh 是对的，"
            "多半是 docker build 缓存吃了旧层，加 --no-cache 重来"
        )
    note(
        "镜像 ID 不同，两边确实是两份代码"
        f"（基线={short_id(ctx.image_baseline)} 待验证={short_id(ctx.image_candidate)}）"
    )


# ── runs.jsonl：断点续跑的账本 ──────────────────────────────────────────────

def runs_path(args):
    return args.output_dir_resolved / RUNS_FILE


def append_run_record(args, record):
    """每完成一侧就落一行。全档要跑近两小时，中途断了不能白跑。"""
    path = runs_path(args)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(record.to_json() + "\n")
        handle.flush()


def normalize_side(raw):
    """
    side 只有「基线 / 待验证」两种，不认的值直接报错——
    硬塞进报告只会把判定算到别的档上。
    """
    side = str(raw)
    if side not in SIDE_LABELS:
        raise ValueError(
            f"side 只能是 {SIDE_BASELINE} / {SIDE_CANDIDATE}，收到 '{raw}'"
        )
    return side


def load_run_records(path):
    if not path.is_file():
        return []
    records = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        try:
            payload = json.loads(line)
        except json.JSONDecodeError as exc:
            raise BenchError(
                f"{path} 第 {lineno} 行不是合法 JSON（{exc}）。\n"
                "        这个文件是脚本自己追加的，损坏多半是上次被 kill 在写一半。\n"
                f"        下一步：删掉坏的那行接着跑（--resume 会跳过已完成组合），"
                f"或者换 --output-dir 重开一轮"
            ) from exc
        try:
            records.append(
                RunRecord(
                    profile=str(payload["profile"]),
                    conns=int(payload["conns"]),
                    pair=int(payload["pair"]),
                    side=normalize_side(payload["side"]),
                    frames=int(payload["frames"]) if payload.get("frames") else None,
                    returncode=int(payload.get("returncode") or 0),
                    log_file=str(payload.get("log_file") or ""),
                    timestamp=str(payload.get("timestamp") or ""),
                    us_per_req=payload.get("us_per_req"),
                    rate_ratio=str(payload.get("rate_ratio") or ""),
                )
            )
        except (KeyError, TypeError, ValueError) as exc:
            raise BenchError(
                f"{path} 第 {lineno} 行缺字段或者类型不对（{exc}）。\n"
                f"        这一行原文：{line[:200]}\n"
                "        下一步：修掉或删掉这一行，--resume 会从剩下的记录接着跑"
            ) from exc
    return records


def dedupe_records(records):
    """
    同一个 (profile, conns, pair, side) 出现多次时取最后一条。
    续跑会把当时没出数的那一侧重跑一遍，于是同一个键在文件里有两行，报告只认新的。
    """
    latest = {}
    for record in records:
        latest[record.key()] = record
    return list(latest.values())


def prepare_run_state(args, ctx):
    """
    决定这次是续跑还是从头跑，返回已完成的 {(profile, conns, pair, side): record}。
    从头跑的时候把 runs.jsonl 清空 —— 不清的话报告会把上一轮的记录也算进来。
    """
    path = runs_path(args)
    if not args.resume:
        if path.exists():
            if ctx.dry_run:
                note(f"dry-run：会覆盖已有的 {RUNS_FILE}")
            else:
                existing = load_run_records(path)
                if existing:
                    warn(
                        f"输出目录里已有 {RUNS_FILE}（{len(existing)} 条记录），本次从头写。\n"
                        "        要接着上次跑请加 --resume"
                    )
                path.write_text("", encoding="utf-8")
        return {}

    records = load_run_records(path)
    ctx.resumed_count = len(records)
    if not records:
        note(f"--resume：{RUNS_FILE} 里还没有完成的组合，按全新跑")
        return {}
    # 没出数的那一侧不算「完成」，这次重跑——将近两小时的回归，别让一次抖动
    # 永久缺一个配对。重跑成功会追加新行，报告取最新那条
    done = {}
    retry = 0
    for record in records:
        if record.frames is None:
            retry += 1
            continue
        done[record.key()] = record
    note(f"--resume：{RUNS_FILE} 里有 {len(records)} 条记录，其中 {len(done)} 个已完成组合会跳过")
    if retry:
        note(f"--resume：{retry} 条记录当时没出数，这次重跑")
    ctx.report_notes.append(
        f"本次用 --resume 接着跑，从 {RUNS_FILE} 里继承了 {len(records)} 条记录"
    )
    if retry:
        ctx.report_notes.append(f"其中 {retry} 条当时没出数，本轮重跑过")
    return done


# ── 阶段 6：奇偶轮交替配对 ─────────────────────────────────────────────────

def stage_run_matrix(args, ctx):
    title = (
        f"交替配对（{args.pairs} 轮）"
        if ctx.ws_mode
        else f"交替配对（{len(ctx.groups)} 档 × {args.pairs} 轮）"
    )
    stage_header(6, title)

    output_dir = args.output_dir_resolved
    done = ctx.resumed
    skipped = 0
    executed = 0
    planned = ctx.planned_invocations or (len(ctx.groups) * 2 * args.pairs)
    calibrated = False

    for index, group in enumerate(ctx.groups, start=1):
        if not ctx.ws_mode:
            note(f"—— 档 {index}/{len(ctx.groups)}：{group.profile} / {group.conns} 连接 ——")
        unit = headline_unit(ctx, group.profile)
        group_started = time.monotonic()
        group_runs = 0

        for pair_index in range(1, args.pairs + 1):
            # 奇数轮基线先跑、偶数轮待验证先跑，顺序残留被配对内部抵消掉
            order = (
                (SIDE_BASELINE, SIDE_CANDIDATE)
                if pair_index % 2 == 1
                else (SIDE_CANDIDATE, SIDE_BASELINE)
            )
            note(f"第 {pair_index} 轮，顺序 {' → '.join(SIDE_LABELS[s] for s in order)}")
            pair_values = {}

            for position, side in enumerate(order, start=1):
                key = (group.profile, group.conns, pair_index, side)
                if key in done:
                    skipped += 1
                    note(
                        f"  [跳过] {group.profile}/{group.conns}/"
                        f"第{pair_index}对/{SIDE_LABELS[side]}"
                    )
                    pair_values[side] = done[key].frames
                    continue

                log_path = output_dir / log_name_for(
                    ctx, group.profile, group.conns, pair_index, position, side
                )
                result, frames, us_per_req, rate_ratio = execute_benchmark(
                    ctx,
                    args,
                    side,
                    group.profile,
                    group.conns,
                    log_path=log_path,
                    skip_tune=True,
                )
                if not ctx.dry_run:
                    append_run_record(
                        args,
                        RunRecord(
                            profile=group.profile,
                            conns=group.conns,
                            pair=pair_index,
                            side=side,
                            frames=frames,
                            returncode=result.returncode,
                            log_file=log_path.name,
                            timestamp=datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                            us_per_req=us_per_req,
                            rate_ratio=rate_ratio,
                        ),
                    )
                pair_values[side] = frames
                executed += 1
                group_runs += 1

                if frames:
                    note(f"  {SIDE_LABELS[side]} = {frames} {unit}")
                elif ctx.dry_run:
                    note(f"  {SIDE_LABELS[side]} = （dry-run 跳过）")
                else:
                    note(
                        f"  {SIDE_LABELS[side]} = 没出数（returncode={result.returncode}），"
                        "这一侧记进报告，配对时会被排除"
                    )

            baseline_value = pair_values.get(SIDE_BASELINE)
            candidate_value = pair_values.get(SIDE_CANDIDATE)
            if not ctx.dry_run and baseline_value and candidate_value:
                diff = (candidate_value - baseline_value) / baseline_value * 100
                if ctx.ws_mode:
                    note(f"  本轮差值：{diff:+.2f}%（待验证相对基线）")
                else:
                    note(f"  本轮差值：{diff:+.2f}%（待验证相对基线，单位 {unit}）")

        # 跑完第一档就有实测单次耗时了，按它重算剩余时间。25s 那个常数是为了让人
        # 开跑前心里有数，实测偏差大的时候（postgres、大连接数冷却、镜像缓存）
        # 得让人早点知道今晚还要等多久，别等两小时后发现才跑完三分之一
        if not ctx.dry_run and not calibrated and group_runs:
            calibrated = True
            per_run = (time.monotonic() - group_started) / group_runs
            left = max(planned - skipped - executed, 0)
            note(
                f"第 {index} 档实测 {per_run:.0f}s/次，剩余 {left} 次，"
                f"预估还需 {format_duration(left * per_run)}"
                f"（开跑时按 {RUN_OVERHEAD_SECONDS}s/次 估的是 {format_duration(ctx.estimated_seconds)}）"
            )

    if skipped:
        note(f"--resume 跳过 {skipped} 个已完成组合（每侧算一个）")
        ctx.report_notes.append(f"--resume 跳过 {skipped} 个已完成组合（每侧算一个）")


# ── 阶段 7：生成报告 ────────────────────────────────────────────────────────

def pick_value(record, kind):
    if record is None or record.frames is None:
        return None
    if kind.key == METRIC_CPU_PER_REQ.key:
        return record.us_per_req
    return record.frames


def throughput_of(record):
    if record is None or record.frames is None:
        return None
    return record.frames


def collect_group(ctx, group, records):
    kind = metric_for(ctx, group.profile)
    notes = []

    mine = [r for r in records if r.profile == group.profile and r.conns == group.conns]
    if not mine:
        notes.append("这一档还没有任何记录（续跑没跑到，或者压根没开始跑）")
    pair_ids = sorted({r.pair for r in mine})
    rows = []
    for pair in pair_ids:
        sides = {r.side: r for r in mine if r.pair == pair}
        rows.append((pair, sides.get(SIDE_BASELINE), sides.get(SIDE_CANDIDATE)))

    valid = [(pick_value(a, kind), pick_value(b, kind)) for _, a, b in rows]
    pairs_valid = [(a, b) for a, b in valid if a is not None and b is not None]

    if kind.key == METRIC_CPU_PER_REQ.key and not pairs_valid:
        # 日志里没有 `=== Fixed-rate:` 行（多半是 cgroup 读不到）时就没有 us/req。
        # 退回吞吐至少还能看出「掉没掉 0」
        notes.append(
            "没解析到每请求 CPU 成本（日志里没有 `=== Fixed-rate:` 行，多半是没读到 cgroup），"
            "这一档退回按吞吐比，仅供参考"
        )
        kind = METRIC_THROUGHPUT
        valid = [(throughput_of(a), throughput_of(b)) for _, a, b in rows]
        pairs_valid = [(a, b) for a, b in valid if a is not None and b is not None]

    diffs = [(b - a) / a * 100 for a, b in pairs_valid if a]
    positives = sum(1 for d in diffs if d > 0)
    negatives = sum(1 for d in diffs if d < 0)
    zeros = sum(1 for d in diffs if d == 0)
    same_sign = bool(diffs) and zeros == 0 and (positives == len(diffs) or negatives == len(diffs))

    median = statistics.median(diffs) if diffs else None
    spread = (max(diffs) - min(diffs)) if diffs else None
    # 分辨力：中位数比离散度还小的时候，这个实验压根测不出这个量级的差异，
    # 「方向不一致」只是噪声的另一种说法，不能读成「差异不存在」
    resolution_ok = bool(diffs) and median != 0 and abs(median) >= spread / 2
    verdict, conclusion = verdict_text(diffs, same_sign, kind)

    return GroupStats(
        profile=group.profile,
        conns=group.conns,
        kind=kind,
        rows=rows,
        valid=pairs_valid,
        diffs=diffs,
        median=median,
        spread=spread,
        resolution_ok=resolution_ok,
        same_sign=same_sign,
        verdict=verdict,
        conclusion=conclusion,
        notes=notes,
    )


def is_better(median, kind):
    if median is None or median == 0:
        return False
    return median < 0 if kind.lower_is_better else median > 0


def direction_word(median, kind):
    if kind.lower_is_better:
        return "省" if median < 0 else "费"
    return "快" if median > 0 else "慢"


def verdict_text(diffs, same_sign, kind):
    n = len(diffs)
    if n == 0:
        return "证据不足：没有任何有效配对。", "没有测到有效数据，下不了结论。"
    if n < 3:
        return (
            f"证据不足：只有 {n} 对有效数据，至少需要 3 对。",
            f"只有 {n} 对有效数据，配对太少，先别下结论。",
        )
    median = statistics.median(diffs)
    magnitude = abs(median)
    word = direction_word(median, kind)
    if same_sign and n >= 6:
        verdict = "显著（符号检验 p≈0.031）"
        tag = "提升是真的" if is_better(median, kind) else "回退是真的"
        return verdict, f"待验证比基线 {word} {magnitude:.2f}%，{n} 对全部同号，{tag}。"
    if same_sign:
        return (
            "初步信号，方向一致",
            f"待验证比基线 {word} {magnitude:.2f}%，{n} 对方向一致，初步信号，建议再跑几对确认。",
        )
    return (
        "方向不一致，回退不成立",
        "差值有正有负，方向不一致，差异多半是噪声，回退不成立。",
    )


def format_percent(value):
    return f"{value:+.2f}%"


def format_span(value):
    """离散度是个宽度，带正负号只会让人多看一眼。"""
    return f"{value:.2f}%"


def format_metric(value, kind):
    if value is None:
        return "—"
    if kind.key == METRIC_CPU_PER_REQ.key:
        return f"{value:.3f}"
    return f"{value:,.0f}"


def read_base_image(dockerfile_path):
    """从 Dockerfile 里抠出第一条 FROM 的镜像名，别在报告里硬编码基座。"""
    try:
        text = dockerfile_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = re.search(r"^FROM\s+(.+)$", text, re.MULTILINE)
    if not match:
        return None
    image = match.group(1).strip()
    # 去掉多阶段构建的 `AS <stage>` 尾巴
    image = re.split(r"\s+[Aa][Ss]\s+", image, maxsplit=1)[0].strip()
    return image or None


def median_of(values):
    values = [v for v in values if v is not None]
    return statistics.median(values) if values else None


def build_summary_table(stats_list):
    lines = [
        "## 2. 汇总",
        "",
        "| Profile | 连接数 | 基线中位数 | 待验证中位数 | 配对差值中位数 | 方向 | 判定 |",
        "|---------|-------:|-----------:|-------------:|---------------:|------|------|",
    ]
    for stats in stats_list:
        baseline_values = [value for value, _ in stats.valid]
        candidate_values = [value for _, value in stats.valid]
        diff_text = format_percent(stats.median) if stats.median is not None else "—"
        lines.append(
            "| "
            + " | ".join(
                [
                    stats.profile,
                    str(stats.conns),
                    format_metric(median_of(baseline_values), stats.kind),
                    format_metric(median_of(candidate_values), stats.kind),
                    diff_text,
                    stats.direction_text,
                    stats.verdict,
                ]
            )
            + " |"
        )
    lines.append("")
    # 同一个 profile 可能有好几档，口径只说明一次
    seen_profiles = set()
    lines.append("单位与口径（不同 profile「好」的方向不一样，看报告前先对一遍）：")
    lines.append("")
    for stats in stats_list:
        if stats.profile in seen_profiles:
            continue
        seen_profiles.add(stats.profile)
        lines.append(f"- {stats.profile}：{stats.kind.unit}，{stats.kind.label}")
    lines.append("")
    return lines


def build_group_section(stats):
    lines = []
    lines.append(f"### {stats.profile} / {stats.conns} 连接")
    lines.append("")
    lines.append(f"比较口径：{stats.kind.label}")
    lines.append("")
    for item in stats.notes:
        lines.append(f"- 注意：{item}")
    if stats.notes:
        lines.append("")
    lines.append(
        f"| 轮次 | 基线 ({stats.kind.unit}) | 待验证 ({stats.kind.unit}) | 差值 (待验证相对基线) |"
    )
    lines.append("|------|------------------:|--------------------:|--------------------:|")
    for pair, baseline_record, candidate_record in stats.rows:
        baseline = pick_value(baseline_record, stats.kind)
        candidate = pick_value(candidate_record, stats.kind)
        if baseline is not None and candidate is not None and baseline:
            diff_text = format_percent((candidate - baseline) / baseline * 100)
        else:
            diff_text = "—"
        lines.append(
            f"| {pair} | {format_metric(baseline, stats.kind)} | "
            f"{format_metric(candidate, stats.kind)} | {diff_text} |"
        )
    lines.append("")

    total_pairs = len(stats.rows)
    lines.append(f"- 有效配对：{len(stats.valid)} / {total_pairs}")
    if stats.diffs:
        positives = sum(1 for d in stats.diffs if d > 0)
        negatives = sum(1 for d in stats.diffs if d < 0)
        zeros = sum(1 for d in stats.diffs if d == 0)
        lines.append(f"- 方向分布：正向 {positives} 对 / 负向 {negatives} 对")
        if zeros:
            lines.append(f"- 零差值：{zeros} 对")
        lines.append(f"- 差值中位数：{format_percent(stats.median)}")
        lines.append(
            f"- 差值范围：{format_percent(min(stats.diffs))} ~ {format_percent(max(stats.diffs))}"
            f"（离散度 {stats.spread:.2f} 个百分点）"
        )
        if stats.resolution_ok:
            lines.append(f"- 分辨力：足够（离散度 {format_span(stats.spread)}）")
        else:
            ratio = (
                f"，是差值中位数的 {stats.spread / abs(stats.median):.1f} 倍"
                if stats.median
                else ""
            )
            lines.append(
                f"- 分辨力：**不足**（离散度 {format_span(stats.spread)}{ratio}）"
            )
            lines.append(
                "  - 结论只能说到「在这个环境里测不出这个量级的差异」，"
                "不能说成「差异不存在」。要下更强的结论，得降噪声或拉长时长"
            )
    else:
        lines.append("- 方向分布：无数据")
        lines.append("- 差值中位数：无数据")
        lines.append("- 差值范围：无数据")
        lines.append("- 分辨力：无数据")
    lines.append(f"- 判定：{stats.verdict}")
    lines.append(f"- 一句话：{stats.conclusion}")
    lines.append("")
    return lines


def build_final_verdict(stats_list):
    lines = ["## 4. 总判定", ""]
    consistent = [
        stats
        for stats in stats_list
        if stats.same_sign and stats.median and len(stats.valid) >= 3
    ]
    regressions = [stats for stats in consistent if stats.is_regression]
    improvements = [stats for stats in consistent if not stats.is_regression]

    if not consistent:
        lines.append(
            "**未发现一致方向的退化**：所有档位的配对差值方向都不一致（或不足 3 对有效数据），"
            "差异落在噪声里。"
        )
        lines.append("")
        return lines

    lines.append("方向一致且非零的档位：")
    lines.append("")
    lines.append("| Profile | 连接数 | 配对差值中位数 | 有效对 | 分辨力 | 性质 |")
    lines.append("|---------|-------:|---------------:|-------:|--------|------|")
    for stats in consistent:
        nature = "待验证更差" if stats.is_regression else "待验证更好"
        lines.append(
            "| "
            + " | ".join(
                [
                    stats.profile,
                    str(stats.conns),
                    format_percent(stats.median),
                    str(len(stats.valid)),
                    "足够" if stats.resolution_ok else "不足",
                    nature,
                ]
            )
            + " |"
        )
    lines.append("")

    hard = [stats for stats in regressions if stats.resolution_ok]
    soft = [stats for stats in regressions if not stats.resolution_ok]
    if hard:
        names = "、".join(f"{s.profile}/{s.conns}c" for s in hard)
        lines.append(
            f"**退化项，需确认**：{names} —— 这几档待验证一致更差，"
            "且离散度小于差值中位数的两倍，"
            "分辨力够。发版前必须查清是不是真回退，别带着它发。"
        )
        lines.append("")
    if soft:
        names = "、".join(f"{s.profile}/{s.conns}c" for s in soft)
        lines.append(
            f"方向上是待验证更差，但分辨力不足：{names}。这只能算信号，不能当退化证据，"
            "要坐实得换更安静的机器或者拉长单次时长。"
        )
        lines.append("")
    if improvements and not regressions:
        names = "、".join(f"{s.profile}/{s.conns}c" for s in improvements)
        lines.append(f"全部方向一致的档位都是待验证更好（{names}），这批改动可以带着发。")
        lines.append("")
    elif improvements:
        names = "、".join(f"{s.profile}/{s.conns}c" for s in improvements)
        lines.append(f"另有 {len(improvements)} 档一致更快/更省：{names}。")
        lines.append("")
    return lines


def build_report(args, ctx):
    """报告一律从 runs.jsonl 生成，不认内存里的东西——续跑出来的结果和一口气跑完的一个样。"""
    records = dedupe_records(load_run_records(runs_path(args)))
    stats_list = [collect_group(ctx, group, records) for group in ctx.groups]

    foreign = [
        record
        for record in records
        if not any(
            record.profile == group.profile and record.conns == group.conns
            for group in ctx.groups
        )
    ]

    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    lines = []
    title = (
        "# WebSocket echo-ws 基线 / 待验证配对压测报告"
        if ctx.ws_mode
        else "# HttpArena 基线 / 待验证配对压测报告（发版回归）"
    )
    lines.append(title)
    lines.append("")
    lines.append(f"生成时间：{now}")
    lines.append("")
    if ctx.ws_mode:
        lines.append(
            "说明：基线是改造前的那份提交，待验证是改造后的提交；"
            "ws-echo 场景的「Best」是 frames/s，越高越好。"
        )
    else:
        lines.append(
            "说明：基线是上一个版本，待验证是这一版。每个 profile×连接数 都按奇偶轮交替配对，"
            "差值是同一对里待验证相对基线的变化百分比。"
        )
        lines.append("")
        lines.append(
            "不同 profile 比的口径不一样：WS 类是 frames/s，固定速率类（latency-*、8gbit）"
            "比的是每请求 CPU 成本 us/req（越低越好），其余是 req/s。"
        )
    lines.append("")

    # 1. 环境信息
    lines.append("## 1. 环境信息")
    lines.append("")
    lines.append("| 项目 | 值 |")
    lines.append("|------|----|")
    lines.append(f"| CPU 核数 | {ctx.cpu_count or '未知'} |")
    lines.append(f"| 虚拟化 | {ctx.virtualization} |")
    lines.append(f"| CPU governor | {ctx.cpu_governor} |")
    lines.append(f"| server cpuset | `{ctx.server_cpus}` |")
    lines.append(f"| gcannon cpuset | `{ctx.gcannon_cpus}` |")
    lines.append(f"| 内核版本 | {ctx.kernel_version} |")
    lines.append(f"| Docker 版本 | {ctx.docker_version} |")
    lines.append(f"| HttpArena commit | {ctx.httparena_commit} |")
    lines.append(f"| Hical 仓库 | `{ctx.hical_repo or '（没解析，版本是原样传进来的）'}` |")
    lines.append(f"| 基线提交 | {ref_report_cell(ctx.baseline_info)} |")
    lines.append(f"| 待验证提交 | {ref_report_cell(ctx.candidate_info)} |")
    lines.append(f"| 基线镜像 ID | `{short_id(ctx.image_baseline)}` |")
    lines.append(f"| 待验证镜像 ID | `{short_id(ctx.image_candidate)}` |")
    lines.append(
        f"| profile 选择 | {ctx.profile_source}"
        f"（{len(ctx.profile_names)} 个 profile，{len(ctx.groups)} 档） |"
    )
    if ctx.ws_mode:
        lines.append(f"| 连接数 | {args.conns} |")
    else:
        lines.append("| 连接数 | 各 profile 自带，见分档详情 |")
    lines.append(f"| 单次时长 | {args.duration} |")
    lines.append(f"| gcannon 线程 | {args.threads} |")
    lines.append(f"| 配对数 | {args.pairs} |")
    lines.append("")
    if ctx.ws_mode:
        lines.append(
            "server 绑 CPU "
            f"{ctx.server_cpus}，gcannon 拿 {ctx.gcannon_cpus}，两边不重核。"
        )
        lines.append("")

    # 2. 汇总表
    lines.extend(build_summary_table(stats_list))

    # 3. 分档详情
    lines.append("## 3. 分档详情")
    lines.append("")
    for stats in stats_list:
        lines.extend(build_group_section(stats))

    # 4. 总判定
    lines.extend(build_final_verdict(stats_list))

    # 5. 失败 / 跳过说明
    lines.append("## 5. 失败 / 跳过说明")
    lines.append("")
    failed = [record for record in records if record.frames is None]
    if failed:
        for record in failed:
            lines.append(
                f"- {record.profile}/{record.conns}/第 {record.pair} 对 "
                f"{SIDE_LABELS[record.side]} 没出数"
                f"（returncode={record.returncode}，日志 {record.log_file}）"
            )
    else:
        lines.append("- 无")
    if foreign:
        lines.append(
            f"- {RUNS_FILE} 里还有 {len(foreign)} 条记录不属于本次选择的范围，没有计入上面的统计"
            "（多半是上一次用了别的 --profiles，续跑时留着没删）"
        )
    for item in ctx.report_notes:
        lines.append(f"- {item}")
    if args.skip_smoke:
        lines.append("- 本次用 --skip-smoke 跳过了冒烟那一轮负载（配对前的镜像 ID 比对照常做了）")
    lines.append("")

    # 6. 附录
    lines.append("## 6. 附录")
    lines.append("")
    lines.append("两个 sha 之外的版本信息：")
    lines.append("")
    lines.append(f"- Docker：{ctx.docker_version}")
    lines.append(f"- HttpArena commit：{ctx.httparena_commit}")
    lines.append(f"- 基线镜像完整 ID：`{ctx.image_baseline}`")
    lines.append(f"- 待验证镜像完整 ID：`{ctx.image_candidate}`")
    base_baseline = read_base_image(framework_dir(ctx, SIDE_BASELINE) / "Dockerfile")
    base_candidate = read_base_image(framework_dir(ctx, SIDE_CANDIDATE) / "Dockerfile")
    lines.append(
        f"- 基线镜像构建基座：{base_baseline or '（读不到 Dockerfile）'}"
        f"（读自 frameworks/{SIDE_FRAMEWORK[SIDE_BASELINE]}/Dockerfile）"
    )
    lines.append(
        f"- 待验证镜像构建基座：{base_candidate or '（读不到 Dockerfile）'}"
        f"（读自 frameworks/{SIDE_FRAMEWORK[SIDE_CANDIDATE]}/Dockerfile）"
    )
    lines.append("")
    lines.append("原始日志文件：")
    lines.append("")
    output_dir = args.output_dir_resolved
    log_files = sorted(p.name for p in output_dir.glob("*.log"))
    if log_files:
        for name in log_files:
            lines.append(f"- `{name}`")
    else:
        lines.append("- 无")
    lines.append("")
    lines.append(f"逐档原始数据：`{RUNS_FILE}`")
    lines.append("")

    return "\n".join(lines)


def stage_report(args, ctx):
    stage_header(7, "生成报告")

    if ctx.dry_run:
        note("dry-run：不生成报告")
        return

    report = build_report(args, ctx)
    report_path = args.output_dir_resolved / "report.md"
    report_path.write_text(report, encoding="utf-8")
    note(f"报告已写入：{report_path}")


# ── 自查 ────────────────────────────────────────────────────────────────────

def docstring_table(header_cell):
    """
    从文件头 docstring 里抠一张 markdown 表：找到表头首格是 header_cell 的那行，
    往下一直读 `|` 开头的行，跳过 |---|---| 那道分隔线。
    """
    lines = (__doc__ or "").splitlines()
    start = None
    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if cells and cells[0] == header_cell:
            start = index + 1
            break
    if start is None:
        raise BenchError(f"文件头 docstring 里找不到表头首格是「{header_cell}」的表")

    rows = []
    for line in lines[start:]:
        stripped = line.strip()
        if not stripped.startswith("|"):
            break
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if all(set(cell) <= set("-: ") for cell in cells):
            continue  # 分隔行
        rows.append(cells)
    return rows


def run_self_checks():
    """
    文件头那张预设表是使用者看到的第一手资料，也最容易跟代码漂移——
    加了预设忘了改文档，这里当场红。跑法：--self-check。
    """
    checks = 0
    failures = []

    def expect(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            failures.append(message)

    # 1) 预设表：docstring 每一行的展开结果必须等于 PROFILE_PRESETS 里的列表
    doc_presets = {}
    for row in docstring_table("预设"):
        expect(len(row) == 4, f"预设表这行列数不是 4：{row}")
        if len(row) != 4:
            continue
        doc_presets[row[0]] = row

    expected_names = set(PROFILE_PRESETS) | {WS_GROUP, SCORED_GROUP, ALL_GROUP}
    expect(
        set(doc_presets) == expected_names,
        f"预设表里的名字和代码对不上：文档多 {sorted(set(doc_presets) - expected_names)}，"
        f"文档少 {sorted(expected_names - set(doc_presets))}",
    )

    for name, profiles in PROFILE_PRESETS.items():
        if name not in doc_presets:
            continue
        listed = [item.strip() for item in doc_presets[name][1].split(",")]
        expect(
            listed == list(profiles),
            f"预设 {name} 文档写的是 {doc_presets[name][1]}，代码是 {', '.join(profiles)}",
        )

    if WS_GROUP in doc_presets:
        expect(
            doc_presets[WS_GROUP][1] == ECHO_WS_PROFILE,
            f"预设 {WS_GROUP} 应该只展开 {ECHO_WS_PROFILE}",
        )

    if SCORED_GROUP in doc_presets:
        cell = doc_presets[SCORED_GROUP][1]
        match = re.search(r"（([^）]*)）", cell)
        listed = [item.strip() for item in match.group(1).split(",")] if match else []
        expect(
            listed == list(SCORED_PROFILES),
            f"预设 {SCORED_GROUP} 那行的档名和 SCORED_PROFILES 对不上：{cell}",
        )
    if ALL_GROUP in doc_presets:
        expect(
            "meta.json" in doc_presets[ALL_GROUP][1],
            "预设表里 all 那行要说明档是从 meta.json 读的",
        )

    # 2) 默认列：预设走快跑规格（3 对 × 5s），ws / scored / all 走默认规格
    for name, row in doc_presets.items():
        match = re.fullmatch(r"(\d+)\s*对\s*×\s*(\d+)s", row[3])
        expect(bool(match), f"预设表 {name} 的默认列写法看不懂：{row[3]}")
        if not match:
            continue
        quick = name in PROFILE_PRESETS
        want_pairs = QUICK_PAIRS if quick else DEFAULT_PAIRS
        want_duration = QUICK_DURATION if quick else DEFAULT_DURATION
        expect(
            int(match.group(1)) == want_pairs and f"{match.group(2)}s" == want_duration,
            f"预设 {name} 文档默认 {row[3]}，代码是 {want_pairs} 对 × {want_duration}",
        )

    # 3) 口径表：三种口径一个不能少，方向必须和 MetricKind.lower_is_better 一致
    kinds = {kind.unit: kind for kind in (METRIC_THROUGHPUT, METRIC_WS, METRIC_CPU_PER_REQ)}
    metric_rows = docstring_table("口径")
    doc_units = [row[0] for row in metric_rows]
    expect(
        set(doc_units) == set(kinds),
        f"口径表的单位和代码对不上：文档 {doc_units}，代码 {sorted(kinds)}",
    )

    known_profiles = set(SCORED_PROFILES) | set(ZRK_ENDPOINTS) | {LATENCY_8CPU_PROFILE}
    for profiles in PROFILE_PRESETS.values():
        known_profiles |= set(profiles)
    by_unit = {}

    for row in metric_rows:
        if len(row) != 4:
            expect(False, f"口径表这行列数不是 4：{row}")
            continue
        unit, used_by, _, direction = row
        kind = kinds.get(unit)
        if kind is None:
            continue
        expect(
            ("越低越好" in direction) == kind.lower_is_better,
            f"口径 {unit} 的「越大越好？」和代码 lower_is_better={kind.lower_is_better} 对不上",
        )
        listed = [item.strip() for item in used_by.replace("、", ",").split(",") if item.strip()]
        unknown = [item for item in listed if item not in known_profiles]
        expect(not unknown, f"口径表 {unit} 列了代码里不认识的档：{', '.join(unknown)}")
        by_unit[unit] = set(listed)

    # 按 metric_for 的判据反推每个档该归哪一档：echo-ws 系列走 ws-echo 端点比帧，
    # ZRK_ENDPOINTS 那几档比 us/req（这几档的 endpoint 就是档名），剩下的都是 req/s。
    # 文档里把某个档放错行，这里就红——echo-ws-limited 就容易被人想当然归到 req/s
    ws_profiles = {name for name in known_profiles if name.startswith(ECHO_WS_PROFILE)}
    expect(
        by_unit.get(METRIC_WS.unit) == ws_profiles,
        f"口径表 frames/s 那行应该是 {sorted(ws_profiles)}，文档写的是 "
        f"{sorted(by_unit.get(METRIC_WS.unit) or [])}",
    )
    expect(
        by_unit.get(METRIC_CPU_PER_REQ.unit) == set(ZRK_ENDPOINTS),
        f"口径表 us/req 那行应该是 {sorted(ZRK_ENDPOINTS)}，文档写的是 "
        f"{sorted(by_unit.get(METRIC_CPU_PER_REQ.unit) or [])}",
    )
    rest = known_profiles - ws_profiles - set(ZRK_ENDPOINTS)
    expect(
        by_unit.get(METRIC_THROUGHPUT.unit) == rest,
        f"口径表 req/s 那行应该是 {sorted(rest)}，文档写的是 "
        f"{sorted(by_unit.get(METRIC_THROUGHPUT.unit) or [])}",
    )

    # 4) 时间成本那段得留着，否则没人知道全档要熬多久
    expect(
        "每次 benchmark.sh 调用约 25~40s" in (__doc__ or ""),
        "文件头缺时间成本那段（每次调用多少秒 / 全档多久 / --only-conns 能砍档数）",
    )

    if failures:
        print(f"\n自查 {checks} 项，失败 {len(failures)} 项：", flush=True)
        for item in failures:
            print(f"  - {item}", flush=True)
        return 1
    print(
        f"自查 {checks} 项全过：文件头的预设表、口径表、默认规格和代码一致",
        flush=True,
    )
    return 0


# ── 入口 ────────────────────────────────────────────────────────────────────

def parse_args(argv):
    parser = argparse.ArgumentParser(
        description=(
            "Hical × HttpArena 的性能回归配对压测：拿这一版和上一版比，看有没有变慢"
            "（默认单跑 echo-ws，也能跑整批 profile）"
        ),
    )
    parser.add_argument("--httparena", type=pathlib.Path, default=pathlib.Path("~/HttpArena"))
    parser.add_argument(
        "--baseline",
        default=REF_AUTO,
        help=(
            f"基线提交，默认 {REF_AUTO}：解析成最近的 tag，一个 tag 都走不到就退成 "
            f"{BASELINE_FALLBACK_REF}。显式传 tag / 分支 / sha 就原样用，不做推导"
        ),
    )
    parser.add_argument(
        "--candidate",
        default=REF_AUTO,
        help=f"待验证提交，默认 {REF_AUTO}：解析成 {CANDIDATE_AUTO_REF}。显式传就原样用",
    )
    parser.add_argument(
        "--hical-repo",
        type=pathlib.Path,
        default=None,
        help=(
            "Hical 仓库；版本现解析、推没推也靠它查。默认用脚本自己所在的仓库"
        ),
    )
    parser.add_argument(
        "--profiles",
        default=WS_GROUP,
        help=(
            "跑哪些档：ws（默认，只跑 echo-ws）、scored（发版回归 10 档）、"
            "all（读 frameworks/hical/meta.json 的 tests）、"
            "分类预设 http/json/websocket/async/db/broad/smoke（3 对 × 5s），"
            "或者逗号列表如 baseline,echo-ws。预设名只能单独用"
        ),
    )
    parser.add_argument(
        "--pairs",
        type=int,
        default=None,
        help=(
            f"配对轮数。默认 {DEFAULT_PAIRS}（分类预设和 smoke 是 {QUICK_PAIRS}）。"
            "6 对全同号才能做符号检验（p≈0.031），3 对只能看方向"
        ),
    )
    parser.add_argument(
        "--conns",
        type=int,
        default=None,
        help=(
            f"只压这一档连接数（默认 {DEFAULT_CONNS}）。只在 --profiles ws 下生效；"
            "其他模式用 profiles.sh 里各档自带的连接数列表，传了会被忽略"
        ),
    )
    parser.add_argument(
        "--only-conns",
        type=int,
        default=None,
        help="每个 profile 只保留这一档连接数，没有该档的 profile 会警告后跳过（ws 模式忽略）",
    )
    parser.add_argument(
        "--duration",
        default=None,
        help=f"单次压测时长，如 10s。默认 {DEFAULT_DURATION}（分类预设和 smoke 是 {QUICK_DURATION}）",
    )
    parser.add_argument("--threads", type=int, default=4, help="gcannon 线程数")
    parser.add_argument("--output-dir", type=pathlib.Path, default=None)
    parser.add_argument(
        "--server-cpus",
        default=None,
        help="手动指定 server 的 cpuset（默认按 nproc 派生前半段，gcannon 拿后半段）",
    )
    parser.add_argument(
        "--resume",
        action="store_true",
        help="从已有 --output-dir 续跑，跳过 runs.jsonl 里已完成的组合",
    )
    parser.add_argument("--skip-smoke", action="store_true", help="跳过冒烟，直接配对")
    parser.add_argument("--dry-run", action="store_true", help="只打印将要执行的命令")
    parser.add_argument(
        "--self-check",
        action="store_true",
        help="只跑脚本自查（文件头 docstring 的预设表 / 口径表和代码对不对得上），不压测",
    )
    args = parser.parse_args(argv)

    # pairs / duration 的默认值取决于 --profiles 是哪一类，留给 resolve_profiles 定；
    # 这里只记「用户显式传了没」，顺手校验
    if args.pairs is not None and args.pairs < 1:
        parser.error("--pairs 至少 1")
    if args.conns is not None and args.conns < 1:
        parser.error("--conns 至少 1")
    if args.only_conns is not None and args.only_conns < 1:
        parser.error("--only-conns 至少 1")
    if args.threads < 1:
        parser.error("--threads 至少 1")
    return args


def main(argv=None):
    args = parse_args(argv)
    if args.self_check:
        try:
            return run_self_checks()
        except BenchError as exc:
            print(f"\n[失败] {exc}", file=sys.stderr, flush=True)
            return 2

    args.conns_given = args.conns is not None
    args.pairs_given = args.pairs is not None
    args.duration_given = args.duration is not None
    if args.conns is None:
        args.conns = DEFAULT_CONNS

    ws_requested = (args.profiles or "").strip() == WS_GROUP
    if args.output_dir is None:
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        args.output_dir = pathlib.Path(
            f"perf-regress-ws-{stamp}" if ws_requested else f"perf-regress-{stamp}"
        )
    args.output_dir_resolved = args.output_dir.expanduser().resolve()
    if not args.dry_run:
        args.output_dir_resolved.mkdir(parents=True, exist_ok=True)

    ctx = BenchContext(
        httparena=pathlib.Path(args.httparena).expanduser().resolve(),
        dry_run=args.dry_run,
    )

    if ws_requested:
        print("Hical WebSocket echo-ws 性能回归压测（基线 / 待验证）", flush=True)
    else:
        print("Hical × HttpArena 性能回归压测（发版回归，基线 / 待验证）", flush=True)

    try:
        resolve_versions(args, ctx)
        print(f"输出目录 = {args.output_dir_resolved}", flush=True)
        if args.dry_run:
            print("（dry-run：只打印命令，不动文件、不跑 docker）", flush=True)
        resolve_profiles(args, ctx)
        if not ctx.ws_mode:
            print(f"profile 选择 = {ctx.profile_source}：{', '.join(ctx.profile_names)}", flush=True)
            print(
                f"档数 = {len(ctx.groups)}，配对数 × 时长 = {args.pairs} × {args.duration}",
                flush=True,
            )
            if ctx.quick_mode and not args.pairs_given and not args.duration_given:
                note(f"预设默认 {QUICK_PAIRS} 对 × {QUICK_DURATION}，显式传 --pairs / --duration 可覆盖")
            if args.conns_given:
                note(f"--conns 在 --profiles {ctx.profile_source} 下不生效，已忽略；连接数用各档自带的")
            if args.only_conns is not None:
                note(f"只跑 {args.only_conns} 连接这一档")
        elif args.only_conns is not None:
            note("--only-conns 在 --profiles ws 下不生效，已忽略（ws 的连接数由 --conns 决定）")
        stage_environment(args, ctx)
        stage_prepare_httparena(args, ctx)
        # 清 runs.jsonl / 读已完成记录放在 httparena 校验之后：路径给错时先报错退出，
        # 别把上一次攒了两小时的记录清掉才发现连目录都不对
        ctx.resumed = prepare_run_state(args, ctx)
        stage_build_frameworks(args, ctx)
        stage_patch_profiles(args, ctx)
        if args.skip_smoke:
            print(f"\n[5/{TOTAL_STAGES}] 冒烟（--skip-smoke，跳过）", flush=True)
        else:
            stage_smoke(args, ctx)
        # 不管跳没跳冒烟，配对前都无条件查一遍镜像 ID，别让同源镜像混进基线 / 待验证
        assert_images_differ(ctx)
        stage_run_matrix(args, ctx)
        stage_report(args, ctx)
    except BenchError as exc:
        print(f"\n[失败] {exc}", file=sys.stderr, flush=True)
        return 2

    if not args.dry_run:
        print(f"\n完成，报告在 {args.output_dir_resolved / 'report.md'}", flush=True)
        print(f"逐档原始数据在 {args.output_dir_resolved / RUNS_FILE}（断了就 --resume 接着跑）", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
