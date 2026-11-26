"""Check binary framing, PPM boundaries, and failed request isolation."""

from io import BytesIO
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from client import read_reply, request_bytes


def run(executable, directory, request):
    process = subprocess.run([executable, "--stdio", str(directory)], input=request,
                             capture_output=True, timeout=30)
    if process.returncode:
        raise RuntimeError(process.stderr.decode(errors="replace"))
    return BytesIO(process.stdout)


def main(executable):
    with tempfile.TemporaryDirectory(prefix="os-protocol-") as directory:
        def reject(request):
            try:
                read_reply(run(executable, directory, request))
            except ValueError as error:
                assert "request failed" in str(error), str(error)
            else:
                raise AssertionError("invalid request accepted")

        for command in ("UNKNOWN", "GET ../disk.img", "STATUS ../../x", "SUBMIT BLUR"):
            reject(request_bytes(command))
        reject(b"SUBMIT GRAYSCALE\n" + struct.pack("!I", 16 * 1024 * 1024 + 1))
        reject(b"SUBMIT GRAYSCALE\n" + struct.pack("!I", 12) + b"short")
        for header in (b"P6\n0 1\n255\n", b"P6\n2049 1\n255\n", b"P6\n1 1\n256\n",
                       b"P3\n1 1\n255\n", b"P6\n1 1\n255\n\x01\x02"):
            reject(request_bytes("SUBMIT GRAYSCALE", header))

        count = 0
        for width, height in ((1, 1), (63, 1), (64, 64), (65, 65), (130, 67)):
            raster = bytes((i * 17 + 10) % 256 for i in range(width * height * 3))
            source = f"P6\n# binary raster test\n{width} {height}\n255\r\n".encode() + raster
            for operation in ("GRAYSCALE", "INVERT"):
                job_id = read_reply(run(executable, directory, request_bytes(f"SUBMIT {operation}", source))).decode()
                status = json.loads(read_reply(run(executable, directory, request_bytes(f"STATUS {job_id}"))))
                expected_count = ((width + 63) // 64) * ((height + 63) // 64)
                assert status["tile_total"] == status["tile_complete"] == expected_count
                result = read_reply(run(executable, directory, request_bytes(f"GET {job_id}")))
                expected = bytearray()
                for offset in range(0, len(raster), 3):
                    red, green, blue = raster[offset:offset + 3]
                    if operation == "INVERT":
                        expected.extend((255 - red, 255 - green, 255 - blue))
                    else:
                        value = (299 * red + 587 * green + 114 * blue + 500) // 1000
                        expected.extend((value,) * 3)
                assert result == f"P6\n{width} {height}\n255\n".encode() + expected
                count += 1
        print(f"PASS: {count} boundary image jobs, comments/CRLF, both operations, oversized/truncated frames, invalid paths")


if __name__ == "__main__":
    main(str(Path(sys.argv[1]).resolve()))
