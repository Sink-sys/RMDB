#!/usr/bin/env python3
"""Run local RMDB functional, isolation, and recovery tests over Wire v3."""

from __future__ import annotations

import argparse
import difflib
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Literal

from test_final_isolation import ALL_CASES as FINAL_ISOLATION_CASES
from test_final_isolation import WireClient
from test_final_isolation import run_case as run_final_isolation_history


ROOT = Path(__file__).resolve().parents[1]
TEST_ROOT = Path(__file__).resolve().parent
CASE_DIR = TEST_ROOT / "cases"
EXPECTED_DIR = TEST_ROOT / "expected"
DEFAULT_BUILD_DIR = ROOT / "build"
DEFAULT_PORT = 8765
_WIRE_OUTPUT: list[str] = []
_WIRE_OUTPUT_LOCK = threading.Lock()


@dataclass(frozen=True)
class TestCase:
    name: str
    sql_path: Path | None
    expected_path: Path | None
    kind: str = "static"


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    detail: str




def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run RMDB document-based SQL tests.")
    parser.add_argument("cases", nargs="*", help="Case names without .sql, or glob fragments.")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--server", type=Path, help="Path to rmdb server binary.")
    parser.add_argument("--timeout", type=float, default=20.0, help="Per-case timeout in seconds.")
    parser.add_argument("--connect-timeout", type=float, default=5.0)
    parser.add_argument("--keep-db", action="store_true", help="Keep generated test databases.")
    parser.add_argument("--list", action="store_true", help="List discovered cases and exit.")
    parser.add_argument("--quiet", action="store_true", help="Only print details for failing cases.")
    parser.add_argument("--show-server-log", action="store_true")
    parser.add_argument("--recovery-row-count", type=int, default=200, help="Rows used by generated recovery tests.")
    return parser.parse_args()


def discover_cases(filters: list[str]) -> list[TestCase]:
    cases: list[TestCase] = []
    for sql_path in sorted(CASE_DIR.glob("*.sql")):
        expected_path = EXPECTED_DIR / f"{sql_path.stem}.expected"
        if not expected_path.exists():
            continue
        if filters and not any(token in sql_path.stem for token in filters):
            continue
        cases.append(TestCase(sql_path.stem, sql_path, expected_path))
    generated: list[TestCase] = [
        TestCase("07_17_join_multitable_composite_prefix", None, None, "join_composite_prefix"),
        TestCase("08_01_transaction_commit", None, None, "txn_commit"),
        TestCase("08_02_transaction_abort", None, None, "txn_abort"),
        TestCase("08_03_transaction_commit_index", None, None, "txn_commit_index"),
        TestCase("08_04_transaction_abort_index", None, None, "txn_abort_index"),
        *[TestCase(name, None, None, "final_isolation") for name in FINAL_ISOLATION_CASES],
        TestCase("10_01_crash_recovery_single_thread", None, None, "recovery_single"),
        TestCase("10_02_crash_recovery_multi_thread", None, None, "recovery_multi"),
        TestCase("10_03_crash_recovery_index", None, None, "recovery_index"),
        TestCase("10_04_crash_recovery_large_data", None, None, "recovery_large"),
        TestCase("10_05_crash_recovery_single_thread_2", None, None, "recovery_single_2"),
        TestCase("10_07_recovery_undo_uncommitted", None, None, "recovery_script_undo_uncommitted"),
        TestCase("10_08_recovery_redo_committed", None, None, "recovery_script_redo_committed"),
        TestCase("10_09_recovery_index_consistency", None, None, "recovery_script_index_consistency"),
        TestCase("10_10_recovery_log_boundary", None, None, "recovery_script_log_boundary"),
        TestCase("10_11_recovery_restart_new_writes", None, None, "recovery_script_restart_new_writes"),
        TestCase("10_12_recovery_multitable_atomic", None, None, "recovery_script_multitable_atomic"),
    ]
    for case in generated:
        if filters and not any(token in case.name for token in filters):
            continue
        cases.append(case)
    cases.sort(key=lambda case: case.name)
    return cases


def strip_line_comment(line: str) -> str:
    in_string = False
    out: list[str] = []
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "'":
            in_string = not in_string
            out.append(ch)
            i += 1
            continue
        if not in_string and ch == "-" and i + 1 < len(line) and line[i + 1] == "-":
            break
        out.append(ch)
        i += 1
    return "".join(out)


def split_sql(sql_text: str) -> list[str]:
    statements: list[str] = []
    current: list[str] = []
    in_string = False
    for raw_line in sql_text.splitlines():
        line = strip_line_comment(raw_line)
        for ch in line:
            current.append(ch)
            if ch == "'":
                in_string = not in_string
            elif ch == ";" and not in_string:
                statement = "".join(current).strip()
                if statement:
                    statements.append(" ".join(statement.split()))
                current.clear()
        current.append(" ")
    tail = "".join(current).strip()
    if tail:
        statements.append(" ".join(tail.split()))
    return statements


