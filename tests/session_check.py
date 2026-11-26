"""Exercise concurrent jobs and admin recovery through a real persistent service."""

import json
import os
from pathlib import Path
import sys
import tempfile
import time

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from client import LocalSession


def main(executable):
    previous = os.environ.get("OS_DEMO_DELAY_MS")
    os.environ["OS_DEMO_DELAY_MS"] = "60"
    try:
        with tempfile.TemporaryDirectory(prefix="os-session-") as directory:
            normal_source = b"P6\n512 512\n255\n" + bytes((10, 80, 200)) * (512 * 512)
            high_source = b"P6\n65 65\n255\n" + bytes((3, 70, 250)) * (65 * 65)
            with LocalSession(executable, directory) as session:
                for invalid in ("SUBMIT BLUR", "ENQUEUE INVERT UNKNOWN", "SUBMIT INVERT HIGH HIGH"):
                    try:
                        session.request(invalid, high_source)
                    except ValueError:
                        pass
                    else:
                        raise AssertionError("invalid operation or flag accepted")
                    assert "high_queue" in json.loads(session.request("QUEUES"))
                normal_id = session.request("AUTH alice demo123\nENQUEUE GRAYSCALE", normal_source).decode()
                for forbidden in (f"AUTH bob demo123\nSTATUS {normal_id}",
                                  "AUTH alice demo123\nQUEUES", "AUTH alice wrong\nACTIVE"):
                    try:
                        session.request(forbidden)
                    except ValueError:
                        pass
                    else:
                        raise AssertionError("unauthorized request accepted")
                deadline = time.monotonic() + 30
                while True:
                    queues = json.loads(session.request("QUEUES"))
                    if queues["workers"]:
                        break
                    assert time.monotonic() < deadline, "no worker became busy"
                    time.sleep(0.01)
                pid = queues["workers"][0]["pid"]
                assert json.loads(session.request(f"KILL {pid}"))["terminated"]
                high_id = session.request("AUTH bob demo123\nENQUEUE INVERT HIGH", high_source).decode()
                while True:
                    high = json.loads(session.request(f"STATUS {high_id}"))
                    normal = json.loads(session.request(f"STATUS {normal_id}"))
                    assert high["state"] != "failed" and normal["state"] != "failed"
                    if high["state"] == "complete":
                        assert normal["tile_complete"] < normal["tile_total"], "high job did not finish while normal work remained"
                        break
                    assert time.monotonic() < deadline, "high-priority job timed out"
                    time.sleep(0.02)
                assert session.request(f"TILE {high_id} 3").startswith(b"P6\n1 1\n255\n")
                while normal["state"] != "complete":
                    assert time.monotonic() < deadline, "normal job timed out"
                    time.sleep(0.03)
                    normal = json.loads(session.request(f"STATUS {normal_id}"))
                events = normal["events"]
                assert any(event["event"] == "worker_exit" and event["worker_pid"] == pid for event in events)
                commits = [event["tile_id"] for event in events if event["event"] == "tile_committed"]
                assert len(commits) == len(set(commits)) == 64
                gray = (299 * 10 + 587 * 80 + 114 * 200 + 500) // 1000
                assert session.request(f"GET {normal_id}") == b"P6\n512 512\n255\n" + bytes((gray,) * 3) * (512 * 512)
                assert session.request(f"GET {high_id}") == b"P6\n65 65\n255\n" + bytes((252, 185, 5)) * (65 * 65)
            with LocalSession(executable, directory) as reopened:
                assert json.loads(reopened.request(f"STATUS {normal_id}"))["state"] == "complete"
                assert reopened.request(f"GET {high_id}").startswith(b"P6\n65 65\n255\n")
            print("PASS: persistent real service, concurrent normal/high jobs, live queues, admin kill, recovery, unique commits, exact pixels, inode reopen")
    finally:
        if previous is None:
            os.environ.pop("OS_DEMO_DELAY_MS", None)
        else:
            os.environ["OS_DEMO_DELAY_MS"] = previous


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()))
