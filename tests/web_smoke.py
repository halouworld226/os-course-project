"""Verify web routes and ownership against a real local C++ backend."""

import json
from contextlib import ExitStack
import os
from pathlib import Path
import struct
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from client import local_request, sample_ppm
from web import WebApp, ppm_to_bmp


def login(app, username):
    status, headers, body = app.dispatch("POST", "/api/login", {}, json.dumps({
        "username": username, "password": "demo123"}).encode())
    assert status == 200, body
    return {"Cookie": headers["Set-Cookie"].split(";", 1)[0]}


def main(executable, persistent=False):
    with tempfile.TemporaryDirectory(prefix="os-web-check-") as directory, ExitStack() as cleanup:
        session = None
        if persistent:
            from client import LocalSession
            previous_delay = os.environ.get("OS_DEMO_DELAY_MS")
            os.environ["OS_DEMO_DELAY_MS"] = "250"
            try:
                session = cleanup.enter_context(LocalSession(executable, directory))
            finally:
                if previous_delay is None:
                    os.environ.pop("OS_DEMO_DELAY_MS", None)
                else:
                    os.environ["OS_DEMO_DELAY_MS"] = previous_delay
            backend = session.request
        else:
            backend = lambda command, payload: local_request(executable, directory, command, payload)
        app = WebApp(backend, directory, serialize_backend=not persistent)
        try:
            assert app.dispatch("GET", "/", {}, b"")[0] == 200
            assert app.dispatch("GET", "/api/jobs", {}, b"")[0] == 401
            assert app.dispatch("POST", "/api/login", {}, b"[]")[0] == 401
            alice, bob, admin = (login(app, username) for username in ("alice", "bob", "admin"))
            assert app.dispatch("POST", "/api/jobs", alice, b"broken")[0] == 400
            assert app.dispatch("POST", "/api/jobs?operation=blur", alice, sample_ppm())[0] == 400
            tickets = []
            for operation in ("grayscale", "invert"):
                status, _, body = app.dispatch("POST", f"/api/jobs?operation={operation}", alice, sample_ppm())
                assert status == 202, body
                tickets.append(json.loads(body)["ticket"])
            if persistent:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    status, _, body = app.dispatch("GET", "/api/queues", admin, b"")
                    assert status == 200, body
                    queues = json.loads(body)
                    busy = [worker for worker in queues["workers"] if worker.get("job_id")]
                    if busy:
                        break
                    time.sleep(.01)
                assert busy, queues
                pid = busy[0]["pid"]
                assert app.dispatch("POST", f"/api/workers/{pid}/terminate", bob, b"")[0] == 403
                response = app.dispatch("POST", f"/api/workers/{pid}/terminate", admin, b"")
                assert response[0] == 200, response[2]
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                records = [json.loads(app.dispatch("GET", f"/api/jobs/{ticket}", alice, b"")[2]) for ticket in tickets]
                if all(record["state"] in ("complete", "failed") for record in records):
                    break
                time.sleep(.05)
            assert all(record["state"] == "complete" for record in records), records
            assert records[0]["job_id"] != records[1]["job_id"]
            if persistent:
                events = [event for record in records for event in record["status"]["events"]]
                assert any(event["event"] == "worker_exit" for event in events), events
                assert any(event["event"] == "tile_requeued" for event in events), events
            for command in (f"AUTH bob demo123\nGET {records[0]['job_id']}", "AUTH alice demo123\nQUEUES"):
                try:
                    backend(command, None)
                except ValueError:
                    pass
                else:
                    raise AssertionError("C++ backend accepted an unauthorized command")
            assert json.loads(app.dispatch("GET", "/api/jobs", bob, b"")[2])["jobs"] == []
            assert len(json.loads(app.dispatch("GET", "/api/jobs", admin, b"")[2])["jobs"]) == 2
            for ticket in tickets:
                assert app.dispatch("GET", f"/api/jobs/{ticket}", bob, b"")[0] == 404
                assert app.dispatch("GET", f"/api/jobs/{ticket}/download", bob, b"")[0] == 404
                status, headers, bmp = app.dispatch("GET", f"/api/jobs/{ticket}/preview", admin, b"")
                assert status == 200 and headers["Content-Type"] == "image/bmp"
                assert bmp[:2] == b"BM" and struct.unpack_from("<ii", bmp, 18) == (130, 67)
                status, _, ppm = app.dispatch("GET", f"/api/jobs/{ticket}/download", alice, b"")
                assert status == 200 and ppm_to_bmp(ppm) == bmp
                if persistent:
                    status, headers, tile = app.dispatch("GET", f"/api/jobs/{ticket}/tiles/0", alice, b"")
                    assert status == 200 and headers["Content-Type"] == "image/bmp", tile
                    assert struct.unpack_from("<ii", tile, 18) == (64, 64)
            assert app.dispatch("POST", "/api/logout", alice, b"")[0] == 200
            assert app.dispatch("GET", "/api/jobs", alice, b"")[0] == 401
        finally:
            app.close()

        reopened = WebApp(backend, directory, serialize_backend=not persistent)
        try:
            alice = login(reopened, "alice")
            assert len(json.loads(reopened.dispatch("GET", "/api/jobs", alice, b"")[2])["jobs"]) == 2
        finally:
            reopened.close()

    pixel = ppm_to_bmp(b"P6\n1 1\n255\n\x01\x02\x03")
    assert pixel[54:] == b"\x03\x02\x01\x00"
    check_async_routes()
    print("PASS: web route dispatch, demo sessions, C++ permission checks, real grayscale/invert, BMP previews, manifest reopen")
    if persistent:
        print("PASS: persistent ENQUEUE/STATUS, actual ready queues, admin busy-worker termination, exit/requeue recovery, tile preview")


