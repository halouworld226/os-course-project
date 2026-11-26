"""Loopback web demonstration backed by the C++ image-processing service."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import nullcontext
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import secrets
import struct
import threading
import time
from urllib.parse import parse_qs, urlsplit

from client import MAX_FRAME, request, sample_ppm


ACCOUNTS = {"alice": "user", "bob": "user", "admin": "admin"}
PASSWORD = "demo123"
STATIC = Path(__file__).resolve().parent / "static"


def ppm_to_bmp(ppm):
    """Convert P6 RGB into a browser-readable BMP without imaging packages."""
    position = 0

    def token():
        nonlocal position
        while position < len(ppm):
            if ppm[position:position + 1].isspace():
                position += 1
            elif ppm[position] == 35:
                end = ppm.find(b"\n", position)
                position = len(ppm) if end < 0 else end + 1
            else:
                break
        start = position
        while position < len(ppm) and not ppm[position:position + 1].isspace() and ppm[position] != 35:
            position += 1
        return ppm[start:position]

    if token() != b"P6":
        raise ValueError("Expected a P6 PPM result")
    width, height, maximum = int(token()), int(token()), int(token())
    if not (1 <= width <= 2048 and 1 <= height <= 2048 and maximum == 255):
        raise ValueError("Invalid PPM dimensions or channel range")
    if position == len(ppm) or not ppm[position:position + 1].isspace():
        raise ValueError("Missing raster separator")
    separator = ppm[position]
    position += 1
    expected = width * height * 3
    if separator == 13 and ppm[position:position + 1] == b"\n" and len(ppm) - position == expected + 1:
        position += 1
    if len(ppm) - position != expected:
        raise ValueError("Invalid PPM raster length")
    stride = (width * 3 + 3) & ~3
    image = bytearray(stride * height)
    for y in range(height):
        source = position + y * width * 3
        target = (height - 1 - y) * stride
        row = ppm[source:source + width * 3]
        image[target:target + width * 3:3] = row[2::3]
        image[target + 1:target + width * 3:3] = row[1::3]
        image[target + 2:target + width * 3:3] = row[0::3]
    header = struct.pack("<2sIHHI", b"BM", 54 + len(image), 0, 0, 54)
    header += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(image), 2835, 2835, 0, 0)
    return header + image


class WebApp:
    def __init__(self, backend, data_directory, serialize_backend=True):
        self.backend = backend
        self.lock = threading.RLock()
        self.backend_lock = threading.Lock()
        self.serialize_backend = serialize_backend
        self.executor = ThreadPoolExecutor(max_workers=2, thread_name_prefix="image-submit")
        self.sessions = {}
        self.manifest = Path(data_directory).resolve() / "web-jobs.json"
        self.manifest.parent.mkdir(parents=True, exist_ok=True)
        self.jobs = json.loads(self.manifest.read_text(encoding="utf-8")) if self.manifest.exists() else {}
        for job in self.jobs.values():
            if job["state"] in ("processing", "queued", "running"):
                job.update(state="failed", error="The web bridge restarted before submission completed")

    def close(self):
        self.executor.shutdown(wait=True)

    def save(self):
        temporary = self.manifest.with_suffix(".tmp")
        temporary.write_text(json.dumps(self.jobs), encoding="utf-8")
        temporary.replace(self.manifest)

    def send(self, command, payload=None, username="admin"):
        authenticated_command = f"AUTH {username} {PASSWORD}\n{command}"
        with self.backend_lock if self.serialize_backend else nullcontext():
            return self.backend(authenticated_command, payload)

    def complete(self, ticket, operation, image, fault, priority):
        try:
            with self.lock:
                username = self.jobs[ticket]["owner"]
            verb = "SUBMIT" if self.serialize_backend else "ENQUEUE"
            command = f"{verb} {operation.upper()}" + (" HIGH" if priority == "high" else "") + (" FAULT" if fault else "")
            job_id = self.send(command, image, username).decode("ascii")
            while True:
                status = json.loads(self.send(f"STATUS {job_id}", username=username))
                with self.lock:
                    self.jobs[ticket].update(state=status["state"], job_id=job_id, status=status)
                    self.save()
                if status["state"] in ("complete", "failed"):
                    break
                time.sleep(.3)
        except Exception as error:
            with self.lock:
                self.jobs[ticket].update(state="failed", error=str(error))
                self.save()

    def authenticated(self, headers):
        cookie = SimpleCookie()
        try:
            cookie.load(headers.get("Cookie", ""))
        except Exception:
            return None
        value = cookie.get("os_session")
        with self.lock:
            return self.sessions.get(value.value) if value else None

    @staticmethod
    def response(status, data, mime="application/json", extra=None):
        payload = json.dumps(data).encode("utf-8") if mime == "application/json" else data
        headers = {"Content-Type": mime, "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff"}
        headers.update(extra or {})
        return status, headers, payload

    def dispatch(self, method, target, headers, body):
        path = urlsplit(target).path
        query = parse_qs(urlsplit(target).query)
        if method == "GET" and path in ("/", "/app.js", "/style.css"):
            filename, mime = {"/": ("index.html", "text/html; charset=utf-8"),
                              "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                              "/style.css": ("style.css", "text/css; charset=utf-8")}[path]
            return self.response(200, (STATIC / filename).read_bytes(), mime)
        if method == "POST" and path == "/api/login":
            try:
                credentials = json.loads(body)
                username = credentials.get("username", "")
                valid = username in ACCOUNTS and secrets.compare_digest(credentials.get("password", ""), PASSWORD)
            except (ValueError, TypeError, AttributeError):
                valid = False
            if not valid:
                return self.response(401, {"error": "Invalid demo account or password"})
            session = secrets.token_urlsafe(32)
            with self.lock:
                self.sessions[session] = username
            return self.response(200, {"username": username, "role": ACCOUNTS[username]}, extra={
                "Set-Cookie": f"os_session={session}; HttpOnly; SameSite=Strict; Path=/"})

        username = self.authenticated(headers)
        if not username:
            return self.response(401, {"error": "Sign in to continue"})
        if method == "GET" and path == "/api/me":
            return self.response(200, {"username": username, "role": ACCOUNTS[username]})
        if method == "GET" and path in ("/api/active", "/api/queues"):
            if ACCOUNTS[username] != "admin":
                return self.response(403, {"error": "Administrator access required"})
            if self.serialize_backend:
                return self.response(200, {"jobs": [], "high_queue": [], "normal_queue": [], "workers": [], "persistent_service": False})
            try:
                return self.response(200, json.loads(self.send("ACTIVE" if path == "/api/active" else "QUEUES")))
            except (OSError, ValueError) as error:
                return self.response(502, {"error": str(error)})
        if method == "POST" and path == "/api/logout":
            cookie = SimpleCookie(headers.get("Cookie", ""))
            with self.lock:
                self.sessions.pop(cookie["os_session"].value, None)
            return self.response(200, {}, extra={"Set-Cookie": "os_session=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict"})
        if method == "POST" and path.startswith("/api/workers/") and path.endswith("/terminate"):
            pid = path.split("/")[3]
            if ACCOUNTS[username] != "admin":
                return self.response(403, {"error": "Administrator access required"})
            if self.serialize_backend or not pid.isdecimal():
                return self.response(400, {"error": "Worker termination requires the persistent C++ TCP service"})
            try:
                result = self.send(f"KILL {pid}").decode("utf-8")
                return self.response(200, {"result": result})
            except (OSError, ValueError) as error:
                return self.response(409, {"error": str(error)})
        if method == "GET" and path == "/api/sample":
            return self.response(200, sample_ppm(), "image/x-portable-pixmap", {"Content-Disposition": 'attachment; filename="sample.ppm"'})
        if method == "POST" and path == "/api/jobs":
            operation = query.get("operation", ["grayscale"])[0]
            fault = query.get("fault", ["0"])[0] == "1"
            priority = query.get("priority", ["normal"])[0]
            if operation not in ("grayscale", "invert") or priority not in ("normal", "high") or not body or len(body) > MAX_FRAME:
                return self.response(400, {"error": "Select grayscale or invert and upload a P6 PPM below 16 MiB"})
            try:
                ppm_to_bmp(body)
            except (ValueError, IndexError):
                return self.response(400, {"error": "Upload a valid P6 PPM image with dimensions up to 2048 × 2048"})
            with self.lock:
                pending = sum(job["state"] in ("processing", "queued", "running") for job in self.jobs.values())
                if pending >= 8:
                    return self.response(429, {"error": "Eight jobs are pending. Wait for a job to finish"})
                ticket = secrets.token_hex(12)
                self.jobs[ticket] = {"ticket": ticket, "owner": username, "operation": operation,
                                     "priority": priority, "fault_requested": fault, "state": "processing"}
                self.save()
            self.executor.submit(self.complete, ticket, operation, body, fault, priority)
            return self.response(202, {"ticket": ticket})
        if method == "GET" and path == "/api/jobs":
            with self.lock:
                jobs = [dict(job) for job in self.jobs.values() if job["owner"] == username or ACCOUNTS[username] == "admin"]
            return self.response(200, {"jobs": jobs})
        if method == "GET" and path.startswith("/api/jobs/"):
            pieces = path.split("/")
            ticket = pieces[3]
            with self.lock:
                job = dict(self.jobs.get(ticket, {}))
            if not job or (job["owner"] != username and ACCOUNTS[username] != "admin"):
                return self.response(404, {"error": "Job not found"})
            if len(pieces) == 4:
                return self.response(200, job)
            if len(pieces) == 6 and pieces[4] == "tiles" and pieces[5].isdecimal():
                try:
                    tile = self.send(f"TILE {job['job_id']} {pieces[5]}", username=username)
                    return self.response(200, ppm_to_bmp(tile), "image/bmp")
                except (KeyError, OSError, ValueError) as error:
                    return self.response(409, {"error": str(error)})
            if len(pieces) == 5 and pieces[4] in ("preview", "download"):
                if job["state"] != "complete":
                    return self.response(409, {"error": "The C++ service has not completed this job"})
                try:
                    image = self.send(f"GET {job['job_id']}", username=username)
                    if pieces[4] == "preview":
                        return self.response(200, ppm_to_bmp(image), "image/bmp")
                    return self.response(200, image, "image/x-portable-pixmap", {
                        "Content-Disposition": f'attachment; filename="{job["job_id"]}.ppm"'})
                except (OSError, ValueError) as error:
                    return self.response(502, {"error": str(error)})
        return self.response(404, {"error": "Unknown endpoint"})


def handler_for(app):
    class Handler(BaseHTTPRequestHandler):
        def handle_request(self):
            try:
                size = int(self.headers.get("Content-Length", "0"))
                if size < 0 or size > MAX_FRAME:
                    response = app.response(413, {"error": "Request exceeds the 16 MiB limit"})
                else:
                    body = self.rfile.read(size)
                    if len(body) != size:
                        raise ValueError("Truncated request")
                    response = app.dispatch(self.command, self.path, self.headers, body)
            except (OSError, ValueError) as error:
                response = app.response(400, {"error": str(error)})
            status, headers, payload = response
            self.send_response(status)
            for key, value in headers.items():
                self.send_header(key, value)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        do_GET = handle_request
        do_POST = handle_request

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="build/os_server", help="C++ executable for a persistent local session")
    parser.add_argument("--data", default="demo-data")
    parser.add_argument("--port", type=int, help="connect to the C++ TCP service on this port")
    parser.add_argument("--web-port", type=int, default=8000)
    args = parser.parse_args()
    session = None
    if args.port:
        backend = lambda command, payload: request(args.port, command, payload)
    else:
        from client import LocalSession
        session = LocalSession(args.server, args.data)
        backend = session.request
    app = WebApp(backend, args.data, serialize_backend=False)
    try:
        with ThreadingHTTPServer(("127.0.0.1", args.web_port), handler_for(app)) as server:
            print(f"Open http://127.0.0.1:{server.server_port} — alice / bob / admin, password {PASSWORD}", flush=True)
            server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.close()
        if session:
            session.close()


if __name__ == "__main__":
    main()
