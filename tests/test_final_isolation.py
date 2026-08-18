#!/usr/bin/env python3
"""SI/SER behavior matrix.

These cases use independent local histories. They cover the supported
configuration and isolation-level combinations without depending on a fixed
workload trace.
"""

from __future__ import annotations

import argparse
import shutil
import signal
import socket
import struct
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build"
PORT = 8765
HANDSHAKE = b"RMDB" + struct.pack("!HH", 3, 0)

CONFIG_CASES = (
    "09_cfg_01_snapshot_persists",
    "09_cfg_02_serializable_persists",
    "09_cfg_03_abort_keeps_configuration",
)

SI_SCENARIOS = (
    "insert_self_visibility",
    "uncommitted_insert_hidden",
    "committed_insert_excluded_from_old_snapshot",
    "dirty_update_hidden",
    "repeatable_read_after_update",
    "repeatable_read_after_delete",
    "phantom_insert_excluded",
    "self_update_visibility",
    "self_delete_visibility",
    "rollback_insert",
    "rollback_update",
    "rollback_delete",
    "active_update_conflict",
    "stale_update_conflict",
    "stale_delete_conflict",
    "multirow_statement_atomicity",
    "indexed_write_conflict",
    "disjoint_row_updates",
)
SI_CASES = tuple(f"09_si_{index:02d}_{name}" for index, name in enumerate(SI_SCENARIOS, 1))

SER_SCENARIOS = (
    "insert_visibility",
    "dirty_update_hidden",
    "repeatable_read_after_update",
    "repeatable_read_after_delete",
    "self_write_visibility",
    "rollback_cleanup",
    "active_write_conflict",
    "stale_write_conflict",
    "disjoint_row_updates",
    "record_write_skew_update",
    "record_write_skew_delete",
    "predicate_phantom_insert",
    "empty_predicate_insert",
    "update_enters_predicate",
    "update_leaves_predicate",
    "delete_matches_predicate",
    "select_detects_invisible_writer",
    "three_transaction_dangerous_chain",
    "commit_order_dangerous_chain",
    "single_rw_dependency_allowed",
    "nonoverlapping_transactions_allowed",
    "read_only_transaction_allowed",
    "aborted_reader_dependency_cleanup",
    "aborted_writer_dependency_cleanup",
    "committed_dependency_cleanup",
    "indexed_predicate_conflict",
    "multirow_predicate_conflict",
)
SER_CASES = tuple(f"09_ser_{index:02d}_{name}" for index, name in enumerate(SER_SCENARIOS, 1))
ALL_CASES = CONFIG_CASES + SI_CASES + SER_CASES


def read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError("connection closed during a Wire field")
        data.extend(chunk)
    return bytes(data)


def read_frame(sock: socket.socket) -> tuple[int, int, int, bytes]:
    size, tag, flags, reserved = struct.unpack("!IBBH", read_exact(sock, 8))
    return tag, flags, reserved, read_exact(sock, size)


class WireClient:
    def __init__(self, timeout: float):
        self.sock = socket.create_connection(("127.0.0.1", PORT), timeout=timeout)
        self.sock.settimeout(timeout)
        self.sock.sendall(HANDSHAKE)
        if read_exact(self.sock, 8) != HANDSHAKE:
            raise RuntimeError("Wire v3 handshake mismatch")

    def close(self) -> None:
        self.sock.close()

    def execute(self, sql: str) -> list[tuple[int, int, int, bytes]]:
        payload = sql.encode("utf-8")
        self.sock.sendall(struct.pack("!IBBH", len(payload), 0x20, 0, 0) + payload)
        frames = []
        while True:
            frame = read_frame(self.sock)
            frames.append(frame)
            if frame[0] in {0x10, 0x11, 0x12, 0x13}:
                return frames


def expect_ok(client: WireClient, sql: str) -> None:
    frames = client.execute(sql)
    if frames != [(0x10, 0, 0, b"")]:
        raise AssertionError((sql, frames))


def expect_abort(client: WireClient, sql: str) -> None:
    frames = client.execute(sql)
    if len(frames) != 1 or frames[0][0] != 0x12:
        raise AssertionError((sql, "expected TRANSACTION_ABORT", frames))


