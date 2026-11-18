#!/usr/bin/env bash
# 用同一套依赖检查宏轨和原生轨，每轨都分别开启和关闭 OpenAPI。
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
work=${HICAL_CONTRACT_WORK_DIR:-"$source_dir/build-native-contract"}
mkdir -p "$work/evidence"
work=$(cd -- "$work" && pwd -P)
evidence=$(mktemp -d "$work/evidence/run.XXXXXX")
printf '契约验收证据：%s\n' "$evidence"
trap 'printf "契约验收证据：%s\n" "$evidence"' EXIT

cc=${HICAL_CC_NATIVE:-gcc-16}
cxx=${HICAL_CXX_NATIVE:-g++-16}
jobs=${HICAL_CONTRACT_JOBS:-2}
: "${HICAL_BOOST_PREFIX:?请指定用原生工具链编译的 Boost 1.90 安装目录}"
boost=$(cd -- "$HICAL_BOOST_PREFIX" && pwd -P)
boost_config="$boost/lib/cmake/Boost-1.90.0"
if [[ ! -f "$boost_config/BoostConfig.cmake" ]]; then
    printf '找不到 Boost 1.90 的 CMake 配置，查找目录是 %s\n' "$boost_config" >&2
    exit 1
fi
export GTEST_COLOR=no

log()
{
    local file=$1
    shift
    {
        printf '+'
        printf ' %q' "$@"
        printf '\n'
        "$@"
    } 2>&1 | tee "$file"
}

{
    cat /etc/os-release
    uname -a
    "$cc" --version
    "$cxx" --version
    cmake --version
    ctest --version
    ninja --version
    python3 --version
    grep '^#define BOOST_LIB_VERSION' "$boost/include/boost/version.hpp"
} 2>&1 | tee "$evidence/versions.log"

common=( -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_CXX_COMPILER=$cxx" "-DBoost_DIR=$boost_config"
    -DHICAL_BUILD_EXAMPLES=OFF -DHICAL_INSTALL_CLI=OFF
    -DHICAL_WITH_HTTP2=OFF -DHICAL_WITH_HTTP_CLIENT=OFF -DHICAL_DISABLE_IO_URING=ON )

run_regression()
{
    local build=$1 records=$2
    shift 2
    local pattern
    pattern=$(IFS='|'; printf '^(%s)[.]' "$*")
    log "$records/regression-discovery.log" ctest --test-dir "$build" -N -R "$pattern"
    ctest --test-dir "$build" --show-only=json-v1 -R "$pattern" > "$records/regression-discovery.json"
    log "$records/regression.log" ctest --test-dir "$build" -R "$pattern" \
        --no-tests=error --output-on-failure --timeout 60 --output-junit "$records/regression.xml"
    log "$records/regression-check.log" python3 "$source_dir/scripts/check_contract_ctest.py" \
        "$records/regression-discovery.json" "$records/regression.xml" "$@"
}

