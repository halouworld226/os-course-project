"""Command-line client for the loopback-only starter server."""

import argparse
from io import BytesIO
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading


MAX_FRAME = 16 * 1024 * 1024


class LocalSession:
    """A persistent C++ service using the same protocol over process pipes."""

    def __init__(self, executable, directory):
        self.lock = threading.Lock()
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([str(Path(executable).resolve()), "--session", str(directory)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.log)

    def request(self, command, payload=None):
        with self.lock:
            if self.process.poll() is not None:
                raise ValueError("local service has exited")
            self.process.stdin.write(request_bytes(command, payload))
            self.process.stdin.flush()
            return read_reply(self.process.stdout)

    def close(self):
        with self.lock:
            if self.process.stdin and not self.process.stdin.closed:
                self.process.stdin.close()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=5)
            self.process.stdout.close()
            self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def request_bytes(command, payload=None):
    line = command.encode("ascii") + b"\n"
    if payload is None:
        return line
    if len(payload) > MAX_FRAME:
        raise ValueError("input exceeds the 16 MiB frame limit")
    return line + struct.pack("!I", len(payload)) + payload


def read_reply(response):
    status = response.readline(8)
    if status not in (b"OK\n", b"ERR\n"):
        raise ValueError("invalid server response")
    header = response.read(4)
    if len(header) != 4:
        raise ValueError("missing response length")
    size = struct.unpack("!I", header)[0]
    if size > MAX_FRAME:
        raise ValueError("response exceeds the frame limit")
    reply = response.read(size)
    if len(reply) != size:
        raise ValueError("truncated server response")
    if status == b"ERR\n":
        raise ValueError(reply.decode("utf-8"))
    return reply


def request(port, command, payload=None):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as sock:
        sock.sendall(request_bytes(command, payload))
        with sock.makefile("rb") as response:
            return read_reply(response)


def local_request(executable, directory, command, payload=None):
    response = subprocess.run([str(Path(executable).resolve()), "--stdio", directory],
                              input=request_bytes(command, payload), capture_output=True, timeout=15)
    if response.returncode != 0:
        raise ValueError(response.stderr.decode(errors="replace").strip() or "local server exited with an error")
    try:
        return read_reply(BytesIO(response.stdout))
    except ValueError as error:
        log = response.stderr.decode(errors="replace").strip()
        raise ValueError(f"{error}\n{log}" if log else str(error)) from error


def sample_ppm():
    width, height = 130, 67
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            pixels.extend(((17 * x + 3 * y) % 256, (7 * x + 19 * y) % 256, (10 + x + 11 * y) % 256))
    pixels[:3] = b"\x0a\x20\x23"
    return f"P6\n{width} {height}\n255\n".encode("ascii") + pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--user", choices=("alice", "bob", "admin"), default="alice")
    parser.add_argument("--password", default="demo123", help="preconfigured local demonstration password")
    parser.add_argument("--local", nargs=2, metavar=("SERVER", "DATA_DIR"), help="use the local server via stdio")
    commands = parser.add_subparsers(dest="action", required=True)
    commands.add_parser("sample").add_argument("output", type=Path)
    submit = commands.add_parser("submit")
    submit.add_argument("input", type=Path)
    submit.add_argument("--operation", choices=("grayscale", "invert"), default="grayscale")
    submit.add_argument("--fault-once", action="store_true", help="terminate one owned worker and retry its tile")
    submit.add_argument("--priority", choices=("normal", "high"), default="normal")
    commands.add_parser("status").add_argument("job_id")
    download = commands.add_parser("download")
    download.add_argument("job_id")
    download.add_argument("output", type=Path)
    args = parser.parse_args()

    def send(command, payload=None):
        command = f"AUTH {args.user} {args.password}\n" + command
        if args.local:
            return local_request(*args.local, command, payload)
        return request(args.port, command, payload)

    try:
        if args.action == "sample":
            args.output.write_bytes(sample_ppm())
        elif args.action == "submit":
            with args.input.open("rb") as source:
                image = source.read(MAX_FRAME + 1)
            command = f"SUBMIT {args.operation.upper()}" + (" FAULT" if args.fault_once else "")
            if args.priority == "high":
                command += " HIGH"
            print(send(command, image).decode("ascii"))
        elif args.action == "status":
            status = send(f"STATUS {args.job_id}")
            print(json.dumps(json.loads(status), indent=2))
        else:
            args.output.write_bytes(send(f"GET {args.job_id}"))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(f"client: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
