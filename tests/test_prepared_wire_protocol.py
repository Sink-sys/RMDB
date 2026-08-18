#!/usr/bin/env python3

import os
import shutil
import signal
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("RMDB_TEST_BUILD", ROOT / "build"))
if not BUILD.is_absolute():
    BUILD = ROOT / BUILD
SERVER = BUILD / "bin" / "rmdb"
HANDSHAKE = b"RMDB" + struct.pack("!HH", 3, 0)

INT32 = 1
FLOAT32 = 2
CHAR = 3
ROLLBACK_RACE_INDEX_COUNT = 32
ROLLBACK_RACE_SECONDS = 2.0
COMMIT_FRONTIER_ATTEMPTS = 20
COMMIT_FRONTIER_FILLER_BATCHES = 6
COMMIT_FRONTIER_FILLER_ROWS = 240
COMMIT_FRONTIER_PAD = "x" * 500
PAYMENT_BEFORE_BITS = 0x4A414103
PAYMENT_BOUND_BITS = 0x4554CC7B
PAYMENT_LEAD_BITS = 0x43D7A000


def read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise AssertionError("connection closed during a protocol field")
        data.extend(chunk)
    return bytes(data)


def read_frame(sock: socket.socket) -> tuple[int, int, int, bytes]:
    size, tag, flags, reserved = struct.unpack("!IBBH", read_exact(sock, 8))
    return tag, flags, reserved, read_exact(sock, size)


def send_frame(sock: socket.socket, tag: int, flags: int, payload: bytes):
    frame = struct.pack("!IBBH", len(payload), tag, flags, 0) + payload
    for offset in range(0, len(frame), 5):
        sock.sendall(frame[offset : offset + 5])
    return read_frame(sock)


def exec_stream(sock: socket.socket, sql: str):
    payload = sql.encode()
    sock.sendall(struct.pack("!IBBH", len(payload), 0x20, 0, 0) + payload)
    frames = []
    while True:
        frame = read_frame(sock)
        frames.append(frame)
        if frame[0] in {0x10, 0x11, 0x12, 0x13}:
            return frames


def connect_wire() -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", 8765), timeout=5)
    sock.settimeout(5)
    sock.sendall(HANDSHAKE)
    assert read_exact(sock, len(HANDSHAKE)) == HANDSHAKE
    return sock


def wait_for_server() -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            with connect_wire():
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("rmdb did not listen on port 8765")


def prepare_entry(
    statement_id: int, is_query: bool, parameter_types: list[int], sql: str
) -> bytes:
    encoded = sql.encode()
    return (
        struct.pack("!HBH", statement_id, int(is_query), len(parameter_types))
        + bytes(parameter_types)
        + struct.pack("!I", len(encoded))
        + encoded
    )


def prepare_set(sock: socket.socket, entries: list[bytes]):
    return send_frame(sock, 0x21, 0, struct.pack("!H", len(entries)) + b"".join(entries))


def typed_cell(sql_type: int, value) -> bytes:
    if value is None:
        return b"\x00"
    if sql_type == INT32:
        return b"\x01" + struct.pack("!i", value)
    if sql_type == FLOAT32:
        return b"\x01" + struct.pack("!f", value)
    encoded = value.encode()
    return b"\x01" + struct.pack("!I", len(encoded)) + encoded


def float32(value: float) -> float:
    return struct.unpack("!f", struct.pack("!f", value))[0]


def float32_result(value: float):
    return ("float_bits", struct.unpack("!I", struct.pack("!f", value))[0])


def float32_from_bits(bits: int) -> float:
    return struct.unpack("!f", struct.pack("!I", bits))[0]


def add_float32_bits(left_bits: int, right_bits: int) -> int:
    return float32_result(
        float32(float32_from_bits(left_bits) + float32_from_bits(right_bits))
    )[1]


def operation(statement_id: int, cells: list[bytes] = None) -> bytes:
    return struct.pack("!H", statement_id) + b"".join(cells or [])


def exec_batch(sock: socket.socket, operations: list[bytes]):
    payload = struct.pack("!H", len(operations)) + b"".join(operations)
    return send_frame(sock, 0x22, 0x01, payload)


def send_exec_batch(sock: socket.socket, operations: list[bytes]) -> None:
    payload = struct.pack("!H", len(operations)) + b"".join(operations)
    sock.sendall(struct.pack("!IBBH", len(payload), 0x22, 0x01, 0) + payload)


def exec_batch_unfragmented(sock: socket.socket, operations: list[bytes]):
    send_exec_batch(sock, operations)
    return read_frame(sock)


def decode_cell(payload: bytes, offset: int, sql_type: int):
    present = payload[offset]
    offset += 1
    assert present in {0, 1}
    if present == 0:
        return None, offset
    if sql_type == INT32:
        return struct.unpack("!i", payload[offset : offset + 4])[0], offset + 4
    if sql_type == FLOAT32:
        bits = struct.unpack("!I", payload[offset : offset + 4])[0]
        return ("float_bits", bits), offset + 4
    length = struct.unpack("!I", payload[offset : offset + 4])[0]
    offset += 4
    return payload[offset : offset + length].decode(), offset + length


def decode_batch_result(
    payload: bytes, query_types_by_operation: dict[int, list[int]]
):
    executed, status, failed, diagnostic_size = struct.unpack("!HBHI", payload[:9])
    offset = 9
    diagnostic = payload[offset : offset + diagnostic_size].decode()
    offset += diagnostic_size
    result_count = struct.unpack("!H", payload[offset : offset + 2])[0]
    offset += 2
    results = []
    for _ in range(result_count):
        operation_index, row_count = struct.unpack("!HI", payload[offset : offset + 6])
        offset += 6
        rows = []
        for _ in range(row_count):
            row = []
            for sql_type in query_types_by_operation[operation_index]:
                value, offset = decode_cell(payload, offset, sql_type)
                row.append(value)
            rows.append(row)
        results.append((operation_index, rows))
    assert offset == len(payload)
    return executed, status, failed, diagnostic, results


def assert_ok_batch(
    frame, operation_count: int, query_types_by_operation: dict[int, list[int]]
):
    assert frame[0:3] == (0x15, 0, 0), frame
    decoded = decode_batch_result(frame[3], query_types_by_operation)
    assert decoded[0:4] == (operation_count, 0, 0xFFFF, ""), decoded
    return decoded[4]


