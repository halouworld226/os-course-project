"""Build verification through the server's network and real worker process."""

from contextlib import contextmanager
from io import BytesIO
import json
from pathlib import Path
import subprocess
import sys
import tempfile


CLIENT = Path(__file__).resolve().parents[1] / "client.py"
sys.path.insert(0, str(CLIENT.parent))
sys.dont_write_bytecode = True
from client import read_reply, request_bytes


def cli(*args, check=True):
    result = subprocess.run([sys.executable, str(CLIENT), *map(str, args)],
                            text=True, capture_output=True, timeout=12)
    if check and result.returncode:
        raise RuntimeError(f"client {args}: {result.stderr.strip()}")
    return result


@contextmanager
def running_server(executable, root):
    with tempfile.TemporaryFile(mode="w+t", encoding="utf-8") as log:
        server = subprocess.Popen([executable, "0", str(root)], stdout=subprocess.PIPE,
                                  stderr=log, text=True)
        try:
            line = server.stdout.readline().strip()
            assert line.startswith("PORT "), line
            yield server.pid, int(line.split()[1])
        except Exception:
            log.seek(0)
            print(log.read(), file=sys.stderr)
            raise
        finally:
            server.terminate()
            server.wait(timeout=5)
            server.stdout.close()


def serial_reference(ppm, operation):
    header, raster = ppm.split(b"\n255\n", 1)
    if operation == "invert":
        return header + b"\n255\n" + bytes(255 - value for value in raster)
    gray = bytearray()
    for offset in range(0, len(raster), 3):
        red, green, blue = raster[offset:offset + 3]
        value = (299 * red + 587 * green + 114 * blue + 500) // 1000
        gray.extend((value, value, value))
    return header + b"\n255\n" + gray


def check_events(status, job_id, server_pid, operation, fault=False):
    assert status["job_id"] == job_id
    assert status["operation"] == operation
    assert status["state"] == "complete"
    assert status["tile_total"] == status["tile_complete"] == 6
    events = status["events"]
    assert len(events) == (15 if fault else 12), events
    pids = {event["worker_pid"] for event in events}
    assert 1 <= len(pids) <= (5 if fault else 4) and min(pids) > 0 and server_pid not in pids
    for tile_id in range(6):
        history = [event for event in events if event["tile_id"] == tile_id]
        dispatched = [event for event in history if event["event"] == "tile_dispatched"][-1]
        commits = [event for event in history if event["event"] == "tile_committed"]
        assert len(commits) == 1
        committed = commits[0]
        assert dispatched["timestamp_ms"] <= committed["timestamp_ms"]
        for event in (dispatched, committed):
            assert event["job_id"] == job_id and event["tile_id"] == tile_id and event["attempt_id"] >= 1
    edge = next(event for event in events if event["tile_id"] == 5)
    assert (edge["x"], edge["y"], edge["width"], edge["height"]) == (128, 64, 2, 3)
    assert sum(event["event"] == "worker_exit" for event in events) == int(fault)
    assert sum(event["event"] == "tile_requeued" for event in events) == int(fault)
    return next(iter(pids))


INVALID_PPM = b"P6\n130 67\n255\n\x01\x02"
SERVER_ERROR = "request failed; see server log"


def verify_tcp(executable, root, source, expected):
    invalid = root / "invalid.ppm"
    invalid.write_bytes(INVALID_PPM)
    jobs = {}
    with running_server(executable, root / "jobs") as (server_pid, port):
        rejection = cli("--port", port, "submit", invalid, check=False)
        assert rejection.returncode == 1 and SERVER_ERROR in rejection.stderr, rejection.stderr
        for operation, reference in expected.items():
            result = root / f"{operation}.ppm"
            job_id = cli("--port", port, "submit", source, "--operation", operation).stdout.strip()
            status = json.loads(cli("--port", port, "status", job_id).stdout)
            worker_pid = check_events(status, job_id, server_pid, operation)
            jobs[operation] = (job_id, result, worker_pid)
            cli("--port", port, "download", job_id, result)
            assert result.read_bytes() == reference, f"{operation} differs from the serial reference"
        assert jobs["grayscale"][0] != jobs["invert"][0]
    with running_server(executable, root / "jobs") as (_, port):
        for operation, (job_id, result, _) in jobs.items():
            cli("--port", port, "download", job_id, result)
            assert result.read_bytes() == expected[operation], f"reopened {operation} output differs from the reference"
    return [(server_pid, item[2]) for item in jobs.values()]


