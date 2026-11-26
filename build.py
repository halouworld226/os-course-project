"""Build with a C++17 compiler and run checks without a Make/Ninja dependency."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--tcp", action="store_true", help="also run real loopback TCP tests")
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--output", type=Path, default=ROOT / "build")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    if not compiler:
        parser.error(f"C++17 compiler {args.compiler!r} was not found")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    windows = sys.platform == "win32"
    suffix = ".exe" if windows else ""

    def run(command):
        print("Running", " ".join(map(str, command)), flush=True)
        subprocess.run(list(map(str, command)), check=True, cwd=ROOT, timeout=180)

    def build(name, sources, network=False):
        executable = output / (name + suffix)
        command = [compiler, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
                   "-I", ROOT / "src", *[ROOT / source for source in sources]]
        if network and windows:
            command += ["-lws2_32"]
        run(command + ["-o", executable])
        return executable

    common = ["src/image.cpp", "src/worker.cpp"]
    server = build("os_server", ["src/server.cpp", "src/net.cpp", "src/storage.cpp", "src/inode_fs.cpp",
                                 "src/scheduler.cpp", "src/job_scheduler.cpp", *common], network=True)
    if args.test:
        run([sys.executable, ROOT / "tests/smoke.py", server, "--stdio"])
        run([sys.executable, ROOT / "tests/protocol_check.py", server])
        for name, sources in (
            ("scheduler_check", ["tests/scheduler_check.cpp", "src/scheduler.cpp", *common]),
            ("shared_scheduler_check", ["tests/shared_scheduler_check.cpp", "src/job_scheduler.cpp", *common]),
            ("worker_handle_check", ["tests/worker_handle_check.cpp", *common]),
            ("storage_check", ["tests/storage_check.cpp", "src/storage.cpp", "src/inode_fs.cpp", "src/image.cpp"]),
        ):
            executable = build(name, sources)
            if name == "storage_check":
                with tempfile.TemporaryDirectory(prefix="os-storage-check-") as directory:
                    run([executable, directory])
            else:
                run([executable])
        run([sys.executable, ROOT / "tests/web_smoke.py", server])
        run([sys.executable, ROOT / "tests/session_check.py", server])
        run([sys.executable, ROOT / "tests/web_smoke.py", server, "--session"])
        if args.tcp:
            run([sys.executable, ROOT / "tests/smoke.py", server, "--tcp"])
    print("Build and requested checks completed.")


if __name__ == "__main__":
    main()