def decode_query(frames: list[tuple[int, int, int, bytes]]) -> list[tuple[object, ...]]:
    if not frames or frames[0][0] != 0x01 or frames[-1][0] != 0x11:
        raise AssertionError(("invalid query frames", frames))
    meta = frames[0][3]
    count = struct.unpack("!H", meta[:2])[0]
    offset = 2
    types = []
    for _ in range(count):
        length = struct.unpack("!H", meta[offset : offset + 2])[0]
        offset += 2 + length
        types.append(meta[offset])
        offset += 1
    rows = []
    for tag, _, _, payload in frames[1:-1]:
        if tag != 0x02:
            raise AssertionError(("unexpected query frame", tag))
        values = []
        offset = 0
        for value_type in types:
            present = payload[offset]
            offset += 1
            if not present:
                values.append(None)
            elif value_type == 1:
                values.append(struct.unpack("!i", payload[offset : offset + 4])[0])
                offset += 4
            elif value_type == 2:
                values.append(struct.unpack("!f", payload[offset : offset + 4])[0])
                offset += 4
            else:
                length = struct.unpack("!I", payload[offset : offset + 4])[0]
                offset += 4
                values.append(payload[offset : offset + length].decode("utf-8"))
                offset += length
        rows.append(tuple(values))
    return rows


def query(client: WireClient, sql: str) -> list[tuple[object, ...]]:
    return decode_query(client.execute(sql))


def configure(client: WireClient, model: str) -> None:
    level = "snapshot isolation" if model == "si" else "serializable"
    expect_ok(client, f"set transaction isolation level {level};")


def setup_base(admin: WireClient, *, indexed: bool = False) -> None:
    expect_ok(admin, "create table iso_t (id int, a int, b int);")
    if indexed:
        expect_ok(admin, "create index iso_t(id);")
    expect_ok(admin, "insert into iso_t values (1, 10, 100);")
    expect_ok(admin, "insert into iso_t values (2, 20, 200);")


def clients(timeout: float, model: str, count: int = 2) -> list[WireClient]:
    result = [WireClient(timeout) for _ in range(count)]
    for client in result:
        configure(client, model)
    return result


def assert_final(timeout: float, expected: list[tuple[int, int, int]]) -> None:
    verifier = WireClient(timeout)
    try:
        actual = query(verifier, "select id, a, b from iso_t order by id;")
        if actual != expected:
            raise AssertionError(("final rows", expected, actual))
    finally:
        verifier.close()


def run_configuration(name: str, timeout: float) -> None:
    admin = WireClient(timeout)
    try:
        setup_base(admin)
        model = "ser" if "serializable" in name else "si"
        configure(admin, model)
        expect_ok(admin, "begin;")
        expect_ok(admin, "update iso_t set a = 11 where id = 1;")
        if "abort" in name:
            expect_ok(admin, "abort;")
            expect_ok(admin, "begin;")
            expect_ok(admin, "update iso_t set a = 12 where id = 1;")
        expect_ok(admin, "commit;")
        expect_ok(admin, "begin;")
        expect_ok(admin, "update iso_t set b = 101 where id = 1;")
        expect_ok(admin, "commit;")
    finally:
        admin.close()
    expected_a = 12 if "abort" in name else 11
    assert_final(timeout, [(1, expected_a, 101), (2, 20, 200)])


