"""把 JSON 和 JUnit 样本写到文件，检查比较器会接受或拒绝哪些结果。"""

from contextlib import redirect_stderr, redirect_stdout
from copy import deepcopy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


CHECKER_PATH = Path(__file__).resolve().parents[2] / "scripts/check_json_contract.py"
MODULE_SPEC = importlib.util.spec_from_file_location("hical_json_contract_checker", CHECKER_PATH)
assert MODULE_SPEC is not None and MODULE_SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(MODULE_SPEC)
MODULE_SPEC.loader.exec_module(CHECKER)


def document(mode):
    """按约定构造一个模式的结果，方便各用例改动。"""
    room = {"room_name": "大厅", "capacity": 10}
    otherRoom = {"room_name": "小厅", "capacity": 20}
    results = [
        ("valid_room", True, room),
        ("missing_name", False, "missing_required"),
        ("empty_name", False, "not_empty"),
        ("missing_capacity", False, "missing_required"),
        ("below_minimum", False, "minimum"),
        ("above_maximum", False, "maximum"),
        ("signed_overflow", False, "integer_range"),
        ("unsigned_overflow", False, "integer_range"),
        ("double_token", False, "integer_token"),
        ("null_name", False, "type"),
        ("unknown_key", True, room),
        ("ignored_field", True, room),
        ("nested_room", True, {"room": room}),
        ("nonempty_room_list", True, {"rooms": [room, otherRoom], "total": 2}),
        ("output_outside_input_range", True, {"room_name": "大厅", "capacity": 101}),
    ]
    inputSchema = {
        "type": "object",
        "properties": {"room_name": {"type": "string", "minLength": 1},
                       "capacity": {"type": "integer", "minimum": 1, "maximum": 100}},
        "required": ["room_name", "capacity"],
    }
    outputSchema = {
        "type": "object", "properties": {"room_name": {"type": "string"}, "capacity": {"type": "integer"}},
        "required": ["room_name", "capacity"],
    }
    native = mode == "native"
    return {
        "mode": mode,
        "build": {"expected_reflection": int(native), "hical_has_reflection": int(native),
                  "cpp_impl_reflection": 202603 if native else 0, "cpp_lib_reflection": 202603 if native else 0,
                  "compiler": "原生工具链" if native else "宏工具链", "cpp_standard": 202600 if native else 202002},
        "cases": [{"id": caseId, "accepted": accepted, "result": deepcopy(result)}
                  for caseId, accepted, result in results],
        "schemas": {"input": inputSchema, "output": outputSchema,
                    "nested_input": {"properties": {"room": deepcopy(inputSchema)}},
                    "nested_output": {"properties": {"room": deepcopy(outputSchema)}},
                    "list_output": {"properties": {"rooms": {"type": "array", "items": deepcopy(outputSchema)},
                                                    "total": {"type": "integer"}}, "required": ["rooms", "total"]}},
    }


def junit(name="JsonContractTest.ExportsGoldenCases", className="JsonContractTest", output=None):
    """生成一份带 GoogleTest 运行标记的 CTest JUnit 样本。"""
    root = ET.Element("testsuite", {"tests": "1", "failures": "0", "errors": "0", "skipped": "0", "disabled": "0"})
    case = ET.SubElement(root, "testcase", {"name": name, "classname": className, "status": "run", "result": "completed"})
    fullName = name if name.startswith("JsonContractTest.") else className + "." + name
    if output is None:
        output = ("[==========] Running 1 test from 1 test suite.\n"
                  f"[ RUN      ] {fullName}\n[       OK ] {fullName}\n")
    ET.SubElement(case, "system-out").text = output
    return ET.tostring(root, encoding="unicode")


def case(documentValue, caseId):
    """按 ID 取出用例，这样测试不用记它在数组里的位置。"""
    return next(item for item in documentValue["cases"] if item["id"] == caseId)


