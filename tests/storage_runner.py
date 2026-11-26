import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="os-inode-check-") as directory:
    subprocess.run([sys.argv[1], directory], check=True, timeout=45)