def run_si(name: str, timeout: float, model: str = "si") -> None:
    scenario = name.split("_", 4)[4]
    admin = WireClient(timeout)
    setup_base(admin, indexed="indexed" in scenario)
    admin.close()
    t1, t2 = clients(timeout, model)
    try:
        if scenario in {"insert_self_visibility", "uncommitted_insert_hidden", "committed_insert_excluded_from_old_snapshot"}:
            expect_ok(t2, "begin;")
            expect_ok(t1, "begin;")
            expect_ok(t1, "insert into iso_t values (3, 30, 300);")
            if query(t1, "select id, a, b from iso_t where id = 3;") != [(3, 30, 300)]:
                raise AssertionError("self insert is not visible")
            if query(t2, "select id from iso_t where id = 3;") != []:
                raise AssertionError("uncommitted insert leaked")
            expect_ok(t1, "commit;")
            if query(t2, "select id from iso_t where id = 3;") != []:
                raise AssertionError("old snapshot observed committed insert")
            expect_ok(t2, "commit;")
            expected = [(1, 10, 100), (2, 20, 200), (3, 30, 300)]
        elif scenario in {"dirty_update_hidden", "repeatable_read_after_update"}:
            expect_ok(t2, "begin;")
            expect_ok(t1, "begin;")
            expect_ok(t1, "update iso_t set a = 11 where id = 1;")
            if query(t1, "select a from iso_t where id = 1;") != [(11,)]:
                raise AssertionError("self update is not visible")
            if query(t2, "select a from iso_t where id = 1;") != [(10,)]:
                raise AssertionError("dirty update leaked")
            expect_ok(t1, "commit;")
            if query(t2, "select a from iso_t where id = 1;") != [(10,)]:
                raise AssertionError("repeatable read changed")
            expect_ok(t2, "commit;")
            expected = [(1, 11, 100), (2, 20, 200)]
        elif scenario in {"repeatable_read_after_delete", "phantom_insert_excluded"}:
            expect_ok(t2, "begin;")
            query(t2, "select id from iso_t where id >= 1;")
            expect_ok(t1, "begin;")
            if "delete" in scenario:
                expect_ok(t1, "delete from iso_t where id = 2;")
                expected = [(1, 10, 100)]
            else:
                expect_ok(t1, "insert into iso_t values (3, 30, 300);")
                expected = [(1, 10, 100), (2, 20, 200), (3, 30, 300)]
            expect_ok(t1, "commit;")
            visible = query(t2, "select id from iso_t where id >= 1 order by id;")
            if visible != [(1,), (2,)]:
                raise AssertionError(("snapshot scan changed", visible))
            expect_ok(t2, "commit;")
        elif scenario in {"self_update_visibility", "self_delete_visibility"}:
            expect_ok(t1, "begin;")
            if "delete" in scenario:
                expect_ok(t1, "delete from iso_t where id = 1;")
                if query(t1, "select id from iso_t where id = 1;") != []:
                    raise AssertionError("self delete remained visible")
                expected = [(2, 20, 200)]
            else:
                expect_ok(t1, "update iso_t set a = 12 where id = 1;")
                if query(t1, "select a from iso_t where id = 1;") != [(12,)]:
                    raise AssertionError("self update missing")
                expected = [(1, 12, 100), (2, 20, 200)]
            expect_ok(t1, "commit;")
        elif scenario.startswith("rollback_"):
            expect_ok(t1, "begin;")
            if scenario.endswith("insert"):
                expect_ok(t1, "insert into iso_t values (3, 30, 300);")
            elif scenario.endswith("update"):
                expect_ok(t1, "update iso_t set a = 99 where id = 1;")
            else:
                expect_ok(t1, "delete from iso_t where id = 1;")
            expect_ok(t1, "abort;")
            expected = [(1, 10, 100), (2, 20, 200)]
        elif scenario in {"active_update_conflict", "indexed_write_conflict"}:
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            expect_ok(t1, "update iso_t set a = 11 where id = 1;")
            expect_abort(t2, "update iso_t set b = 101 where id = 1;")
            expect_ok(t1, "commit;")
            expected = [(1, 11, 100), (2, 20, 200)]
        elif scenario in {"stale_update_conflict", "stale_delete_conflict", "multirow_statement_atomicity"}:
            expect_ok(t2, "begin;")
            query(t2, "select id, a, b from iso_t;")
            expect_ok(t1, "begin;")
            expect_ok(t1, "update iso_t set a = 21 where id = 2;")
            expect_ok(t1, "commit;")
            if scenario == "stale_delete_conflict":
                expect_abort(t2, "delete from iso_t where id = 2;")
            elif scenario == "multirow_statement_atomicity":
                expect_abort(t2, "update iso_t set b = 999 where id >= 1;")
            else:
                expect_abort(t2, "update iso_t set b = 201 where id = 2;")
            expected = [(1, 10, 100), (2, 21, 200)]
        else:
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            expect_ok(t1, "update iso_t set a = 11 where id = 1;")
            expect_ok(t2, "update iso_t set a = 21 where id = 2;")
            expect_ok(t1, "commit;")
            expect_ok(t2, "commit;")
            expected = [(1, 11, 100), (2, 21, 200)]
    finally:
        t1.close()
        t2.close()
    assert_final(timeout, expected)


def run_ser_base(scenario: str, timeout: float) -> bool:
    mapping = {
        "insert_visibility": "insert_self_visibility",
        "dirty_update_hidden": "dirty_update_hidden",
        "repeatable_read_after_update": "repeatable_read_after_update",
        "repeatable_read_after_delete": "repeatable_read_after_delete",
        "self_write_visibility": "self_update_visibility",
        "rollback_cleanup": "rollback_update",
        "active_write_conflict": "active_update_conflict",
        "stale_write_conflict": "stale_update_conflict",
        "disjoint_row_updates": "disjoint_row_updates",
    }
    if scenario not in mapping:
        return False
    # The SI histories only differ in the configured level; SER inherits SI.
    synthetic = "09_si_00_" + mapping[scenario]
    run_si(synthetic, timeout, "ser")
    return True