def check_async_routes():
    calls = []
    status_calls = 0

    def backend(command, payload):
        nonlocal status_calls
        authentication, command = command.split("\n", 1)
        assert authentication in ("AUTH alice demo123", "AUTH admin demo123")
        if command.startswith("ENQUEUE ") or command.startswith("STATUS "):
            assert authentication == "AUTH alice demo123"
        if command.startswith("KILL ") or command in ("ACTIVE", "QUEUES"):
            assert authentication == "AUTH admin demo123"
        calls.append(command)
        if command.startswith("ENQUEUE "):
            return b"1000-0"
        if command == "STATUS 1000-0":
            status_calls += 1
            return json.dumps({"state": "running" if status_calls == 1 else "complete",
                               "width": 1, "height": 1, "events": []}).encode()
        if command == "TILE 1000-0 0":
            return b"P6\n1 1\n255\n\x01\x02\x03"
        if command == "KILL 123":
            return b"terminated"
        if command == "ACTIVE":
            return b'{"jobs":[]}'
        if command == "QUEUES":
            return b'{"high_queue":[],"normal_queue":[],"workers":[]}'
        raise AssertionError(command)

    with tempfile.TemporaryDirectory(prefix="os-web-async-") as directory:
        app = WebApp(backend, directory, serialize_backend=False)
        try:
            alice, admin = login(app, "alice"), login(app, "admin")
            status, _, body = app.dispatch("POST", "/api/jobs?operation=invert&priority=high&fault=1", alice, sample_ppm())
            assert status == 202
            ticket = json.loads(body)["ticket"]
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                record = json.loads(app.dispatch("GET", f"/api/jobs/{ticket}", alice, b"")[2])
                if record["state"] == "complete":
                    break
                time.sleep(.05)
            assert record["state"] == "complete" and status_calls >= 2
            assert calls[0] == "ENQUEUE INVERT HIGH FAULT"
            assert app.dispatch("POST", "/api/workers/123/terminate", alice, b"")[0] == 403
            assert app.dispatch("POST", "/api/workers/123/terminate", admin, b"")[0] == 200
            assert "KILL 123" in calls
            assert app.dispatch("GET", "/api/active", alice, b"")[0] == 403
            assert app.dispatch("GET", "/api/active", admin, b"")[0] == 200
            assert app.dispatch("GET", "/api/queues", alice, b"")[0] == 403
            assert app.dispatch("GET", "/api/queues", admin, b"")[0] == 200
            status, headers, preview = app.dispatch("GET", f"/api/jobs/{ticket}/tiles/0", alice, b"")
            assert status == 200 and headers["Content-Type"] == "image/bmp" and preview[:2] == b"BM"
        finally:
            app.close()


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()), "--session" in sys.argv[2:])