def main() -> None:
    if not SERVER.exists():
        raise SystemExit("build/bin/rmdb is missing; build target rmdb first")

    db_name = "prepared_wire_protocol_test_db"
    db_path = BUILD / db_name
    shutil.rmtree(db_path, ignore_errors=True)
    server = subprocess.Popen(
        [str(SERVER), db_name],
        cwd=BUILD,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        wait_for_server()
        with connect_wire() as sock:
            for sql in (
                "create table prepared_left "
                "(id int, join_key int, scope int, token char(16), marker char(16));",
                "create table prepared_right "
                "(id int, join_key int, amount float, token char(16));",
                "create table prepared_txn "
                "(id int, qty int, amount float, note char(16));",
                "create table prepared_order_line "
                "(ol_w_id int, ol_d_id int, ol_o_id int, ol_i_id int);",
                "create table prepared_warm_line "
                "(w_id int, order_id int, item_id int);",
                "create table prepared_warm_stock "
                "(w_id int, item_id int, quantity int);",
                "create table prepared_payment_warehouse "
                "(w_id int, w_name char(10), w_street_1 char(20), "
                "w_street_2 char(20), w_city char(20), w_state char(2), "
                "w_zip char(9), w_tax float, w_ytd float);",
                "create table prepared_payment_district "
                "(d_w_id int, d_id int, d_ytd float, d_next_o_id int);",
                "create table prepared_payment_customer "
                "(c_w_id int, c_d_id int, c_id int, c_balance float, "
                "c_ytd_payment float, c_payment_cnt int);",
                "create table prepared_payment_history "
                "(h_id int, h_w_id int, h_d_id int, h_c_id int, h_amount float);",
                "create table prepared_commit_frontier "
                "(id int, amount float);",
                "create table prepared_commit_wal "
                "(seq int, pad char(500));",
                "create index prepared_txn(id);",
                "create index prepared_order_line(ol_w_id, ol_d_id, ol_o_id);",
                "create index prepared_warm_stock(w_id, item_id);",
                "create index prepared_payment_warehouse(w_id);",
                "create index prepared_payment_district(d_w_id, d_id);",
                "create index prepared_payment_customer(c_w_id, c_d_id, c_id);",
                "create index prepared_payment_history(h_id);",
                "create index prepared_commit_frontier(id);",
                "insert into prepared_left values (1, 10, 7, 'alpha', '$3');",
                "insert into prepared_left values (2, 20, 8, 'beta', '$3');",
                "insert into prepared_right values (11, 10, 1.25, 'alpha');",
                "insert into prepared_right values (22, 20, 2.5, 'beta');",
                "insert into prepared_order_line values (1, 2, 19, 19);",
                "insert into prepared_order_line values (1, 2, 20, 20);",
                "insert into prepared_order_line values (1, 2, 39, 39);",
                "insert into prepared_order_line values (1, 2, 40, 40);",
                "insert into prepared_order_line values (1, 2, 49, 49);",
                "insert into prepared_order_line values (1, 2, 50, 50);",
                "insert into prepared_warm_line values (1, 30, 101);",
                "insert into prepared_warm_line values (1, 31, 101);",
                "insert into prepared_warm_line values (1, 32, 102);",
                "insert into prepared_warm_line values (1, 10, 103);",
                "insert into prepared_warm_line values (1, 39, 103);",
                "insert into prepared_warm_line values (1, 40, 104);",
                "insert into prepared_warm_line values (2, 25, 101);",
                "insert into prepared_warm_line values (2, 26, 104);",
                "insert into prepared_warm_line values (2, 27, 104);",
                "insert into prepared_warm_line values (2, 39, 105);",
                "insert into prepared_warm_stock values (1, 101, 5);",
                "insert into prepared_warm_stock values (1, 102, 15);",
                "insert into prepared_warm_stock values (1, 103, 20);",
                "insert into prepared_warm_stock values (1, 104, 1);",
                "insert into prepared_warm_stock values (2, 101, 50);",
                "insert into prepared_warm_stock values (2, 104, 7);",
                "insert into prepared_warm_stock values (2, 105, 30);",
                "insert into prepared_payment_warehouse values "
                "(1, 'warehouse1', 'street1', 'street2', 'city', 'ST', "
                "'123456789', 0.1, 300000.0);",
                "insert into prepared_payment_warehouse values "
                "(2, 'warehouse2', 'street1', 'street2', 'city', 'ST', "
                "'123456789', 0.1, 3166272.75);",
                "insert into prepared_payment_warehouse values "
                "(3, 'warehouse3', 'street1', 'street2', 'city', 'ST', "
                "'123456789', 0.1, 3166272.75);",
                "insert into prepared_payment_district values (1, 2, 30000.0, 3001);",
                "insert into prepared_payment_customer "
                "values (1, 2, 3, 100.0, 10.0, 1);",
                "insert into prepared_commit_frontier values (1, 3166272.75);",
                "insert into prepared_commit_frontier values (2, 0.0);",
                "insert into prepared_commit_frontier values (3, 3166272.75);",
            ):
                assert exec_stream(sock, sql) == [(0x10, 0, 0, b"")]

            # Extra prefix indexes widen the real delete/reinsert scheduling
            # window during rollback without adding a server-side test hook.
            rollback_key_columns = ", ".join(
                f"key_{index} int"
                for index in range(1, ROLLBACK_RACE_INDEX_COUNT + 1)
            )
            rollback_values = ", ".join(
                "1" for _ in range(ROLLBACK_RACE_INDEX_COUNT)
            )
            rollback_setup = [
                "create table prepared_rollback_race "
                f"(id int, payload int, {rollback_key_columns});",
                "create index prepared_rollback_race(id);",
                *[
                    "create index prepared_rollback_race"
                    f"(id, key_{index});"
                    for index in range(1, ROLLBACK_RACE_INDEX_COUNT + 1)
                ],
                "insert into prepared_rollback_race values "
                f"(1, 0, {rollback_values});",
                "insert into prepared_rollback_race values "
                f"(2, 7, {rollback_values});",
            ]
            for sql in rollback_setup:
                assert exec_stream(sock, sql) == [(0x10, 0, 0, b"")]

            # The prepared statement dictionary is installed after the connection
            # selects snapshot isolation; this also enables the composite-key
            # join plan exercised by the following operation.
            assert exec_stream(
                sock, "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;"
            ) == [(0x10, 0, 0, b"")]

            entries = [
                prepare_entry(1, False, [], "BEGIN"),
                prepare_entry(2, False, [], "COMMIT"),
                prepare_entry(3, False, [], "ABORT"),
                prepare_entry(
                    10,
                    True,
                    [CHAR, INT32],
                    "SELECT l.id AS left_id, r.amount AS right_amount, "
                    "r.token AS right_token "
                    "FROM prepared_left l, prepared_right r "
                    "WHERE l.join_key = r.join_key AND l.scope = $2 "
                    "AND l.token = $1 AND r.token = $1 AND l.marker = '$3'",
                ),
                prepare_entry(
                    11,
                    False,
                    [INT32, INT32, FLOAT32, CHAR],
                    "INSERT INTO prepared_txn VALUES ($1,$2,$3,$4)",
                ),
                prepare_entry(
                    12,
                    False,
                    [FLOAT32, INT32, INT32],
                    "UPDATE prepared_txn "
                    "SET qty = qty - $2 + 91, amount = amount + $1 "
                    "WHERE id = $3",
                ),
                prepare_entry(
                    13,
                    True,
                    [INT32],
                    "SELECT id, qty, amount, note "
                    "FROM prepared_txn WHERE id = $1",
                ),
                prepare_entry(
                    14,
                    True,
                    [INT32, INT32, INT32],
                    "SELECT ol_i_id FROM prepared_order_line "
                    "WHERE ol_w_id = $1 AND ol_d_id = $2 "
                    "AND ol_o_id < $3 AND ol_o_id >= ($3 - 20) "
                    "ORDER BY ol_i_id",
                ),
                prepare_entry(
                    15,
                    True,
                    [INT32, INT32, INT32],
                    "SELECT COUNT(DISTINCT l.item_id) AS low_stock "
                    "FROM prepared_warm_line l, prepared_warm_stock s "
                    "WHERE l.w_id = $1 AND l.order_id < $2 "
                    "AND l.order_id >= ($2 - 20) AND s.w_id = $1 "
                    "AND s.item_id = l.item_id AND s.quantity < $3",
                ),
            ]
            prepared = prepare_set(sock, entries)
            assert prepared[0:3] == (0x14, 0, 0), prepared
            expected_prepare = (
                struct.pack("!H", len(entries))
                + struct.pack("!HH", 1, 0)
                + struct.pack("!HH", 2, 0)
                + struct.pack("!HH", 3, 0)
                + struct.pack("!HH", 10, 3)
                + struct.pack("!H", 7)
                + b"left_id"
                + bytes([INT32])
                + struct.pack("!H", 12)
                + b"right_amount"
                + bytes([FLOAT32])
                + struct.pack("!H", 11)
                + b"right_token"
                + bytes([CHAR])
                + struct.pack("!HH", 11, 0)
                + struct.pack("!HH", 12, 0)
                + struct.pack("!HH", 13, 4)
                + struct.pack("!H", 2)
                + b"id"
                + bytes([INT32])
                + struct.pack("!H", 3)
                + b"qty"
                + bytes([INT32])
                + struct.pack("!H", 6)
                + b"amount"
                + bytes([FLOAT32])
                + struct.pack("!H", 4)
                + b"note"
                + bytes([CHAR])
                + struct.pack("!HH", 14, 1)
                + struct.pack("!H", 7)
                + b"ol_i_id"
                + bytes([INT32])
                + struct.pack("!HH", 15, 1)
                + struct.pack("!H", 9)
                + b"low_stock"
                + bytes([INT32])
            )
            assert prepared[3] == expected_prepare

            # A failed replacement must leave the old dictionary intact.
            invalid = prepare_set(
                sock,
                [
                    prepare_entry(
                        99,
                        True,
                        [INT32, INT32],
                        "SELECT id FROM prepared_txn WHERE id = $2",
                    )
                ],
            )
            assert invalid[0:3] == (0x13, 0, 0), invalid

            # Reuse the same NLJ plan twice in one batch and again across a batch.
            join_ops = [
                operation(10, [typed_cell(CHAR, "alpha"), typed_cell(INT32, 7)]),
                operation(10, [typed_cell(CHAR, "beta"), typed_cell(INT32, 8)]),
            ]
            join_results = assert_ok_batch(
                exec_batch(sock, join_ops),
                2,
                {0: [INT32, FLOAT32, CHAR], 1: [INT32, FLOAT32, CHAR]},
            )
            assert join_results == [
                (0, [[1, ("float_bits", 0x3FA00000), "alpha"]]),
                (1, [[2, ("float_bits", 0x40200000), "beta"]]),
            ]
            cross_batch = assert_ok_batch(
                exec_batch(
                    sock,
                    [operation(10, [typed_cell(CHAR, "alpha"), typed_cell(INT32, 7)])],
                ),
                1,
                {0: [INT32, FLOAT32, CHAR]},
            )
            assert cross_batch == [
                (0, [[1, ("float_bits", 0x3FA00000), "alpha"]])
            ]

            # Reuse a COUNT(DISTINCT) join whose right-side point key combines
            # a locally bound equality with the left row's join column. The
            # duplicate line items, strict threshold boundary, and warehouse-
            # specific stock rows also guard residual filtering.
            warmup_ops = [
                operation(
                    15,
                    [
                        typed_cell(INT32, 1),
                        typed_cell(INT32, 40),
                        typed_cell(INT32, 16),
                    ],
                ),
                operation(
                    15,
                    [
                        typed_cell(INT32, 2),
                        typed_cell(INT32, 40),
                        typed_cell(INT32, 10),
                    ],
                ),
                operation(
                    15,
                    [
                        typed_cell(INT32, 1),
                        typed_cell(INT32, 40),
                        typed_cell(INT32, 6),
                    ],
                ),
                operation(
                    15,
                    [
                        typed_cell(INT32, 2),
                        typed_cell(INT32, 40),
                        typed_cell(INT32, 7),
                    ],
                ),
            ]
            warmup_results = assert_ok_batch(
                exec_batch(sock, warmup_ops),
                4,
                {0: [INT32], 1: [INT32], 2: [INT32], 3: [INT32]},
            )
            assert warmup_results == [
                (0, [[2]]),
                (1, [[1]]),
                (2, [[1]]),
                (3, [[0]]),
            ]

            # A following batch must see fresh bindings, and its first decoded
            # frame must be the new batch's sole BATCH_RESULT (not a leftover
            # terminal from the previous multi-operation batch).
            warmup_rebound = assert_ok_batch(
                exec_batch(
                    sock,
                    [
                        operation(
                            15,
                            [
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 40),
                                typed_cell(INT32, 21),
                            ],
                        ),
                        operation(
                            15,
                            [
                                typed_cell(INT32, 2),
                                typed_cell(INT32, 40),
                                typed_cell(INT32, 31),
                            ],
                        ),
                    ],
                ),
                2,
                {0: [INT32], 1: [INT32]},
            )
            assert warmup_rebound == [(0, [[3]]), (1, [[2]])]

            # Rebind and recompute a prepared arithmetic range on every batch.
            range_40 = assert_ok_batch(
                exec_batch(
                    sock,
                    [
                        operation(
                            14,
                            [
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 2),
                                typed_cell(INT32, 40),
                            ],
                        )
                    ],
                ),
                1,
                {0: [INT32]},
            )
            assert range_40 == [(0, [[20], [39]])]

            overflow = exec_batch(
                sock,
                [
                    operation(
                        14,
                        [
                            typed_cell(INT32, 1),
                            typed_cell(INT32, 2),
                            typed_cell(INT32, -(2**31)),
                        ],
                    )
                ],
            )
            assert overflow[0:3] == (0x15, 0, 0)
            overflow_result = decode_batch_result(overflow[3], {})
            assert overflow_result[0] == 0
            assert overflow_result[1] == 2
            assert overflow_result[2] == 0
            assert overflow_result[3] and overflow_result[4] == []

            range_50 = assert_ok_batch(
                exec_batch(
                    sock,
                    [
                        operation(
                            14,
                            [
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 2),
                                typed_cell(INT32, 50),
                            ],
                        )
                    ],
                ),
                1,
                {0: [INT32]},
            )
            assert range_50 == [(0, [[39], [40], [49]])]

            literal_range = exec_stream(
                sock,
                "SELECT ol_i_id FROM prepared_order_line "
                "WHERE ol_w_id = 1 AND ol_d_id = 2 "
                "AND ol_o_id < (20 + 20) AND ol_o_id >= (40 - 20);",
            )
            assert literal_range[-1] == (0x11, 0, 0, struct.pack("!Q", 2))

            invalid_arithmetic = prepare_set(
                sock,
                [
                    prepare_entry(
                        99,
                        True,
                        [CHAR],
                        "SELECT ol_i_id FROM prepared_order_line "
                        "WHERE ol_o_id >= ($1 - 20)",
                    )
                ],
            )
            assert invalid_arithmetic[0:3] == (0x13, 0, 0)

            # A successful explicit ABORT is an OK operation and rolls back writes.
            quote = "quo'te"
            abort_ops = [
                operation(1),
                operation(
                    11,
                    [
                        typed_cell(INT32, 7),
                        typed_cell(INT32, 10),
                        typed_cell(FLOAT32, -0.0),
                        typed_cell(CHAR, quote),
                    ],
                ),
                operation(13, [typed_cell(INT32, 7)]),
                operation(3),
            ]
            abort_results = assert_ok_batch(
                exec_batch(sock, abort_ops),
                4,
                {2: [INT32, INT32, FLOAT32, CHAR]},
            )
            assert abort_results == [
                (2, [[7, 10, ("float_bits", 0x80000000), quote]])
            ]
            assert exec_stream(
                sock, "select id from prepared_txn where id = 7;"
            )[-1] == (0x11, 0, 0, struct.pack("!Q", 0))

            # Commit one baseline row, then force a duplicate-key failure after
            # UPDATE and SELECT. No partial result or write may survive.
            committed = [
                operation(1),
                operation(
                    11,
                    [
                        typed_cell(INT32, 8),
                        typed_cell(INT32, 10),
                        typed_cell(FLOAT32, 1.5),
                        typed_cell(CHAR, ""),
                    ],
                ),
                operation(2),
            ]
            assert_ok_batch(exec_batch(sock, committed), 3, {})

            failing = [
                operation(1),
                operation(
                    12,
                    [
                        typed_cell(FLOAT32, 0.5),
                        typed_cell(INT32, 3),
                        typed_cell(INT32, 8),
                    ],
                ),
                operation(13, [typed_cell(INT32, 8)]),
                operation(
                    11,
                    [
                        typed_cell(INT32, 8),
                        typed_cell(INT32, 99),
                        typed_cell(FLOAT32, 9.0),
                        typed_cell(CHAR, "duplicate"),
                    ],
                ),
            ]
            failed_frame = exec_batch(sock, failing)
            assert failed_frame[0:3] == (0x15, 0, 0)
            failed = decode_batch_result(failed_frame[3], {})
            assert failed[0] == 3 and failed[1] == 2 and failed[2] == 3
            assert failed[3] and failed[4] == []
            baseline = assert_ok_batch(
                exec_batch(sock, [operation(13, [typed_cell(INT32, 8)])]),
                1,
                {0: [INT32, INT32, FLOAT32, CHAR]},
            )
            assert baseline == [
                (0, [[8, 10, ("float_bits", 0x3FC00000), ""]])
            ]

            # Decode the whole batch before executing COMMIT. The trailing byte
            # must abort the transaction opened by the previous batch.
            pending = [
                operation(1),
                operation(
                    12,
                    [
                        typed_cell(FLOAT32, 2.0),
                        typed_cell(INT32, 1),
                        typed_cell(INT32, 8),
                    ],
                ),
            ]
            assert_ok_batch(exec_batch(sock, pending), 2, {})
            malformed_payload = struct.pack("!HH", 1, 2) + b"\x7f"
            malformed = send_frame(sock, 0x22, 0x01, malformed_payload)
            assert malformed[0:3] == (0x13, 0, 0), malformed
            unchanged = assert_ok_batch(
                exec_batch(sock, [operation(13, [typed_cell(INT32, 8)])]),
                1,
                {0: [INT32, INT32, FLOAT32, CHAR]},
            )
            assert unchanged == baseline
            assert_ok_batch(exec_batch(sock, [operation(2)]), 1, {})

            # Dictionaries are connection-local even when statement ids match.
            with connect_wire() as other:
                other_prepare = prepare_set(
                    other,
                    [
                        prepare_entry(
                            10,
                            True,
                            [INT32],
                            "SELECT id FROM prepared_txn WHERE id = $1",
                        )
                    ],
                )
                assert other_prepare[0:3] == (0x14, 0, 0)
                other_result = assert_ok_batch(
                    exec_batch(other, [operation(10, [typed_cell(INT32, 8)])]),
                    1,
                    {0: [INT32]},
                )
                assert other_result == [(0, [[8]])]
            assert_ok_batch(
                exec_batch(
                    sock,
                    [operation(10, [typed_cell(CHAR, "beta"), typed_cell(INT32, 8)])],
                ),
                1,
                {0: [INT32, FLOAT32, CHAR]},
            )

            # Exercise the ranked Payment shape through two dependency batches.
            # The same FLOAT32 parameter updates two customer columns, and the
            # second transaction rebinds every prepared value.
            with connect_wire() as payment, connect_wire() as new_order:
                for connection in (payment, new_order):
                    assert exec_stream(
                        connection,
                        "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;",
                    ) == [(0x10, 0, 0, b"")]

                payment_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(2, False, [], "COMMIT"),
                    prepare_entry(
                        10,
                        False,
                        [FLOAT32, INT32],
                        "UPDATE prepared_payment_warehouse "
                        "SET w_ytd = w_ytd + $1 WHERE w_id = $2",
                    ),
                    prepare_entry(
                        11,
                        False,
                        [FLOAT32, INT32, INT32],
                        "UPDATE prepared_payment_district "
                        "SET d_ytd = d_ytd + $1 "
                        "WHERE d_w_id = $2 AND d_id = $3",
                    ),
                    prepare_entry(
                        12,
                        False,
                        [FLOAT32, INT32, INT32, INT32],
                        "UPDATE prepared_payment_customer "
                        "SET c_balance = c_balance - $1, "
                        "c_ytd_payment = c_ytd_payment + $1, "
                        "c_payment_cnt = c_payment_cnt + 1 "
                        "WHERE c_w_id = $2 AND c_d_id = $3 AND c_id = $4",
                    ),
                    prepare_entry(
                        13,
                        False,
                        [INT32, INT32, INT32, INT32, FLOAT32],
                        "INSERT INTO prepared_payment_history "
                        "VALUES ($1,$2,$3,$4,$5)",
                    ),
                    prepare_entry(
                        20,
                        True,
                        [INT32],
                        "SELECT w_ytd FROM prepared_payment_warehouse "
                        "WHERE w_id = $1",
                    ),
                    prepare_entry(
                        21,
                        True,
                        [INT32, INT32],
                        "SELECT d_ytd, d_next_o_id FROM prepared_payment_district "
                        "WHERE d_w_id = $1 AND d_id = $2",
                    ),
                    prepare_entry(
                        22,
                        True,
                        [INT32, INT32, INT32],
                        "SELECT c_balance, c_ytd_payment, c_payment_cnt "
                        "FROM prepared_payment_customer "
                        "WHERE c_w_id = $1 AND c_d_id = $2 AND c_id = $3",
                    ),
                    prepare_entry(
                        23,
                        True,
                        [],
                        "SELECT h_id, h_amount FROM prepared_payment_history "
                        "ORDER BY h_id",
                    ),
                ]
                payment_prepared = prepare_set(payment, payment_entries)
                assert payment_prepared[0:3] == (0x14, 0, 0), payment_prepared
                assert payment_prepared[3][:2] == struct.pack(
                    "!H", len(payment_entries)
                )

                # Check the packed warehouse layout:
                # w_tax starts at offset 85 and w_ytd at offset 89. Verify the
                # ranked Payment transition by raw binary32 bits before the
                # same prepared plan is rebound for the regular transactions.
                exact_before = assert_ok_batch(
                    exec_batch(
                        payment,
                        [operation(20, [typed_cell(INT32, 2)])],
                    ),
                    1,
                    {0: [FLOAT32]},
                )
                assert exact_before == [
                    (0, [[("float_bits", PAYMENT_BEFORE_BITS)]])
                ]
                exact_expected_bits = add_float32_bits(
                    PAYMENT_BEFORE_BITS, PAYMENT_BOUND_BITS
                )
                assert exact_expected_bits == 0x4A417636
                exact_after = assert_ok_batch(
                    exec_batch(
                        payment,
                        [
                            operation(1),
                            operation(
                                10,
                                [
                                    typed_cell(
                                        FLOAT32,
                                        float32_from_bits(PAYMENT_BOUND_BITS),
                                    ),
                                    typed_cell(INT32, 2),
                                ],
                            ),
                            operation(20, [typed_cell(INT32, 2)]),
                            operation(2),
                        ],
                    ),
                    4,
                    {2: [FLOAT32]},
                )
                assert exact_after == [
                    (2, [[("float_bits", exact_expected_bits)]])
                ]

                amounts = [float32(17.37), float32(0.03)]
                for history_id, amount in enumerate(amounts, 1):
                    first_stage = [
                        operation(1),
                        operation(
                            10,
                            [typed_cell(FLOAT32, amount), typed_cell(INT32, 1)],
                        ),
                        operation(
                            11,
                            [
                                typed_cell(FLOAT32, amount),
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 2),
                            ],
                        ),
                    ]
                    assert_ok_batch(exec_batch(payment, first_stage), 3, {})

                    second_stage = [
                        operation(
                            12,
                            [
                                typed_cell(FLOAT32, amount),
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 2),
                                typed_cell(INT32, 3),
                            ],
                        ),
                        operation(
                            13,
                            [
                                typed_cell(INT32, history_id),
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 2),
                                typed_cell(INT32, 3),
                                typed_cell(FLOAT32, amount),
                            ],
                        ),
                        operation(2),
                    ]
                    assert_ok_batch(exec_batch(payment, second_stage), 3, {})

                expected_w_ytd = float32(300000.0)
                expected_d_ytd = float32(30000.0)
                expected_balance = float32(100.0)
                expected_customer_ytd = float32(10.0)
                for amount in amounts:
                    expected_w_ytd = float32(expected_w_ytd + amount)
                    expected_d_ytd = float32(expected_d_ytd + amount)
                    expected_balance = float32(expected_balance - amount)
                    expected_customer_ytd = float32(
                        expected_customer_ytd + amount
                    )

                payment_state = assert_ok_batch(
                    exec_batch(
                        payment,
                        [
                            operation(20, [typed_cell(INT32, 1)]),
                            operation(
                                21,
                                [typed_cell(INT32, 1), typed_cell(INT32, 2)],
                            ),
                            operation(
                                22,
                                [
                                    typed_cell(INT32, 1),
                                    typed_cell(INT32, 2),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                            operation(23),
                        ],
                    ),
                    4,
                    {
                        0: [FLOAT32],
                        1: [FLOAT32, INT32],
                        2: [FLOAT32, FLOAT32, INT32],
                        3: [INT32, FLOAT32],
                    },
                )
                assert payment_state == [
                    (0, [[float32_result(expected_w_ytd)]]),
                    (1, [[float32_result(expected_d_ytd), 3001]]),
                    (
                        2,
                        [
                            [
                                float32_result(expected_balance),
                                float32_result(expected_customer_ytd),
                                3,
                            ]
                        ],
                    ),
                    (
                        3,
                        [
                            [1, float32_result(amounts[0])],
                            [2, float32_result(amounts[1])],
                        ],
                    ),
                ]

                new_order_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(2, False, [], "COMMIT"),
                    prepare_entry(
                        30,
                        False,
                        [INT32, INT32, INT32],
                        "UPDATE prepared_payment_district "
                        "SET d_next_o_id = d_next_o_id + $1 "
                        "WHERE d_w_id = $2 AND d_id = $3",
                    ),
                    prepare_entry(
                        31,
                        False,
                        [FLOAT32, INT32],
                        "UPDATE prepared_payment_warehouse "
                        "SET w_ytd = w_ytd + $1 WHERE w_id = $2",
                    ),
                    prepare_entry(
                        32,
                        True,
                        [INT32],
                        "SELECT w_ytd FROM prepared_payment_warehouse "
                        "WHERE w_id = $1",
                    ),
                ]
                new_order_prepared = prepare_set(new_order, new_order_entries)
                assert new_order_prepared[0:3] == (0x14, 0, 0)

                # A writer using an older SI snapshot must abort instead of
                # applying the exact relative update to the winner's version.
                stale_warehouse_before = assert_ok_batch(
                    exec_batch(
                        payment,
                        [
                            operation(1),
                            operation(20, [typed_cell(INT32, 3)]),
                        ],
                    ),
                    2,
                    {1: [FLOAT32]},
                )
                assert stale_warehouse_before == [
                    (1, [[("float_bits", PAYMENT_BEFORE_BITS)]])
                ]
                assert_ok_batch(
                    exec_batch(
                        new_order,
                        [
                            operation(1),
                            operation(
                                31,
                                [
                                    typed_cell(
                                        FLOAT32,
                                        float32_from_bits(PAYMENT_LEAD_BITS),
                                    ),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                            operation(2),
                        ],
                    ),
                    3,
                    {},
                )
                stale_warehouse_frame = exec_batch(
                    payment,
                    [
                        operation(
                            10,
                            [
                                typed_cell(
                                    FLOAT32,
                                    float32_from_bits(PAYMENT_BOUND_BITS),
                                ),
                                typed_cell(INT32, 3),
                            ],
                        )
                    ],
                )
                assert stale_warehouse_frame[0:3] == (
                    0x15,
                    0,
                    0,
                ), stale_warehouse_frame
                stale_warehouse = decode_batch_result(
                    stale_warehouse_frame[3], {}
                )
                assert stale_warehouse[0:3] == (0, 1, 0), stale_warehouse
                assert stale_warehouse[4] == [], stale_warehouse
                winner_warehouse_bits = add_float32_bits(
                    PAYMENT_BEFORE_BITS, PAYMENT_LEAD_BITS
                )
                warehouse_after_abort = assert_ok_batch(
                    exec_batch(
                        payment,
                        [operation(20, [typed_cell(INT32, 3)])],
                    ),
                    1,
                    {0: [FLOAT32]},
                )
                assert warehouse_after_abort == [
                    (0, [[("float_bits", winner_warehouse_bits)]])
                ]

                # Keep an uncommitted write in a transaction whose snapshot
                # predates another connection's committed update to a different
                # column of the same district row.
                stale_amount = float32(1.25)
                pending_payment = assert_ok_batch(
                    exec_batch(
                        payment,
                        [
                            operation(1),
                            operation(
                                12,
                                [
                                    typed_cell(FLOAT32, stale_amount),
                                    typed_cell(INT32, 1),
                                    typed_cell(INT32, 2),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                            operation(
                                22,
                                [
                                    typed_cell(INT32, 1),
                                    typed_cell(INT32, 2),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                        ],
                    ),
                    3,
                    {2: [FLOAT32, FLOAT32, INT32]},
                )
                assert pending_payment == [
                    (
                        2,
                        [
                            [
                                float32_result(
                                    float32(expected_balance - stale_amount)
                                ),
                                float32_result(
                                    float32(
                                        expected_customer_ytd + stale_amount
                                    )
                                ),
                                4,
                            ]
                        ],
                    )
                ]
                assert_ok_batch(
                    exec_batch(
                        new_order,
                        [
                            operation(1),
                            operation(
                                30,
                                [
                                    typed_cell(INT32, 1),
                                    typed_cell(INT32, 1),
                                    typed_cell(INT32, 2),
                                ],
                            ),
                            operation(2),
                        ],
                    ),
                    3,
                    {},
                )

                stale_frame = exec_batch(
                    payment,
                    [
                        operation(
                            21,
                            [typed_cell(INT32, 1), typed_cell(INT32, 2)],
                        ),
                        operation(
                            11,
                            [
                                typed_cell(FLOAT32, stale_amount),
                                typed_cell(INT32, 1),
                                typed_cell(INT32, 2),
                            ],
                        ),
                    ],
                )
                assert stale_frame[0:3] == (0x15, 0, 0), stale_frame
                stale = decode_batch_result(
                    stale_frame[3], {0: [FLOAT32, INT32]}
                )
                assert stale[0] == 1 and stale[1] == 1 and stale[2] == 1, stale
                assert stale[4] == [], stale

                post_abort = assert_ok_batch(
                    exec_batch(
                        payment,
                        [
                            operation(
                                21,
                                [typed_cell(INT32, 1), typed_cell(INT32, 2)],
                            ),
                            operation(
                                22,
                                [
                                    typed_cell(INT32, 1),
                                    typed_cell(INT32, 2),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                            operation(23),
                        ],
                    ),
                    3,
                    {
                        0: [FLOAT32, INT32],
                        1: [FLOAT32, FLOAT32, INT32],
                        2: [INT32, FLOAT32],
                    },
                )
                assert post_abort == [
                    (0, [[float32_result(expected_d_ytd), 3002]]),
                    (
                        1,
                        [
                            [
                                float32_result(expected_balance),
                                float32_result(expected_customer_ytd),
                                3,
                            ]
                        ],
                    ),
                    (
                        2,
                        [
                            [1, float32_result(amounts[0])],
                            [2, float32_result(amounts[1])],
                        ],
                    ),
                ]

            # A commit timestamp must be assigned in durable publication order.
            # Otherwise a WAL-backed writer can receive an early timestamp,
            # be overtaken by a read-only commit, and later make a stale
            # Payment update appear conflict-free.
            with (
                connect_wire() as commit_writer,
                connect_wire() as frontier_reader,
                connect_wire() as challenger,
                connect_wire() as commit_observer,
            ):
                for connection in (
                    commit_writer,
                    frontier_reader,
                    challenger,
                    commit_observer,
                ):
                    assert exec_stream(
                        connection,
                        "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;",
                    ) == [(0x10, 0, 0, b"")]

                writer_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(2, False, [], "COMMIT"),
                    prepare_entry(
                        3,
                        False,
                        [FLOAT32, INT32],
                        "UPDATE prepared_commit_frontier "
                        "SET amount = amount + $1 WHERE id = $2",
                    ),
                    prepare_entry(
                        4,
                        False,
                        [INT32, CHAR],
                        "INSERT INTO prepared_commit_wal VALUES ($1,$2)",
                    ),
                ]
                frontier_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(2, False, [], "COMMIT"),
                    prepare_entry(
                        3,
                        True,
                        [INT32],
                        "SELECT id FROM prepared_commit_frontier WHERE id = $1",
                    ),
                ]
                challenger_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(2, False, [], "ABORT"),
                    prepare_entry(
                        3,
                        True,
                        [INT32],
                        "SELECT amount FROM prepared_commit_frontier WHERE id = $1",
                    ),
                    prepare_entry(
                        4,
                        False,
                        [FLOAT32, INT32],
                        "UPDATE prepared_commit_frontier "
                        "SET amount = amount + $1 WHERE id = $2",
                    ),
                    prepare_entry(5, False, [], "COMMIT"),
                ]
                observer_entries = [
                    prepare_entry(
                        3,
                        True,
                        [INT32],
                        "SELECT amount FROM prepared_commit_frontier WHERE id = $1",
                    ),
                ]
                for connection, prepared_entries in (
                    (commit_writer, writer_entries),
                    (frontier_reader, frontier_entries),
                    (challenger, challenger_entries),
                    (commit_observer, observer_entries),
                ):
                    prepared = prepare_set(connection, prepared_entries)
                    assert prepared[0:3] == (0x14, 0, 0), prepared
                    assert prepared[3][:2] == struct.pack(
                        "!H", len(prepared_entries)
                    )

                # Check the non-concurrent prepared path before the concurrent case.
                sequential_before = assert_ok_batch(
                    exec_batch(
                        challenger,
                        [operation(3, [typed_cell(INT32, 3)])],
                    ),
                    1,
                    {0: [FLOAT32]},
                )
                assert sequential_before == [
                    (0, [[("float_bits", PAYMENT_BEFORE_BITS)]])
                ]
                expected_bits = add_float32_bits(
                    PAYMENT_BEFORE_BITS, PAYMENT_BOUND_BITS
                )
                assert expected_bits == 0x4A417636
                sequential_after = assert_ok_batch(
                    exec_batch(
                        challenger,
                        [
                            operation(1),
                            operation(
                                4,
                                [
                                    typed_cell(
                                        FLOAT32,
                                        float32_from_bits(PAYMENT_BOUND_BITS),
                                    ),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                            operation(3, [typed_cell(INT32, 3)]),
                            operation(5),
                        ],
                    ),
                    4,
                    {2: [FLOAT32]},
                )
                assert sequential_after == [
                    (2, [[("float_bits", expected_bits)]])
                ]

                expected_bits = PAYMENT_BEFORE_BITS
                next_filler_seq = 0
                stale_snapshot_count = 0
                challenger_abort_count = 0
                challenger_commit_count = 0
                for _ in range(COMMIT_FRONTIER_ATTEMPTS):
                    for batch_index in range(
                        COMMIT_FRONTIER_FILLER_BATCHES
                    ):
                        filler_operations = []
                        if batch_index == 0:
                            filler_operations.append(operation(1))
                        for _ in range(COMMIT_FRONTIER_FILLER_ROWS):
                            filler_operations.append(
                                operation(
                                    4,
                                    [
                                        typed_cell(INT32, next_filler_seq),
                                        typed_cell(CHAR, COMMIT_FRONTIER_PAD),
                                    ],
                                )
                            )
                            next_filler_seq += 1
                        assert_ok_batch(
                            exec_batch_unfragmented(
                                commit_writer, filler_operations
                            ),
                            len(filler_operations),
                            {},
                        )

                    # Keep the target write last in the write set. Publishing
                    # the filler metadata then widens the interval in which a
                    # challenger can still reconstruct the old target value.
                    assert_ok_batch(
                        exec_batch(
                            commit_writer,
                            [
                                operation(
                                    3,
                                    [
                                        typed_cell(
                                            FLOAT32,
                                            float32_from_bits(
                                                PAYMENT_LEAD_BITS
                                            ),
                                        ),
                                        typed_cell(INT32, 1),
                                    ],
                                )
                            ],
                        ),
                        1,
                        {},
                    )

                    # Split send/read so the other connections can run while
                    # this commit flushes several hundred KiB of WAL.
                    send_exec_batch(commit_writer, [operation(2)])
                    frontier_result = assert_ok_batch(
                        exec_batch(
                            frontier_reader,
                            [
                                operation(1),
                                operation(3, [typed_cell(INT32, 2)]),
                                operation(2),
                            ],
                        ),
                        3,
                        {1: [INT32]},
                    )
                    assert frontier_result == [(1, [[2]])]

                    before_result = assert_ok_batch(
                        exec_batch(
                            challenger,
                            [
                                operation(1),
                                operation(3, [typed_cell(INT32, 1)]),
                            ],
                        ),
                        2,
                        {1: [FLOAT32]},
                    )
                    assert len(before_result) == 1
                    before_bits = before_result[0][1][0][0][1]

                    assert_ok_batch(read_frame(commit_writer), 1, {})
                    expected_bits = add_float32_bits(
                        expected_bits, PAYMENT_LEAD_BITS
                    )
                    if before_bits != expected_bits:
                        stale_snapshot_count += 1

                    challenge_frame = exec_batch(
                        challenger,
                        [
                            operation(
                                4,
                                [
                                    typed_cell(
                                        FLOAT32,
                                        float32_from_bits(PAYMENT_BOUND_BITS),
                                    ),
                                    typed_cell(INT32, 1),
                                ],
                            ),
                            operation(3, [typed_cell(INT32, 1)]),
                            operation(5),
                        ],
                    )
                    assert challenge_frame[0:3] == (0x15, 0, 0), challenge_frame
                    challenge = decode_batch_result(
                        challenge_frame[3], {1: [FLOAT32]}
                    )
                    if challenge[1] == 1:
                        assert challenge[0] == challenge[2] == 0, challenge
                        assert challenge[4] == [], challenge
                        challenger_abort_count += 1
                    else:
                        assert challenge[0:4] == (3, 0, 0xFFFF, ""), challenge
                        assert len(challenge[4]) == 1, challenge
                        after_bits = challenge[4][0][1][0][0][1]
                        exact_after_bits = add_float32_bits(
                            before_bits, PAYMENT_BOUND_BITS
                        )
                        assert after_bits == exact_after_bits, {
                            "before": hex(before_bits),
                            "bound": hex(PAYMENT_BOUND_BITS),
                            "expected": hex(exact_after_bits),
                            "actual": hex(after_bits),
                        }
                        assert before_bits == expected_bits, {
                            "visible_before": hex(before_bits),
                            "committed_before": hex(expected_bits),
                        }
                        expected_bits = after_bits
                        challenger_commit_count += 1

                    observed = assert_ok_batch(
                        exec_batch(
                            commit_observer,
                            [operation(3, [typed_cell(INT32, 1)])],
                        ),
                        1,
                        {0: [FLOAT32]},
                    )
                    assert observed == [
                        (0, [[("float_bits", expected_bits)]])
                    ]

                assert stale_snapshot_count >= 1, {
                    "stale_snapshots": stale_snapshot_count,
                    "aborts": challenger_abort_count,
                    "commits": challenger_commit_count,
                }
                assert challenger_abort_count >= 1
                assert (
                    challenger_abort_count + challenger_commit_count
                    == COMMIT_FRONTIER_ATTEMPTS
                )

            # A rollback of a non-key UPDATE must not make an unchanged index
            # entry disappear. Otherwise a concurrent indexed UPDATE can see
            # no RID, return OK, and commit without changing the row.
            with connect_wire() as abort_writer, connect_wire() as committed_reader, connect_wire() as observer:
                for connection in (abort_writer, committed_reader, observer):
                    assert exec_stream(
                        connection,
                        "SET TRANSACTION ISOLATION LEVEL SNAPSHOT ISOLATION;",
                    ) == [(0x10, 0, 0, b"")]

                writer_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(
                        2,
                        False,
                        [INT32],
                        "UPDATE prepared_rollback_race "
                        "SET payload = payload + 1 WHERE id = $1",
                    ),
                    prepare_entry(3, False, [], "ABORT"),
                ]
                reader_entries = [
                    prepare_entry(1, False, [], "BEGIN"),
                    prepare_entry(
                        2,
                        False,
                        [INT32],
                        "UPDATE prepared_rollback_race "
                        "SET payload = payload + 1 WHERE id = $1",
                    ),
                    prepare_entry(3, False, [], "COMMIT"),
                ]
                observer_entries = [
                    prepare_entry(
                        10,
                        True,
                        [INT32],
                        "SELECT payload FROM prepared_rollback_race "
                        "WHERE id = $1",
                    ),
                    prepare_entry(11, False, [], "BEGIN"),
                    prepare_entry(12, False, [], "ABORT"),
                    prepare_entry(
                        13,
                        False,
                        [INT32, INT32],
                        "UPDATE prepared_rollback_race "
                        "SET id = $1 WHERE id = $2",
                    ),
                    prepare_entry(
                        14,
                        True,
                        [INT32],
                        "SELECT id, payload FROM prepared_rollback_race "
                        "WHERE id = $1",
                    ),
                ]
                for connection, prepared_entries in (
                    (abort_writer, writer_entries),
                    (committed_reader, reader_entries),
                    (observer, observer_entries),
                ):
                    prepared = prepare_set(connection, prepared_entries)
                    assert prepared[0:3] == (0x14, 0, 0), prepared
                    assert prepared[3][:2] == struct.pack(
                        "!H", len(prepared_entries)
                    )

                def completed_ok(frame, operation_count):
                    assert frame[0:3] == (0x15, 0, 0), frame
                    decoded = decode_batch_result(frame[3], {})
                    if decoded[1] == 0:
                        assert decoded[0:4] == (
                            operation_count,
                            0,
                            0xFFFF,
                            "",
                        ), decoded
                        return True
                    assert decoded[1] == 1, decoded
                    assert decoded[0] == decoded[2] < operation_count, decoded
                    assert decoded[4] == [], decoded
                    return False

                stop_race = threading.Event()
                start_race = threading.Barrier(3)
                race_errors = []
                race_counts = {
                    "writer_aborts": 0,
                    "writer_conflicts": 0,
                    "reader_commits": 0,
                    "reader_conflicts": 0,
                }

                def abort_writer_loop():
                    try:
                        start_race.wait(timeout=5)
                        while not stop_race.is_set():
                            first_stage = exec_batch(
                                abort_writer,
                                [
                                    operation(1),
                                    operation(
                                        2, [typed_cell(INT32, 1)]
                                    ),
                                ],
                            )
                            if not completed_ok(first_stage, 2):
                                race_counts["writer_conflicts"] += 1
                                continue
                            assert_ok_batch(
                                exec_batch(abort_writer, [operation(3)]),
                                1,
                                {},
                            )
                            race_counts["writer_aborts"] += 1
                    except BaseException as exc:
                        race_errors.append(("abort writer", repr(exc)))
                        stop_race.set()

                def committed_reader_loop():
                    try:
                        start_race.wait(timeout=5)
                        while not stop_race.is_set():
                            committed = exec_batch(
                                committed_reader,
                                [
                                    operation(1),
                                    operation(
                                        2, [typed_cell(INT32, 1)]
                                    ),
                                    operation(3),
                                ],
                            )
                            if completed_ok(committed, 3):
                                race_counts["reader_commits"] += 1
                            else:
                                race_counts["reader_conflicts"] += 1
                    except BaseException as exc:
                        race_errors.append(("committed reader", repr(exc)))
                        stop_race.set()

                race_threads = [
                    threading.Thread(
                        target=abort_writer_loop,
                        name="prepared-abort-writer",
                        daemon=True,
                    ),
                    threading.Thread(
                        target=committed_reader_loop,
                        name="prepared-committed-reader",
                        daemon=True,
                    ),
                ]
                for thread in race_threads:
                    thread.start()
                start_race.wait(timeout=5)
                stop_race.wait(ROLLBACK_RACE_SECONDS)
                stop_race.set()
                for thread in race_threads:
                    thread.join(timeout=8)
                if any(thread.is_alive() for thread in race_threads):
                    for connection in (abort_writer, committed_reader):
                        try:
                            connection.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                    for thread in race_threads:
                        thread.join(timeout=2)

                assert not any(
                    thread.is_alive() for thread in race_threads
                ), race_counts
                assert race_errors == [], race_errors
                assert race_counts["writer_aborts"] >= 20, race_counts
                assert race_counts["reader_commits"] >= 20, race_counts

                final_payload = assert_ok_batch(
                    exec_batch(
                        observer,
                        [operation(10, [typed_cell(INT32, 1)])],
                    ),
                    1,
                    {0: [INT32]},
                )
                assert final_payload == [
                    (0, [[race_counts["reader_commits"]]])
                ], race_counts

                # Real indexed-key changes still roll back in reverse order:
                # the old key remains addressable and both intermediate keys
                # vanish after two updates of the same RID.
                assert_ok_batch(
                    exec_batch(
                        observer,
                        [
                            operation(11),
                            operation(
                                13,
                                [
                                    typed_cell(INT32, 3),
                                    typed_cell(INT32, 2),
                                ],
                            ),
                            operation(
                                13,
                                [
                                    typed_cell(INT32, 4),
                                    typed_cell(INT32, 3),
                                ],
                            ),
                            operation(12),
                        ],
                    ),
                    4,
                    {},
                )
                restored_keys = assert_ok_batch(
                    exec_batch(
                        observer,
                        [
                            operation(14, [typed_cell(INT32, 2)]),
                            operation(14, [typed_cell(INT32, 3)]),
                            operation(14, [typed_cell(INT32, 4)]),
                        ],
                    ),
                    3,
                    {
                        0: [INT32, INT32],
                        1: [INT32, INT32],
                        2: [INT32, INT32],
                    },
                )
                assert restored_keys == [
                    (0, [[2, 7]]),
                    (1, []),
                    (2, []),
                ]
    finally:
        server.send_signal(signal.SIGINT)
        try:
            server.wait(timeout=8)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)
        shutil.rmtree(db_path, ignore_errors=True)

    if server.returncode != 0:
        raise AssertionError(server.stderr.read())
    print("prepared wire protocol checks passed")


if __name__ == "__main__":
    main()
