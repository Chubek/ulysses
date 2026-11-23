"""Linux integration tests for the real Dublin and Telemachaid processes."""
from contextlib import contextmanager
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time


@contextmanager
def running(binary, env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, pass_fds=()):
    with subprocess.Popen([binary], env=env, stdin=stdin, stdout=stdout,
                          pass_fds=pass_fds) as proc:
        try:
            yield proc
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait(timeout=5)


def read_ready(proc):
    assert select.select([proc.stdout], [], [], 5)[0], "Dublin did not start"
    line = proc.stdout.readline().decode().strip()
    assert line.startswith("dublind listening on 127.0.0.1:"), line
    return int(line.rsplit(":", 1)[1])


def request(port, wire):
    with socket.create_connection(("127.0.0.1", port), timeout=8) as connection:
        connection.sendall(wire)
        response = bytearray()
        while True:
            data = connection.recv(4096)
            if not data:
                break
            response.extend(data)
    headers, body = bytes(response).split(b"\r\n\r\n", 1)
    lines = headers.split(b"\r\n")
    assert b"Connection: close" in lines
    if not wire.startswith(b"HEAD "):
        assert f"Content-Length: {len(body)}".encode() in lines
    return int(lines[0].split()[1]), body


def get(port, target):
    return request(port, f"GET {target} HTTP/1.1\r\nHost: localhost\r\n\r\n".encode())


def test_dublin(build):
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        worker = root / "tracked-worker"
        worker.write_text('#!/bin/sh\nprintf "%s\\n" "$$" > "$TEST_WORKER_PID"\n'
                          'exec "$TEST_REAL_WORKER"\n', encoding="utf-8")
        worker.chmod(0o700)
        test_worker_recovery(build, worker, root / "worker.pid")

    env = dict(os.environ, ULYSSES_PORT="0", ULYSSES_MAX_CONNECTIONS="2",
               ULYSSES_WORKER=str(build / "absent-worker"))
    with running(build / "dublind", env) as proc:
        port = read_ready(proc)
        assert get(port, "/api/hello")[0] == 503
        assert get(port, "/health") == (200, b"ok\n")
        assert proc.wait(timeout=5) == 0


def test_worker_recovery(build, worker, pid_file):
    env = dict(os.environ, ULYSSES_PORT="0", ULYSSES_MAX_CONNECTIONS="12",
               ULYSSES_WORKER=str(worker), TEST_WORKER_PID=str(pid_file),
               TEST_REAL_WORKER=str(build / "telemachaid-worker"))
    with running(build / "dublind", env) as proc:
        port = read_ready(proc)
        assert get(port, "/") == (200, b"Ulysses\n")
        assert get(port, "/health") == (200, b"ok\n")
        assert get(port, "/api/hello?x=y") == (200, b"Hello from Erin\n")
        assert get(port, "/api/bloom") == (200, b"Hello from Bloomscript")
        assert get(port, "/missing")[0] == 404
        assert request(port, b"POST / HTTP/1.1\r\nHost: localhost\r\n\r\n")[0] == 405
        assert request(port, b"HEAD / HTTP/1.1\r\nHost: localhost\r\n\r\n") == (405, b"")
        assert request(port, b"GET / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\n"
                             b"Content-Length: 2\r\n\r\n")[0] == 400
        prefix = b"GET / HTTP/1.1\r\nHost: localhost\r\nX: "
        assert request(port, prefix.ljust(8192, b"x"))[0] == 431

        child = int(pid_file.read_text())
        os.kill(child, signal.SIGKILL)
        assert get(port, "/api/hello")[0] == 502
        assert get(port, "/health") == (200, b"ok\n")
        time.sleep(0.12)
        assert get(port, "/api/hello") == (200, b"Hello from Erin\n")
        assert int(pid_file.read_text()) != child
        assert proc.wait(timeout=5) == 0


def test_stale_worker_message(build):
    host, guest = socket.socketpair()
    with host, guest:
        host.settimeout(5)
        with running(build / "telemachaid-worker", os.environ, guest, guest) as proc:
            guest.close()
            start = b"U1\n1\n17\n1\n"
            host.sendall(struct.pack("!I", len(start)) + start)
            with host.makefile("rb") as stream:
                size, = struct.unpack("!I", stream.read(4))
                assert stream.read(size) == b"U1\n4\n17\n1\nready"
                stale = b"U1\n2\n16\n2\n"
                host.sendall(struct.pack("!I", len(stale)) + stale)
                assert stream.read(1) == b""
            assert proc.wait(timeout=5) == 1


def test_bloom(build):
    source = b"text hello\ntext  world\nconcat\nreturn\n"
    compiled = subprocess.run([build / "bloomc"], input=source, capture_output=True, check=True)
    assert compiled.stdout == b"B1\nThello\nT world\nC\nR\n"
    executed = subprocess.run([build / "bloomrun"], input=compiled.stdout, capture_output=True,
                              check=True)
    assert executed.stdout == b"hello world"
    invalid = subprocess.run([build / "bloomrun"], input=b"B1\nC\nR\n", capture_output=True)
    assert invalid.returncode == 1 and not invalid.stdout


if __name__ == "__main__":
    build = Path(sys.argv[1]).resolve()
    test_bloom(build)
    test_stale_worker_message(build)
    test_dublin(build)
    print("Ulysses HTTP, worker recovery, and Bloomscript integration passed")