run_track()
{
    local mode=$1 openapi=$2 reflection=OFF
    local name="$mode-$openapi" openapi_option=${openapi^^}
    local build="$work/$name" records="$evidence/$name"
    local targets=(hical_core) suites=()
    mkdir -p "$records"
    if [[ $mode == native ]]; then
        reflection=ON
    fi
    if [[ $openapi == on ]]; then
        targets+=(test_json_contract)
        if [[ $mode == native ]]; then
            targets+=(test_meta_anno test_meta_schema test_collect_member test_ct_conv)
            suites+=(MetaAnnoTest MetaSchemaTest CollectMemberTest CtConvTest)
        fi
    else
        targets+=(test_json_writer)
        suites+=(JsonWriterTest JsonWriterResponseTest)
        if [[ $mode == native ]]; then
            targets+=(test_meta_anno)
            suites+=(MetaAnnoTest)
        else
            targets+=(test_reflection test_compile_time_json)
            suites+=(MetaJsonTest MetaJsonValidationTest MetaJsonFieldMergeTest MetaJsonContractTest
                JsonFieldRulesTest CompileTimeJsonTest CompileTimeJsonContractTest)
        fi
    fi
    log "$records/configure.log" cmake -S "$source_dir" -B "$build" "${common[@]}" \
        "-DCMAKE_PREFIX_PATH=$boost" -DHICAL_BUILD_TESTS=ON -DHICAL_WITH_DATABASE=OFF \
        -DHICAL_WITH_BENCHMARK=OFF "-DHICAL_ENABLE_REFLECTION=$reflection" "-DHICAL_WITH_OPENAPI=$openapi_option"
    cp -- "$build/compile_commands.json" "$records/compile_commands.json"
    log "$records/build.log" cmake --build "$build" --parallel "$jobs" --target "${targets[@]}"
    if [[ $openapi == on ]]; then
        log "$records/contract-discovery.log" ctest --test-dir "$build" -N -R '^JsonContractTest[.]'
        HICAL_JSON_CONTRACT_OUTPUT="$records/contract.json" log "$records/contract.log" \
            ctest --test-dir "$build" -R '^JsonContractTest[.]' --no-tests=error --output-on-failure \
            --timeout 60 --output-junit "$records/contract.xml"
    else
        log "$records/targets.log" cmake --build "$build" --target help
        log "$records/contract-discovery.log" ctest --test-dir "$build" -N -R '^(JsonContractTest[.]|test_json_contract)'
        if grep -q '^test_json_contract:' "$records/targets.log" ||
            ! grep -q '^Total Tests: 0$' "$records/contract-discovery.log"; then
            printf '%s 关闭了 OpenAPI，但仍注册了 JsonContractTest。\n' "$name" >&2
            return 1
        fi
    fi
    if ((${#suites[@]})); then
        run_regression "$build" "$records" "${suites[@]}"
    fi
}

configure_consumer()
{
    local logfile=$1 build=$2 package_dir=$3 native=$4 openapi=$5
    shift 5
    log "$logfile" cmake -S "$source_dir/tests/contract/install_consumer" -B "$build" \
        -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "-DCMAKE_CXX_COMPILER=$cxx" "-DBoost_DIR=$boost_config" "-Dhical_DIR=$package_dir" \
        "-DHICAL_EXPECT_NATIVE=$native" "-DHICAL_EXPECT_OPENAPI=$openapi" "$@"
}

run_consumer()
{
    local name=$1 native=$2 openapi=$3
    local records="$evidence/$name" prefix="$evidence/$name/install" build="$evidence/$name/consumer"
    log "$records/install.log" cmake --install "$work/$name" --prefix "$prefix"
    configure_consumer "$records/consumer-configure.log" "$build" "$prefix/lib/cmake/hical" "$native" "$openapi"
    log "$records/consumer-build.log" cmake --build "$build" --parallel "$jobs"
    log "$records/consumer-test.log" ctest --test-dir "$build" --no-tests=error --output-on-failure \
        --timeout 60 --output-junit "$records/consumer.xml"
}

verify_consumer_isolation()
{
    local records="$evidence/macro-on"
    local missing="$records/missing-install/lib/cmake/hical" logfile="$records/consumer-missing-configure.log"
    mkdir -p "$missing"
    # 让 CMake 也能找到刚验证过的同模式包，看看缺配置时会不会回退到这份包。
    if configure_consumer "$logfile" "$records/consumer-missing" "$missing" 0 1 \
        "-DCMAKE_PREFIX_PATH=$records/install"; then
        printf '安装目录缺少包配置，但测试工程仍然配置成功。\n' >&2
        return 1
    fi
    if ! grep -Fq 'HICAL_INSTALL_CONFIG_MISSING:' "$logfile"; then
        printf '测试工程配置失败，但日志里没有缺少包配置的报错。请查看 %s\n' "$logfile" >&2
        return 1
    fi
}

# 房间示例和同步分派编译期用例不在主仓构建里，只靠这里编一次，免得悄悄烂掉。
# 两个工程都要求安装包开了 OpenAPI，所以只在 *-on 的两个模式上跑。
run_example()
{
    local name=$1
    local prefix="$evidence/$name/install" records="$evidence/$name"
    local build="$records/room-example" cases="$records/compile-cases"
    log "$records/example-configure.log" cmake -S "$source_dir/examples/api_contract" -B "$build" \
        -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$cxx" "-DBoost_DIR=$boost_config" \
        "-DCMAKE_PREFIX_PATH=$prefix"
    log "$records/example-build.log" cmake --build "$build" --parallel "$jobs"
    log "$records/compile-cases-configure.log" cmake -S "$source_dir/tests/contract/api_contract/compile_cases" \
        -B "$cases" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=$cxx" \
        "-DBoost_DIR=$boost_config" "-DCMAKE_PREFIX_PATH=$prefix"
    log "$records/compile-cases-build.log" cmake --build "$cases" --parallel "$jobs"
    log "$records/compile-cases-test.log" ctest --test-dir "$cases" --no-tests=error \
        --output-on-failure --timeout 120
}

log "$evidence/comparator-unittest.log" python3 -m unittest discover -s "$source_dir/tests/contract" \
    -p test_check_json_contract.py -v
grep -Eq '^Ran [1-9][0-9]* tests? in ' "$evidence/comparator-unittest.log"
run_track macro on
run_track native on
log "$evidence/comparison.log" python3 "$source_dir/scripts/check_json_contract.py" \
    --macro-json "$evidence/macro-on/contract.json" --native-json "$evidence/native-on/contract.json" \
    --macro-junit "$evidence/macro-on/contract.xml" --native-junit "$evidence/native-on/contract.xml"
run_track macro off
run_track native off
run_consumer macro-on 0 1
verify_consumer_isolation
run_consumer native-on 1 1
run_consumer macro-off 0 0
run_consumer native-off 1 0
run_example macro-on
run_example native-on