def is_table_line(line: str) -> bool:
    return line.startswith("|") and line.endswith("|")


def split_table_cells(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip("|").split("|")]


def is_header_like(line: str) -> bool:
    if not is_table_line(line):
        return False
    cells = split_table_cells(line)
    if not cells:
        return False
    return all(re.fullmatch(r"[A-Za-z_][A-Za-z_]*", cell) for cell in cells)


def is_plan_line(line: str) -> bool:
    stripped = line.lstrip("\t")
    return stripped.startswith(("Project(", "Filter(", "Join(", "Scan("))


def normalize_output(
    text: str,
    *,
    preserve_table_order: bool = False,
    preserve_plan_indentation: bool = False,
) -> list[str]:
    normalized: list[str] = []
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if set(stripped) <= {"+", "-"}:
            continue
        if stripped.startswith("record count:"):
            continue
        if preserve_plan_indentation and is_plan_line(line):
            normalized.append(line.rstrip())
        else:
            normalized.append(stripped)
    if preserve_table_order:
        return normalized

    canonical: list[str] = []
    current_header: str | None = None
    current_rows: list[str] = []

    def flush_table() -> None:
        nonlocal current_header, current_rows
        if current_header is not None:
            canonical.append(current_header)
            canonical.extend(sorted(current_rows))
            current_header = None
            current_rows = []

    for line in normalized:
        if is_header_like(line):
            flush_table()
            current_header = line
            current_rows = []
        elif is_table_line(line) and current_header is not None:
            current_rows.append(line)
        else:
            flush_table()
            canonical.append(line)
    flush_table()
    return canonical


def canonicalize_case_output(case_name: str, lines: list[str]) -> list[str]:
    canonical = list(lines)
    if case_name in {"03_03_index_maintenance"}:
        canonical = collapse_consecutive_duplicates(canonical)
    return canonical


def collapse_consecutive_duplicates(lines: list[str]) -> list[str]:
    collapsed: list[str] = []
    for line in lines:
        if collapsed and collapsed[-1] == line:
            continue
        collapsed.append(line)
    return collapsed


def split_table_blocks(lines: list[str]) -> list[list[str]]:
    blocks: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        if is_header_like(line):
            if current:
                blocks.append(current)
            current = [line]
        elif current:
            current.append(line)
    if current:
        blocks.append(current)
    return blocks


def wait_for_server(port: int, timeout: float) -> None:
    if port != DEFAULT_PORT:
        raise ValueError(f"Wire test runner only supports port {DEFAULT_PORT}")
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        try:
            probe = WireClient(min(0.5, max(0.1, deadline - time.monotonic())))
            frames = probe.execute("show tables;")
            probe.close()
            if frames and frames[0][0] == 0x01 and frames[-1][0] == 0x11:
                return
        except (OSError, RuntimeError, socket.timeout) as exc:
            last_error = exc
            time.sleep(0.05)
    raise RuntimeError(f"server did not become Wire v3 ready on port {port}: {last_error}")


def decode_meta(payload: bytes) -> tuple[list[str], list[int]]:
    count = struct.unpack("!H", payload[:2])[0]
    names: list[str] = []
    types: list[int] = []
    offset = 2
    for _ in range(count):
        length = struct.unpack("!H", payload[offset : offset + 2])[0]
        offset += 2
        names.append(payload[offset : offset + length].decode("utf-8"))
        offset += length
        types.append(payload[offset])
        offset += 1
    return names, types


def decode_row(payload: bytes, types: list[int]) -> list[str]:
    values: list[str] = []
    offset = 0
    for value_type in types:
        present = payload[offset]
        offset += 1
        if not present:
            values.append("NULL")
        elif value_type == 1:
            values.append(str(struct.unpack("!i", payload[offset : offset + 4])[0]))
            offset += 4
        elif value_type == 2:
            values.append(f"{struct.unpack('!f', payload[offset : offset + 4])[0]:.6f}")
            offset += 4
        elif value_type == 3:
            length = struct.unpack("!I", payload[offset : offset + 4])[0]
            offset += 4
            values.append(payload[offset : offset + length].decode("utf-8"))
            offset += length
        else:
            raise RuntimeError(f"unknown Wire column type: {value_type}")
    return values