class JsonContractComparatorTest(unittest.TestCase):
    def setUp(self):
        """给四个输入文件建一个临时目录，测完自动清理。"""
        temporary = tempfile.TemporaryDirectory(prefix="hical-json-contract-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.paths = {"macro-json": self.directory / "macro.json", "native-json": self.directory / "native.json",
                      "macro-junit": self.directory / "macro.xml", "native-junit": self.directory / "native.xml"}

    def arguments(self):
        """返回四组命令行参数和对应的文件路径。"""
        return [argument for name, path in self.paths.items() for argument in ("--" + name, str(path))]

    def runMain(self):
        """从命令行入口跑一次检查，返回退出码、标准输出和错误输出。"""
        output, errors = io.StringIO(), io.StringIO()
        with redirect_stdout(output), redirect_stderr(errors):
            code = CHECKER.main(self.arguments())
        return code, output.getvalue(), errors.getvalue()

    def check(self, macro=None, native=None, macroText=None, nativeText=None, macroXml=None, nativeXml=None):
        """写好 JSON 和 XML 文件再检查，调用方可以替换任意输入。"""
        for mode, value, raw in (("macro", macro, macroText), ("native", native, nativeText)):
            if value is None:
                value = document(mode)
            text = json.dumps(value, ensure_ascii=False) if raw is None else raw
            self.paths[mode + "-json"].write_text(text, encoding="utf-8")
        self.paths["macro-junit"].write_text(junit() if macroXml is None else macroXml, encoding="utf-8")
        self.paths["native-junit"].write_text(junit() if nativeXml is None else nativeXml, encoding="utf-8")
        return self.runMain()

    def assertRejected(self, diagnostic, **fixtures):
        """确认检查因预期的原因失败，错误输出里也没有 traceback。"""
        code, _, errors = self.check(**fixtures)
        self.assertEqual(code, 1, errors)
        self.assertIn(diagnostic, errors)
        self.assertNotIn("Traceback", errors)

    def test_valid_outputs_and_build_records_pass(self):
        code, output, errors = self.check()
        self.assertEqual(code, 0, errors)
        self.assertIn("15 个固定用例", output)
        self.assertIn("5 份 Schema", output)

    def test_case_and_object_key_order_do_not_change_results(self):
        native = document("native")
        native["cases"].reverse()
        current = case(native, "valid_room")
        current["result"] = dict(reversed(list(current["result"].items())))
        self.assertEqual(self.check(native=native)[0], 0)

    def test_required_string_arrays_can_be_reordered_deep_in_schemas(self):
        native = document("native")
        native["schemas"]["input"]["required"].reverse()
        native["schemas"]["nested_output"]["properties"]["room"]["required"].reverse()
        native["schemas"]["list_output"]["properties"]["rooms"]["items"]["required"].reverse()
        self.assertEqual(self.check(native=native)[0], 0)

    def test_missing_duplicate_and_unknown_case_ids_are_rejected(self):
        for mutation, diagnostic in (("missing", "缺少"), ("duplicate", "重复 ID"), ("unknown", "未声明")):
            with self.subTest(mutation=mutation):
                native = document("native")
                if mutation == "missing":
                    native["cases"].pop()
                elif mutation == "duplicate":
                    native["cases"].append(deepcopy(native["cases"][0]))
                else:
                    native["cases"][0]["id"] = "unexpected_case"
                self.assertRejected(diagnostic, native=native)

    def test_empty_cases_are_rejected(self):
        native = document("native")
        native["cases"] = []
        self.assertRejected("非空对象数组", native=native)

    def test_fake_native_flags_and_old_standard_macros_are_rejected(self):
        for key, value in (("expected_reflection", 0), ("hical_has_reflection", 0),
                           ("cpp_impl_reflection", 0), ("cpp_lib_reflection", 202505)):
            with self.subTest(key=key):
                native = document("native")
                native["build"][key] = value
                self.assertRejected("build." + key, native=native)

    def test_macro_flags_and_standard_macros_must_all_be_zero(self):
        for key in ("expected_reflection", "hical_has_reflection", "cpp_impl_reflection", "cpp_lib_reflection"):
            with self.subTest(key=key):
                macro = document("macro")
                macro["build"][key] = 1
                self.assertRejected("build." + key, macro=macro)

    def test_build_fields_require_integers_instead_of_bool_or_float(self):
        for key in ("expected_reflection", "hical_has_reflection", "cpp_impl_reflection", "cpp_lib_reflection"):
            for value in (True, False, 1.0, "1"):
                with self.subTest(key=key, value=value):
                    native = document("native")
                    native["build"][key] = value
                    self.assertRejected("必须是非负整数", native=native)

    def test_native_standard_macro_boundary_is_accepted(self):
        native = document("native")
        native["build"].update(cpp_impl_reflection=202506, cpp_lib_reflection=202506)
        self.assertEqual(self.check(native=native)[0], 0)

    def test_mode_and_closed_document_fields_are_checked(self):
        native = document("native")
        native["mode"] = "macro"
        self.assertRejected("native.mode", native=native)
        native = document("native")
        native["extra"] = True
        self.assertRejected("未声明字段", native=native)
        native = document("native")
        native["build"]["forced"] = True
        self.assertRejected("未声明字段", native=native)

    def test_accepted_requires_a_boolean(self):
        native = document("native")
        case(native, "valid_room")["accepted"] = 1
        self.assertRejected("accepted 必须是布尔值", native=native)

    def test_accepted_results_require_json_objects_or_arrays(self):
        for value in (None, "serialized marker", True, 1, 1.0):
            with self.subTest(value=value):
                native = document("native")
                case(native, "valid_room")["result"] = value
                self.assertRejected("实际 JSON 对象或数组", native=native)

    def test_rejected_results_require_nonempty_classification_strings(self):
        for value in ({}, [], None, 1, "", " \n "):
            with self.subTest(value=value):
                native = document("native")
                case(native, "empty_name")["result"] = value
                self.assertRejected("非空分类字符串", native=native)

    def test_successful_array_results_are_supported(self):
        macro, native = document("macro"), document("native")
        for value in (macro, native):
            case(value, "valid_room")["result"] = [{"capacity": 1}]
        self.assertEqual(self.check(macro=macro, native=native)[0], 0)

    def test_true_and_one_are_different_json_values(self):
        macro, native = document("macro"), document("native")
        case(macro, "valid_room")["result"]["flag"] = True
        case(native, "valid_room")["result"]["flag"] = 1
        self.assertRejected("JSON 类型不一致", macro=macro, native=native)

    def test_integer_and_float_are_different_json_values(self):
        native = document("native")
        case(native, "valid_room")["result"]["capacity"] = 10.0
        self.assertRejected("JSON 类型不一致", native=native)

    def test_business_arrays_keep_their_order(self):
        native = document("native")
        case(native, "nonempty_room_list")["result"]["rooms"].reverse()
        self.assertRejected("cases.nonempty_room_list.result.rooms[0]", native=native)

    def test_business_fields_named_required_keep_array_order(self):
        macro, native = document("macro"), document("native")
        case(macro, "valid_room")["result"]["required"] = ["b", "a"]
        case(native, "valid_room")["result"]["required"] = ["a", "b"]
        self.assertRejected("cases.valid_room.result.required[0]", macro=macro, native=native)

    def test_other_schema_arrays_keep_order_and_required_does_not_deduplicate(self):
        macro, native = document("macro"), document("native")
        macro["schemas"]["input"]["enum"] = ["a", "b"]
        native["schemas"]["input"]["enum"] = ["b", "a"]
        self.assertRejected("schemas.input.enum[0]", macro=macro, native=native)
        macro, native = document("macro"), document("native")
        native["schemas"]["input"]["required"].append("capacity")
        self.assertRejected("数组长度不一致", macro=macro, native=native)
        macro["schemas"]["input"]["required"] = [1, 2]
        native["schemas"]["input"]["required"] = [2, 1]
        self.assertRejected("schemas.input.required[0]", macro=macro, native=native)

    def test_schema_constraints_and_field_sets_must_match(self):
        native = document("native")
        native["schemas"]["input"]["properties"]["capacity"]["maximum"] = 99
        self.assertRejected("schemas.input.properties.capacity.maximum", native=native)
        native = document("native")
        del native["schemas"]["list_output"]
        self.assertRejected("缺少字段", native=native)

    def test_duplicate_json_keys_are_rejected_even_in_nested_objects(self):
        raw = json.dumps(document("macro"), ensure_ascii=False)
        duplicateMode = raw.replace('"mode": "macro"', '"mode": "macro", "mode": "macro"', 1)
        self.assertRejected("重复 JSON 键", macroText=duplicateMode)
        self.assertRejected("重复 JSON 键", macroText=raw.replace('"capacity": 10', '"capacity": 10, "capacity": 11', 1))

    def test_nonstandard_numbers_and_float_overflow_are_rejected(self):
        for value in (float("nan"), float("inf"), float("-inf")):
            with self.subTest(value=value):
                native = document("native")
                case(native, "valid_room")["result"]["capacity"] = value
                self.assertRejected("非标准数值", native=native)
        raw = json.dumps(document("native")).replace('"capacity": 10', '"capacity": 1e9999', 1)
        self.assertRejected("有限浮点范围", nativeText=raw)

    def test_junit_accepts_both_ctest_naming_forms(self):
        self.assertEqual(self.check(nativeXml=junit(name="ExportsGoldenCases"))[0], 0)
        self.assertEqual(self.check(nativeXml=junit(className="JsonContractTest.ExportsGoldenCases"))[0], 0)
        wrapped = '<testsuites tests="1">' + junit(name="ExportsGoldenCases") + '</testsuites>'
        self.assertEqual(self.check(nativeXml=wrapped)[0], 0)

    def test_junit_accepts_escaped_color_from_ctest(self):
        # 这段彩色日志来自 ctest --output-junit，只换了用例名。
        output = (
            "[NON-XML-CHAR-0x1B][m[NON-XML-CHAR-0x1B][0;32m[==========] "
            "[NON-XML-CHAR-0x1B][mRunning 1 test from 1 test suite.\n"
            "[NON-XML-CHAR-0x1B][0;32m[ RUN      ] [NON-XML-CHAR-0x1B][mJsonContractTest.ExportsGoldenCases\n"
            "[NON-XML-CHAR-0x1B][0;32m[       OK ] [NON-XML-CHAR-0x1B][mJsonContractTest.ExportsGoldenCases (0 ms)\n"
        )
        code, _, errors = self.check(nativeXml=junit(output=output))
        self.assertEqual(code, 0, errors)

    def test_junit_failure_error_and_skip_nodes_are_rejected(self):
        for tag in ("failure", "error", "skipped", "disabled", "notrun"):
            with self.subTest(tag=tag):
                root = ET.fromstring(junit())
                ET.SubElement(root.find("testcase"), tag)
                self.assertRejected(tag, nativeXml=ET.tostring(root, encoding="unicode"))

    def test_junit_summary_failures_and_skips_are_rejected_without_child_nodes(self):
        for attribute in ("failures", "errors", "skipped", "disabled", "notrun"):
            with self.subTest(attribute=attribute):
                root = ET.fromstring(junit())
                root.set(attribute, "1")
                self.assertRejected(attribute + " 非零", nativeXml=ET.tostring(root, encoding="unicode"))

    def test_junit_disabled_or_notrun_status_is_rejected(self):
        for attribute, value in (("status", "notrun"), ("status", "disabled"), ("result", "suppressed")):
            with self.subTest(attribute=attribute, value=value):
                root = ET.fromstring(junit())
                root.find("testcase").set(attribute, value)
                self.assertRejected("未成功执行", nativeXml=ET.tostring(root, encoding="unicode"))

    def test_junit_needs_contract_cases_and_matching_nonzero_counts(self):
        self.assertRejected("JsonContractTest.ExportsGoldenCases",
                            nativeXml=junit(name="OtherTest.Passes", className="OtherTest"))
        self.assertRejected("没有实际测试用例", nativeXml='<testsuite tests="0"/>')
        root = ET.fromstring(junit())
        root.set("tests", "0")
        self.assertRejected("tests 计数", nativeXml=ET.tostring(root, encoding="unicode"))

    def test_junit_needs_the_export_case_and_allows_other_executed_contract_cases(self):
        extra = junit(name="JsonContractTest.ValidatesNestedInput")
        self.assertRejected("JsonContractTest.ExportsGoldenCases", nativeXml=extra)
        combined = '<testsuites tests="2">' + junit() + extra + '</testsuites>'
        code, output, errors = self.check(nativeXml=combined)
        self.assertEqual(code, 0, errors)
        self.assertIn("原生 2 项", output)

    def test_junit_rejects_duplicate_export_case_across_naming_forms(self):
        duplicate = '<testsuites tests="2">' + junit() + junit(name="ExportsGoldenCases") + '</testsuites>'
        self.assertRejected("重复用例 'JsonContractTest.ExportsGoldenCases'", nativeXml=duplicate)

    def test_junit_zero_tests_output_rejects_a_false_green_report(self):
        root = ET.fromstring(junit(output="[==========] Running 0 tests from 0 test suites.\n"))
        original = root.find("testcase")
        for index in range(7):
            extra = deepcopy(original)
            extra.set("name", f"JsonContractTest.Case{index}")
            root.append(extra)
        root.set("tests", "8")
        self.assertRejected("实际空跑", nativeXml=ET.tostring(root, encoding="unicode"))

    def test_junit_zero_test_marker_is_checked_everywhere_and_with_escaped_color(self):
        for zeroTests in ("Running 0 test", "Running [NON-XML-CHAR-0x1B][32m0[NON-XML-CHAR-0x1B][m tests"):
            output = f"{zeroTests}\n[ RUN ] JsonContractTest.ExportsGoldenCases\n"
            with self.subTest(output=output):
                self.assertRejected("实际空跑", nativeXml=junit(output=output))
        root = ET.fromstring(junit())
        ET.SubElement(root, "system-out").text = "Running 0 tests"
        self.assertRejected("实际空跑", nativeXml=ET.tostring(root, encoding="unicode"))

    def test_junit_needs_a_run_marker_for_the_specific_case(self):
        for output in ("[==========] Running 1 test\n", "[ RUN ] JsonContractTest.Other\n", ""):
            with self.subTest(output=output):
                self.assertRejected("对应 [ RUN ]", nativeXml=junit(output=output))

    def test_invalid_xml_and_invalid_json_have_normal_diagnostics(self):
        self.assertRejected("JUnit 文件读取失败", nativeXml="<testsuite>")
        self.assertRejected("JSON 文件读取失败", macroText="{")
        self.assertRejected("根元素必须", nativeXml="<report/>")

    def test_public_cli_returns_success_and_nonzero_failure_without_tracebacks(self):
        self.assertEqual(self.check()[0], 0)
        command = [sys.executable, str(CHECKER_PATH), *self.arguments()]
        passed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=10, check=False)
        self.assertEqual(passed.returncode, 0, passed.stderr)
        for name, invalid in (("missing fields", {}), ("unpaired surrogate key", {**document("native"), "\ud800": True})):
            with self.subTest(name=name):
                self.paths["native-json"].write_text(json.dumps(invalid), encoding="utf-8")
                failed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", timeout=10, check=False)
                self.assertNotEqual(failed.returncode, 0)
                self.assertIn("双轨契约检查失败", failed.stderr)
                self.assertNotIn("Traceback", failed.stderr)


if __name__ == "__main__":
    unittest.main()