def run_ser(name: str, timeout: float) -> None:
    scenario = name.split("_", 4)[4]
    if run_ser_base(scenario, timeout):
        return
    admin = WireClient(timeout)
    setup_base(admin, indexed="indexed" in scenario)
    admin.close()
    session_count = 3 if "three_transaction" in scenario else 2
    sessions = clients(timeout, "ser", session_count)
    t1, t2 = sessions[0], sessions[1]
    try:
        if scenario in {"single_rw_dependency_allowed", "nonoverlapping_transactions_allowed", "read_only_transaction_allowed"}:
            expect_ok(t1, "begin;")
            query(t1, "select id from iso_t where id = 1;")
            if scenario == "nonoverlapping_transactions_allowed":
                expect_ok(t1, "commit;")
                expect_ok(t2, "begin;")
                expect_ok(t2, "update iso_t set a = 11 where id = 1;")
                expect_ok(t2, "commit;")
            elif scenario == "read_only_transaction_allowed":
                expect_ok(t2, "begin;")
                query(t2, "select id from iso_t where id = 2;")
                expect_ok(t1, "commit;")
                expect_ok(t2, "commit;")
            else:
                expect_ok(t2, "begin;")
                expect_ok(t2, "update iso_t set a = 11 where id = 1;")
                expect_ok(t2, "commit;")
                expect_ok(t1, "commit;")
            expected = [(1, 11 if scenario != "read_only_transaction_allowed" else 10, 100), (2, 20, 200)]
        elif scenario in {"aborted_reader_dependency_cleanup", "aborted_writer_dependency_cleanup", "committed_dependency_cleanup"}:
            expect_ok(t1, "begin;")
            query(t1, "select id from iso_t where a >= 10;")
            if scenario == "aborted_reader_dependency_cleanup":
                expect_ok(t1, "abort;")
            else:
                expect_ok(t2, "begin;")
                expect_ok(t2, "update iso_t set a = 11 where id = 1;")
                if scenario == "aborted_writer_dependency_cleanup":
                    expect_ok(t2, "abort;")
                else:
                    expect_ok(t2, "commit;")
                expect_ok(t1, "commit;")
            fresh = WireClient(timeout)
            configure(fresh, "ser")
            expect_ok(fresh, "begin;")
            expect_ok(fresh, "update iso_t set b = 101 where id = 1;")
            expect_ok(fresh, "commit;")
            fresh.close()
            expected_a = 11 if scenario == "committed_dependency_cleanup" else 10
            expected = [(1, expected_a, 101), (2, 20, 200)]
        elif scenario == "three_transaction_dangerous_chain":
            t3 = sessions[2]
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            expect_ok(t3, "begin;")
            query(t1, "select id from iso_t where id = 1;")
            query(t2, "select id from iso_t where id = 2;")
            expect_ok(t3, "update iso_t set a = 11 where id = 1;")
            expect_ok(t3, "commit;")
            expect_ok(t1, "update iso_t set a = 21 where id = 2;")
            expect_abort(t2, "select id from iso_t where id = 1;")
            expect_ok(t1, "commit;")
            expected = [(1, 11, 100), (2, 21, 200)]
        elif scenario in {"predicate_phantom_insert", "empty_predicate_insert"}:
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            predicate = "a > 1000" if scenario == "empty_predicate_insert" else "a >= 10"
            query(t1, f"select id from iso_t where {predicate};")
            query(t2, f"select id from iso_t where {predicate};")
            if scenario == "empty_predicate_insert":
                expect_ok(t1, "insert into iso_t values (3, 2001, 300);")
                expect_abort(t2, "insert into iso_t values (4, 2002, 400);")
                expected = [(1, 10, 100), (2, 20, 200), (3, 2001, 300)]
            else:
                expect_ok(t1, "insert into iso_t values (3, 30, 300);")
                expect_abort(t2, "insert into iso_t values (4, 40, 400);")
                expected = [(1, 10, 100), (2, 20, 200), (3, 30, 300)]
            expect_ok(t1, "commit;")
        elif scenario in {"update_enters_predicate", "update_leaves_predicate"}:
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            if scenario == "update_enters_predicate":
                query(t1, "select id from iso_t where a >= 15;")
                query(t2, "select id from iso_t where a < 15;")
                expect_ok(t1, "update iso_t set a = 16 where id = 1;")
                expect_abort(t2, "update iso_t set a = 14 where id = 2;")
                expected = [(1, 16, 100), (2, 20, 200)]
            else:
                query(t1, "select id from iso_t where a < 15;")
                query(t2, "select id from iso_t where a >= 15;")
                expect_ok(t1, "update iso_t set a = 5 where id = 2;")
                expect_abort(t2, "update iso_t set a = 25 where id = 1;")
                expected = [(1, 10, 100), (2, 5, 200)]
            expect_ok(t1, "commit;")
        elif scenario == "select_detects_invisible_writer":
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            query(t1, "select id from iso_t where id = 1;")
            expect_ok(t2, "update iso_t set a = 11 where id = 1;")
            expect_ok(t1, "update iso_t set a = 21 where id = 2;")
            expect_abort(t2, "select id from iso_t where id = 2;")
            expect_ok(t1, "commit;")
            expected = [(1, 10, 100), (2, 21, 200)]
        elif scenario == "commit_order_dangerous_chain":
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            query(t1, "select id from iso_t where id = 1;")
            query(t2, "select id from iso_t where id = 2;")
            expect_ok(t1, "update iso_t set a = 21 where id = 2;")
            expect_ok(t1, "commit;")
            expect_abort(t2, "update iso_t set b = 101 where id = 1;")
            expected = [(1, 10, 100), (2, 21, 200)]
        else:
            # Two record/predicate reads followed by crossing writes form the
            # canonical two-session SSI dangerous structure.
            expect_ok(t1, "begin;")
            expect_ok(t2, "begin;")
            if scenario == "indexed_predicate_conflict":
                predicate = "id >= 1"
            elif scenario == "multirow_predicate_conflict":
                predicate = "a >= 10"
            else:
                predicate = "id = 1"
            query(t1, f"select id from iso_t where {predicate};")
            query(t2, "select id from iso_t where id = 2;")
            if scenario in {"record_write_skew_delete", "delete_matches_predicate"}:
                expect_ok(t1, "delete from iso_t where id = 2;")
                expect_abort(t2, "delete from iso_t where id = 1;")
                expected = [(1, 10, 100)]
            else:
                expect_ok(t1, "update iso_t set a = 21 where id = 2;")
                expect_abort(t2, "update iso_t set b = 101 where id = 1;")
                expected = [(1, 10, 100), (2, 21, 200)]
            expect_ok(t1, "commit;")
    finally:
        for session in sessions:
            session.close()
    assert_final(timeout, expected)