def capture_wire_response(frames: list[tuple[int, int, int, bytes]]) -> None:
    lines: list[str] = []
    terminal = frames[-1][0] if frames else None
    if frames and frames[0][0] == 0x01:
        names, types = decode_meta(frames[0][3])
        lines.append("| " + " | ".join(names) + " |")
        for tag, _, _, payload in frames[1:-1]:
            if tag != 0x02:
                raise RuntimeError(f"unexpected Wire frame in query response: {tag:#x}")
            lines.append("| " + " | ".join(decode_row(payload, types)) + " |")
    elif terminal == 0x12:
        lines.append("abort")
    elif terminal == 0x13:
        lines.append("failure")
    elif terminal != 0x10:
        raise RuntimeError(f"unexpected Wire terminal frame: {terminal}")
    if lines:
        with _WIRE_OUTPUT_LOCK:
            _WIRE_OUTPUT.extend(lines)


class RmdbClient:
    def __init__(self, port: int, timeout: float):
        if port != DEFAULT_PORT:
            raise ValueError(f"Wire test runner only supports port {DEFAULT_PORT}")
        self.wire = WireClient(timeout)
        self.sock = self.wire.sock

    def close(self) -> None:
        self.wire.close()

    def execute(self, statement: str) -> list[tuple[int, int, int, bytes]]:
        frames = self.wire.execute(statement)
        capture_wire_response(frames)
        return frames


def send_sql(statements: list[str], port: int, timeout: float) -> None:
    client = RmdbClient(port, timeout)
    try:
        for statement in statements:
            client.execute(statement)
    finally:
        client.close()


def read_actual_output(build_dir: Path, db_name: str) -> str:
    del build_dir, db_name
    with _WIRE_OUTPUT_LOCK:
        return "\n".join(_WIRE_OUTPUT)


def cleanup_outputs(build_dir: Path, db_name: str) -> None:
    del build_dir, db_name
    with _WIRE_OUTPUT_LOCK:
        _WIRE_OUTPUT.clear()


def cleanup_db(build_dir: Path, db_name: str) -> None:
    for path in [build_dir / db_name, ROOT / db_name]:
        if path.exists():
            shutil.rmtree(path)


def start_server(server: Path, build_dir: Path, db_name: str, server_log: Path) -> subprocess.Popen[str]:
    log_file = server_log.open("w", encoding="utf-8")
    return subprocess.Popen(
        [str(server), db_name],
        cwd=build_dir,
        stdout=log_file,
        stderr=subprocess.STDOUT,
        text=True,
    )