def verify_stdio(executable, root, source, expected):
    def process(command, payload=None):
        request = request_bytes(command, payload)
        with subprocess.Popen([executable, "--stdio", str(root / "jobs")], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE) as server:
            try:
                stdout, stderr = server.communicate(request, timeout=12)
            except subprocess.TimeoutExpired:
                server.kill()
                server.communicate()
                raise
        assert server.returncode == 0, stderr.decode(errors="replace")
        return server.pid, BytesIO(stdout), stderr

    _, response, _ = process("SUBMIT GRAYSCALE", INVALID_PPM)
    try:
        read_reply(response)
    except ValueError as error:
        assert str(error) == SERVER_ERROR, str(error)
    else:
        raise AssertionError("malformed input was accepted")

    processes = []
    jobs = set()
    for operation, reference in expected.items():
        server_pid, response, log = process(f"SUBMIT {operation.upper()}", source.read_bytes())
        try:
            job_id = read_reply(response).decode("ascii")
        except ValueError as error:
            raise RuntimeError(log.decode(errors="replace")) from error
        assert job_id not in jobs, "duplicate job ID"
        jobs.add(job_id)
        local = ("--user", "admin", "--local", executable, root / "jobs")
        status = json.loads(cli(*local, "status", job_id).stdout)
        worker_pid = check_events(status, job_id, server_pid, operation)
        result = root / f"{operation}.ppm"
        cli(*local, "download", job_id, result)
        assert result.read_bytes() == reference, f"reopened {operation} output differs from the reference"
        processes.append((server_pid, worker_pid))

    server_pid, response, _ = process("SUBMIT INVERT HIGH FAULT", source.read_bytes())
    fault_id = read_reply(response).decode("ascii")
    status = json.loads(cli(*local, "status", fault_id).stdout)
    assert status["priority"] == "high"
    check_events(status, fault_id, server_pid, "invert", fault=True)
    output = root / "recovered.ppm"
    cli(*local, "download", fault_id, output)
    assert output.read_bytes() == expected["invert"], "recovery output differs"
    _, response, _ = process(f"TILE {fault_id} 5")
    edge = read_reply(response)
    assert edge.startswith(b"P6\n2 3\n255\n") and len(edge.split(b"255\n", 1)[1]) == 18

    single = root / "one-pixel.ppm"
    single.write_bytes(b"P6\n1 1\n255\n\x00\x0a\xff")
    job_id = cli(*local, "submit", single, "--operation", "invert").stdout.strip()
    result = root / "one-pixel-invert.ppm"
    cli(*local, "download", job_id, result)
    assert result.read_bytes() == serial_reference(single.read_bytes(), "invert")
    return processes


def main(executable, mode):
    with tempfile.TemporaryDirectory(prefix="os-starter-") as directory:
        root = Path(directory)
        source = root / "source.ppm"
        cli("sample", source)
        expected = {operation: serial_reference(source.read_bytes(), operation) for operation in ("grayscale", "invert")}
        verify = {"--tcp": verify_tcp, "--stdio": verify_stdio}[mode]
        processes = verify(executable, root, source, expected)
        print(f"PASS: {mode[2:]} grayscale and invert, six tiles each, pixel equality, malformed upload rejected, "
              f"temporary storage reopened; server/worker PID pairs: {processes}")


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()), sys.argv[2] if len(sys.argv) > 2 else "--tcp")