def wait_for_server(timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            probe = WireClient(0.5)
            probe.close()
            return
        except (OSError, RuntimeError):
            time.sleep(0.05)
    raise RuntimeError("rmdb did not become Wire-ready")


def terminate(proc: subprocess.Popen[str]) -> None:
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)


def run_case(name: str, server: Path, build: Path, timeout: float, keep_db: bool) -> None:
    db_name = "final_iso_" + name
    db_path = build / db_name
    if not keep_db:
        shutil.rmtree(db_path, ignore_errors=True)
    log_path = ROOT / "tests" / f"{name}.server.log"
    with log_path.open("w", encoding="utf-8") as log:
        proc = subprocess.Popen([str(server), db_name], cwd=build, stdout=log, stderr=subprocess.STDOUT, text=True)
    try:
        wait_for_server(timeout)
        if name in CONFIG_CASES:
            run_configuration(name, timeout)
        elif name in SI_CASES:
            run_si(name, timeout)
        else:
            run_ser(name, timeout)
    finally:
        terminate(proc)
    if not keep_db:
        shutil.rmtree(db_path, ignore_errors=True)
        log_path.unlink(missing_ok=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the public-contract 3+18+27 isolation matrix.")
    parser.add_argument("cases", nargs="*")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--server", type=Path)
    parser.add_argument("--timeout", type=float, default=20.0)
    parser.add_argument("--keep-db", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    selected = [name for name in ALL_CASES if not args.cases or any(token in name for token in args.cases)]
    if args.list:
        print("\n".join(selected))
        return 0
    build = args.build_dir.resolve()
    server = (args.server or build / "bin" / "rmdb").resolve()
    if not server.exists():
        raise SystemExit(f"server binary not found: {server}")
    for name in selected:
        print(f"[ RUN      ] {name}")
        run_case(name, server, build, args.timeout, args.keep_db)
        print(f"[       OK ] {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