def terminate_server(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def preserve_table_order_for_case(case_name: str) -> bool:
    return case_name.startswith("08_") or case_name.startswith("09_") or case_name in {
        "05_02_group_by_having",
        "05_04_order_limit",
        "05_05_group_multi_hidden",
    }


def preserve_plan_indentation_for_case(case_name: str) -> bool:
    return case_name.startswith("07_")


def compare_lines(
    expected: list[str],
    actual_text: str,
    case_name: str,
    *,
    preserve_table_order: bool = False,
    preserve_plan_indentation: bool = False,
) -> str | None:
    expected = canonicalize_case_output(case_name, expected)
    actual = canonicalize_case_output(
        case_name,
        normalize_output(
            actual_text,
            preserve_table_order=preserve_table_order,
            preserve_plan_indentation=preserve_plan_indentation,
        ),
    )
    if expected == actual:
        return None
    return "\n".join(
        difflib.unified_diff(
            expected,
            actual,
            fromfile=f"expected/{case_name}.expected",
            tofile=f"actual/{case_name}.output",
            lineterm="",
        )
    ) or "output mismatch"


def run_statements_with_server(
    case: TestCase,
    statements: list[str],
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    expected: list[str] | None = None,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        send_sql(statements, DEFAULT_PORT, timeout)
        time.sleep(0.1)
    except Exception as exc:
        terminate_server(proc)
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        terminate_server(proc)

    if expected is None:
        return TestResult(case, True, "passed")
    detail = compare_lines(
        expected,
        read_actual_output(build_dir, db_name),
        case.name,
        preserve_table_order=preserve_table_order_for_case(case.name),
        preserve_plan_indentation=preserve_plan_indentation_for_case(case.name),
    )
    if detail is None:
        if not show_server_log and server_log.exists():
            server_log.unlink()
        if not keep_db:
            cleanup_db(build_dir, db_name)
        return TestResult(case, True, "passed")
    if show_server_log and server_log.exists():
        detail += "\n\n--- server log tail ---\n"
        detail += server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
    return TestResult(case, False, detail)


def run_composite_prefix_join_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)
    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    clients: list[RmdbClient] = []
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        setup = RmdbClient(DEFAULT_PORT, timeout)
        clients.append(setup)
        setup.execute("create table prefix_left (tenant int,key_id int,wanted int);")
        setup.execute("create table prefix_mid (tenant int,key_id int);")
        setup.execute(
            "create table prefix_right "
            "(tenant int,key_id int,seq int,filter_value int,tag char(8));"
        )
        setup.execute("insert into prefix_left values(1,10,7);")
        setup.execute("insert into prefix_left values(1,20,8);")
        setup.execute("insert into prefix_mid values(1,10);")
        setup.execute("insert into prefix_mid values(1,20);")
        setup.execute("insert into prefix_right values(1,10,0,7,'zero');")
        setup.execute("insert into prefix_right values(1,10,1,7,'keep10');")
        setup.execute("insert into prefix_right values(1,10,2,9,'reject');")
        setup.execute("insert into prefix_right values(1,20,1,8,'keep21');")
        setup.execute("insert into prefix_right values(1,20,2,8,'keep22');")
        setup.execute("insert into prefix_right values(2,10,1,7,'tenant2');")

        query = (
            "select count(*) as matches from prefix_left pl, prefix_mid pm, prefix_right pr "
            "where pl.tenant = pm.tenant and pl.key_id = pm.key_id "
            "and pr.tenant = pl.tenant and pm.key_id = pr.key_id "
            "and pr.filter_value = pl.wanted and pr.seq > 0;"
        )
        reverse_query = (
            "select count(*) as matches from prefix_right pr, prefix_mid pm, prefix_left pl "
            "where pl.tenant = pm.tenant and pl.key_id = pm.key_id "
            "and pl.tenant = pr.tenant and pr.key_id = pm.key_id "
            "and pl.wanted = pr.filter_value and pr.seq > 0;"
        )
        mixed_query = (
            "select count(*) as matches from prefix_left pl, prefix_mid pm, prefix_right pr "
            "where pl.tenant = pm.tenant and pl.key_id = pm.key_id and pl.tenant = 1 "
            "and pr.tenant = 1 and pm.key_id = pr.key_id "
            "and pr.filter_value = pl.wanted and pr.seq > 0;"
        )
        cleanup_outputs(build_dir, db_name)
        setup.execute(query)
        setup.execute(reverse_query)
        setup.execute(mixed_query)
        setup.execute("create index prefix_right(tenant,key_id,seq);")
        setup.execute(query)
        setup.execute(reverse_query)
        setup.execute(mixed_query)

        reader = RmdbClient(DEFAULT_PORT, timeout)
        writer = RmdbClient(DEFAULT_PORT, timeout)
        verifier = RmdbClient(DEFAULT_PORT, timeout)
        serializable = RmdbClient(DEFAULT_PORT, timeout)
        clients.extend([reader, writer, verifier, serializable])
        reader.execute("set transaction isolation level snapshot isolation;")
        writer.execute("set transaction isolation level snapshot isolation;")
        reader.execute("begin;")
        reader.execute(query)
        writer.execute("begin;")
        writer.execute(
            "update prefix_right set filter_value = 99 "
            "where tenant = 1 and key_id = 10 and seq = 1;"
        )
        writer.execute("commit;")
        reader.execute(query)
        reader.execute("commit;")
        verifier.execute(query)
        serializable.execute("set transaction isolation level serializable;")
        serializable.execute("begin;")
        serializable.execute(query)
        serializable.execute("commit;")

        expected = [
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 3 |",
            "| matches |",
            "| 2 |",
            "| matches |",
            "| 2 |",
        ]
        detail = compare_lines(
            expected,
            read_actual_output(build_dir, db_name),
            case.name,
            preserve_table_order=True,
        )
        return TestResult(case, detail is None, detail or "multi-table composite prefix semantics passed")
    except Exception as exc:
        detail = f"runtime error: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        for client in clients:
            client.close()
        terminate_server(proc)


def run_index_compat_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    if case.name == "03_01_index_ddl":
        statements = [
            "create table warehouse (id int, name char(8));",
            "create index warehouse (id);",
            "show index from warehouse;",
            "create index warehouse (id,name);",
            "show index from warehouse;",
        ]
        expected = [
            "| warehouse | unique | (id) |",
            "| warehouse | unique | (id) |",
            "| warehouse | unique | (id,name) |",
        ]
        return run_statements_with_server(
            case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
        )

    if case.name == "03_02_index_query":
        statements = [
            "create table warehouse_id (w_id int, name char(8));",
            "insert into warehouse_id values (10, 'qweruiop');",
            "insert into warehouse_id values (534, 'asdfhjkl');",
            "insert into warehouse_id values (100,'qwerghjk');",
            "insert into warehouse_id values (500,'bgtyhnmj');",
            "create index warehouse_id(w_id);",
            "select * from warehouse_id where w_id = 10;",
            "select * from warehouse_id where w_id < 534 and w_id > 100;",
            "create table warehouse_name (w_id int, name char(8));",
            "insert into warehouse_name values (10, 'qweruiop');",
            "insert into warehouse_name values (534, 'asdfhjkl');",
            "insert into warehouse_name values (100,'qwerghjk');",
            "insert into warehouse_name values (500,'bgtyhnmj');",
            "create index warehouse_name(name);",
            "select * from warehouse_name where name = 'qweruiop';",
            "select * from warehouse_name where name > 'qwerghjk';",
            "select * from warehouse_name where name > 'aszdefgh' and name < 'qweraaaa';",
            "create table warehouse_mix (w_id int, name char(8));",
            "insert into warehouse_mix values (10, 'qweruiop');",
            "insert into warehouse_mix values (534, 'asdfhjkl');",
            "insert into warehouse_mix values (100,'qwerghjk');",
            "insert into warehouse_mix values (500,'bgtyhnmj');",
            "create index warehouse_mix(w_id,name);",
            "select * from warehouse_mix where w_id = 100 and name = 'qwerghjk';",
            "select * from warehouse_mix where w_id < 600 and name > 'bztyhnmj';",
        ]
        expected = normalize_output((EXPECTED_DIR / f"{case.name}.expected").read_text(encoding="utf-8"))
        return run_statements_with_server(
            case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
        )

    if case.name == "03_03_index_maintenance":
        statements = [
            "create table warehouse_single (w_id int, name char(8));",
            "insert into warehouse_single values (10 , 'qweruiop');",
            "insert into warehouse_single values (534, 'asdfhjkl');",
            "select * from warehouse_single where w_id = 10;",
            "select * from warehouse_single where w_id < 534 and w_id > 100;",
            "create index warehouse_single(w_id);",
            "insert into warehouse_single values (500, 'lastdanc');",
            "insert into warehouse_single values (10, 'uiopqwer');",
            "update warehouse_single set w_id = 507 where w_id = 534;",
            "select * from warehouse_single where w_id = 10;",
            "select * from warehouse_single where w_id < 534 and w_id > 100;",
            "create table warehouse_multi (w_id int, name char(8));",
            "insert into warehouse_multi values (10 , 'qweruiop');",
            "insert into warehouse_multi values (507, 'asdfhjkl');",
            "insert into warehouse_multi values (500, 'lastdanc');",
            "create index warehouse_multi(w_id,name);",
            "insert into warehouse_multi values(10,'qqqqoooo');",
            "insert into warehouse_multi values(500,'lastdanc');",
            "update warehouse_multi set w_id = 10, name = 'qqqqoooo' where w_id = 507 and name = 'asdfhjkl';",
            "select * from warehouse_multi;",
        ]
        expected = normalize_output((EXPECTED_DIR / f"{case.name}.expected").read_text(encoding="utf-8"))
        return run_statements_with_server(
            case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
        )

    raise RuntimeError(f"unknown index compatibility case: {case.name}")


def run_static_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    assert case.sql_path is not None
    assert case.expected_path is not None
    statements = split_sql(case.sql_path.read_text(encoding="utf-8"))
    preserve_table_order = preserve_table_order_for_case(case.name)
    preserve_plan_indentation = preserve_plan_indentation_for_case(case.name)
    expected = normalize_output(
        case.expected_path.read_text(encoding="utf-8"),
        preserve_table_order=preserve_table_order,
        preserve_plan_indentation=preserve_plan_indentation,
    )
    return run_statements_with_server(
        case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
    )


def run_transaction_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    with_index = case.kind.endswith("_index")
    is_commit = "commit" in case.kind
    statements = ["create table student (id int, name char(8), score float);"]
    if with_index:
        statements.append("create index student(id);")
    statements.extend([
        "insert into student values (1, 'xiaohong', 90.0);",
        "begin;",
        "insert into student values (2, 'xiaoming', 99.0);",
    ])
    if is_commit:
        statements.extend(["commit;", "select * from student;"])
        expected = [
            "| id | name | score |",
            "| 1 | xiaohong | 90.000000 |",
            "| 2 | xiaoming | 99.000000 |",
        ]
    else:
        statements.extend(["abort;", "select * from student;"])
        expected = ["| id | name | score |", "| 1 | xiaohong | 90.000000 |"]
    return run_statements_with_server(
        case, statements, server, build_dir, timeout, connect_timeout, keep_db, show_server_log, expected
    )


def recovery_workload(client: RmdbClient, rows: int, use_index: bool, workers: int) -> None:
    client.execute("create table warehouse (w_id int, w_name char(10), w_ytd float);")
    if use_index:
        client.execute("create index warehouse(w_id);")
    for i in range(1, rows + 1):
        client.execute(f"insert into warehouse values ({i}, 'wh{i % 10000:04d}', {float(i):.6f});")

    errors: list[str] = []
    def run_worker(worker_id: int) -> None:
        local = RmdbClient(DEFAULT_PORT, client.sock.gettimeout() or 20.0)
        try:
            start = worker_id + 1
            step = max(1, workers)
            for key in range(start, rows + 1, step):
                local.execute("begin;")
                local.execute(f"update warehouse set w_ytd={1000.0 + key:.6f} where w_id={key};")
                local.execute("commit;")
        except Exception as exc:
            errors.append(str(exc))
        finally:
            local.close()

    threads = [threading.Thread(target=run_worker, args=(i,)) for i in range(workers)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    if errors:
        raise RuntimeError(errors[0])


def run_recovery_trial(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    rows: int,
    use_index: bool,
    workers: int,
    validate_query: bool = False,
    suffix: str = "",
) -> tuple[bool, str, float]:
    db_name = f"doc_test_{case.name}{suffix}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)
    server_log = TEST_ROOT / f"{case.name}{suffix}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        recovery_workload(client, rows, use_index, workers)
        client.close()
        client = None
        proc.kill()
        proc.wait(timeout=3)
    except Exception as exc:
        if client is not None:
            client.close()
        terminate_server(proc)
        detail = f"runtime error before restart: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return False, detail, float("inf")

    restart_log = TEST_ROOT / f"{case.name}{suffix}.restart.server.log"
    recovery_time = float("inf")
    for attempt in range(3):
        proc = start_server(server, build_dir, db_name, restart_log)
        try:
            start = time.perf_counter()
            wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))
            recovery_time = time.perf_counter() - start
            break
        except Exception as exc:
            terminate_server(proc)
            if attempt == 2:
                detail = f"runtime error after restart: {exc}"
                if show_server_log and restart_log.exists():
                    detail += "\n" + restart_log.read_text(encoding="utf-8", errors="replace")[-4000:]
                return False, detail, float("inf")
            time.sleep(1.0)
    try:
        if not validate_query:
            return True, f"passed, recovery_time={recovery_time:.3f}s", recovery_time
        cleanup_outputs(build_dir, db_name)
        client = RmdbClient(DEFAULT_PORT, timeout)
        sample_keys = sorted({1, max(1, rows // 2), rows})
        for key in sample_keys:
            client.execute(f"select * from warehouse where w_id = {key};")
        time.sleep(0.1)
        client.close()
        client = None
        expected = []
        for key in sample_keys:
            expected.extend([
                "| w_id | w_name | w_ytd |",
                f"| {key} | wh{key % 10000:04d} | {1000.0 + key:.6f} |",
            ])
        detail = compare_lines(expected, read_actual_output(build_dir, db_name), case.name)
        if detail is None:
            return True, f"passed, recovery_time={recovery_time:.3f}s", recovery_time
        return False, detail, recovery_time
    except Exception as exc:
        detail = f"runtime error after restart: {exc}"
        if show_server_log and restart_log.exists():
            detail += "\n" + restart_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return False, detail, float("inf")
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)


def recovery_script(case: TestCase) -> tuple[list[str], list[str], list[str]]:
    if case.kind == "recovery_script_undo_uncommitted":
        before_crash = [
            "create table rec_undo (id int, val int);",
            "insert into rec_undo values (1, 10);",
            "insert into rec_undo values (2, 20);",
            "begin;",
            "update rec_undo set val = 100 where id = 1;",
            "delete from rec_undo where id = 2;",
            "insert into rec_undo values (3, 30);",
        ]
        after_restart = ["select * from rec_undo;"]
        expected = ["| id | val |", "| 1 | 10 |", "| 2 | 20 |"]
    elif case.kind == "recovery_script_redo_committed":
        before_crash = [
            "create table rec_redo (id int, val int);",
            "insert into rec_redo values (1, 10);",
            "insert into rec_redo values (2, 20);",
            "insert into rec_redo values (3, 30);",
            "begin;",
            "update rec_redo set val = 110 where id = 1;",
            "delete from rec_redo where id = 2;",
            "insert into rec_redo values (4, 40);",
            "commit;",
        ]
        after_restart = ["select * from rec_redo;"]
        expected = ["| id | val |", "| 1 | 110 |", "| 3 | 30 |", "| 4 | 40 |"]
    elif case.kind == "recovery_script_index_consistency":
        before_crash = [
            "create table rec_idx (id int, val int);",
            "create index rec_idx(id);",
            "insert into rec_idx values (1, 10);",
            "insert into rec_idx values (2, 20);",
            "begin;",
            "update rec_idx set id = 3 where id = 1;",
            "delete from rec_idx where id = 2;",
            "insert into rec_idx values (4, 40);",
            "commit;",
            "begin;",
            "insert into rec_idx values (5, 50);",
            "update rec_idx set id = 6 where id = 3;",
            "delete from rec_idx where id = 4;",
        ]
        after_restart = [
            "select * from rec_idx where id = 3;",
            "select * from rec_idx where id = 2;",
            "select * from rec_idx where id = 4;",
            "select * from rec_idx where id = 5;",
            "select * from rec_idx where id = 6;",
        ]
        expected = [
            "| id | val |",
            "| 3 | 10 |",
            "| id | val |",
            "| id | val |",
            "| 4 | 40 |",
            "| id | val |",
            "| id | val |",
        ]
    elif case.kind == "recovery_script_log_boundary":
        before_crash = [
            "create table rec_ckpt (id int, val int);",
            "insert into rec_ckpt values (1, 10);",
            "begin;",
            "update rec_ckpt set val = 11 where id = 1;",
            "commit;",
            "begin;",
            "insert into rec_ckpt values (2, 20);",
            "commit;",
            "begin;",
            "update rec_ckpt set val = 12 where id = 1;",
            "commit;",
            "begin;",
            "insert into rec_ckpt values (3, 30);",
            "update rec_ckpt set val = 200 where id = 2;",
            "delete from rec_ckpt where id = 1;",
        ]
        after_restart = ["select * from rec_ckpt;"]
        expected = ["| id | val |", "| 1 | 12 |", "| 2 | 20 |"]
    elif case.kind == "recovery_script_restart_new_writes":
        before_crash = [
            "create table rec_restart (id int, val int);",
            "insert into rec_restart values (1, 10);",
            "begin;",
            "insert into rec_restart values (2, 200);",
        ]
        after_restart = [
            "begin;",
            "insert into rec_restart values (2, 20);",
            "update rec_restart set val = 15 where id = 1;",
            "commit;",
            "select * from rec_restart;",
        ]
        expected = ["| id | val |", "| 1 | 15 |", "| 2 | 20 |"]
    elif case.kind == "recovery_script_multitable_atomic":
        before_crash = [
            "create table rec_a (id int, val int);",
            "create table rec_b (id int, val int);",
            "begin;",
            "insert into rec_a values (1, 10);",
            "insert into rec_b values (1, 100);",
            "commit;",
            "begin;",
            "update rec_a set val = 99 where id = 1;",
            "insert into rec_b values (2, 200);",
        ]
        after_restart = [
            "select * from rec_a;",
            "select * from rec_b;",
        ]
        expected = ["| id | val |", "| 1 | 10 |", "| id | val |", "| 1 | 100 |"]
    else:
        raise RuntimeError(f"unknown recovery script case kind: {case.kind}")
    return before_crash, after_restart, expected


def run_recovery_script_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
) -> TestResult:
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    before_crash, after_restart, expected = recovery_script(case)
    server_log = TEST_ROOT / f"{case.name}.server.log"
    proc = start_server(server, build_dir, db_name, server_log)
    client: RmdbClient | None = None
    try:
        wait_for_server(DEFAULT_PORT, connect_timeout)
        client = RmdbClient(DEFAULT_PORT, timeout)
        for statement in before_crash:
            client.execute(statement)
        client.close()
        client = None
        proc.kill()
        proc.wait(timeout=3)
    except Exception as exc:
        if client is not None:
            client.close()
        terminate_server(proc)
        detail = f"runtime error before restart: {exc}"
        if show_server_log and server_log.exists():
            detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)

    restart_log = TEST_ROOT / f"{case.name}.restart.server.log"
    proc = start_server(server, build_dir, db_name, restart_log)
    try:
        wait_for_server(DEFAULT_PORT, max(connect_timeout, 20.0))
        cleanup_outputs(build_dir, db_name)
        client = RmdbClient(DEFAULT_PORT, timeout)
        for statement in after_restart:
            client.execute(statement)
        time.sleep(0.1)
        detail = compare_lines(expected, read_actual_output(build_dir, db_name), case.name)
        return TestResult(case, detail is None, detail or "passed")
    except Exception as exc:
        detail = f"runtime error after restart: {exc}"
        if show_server_log and restart_log.exists():
            detail += "\n" + restart_log.read_text(encoding="utf-8", errors="replace")[-4000:]
        return TestResult(case, False, detail)
    finally:
        if client is not None:
            client.close()
        terminate_server(proc)


