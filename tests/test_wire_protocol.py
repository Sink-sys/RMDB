#!/usr/bin/env python3

import os
import shutil
import signal
import socket
import struct
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get("RMDB_TEST_BUILD", ROOT / "build"))
SERVER = BUILD / "bin" / "rmdb"
HANDSHAKE = b"RMDB" + struct.pack("!HH", 3, 0)


def read_exact(sock: socket.socket, size: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < size:
        chunk = sock.recv(size - len(chunks))
        if not chunk:
            raise AssertionError("connection closed before a complete protocol field")
        chunks.extend(chunk)
    return bytes(chunks)


def read_frame(sock: socket.socket) -> tuple[int, int, int, bytes]:
    payload_bytes, tag, flags, reserved = struct.unpack("!IBBH", read_exact(sock, 8))
    return tag, flags, reserved, read_exact(sock, payload_bytes)


def exec_stream(sock: socket.socket, sql: str) -> list[tuple[int, int, int, bytes]]:
    payload = sql.encode("utf-8")
    request = struct.pack("!IBBH", len(payload), 0x20, 0, 0) + payload
    for offset in range(0, len(request), 3):
        sock.sendall(request[offset : offset + 3])

    frames = []
    while True:
        frame = read_frame(sock)
        frames.append(frame)
        if frame[0] in {0x10, 0x11, 0x12, 0x13}:
            return frames


def wait_for_server() -> None:
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", 8765), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise AssertionError("rmdb did not listen on port 8765")


def connect_wire() -> socket.socket:
    sock = socket.create_connection(("127.0.0.1", 8765), timeout=3)
    sock.settimeout(3)
    sock.sendall(HANDSHAKE)
    assert read_exact(sock, 8) == HANDSHAKE
    return sock


def main() -> None:
    if not SERVER.exists():
        raise SystemExit("build/bin/rmdb is missing; build target rmdb first")

    db_name = "wire_protocol_test_db"
    db_path = BUILD / db_name
    csv_path = BUILD / "wire_not_equal_probe.csv"
    shutil.rmtree(db_path, ignore_errors=True)
    csv_path.write_text("id,y\n1,0\n2,0\n3,0\n", encoding="utf-8")
    server = subprocess.Popen(
        [str(SERVER), db_name],
        cwd=BUILD,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        wait_for_server()
        with socket.create_connection(("127.0.0.1", 8765), timeout=3) as sock:
            sock.settimeout(3)
            sock.sendall(HANDSHAKE[:3])
            sock.sendall(HANDSHAKE[3:])
            assert read_exact(sock, 8) == HANDSHAKE

            frames = exec_stream(sock, "show tables;")
            assert frames == [
                (0x01, 0, 0, b"\x00\x01\x00\x06Tables\x03"),
                (0x11, 0, 0, struct.pack("!Q", 0)),
            ]

            assert exec_stream(
                sock, "create table readiness_probe (id int, amount float, note char(8));"
            ) == [(0x10, 0, 0, b"")]

            frames = exec_stream(sock, "show tables;")
            assert frames[0] == (0x01, 0, 0, b"\x00\x01\x00\x06Tables\x03")
            assert frames[1] == (
                0x02,
                0,
                0,
                b"\x01" + struct.pack("!I", len("readiness_probe")) + b"readiness_probe",
            )
            assert frames[2] == (0x11, 0, 0, struct.pack("!Q", 1))

            frames = exec_stream(sock, "desc readiness_probe;")
            assert frames[0] == (
                0x01,
                0,
                0,
                b"\x00\x03\x00\x05Field\x03\x00\x04Type\x03\x00\x05Index\x03",
            )
            assert [frame[0] for frame in frames] == [0x01, 0x02, 0x02, 0x02, 0x11]
            assert frames[-1] == (0x11, 0, 0, struct.pack("!Q", 3))

            assert exec_stream(
                sock, "set transaction isolation level snapshot isolation;"
            ) == [(0x10, 0, 0, b"")]

            assert exec_stream(
                sock, "alter table readiness_probe add ignored int;"
            )[0][0] == 0x13

            assert exec_stream(
                sock, "insert into readiness_probe values (7, 1.5, 'ok');"
            ) == [(0x10, 0, 0, b"")]

            frames = exec_stream(
                sock, "select id, amount as amount_after_commit, note from readiness_probe where id = 7;"
            )
            assert frames == [
                (
                    0x01,
                    0,
                    0,
                    b"\x00\x03\x00\x02id\x01\x00\x13amount_after_commit\x02\x00\x04note\x03",
                ),
                (
                    0x02,
                    0,
                    0,
                    b"\x01"
                    + struct.pack("!i", 7)
                    + b"\x01"
                    + struct.pack("!f", 1.5)
                    + b"\x01\x00\x00\x00\x02ok",
                ),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]

            frames = exec_stream(
                sock, "select id from readiness_probe where id = 99;"
            )
            assert frames == [
                (0x01, 0, 0, b"\x00\x01\x00\x02id\x01"),
                (0x11, 0, 0, struct.pack("!Q", 0)),
            ]

            frames = exec_stream(
                sock, "select sum(id) as sum_empty from readiness_probe where id = 99;"
            )
            assert frames == [
                (0x01, 0, 0, b"\x00\x01\x00\tsum_empty\x01"),
                (0x02, 0, 0, b"\x01" + struct.pack("!i", 0)),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]

            assert exec_stream(
                sock, "insert into readiness_probe values (7, 1.5, 'ok');"
            ) == [(0x10, 0, 0, b"")]
            frames = exec_stream(
                sock, "select distinct id, amount as distinct_amount, note from readiness_probe;"
            )
            assert frames == [
                (
                    0x01,
                    0,
                    0,
                    b"\x00\x03\x00\x02id\x01\x00\x0fdistinct_amount\x02\x00\x04note\x03",
                ),
                (
                    0x02,
                    0,
                    0,
                    b"\x01"
                    + struct.pack("!i", 7)
                    + b"\x01"
                    + struct.pack("!f", 1.5)
                    + b"\x01\x00\x00\x00\x02ok",
                ),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]

            assert exec_stream(
                sock, "insert into readiness_probe values (8, 2.5, 'other');"
            ) == [(0x10, 0, 0, b"")]
            frames = exec_stream(
                sock, "select count(distinct id) as unique_ids from readiness_probe;"
            )
            assert frames == [
                (0x01, 0, 0, b"\x00\x01\x00\nunique_ids\x01"),
                (0x02, 0, 0, b"\x01" + struct.pack("!i", 2)),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]

            frames = exec_stream(
                sock,
                "select count(distinct (id)) as unique_ids_parenthesized "
                "from readiness_probe;",
            )
            assert frames == [
                (
                    0x01,
                    0,
                    0,
                    b"\x00\x01\x00\x18unique_ids_parenthesized\x01",
                ),
                (0x02, 0, 0, b"\x01" + struct.pack("!i", 2)),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]

            frames = exec_stream(
                sock,
                "select distinct id as distinct_key from readiness_probe "
                "order by distinct_key desc;",
            )
            assert frames == [
                (0x01, 0, 0, b"\x00\x01\x00\x0cdistinct_key\x01"),
                (0x02, 0, 0, b"\x01" + struct.pack("!i", 8)),
                (0x02, 0, 0, b"\x01" + struct.pack("!i", 7)),
                (0x11, 0, 0, struct.pack("!Q", 2)),
            ]

            for table_name, with_index in (
                ("abort_probe_plain", False),
                ("abort_probe_indexed", True),
            ):
                assert exec_stream(
                    sock,
                    f"create table {table_name} "
                    "(k int, g int, amount float, label char(12));",
                ) == [(0x10, 0, 0, b"")]
                if with_index:
                    assert exec_stream(
                        sock, f"create index {table_name}(k);"
                    ) == [(0x10, 0, 0, b"")]
                assert exec_stream(
                    sock,
                    f"insert into {table_name} values "
                    "(498604, 1, 321.5, '90wiloky9esc');",
                ) == [(0x10, 0, 0, b"")]

                with connect_wire() as abort_sock:
                    assert exec_stream(abort_sock, "begin;") == [(0x10, 0, 0, b"")]
                    assert exec_stream(
                        abort_sock,
                        f"update {table_name} set k = 498605, g = 9, amount = 9.5 "
                        "where k = 498604;",
                    ) == [(0x10, 0, 0, b"")]
                    assert exec_stream(
                        abort_sock, f"delete from {table_name} where k = 498605;"
                    ) == [(0x10, 0, 0, b"")]
                    assert exec_stream(
                        abort_sock,
                        f"select k, g, amount, label from {table_name} where k = 498604;",
                    )[-1] == (0x11, 0, 0, struct.pack("!Q", 0))
                    assert exec_stream(
                        abort_sock,
                        f"insert into {table_name} values (7, 7, 7.0, 'temporary');",
                    ) == [(0x10, 0, 0, b"")]
                    assert exec_stream(
                        abort_sock, f"delete from {table_name} where k = 7;"
                    ) == [(0x10, 0, 0, b"")]
                    assert exec_stream(abort_sock, "abort;") == [(0x10, 0, 0, b"")]

                frames = exec_stream(
                    sock,
                    f"select k, g, amount, label from {table_name} where k = 498604;",
                )
                expected_frames = [
                    (
                        0x01,
                        0,
                        0,
                        b"\x00\x04\x00\x01k\x01\x00\x01g\x01"
                        b"\x00\x06amount\x02\x00\x05label\x03",
                    ),
                    (
                        0x02,
                        0,
                        0,
                        b"\x01"
                        + struct.pack("!i", 498604)
                        + b"\x01"
                        + struct.pack("!i", 1)
                        + b"\x01"
                        + struct.pack("!f", 321.5)
                        + b"\x01\x00\x00\x00\x0c90wiloky9esc",
                    ),
                    (0x11, 0, 0, struct.pack("!Q", 1)),
                ]
                assert frames == expected_frames, (table_name, frames)
                assert exec_stream(
                    sock, f"select k from {table_name} where k = 7;"
                )[-1] == (0x11, 0, 0, struct.pack("!Q", 0))

            with connect_wire() as snapshot_reader, connect_wire() as snapshot_writer:
                assert exec_stream(
                    snapshot_reader, "set transaction isolation level snapshot isolation;"
                ) == [(0x10, 0, 0, b"")]
                assert exec_stream(
                    snapshot_writer, "set transaction isolation level snapshot isolation;"
                ) == [(0x10, 0, 0, b"")]
                assert exec_stream(snapshot_reader, "begin;") == [(0x10, 0, 0, b"")]
                assert exec_stream(
                    snapshot_reader, "select id from readiness_probe where id = 8;"
                )[-1] == (0x11, 0, 0, struct.pack("!Q", 1))
                assert exec_stream(snapshot_writer, "begin;") == [(0x10, 0, 0, b"")]
                assert exec_stream(
                    snapshot_writer, "delete from readiness_probe where id = 8;"
                ) == [(0x10, 0, 0, b"")]
                assert exec_stream(snapshot_writer, "commit;") == [(0x10, 0, 0, b"")]
                assert exec_stream(
                    snapshot_reader, "select id from readiness_probe where id = 8;"
                )[-1] == (0x11, 0, 0, struct.pack("!Q", 1))
                assert exec_stream(
                    snapshot_reader, "delete from readiness_probe where id = 8;"
                )[0][0] == 0x12

            with connect_wire() as verifier:
                assert exec_stream(
                    verifier, "select id from readiness_probe where id = 8;"
                ) == [
                    (0x01, 0, 0, b"\x00\x01\x00\x02id\x01"),
                    (0x11, 0, 0, struct.pack("!Q", 0)),
                ]

            error = exec_stream(sock, "this is not sql;")
            assert len(error) == 1 and error[0][0:3] == (0x13, 0, 0)

            opaque_name = "opaque999999999999999999999999999999"
            assert exec_stream(
                sock, f"create table {opaque_name} (value2026 int);"
            ) == [(0x10, 0, 0, b"")]
            assert exec_stream(
                sock, f"drop table {opaque_name};"
            ) == [(0x10, 0, 0, b"")]

            assert exec_stream(
                sock, "create table float_sum_probe (amount float);"
            ) == [(0x10, 0, 0, b"")]
            for value in ("16777216.0", "1.0", "1.0"):
                assert exec_stream(
                    sock, f"insert into float_sum_probe values ({value});"
                ) == [(0x10, 0, 0, b"")]
            frames = exec_stream(sock, "select sum(amount) as precise_sum from float_sum_probe;")
            assert frames == [
                (0x01, 0, 0, b"\x00\x01\x00\x0bprecise_sum\x02"),
                (0x02, 0, 0, b"\x01" + struct.pack("!f", 16777218.0)),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]
            assert exec_stream(
                sock, "drop table float_sum_probe;"
            ) == [(0x10, 0, 0, b"")]

            assert exec_stream(
                sock,
                "create table wire_chain_probe "
                "(pk int, base int, mirror int, amount float, guard int);",
            ) == [(0x10, 0, 0, b"")]
            assert exec_stream(
                sock,
                "insert into wire_chain_probe values (1, 10, 0, 1607.808, 5);",
            ) == [(0x10, 0, 0, b"")]
            assert exec_stream(
                sock,
                "update wire_chain_probe set amount = amount "
                "where pk = 1 and guard < 10;",
            ) == [(0x10, 0, 0, b"")]
            assert exec_stream(
                sock,
                "update wire_chain_probe "
                "set base = base - 1 + 91, mirror = base + 5 - 0, "
                "amount = amount - 53.236 + 8888.298, guard = guard + 1 "
                "where pk = 1 and guard < 10 and base < 20;",
            ) == [(0x10, 0, 0, b"")]
            frames = exec_stream(sock, "select * from wire_chain_probe where pk = 1;")
            assert frames == [
                (
                    0x01,
                    0,
                    0,
                    b"\x00\x05\x00\x02pk\x01\x00\x04base\x01"
                    b"\x00\x06mirror\x01\x00\x06amount\x02\x00\x05guard\x01",
                ),
                (
                    0x02,
                    0,
                    0,
                    b"\x01"
                    + struct.pack("!i", 1)
                    + b"\x01"
                    + struct.pack("!i", 100)
                    + b"\x01"
                    + struct.pack("!i", 15)
                    + b"\x01"
                    + struct.pack("!I", 0x46232B7B)
                    + b"\x01"
                    + struct.pack("!i", 6),
                ),
                (0x11, 0, 0, struct.pack("!Q", 1)),
            ]

            assert exec_stream(
                sock, "create table wire_not_equal_probe (id int, y float);"
            ) == [(0x10, 0, 0, b"")]
            assert exec_stream(
                sock, f"load {csv_path} into wire_not_equal_probe;"
            ) == [(0x10, 0, 0, b"")]

            def assert_scalar_int(sql: str, name: str, expected: int) -> None:
                encoded_name = name.encode("utf-8")
                assert exec_stream(sock, sql) == [
                    (
                        0x01,
                        0,
                        0,
                        b"\x00\x01" + struct.pack("!H", len(encoded_name)) + encoded_name + b"\x01",
                    ),
                    (0x02, 0, 0, b"\x01" + struct.pack("!i", expected)),
                    (0x11, 0, 0, struct.pack("!Q", 1)),
                ]

            assert_scalar_int(
                "select count(*) as bang from wire_not_equal_probe where y != 0;",
                "bang",
                0,
            )
            assert_scalar_int(
                "select count(*) as angle from wire_not_equal_probe where y <> 0;",
                "angle",
                0,
            )
            assert_scalar_int(
                "select count(*) as zeros from wire_not_equal_probe where y = 0;",
                "zeros",
                3,
            )
            invalid = exec_stream(
                sock, "select count(*) as invalid from wire_not_equal_probe where y ! = 0;"
            )
            assert len(invalid) == 1 and invalid[0][0:3] == (0x13, 0, 0)
            assert_scalar_int(
                "select count(*) as alive from wire_not_equal_probe where y = 0;",
                "alive",
                3,
            )

        # A grader closes a case connection as soon as it sees a mismatch. Its RST
        # must not deliver SIGPIPE to the process and kill later functional cases.
        for _ in range(8):
            reset_sock = connect_wire()
            linger = struct.pack("ii", 1, 0)
            reset_sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger)
            payload = b"show tables;"
            reset_sock.sendall(struct.pack("!IBBH", len(payload), 0x20, 0, 0) + payload)
            reset_sock.close()
        time.sleep(0.1)
        assert server.poll() is None
        with connect_wire() as probe_sock:
            assert exec_stream(probe_sock, "show tables;")[-1][0] == 0x11
    finally:
        server.send_signal(signal.SIGINT)
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)
        shutil.rmtree(db_path, ignore_errors=True)
        csv_path.unlink(missing_ok=True)

    if server.returncode != 0:
        raise AssertionError(server.stderr.read())
    print("wire protocol readiness checks passed")


if __name__ == "__main__":
    main()