def run_recovery_case(
    case: TestCase,
    server: Path,
    build_dir: Path,
    timeout: float,
    connect_timeout: float,
    keep_db: bool,
    show_server_log: bool,
    rows: int,
) -> TestResult:
    if case.kind.startswith("recovery_script_"):
        return run_recovery_script_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)

    use_index = case.kind == "recovery_index"
    workers = 4 if case.kind in {"recovery_multi", "recovery_large"} else 1
    actual_rows = rows
    if case.kind == "recovery_single":
        actual_rows = max(20, rows // 10)
    elif case.kind in {"recovery_index", "recovery_multi"}:
        actual_rows = max(50, rows // 4)
    elif case.kind in {"recovery_large", "recovery_single_2"}:
        actual_rows = max(rows, 200)

    ok, detail, _ = run_recovery_trial(
        case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
        actual_rows, use_index, workers, validate_query=True
    )
    return TestResult(case, ok, detail)


def run_case(case: TestCase, server: Path, build_dir: Path, timeout: float, connect_timeout: float,
             keep_db: bool, show_server_log: bool, recovery_row_count: int = 200) -> TestResult:
    if case.kind == "join_composite_prefix":
        return run_composite_prefix_join_case(
            case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log
        )
    if case.name in {"03_01_index_ddl", "03_02_index_query", "03_03_index_maintenance"}:
        return run_index_compat_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.name in {"02_02_insert_select", "05_04_order_limit"}:
        return run_static_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.kind.startswith("txn_"):
        return run_transaction_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log)
    if case.kind == "final_isolation":
        try:
            run_final_isolation_history(case.name, server, build_dir, timeout, keep_db)
        except Exception as exc:
            return TestResult(case, False, f"runtime error: {exc}")
        return TestResult(case, True, "passed")
    if case.kind.startswith("recovery_"):
        return run_recovery_case(case, server, build_dir, timeout, connect_timeout, keep_db, show_server_log,
                                 recovery_row_count)
    assert case.sql_path is not None
    assert case.expected_path is not None
    db_name = f"doc_test_{case.name}"
    cleanup_outputs(build_dir, db_name)
    if not keep_db:
        cleanup_db(build_dir, db_name)

    server_log = TEST_ROOT / f"{case.name}.server.log"
    with server_log.open("w", encoding="utf-8") as log_file:
        proc = subprocess.Popen(
            [str(server), db_name],
            cwd=build_dir,
            stdout=log_file,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_server(DEFAULT_PORT, connect_timeout)
            statements = split_sql(case.sql_path.read_text(encoding="utf-8"))
            send_sql(statements, DEFAULT_PORT, timeout)
            time.sleep(0.1)
        except Exception as exc:
            terminate_server(proc)
            detail = f"runtime error: {exc}"
            if show_server_log and server_log.exists():
                detail += "\n" + server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
            return TestResult(case, False, detail)
        finally:
            terminate_server(proc)

    preserve_table_order = preserve_table_order_for_case(case.name)
    preserve_plan_indentation = preserve_plan_indentation_for_case(case.name)
    expected = canonicalize_case_output(
        case.name,
        normalize_output(
            case.expected_path.read_text(encoding="utf-8"),
            preserve_table_order=preserve_table_order,
            preserve_plan_indentation=preserve_plan_indentation,
        ),
    )
    actual = canonicalize_case_output(
        case.name,
        normalize_output(
            read_actual_output(build_dir, db_name),
            preserve_table_order=preserve_table_order,
            preserve_plan_indentation=preserve_plan_indentation,
        ),
    )
    if expected == actual:
        if not show_server_log and server_log.exists():
            server_log.unlink()
        if not keep_db:
            cleanup_db(build_dir, db_name)
        return TestResult(case, True, "passed")

    diff = "\n".join(
        difflib.unified_diff(
            expected,
            actual,
            fromfile=f"expected/{case.name}.expected",
            tofile=f"actual/{case.name}.output",
            lineterm="",
        )
    )
    detail = diff or "output mismatch"
    if show_server_log and server_log.exists():
        detail += "\n\n--- server log tail ---\n"
        detail += server_log.read_text(encoding="utf-8", errors="replace")[-4000:]
    return TestResult(case, False, detail)


def main() -> int:
    args = parse_args()
    build_dir = args.build_dir.resolve()
    server = (args.server or (build_dir / "bin" / "rmdb")).resolve()
    cases = discover_cases(args.cases)

    if args.list:
        for case in cases:
            print(case.name)
        return 0

    if not cases:
        print("No test cases selected.", file=sys.stderr)
        return 2
    if not server.exists():
        print(f"Server binary not found: {server}", file=sys.stderr)
        print("Build it first, e.g. cmake --build build --target rmdb", file=sys.stderr)
        return 2
    if not build_dir.exists():
        print(f"Build directory not found: {build_dir}", file=sys.stderr)
        return 2

    results: list[TestResult] = []
    for case in cases:
        if not args.quiet:
            print(f"[ RUN      ] {case.name}")
        result = run_case(
            case,
            server,
            build_dir,
            args.timeout,
            args.connect_timeout,
            args.keep_db,
            args.show_server_log,
            args.recovery_row_count,
        )
        results.append(result)
        status = "       OK " if result.passed else "  FAILED "
        if not args.quiet:
            print(f"[{status}] {case.name}")
        if not result.passed:
            if args.quiet:
                print(f"[{status}] {case.name}")
            print(result.detail)

    failed = [result for result in results if not result.passed]
    if not args.quiet:
        print(f"\nSummary: {len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
