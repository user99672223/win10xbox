"""Real program over the network: curl.exe (tools/ci/build_curl.ps1) through win10xbox against a host-side HTTP server.

    python -I realcurl_http.py --host <win10xbox.exe> --machine <machine dir> [--emulator <wine>]

Starts a small HTTP/1.1 server on 127.0.0.1 (an ephemeral port, in a thread of this process), then runs the guest
curl through the layer for each case below and checks its exit code, its stdout, the file it wrote into the machine
folder and what the server saw (method, path, User-Agent, request body):

  GET  /hello.txt             body to stdout
  GET  /big.bin -o <file>     1 MiB, compared by SHA-256 (several recv() calls, -w size_download)
  POST /echo -d ...           the request body travels guest -> host, the server answers with what it received
  GET  /redirect -L           a 302 followed on the same connection
  GET  /missing -f            404 -> curl exit 22
  HEAD /hello.txt -I          the status line and headers
  a refused port              curl exit 7 (connect failure must be reported, not hang)

Every guest run is bounded by a timeout. Exit code 0 when every case passed, 1 otherwise.
"""
import argparse
import hashlib
import http.server
import os
import socket
import subprocess
import sys
import threading

HELLO = b"hello from the host http server\n"
BIG = b"".join(hashlib.sha256(str(i).encode()).digest() for i in range(32768))  # 1 MiB, deterministic
SEEN = []
SEEN_LOCK = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "win10xbox-test/1"

    def log_message(self, fmt, *args):
        sys.stdout.write("server: " + (fmt % args) + "\n")
        sys.stdout.flush()

    def _record(self, body=b""):
        with SEEN_LOCK:
            SEEN.append((self.command, self.path, self.headers.get("User-Agent", ""), body))

    def _send(self, code, body, ctype="text/plain", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self._record()
        if self.path == "/hello.txt":
            self._send(200, HELLO)
        elif self.path == "/big.bin":
            self._send(200, BIG, "application/octet-stream")
        elif self.path == "/redirect":
            self._send(302, b"moved\n", extra={"Location": "/hello.txt"})
        else:
            self._send(404, b"not found\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n) if n else b""
        self._record(body)
        if self.path == "/echo":
            self._send(200, b"received " + str(len(body)).encode() + b" bytes: " + body + b"\n")
        else:
            self._send(404, b"not found\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--machine", required=True)
    ap.add_argument("--emulator", default="")
    ap.add_argument("--timeout", type=int, default=180)
    a = ap.parse_args()

    machine = a.machine
    if a.emulator and machine.startswith("/"):
        machine = "Z:" + machine
    machine = machine.replace("/", "\\")
    work = os.path.join(a.machine, "workcurl_http")
    os.makedirs(work, exist_ok=True)
    big_out = os.path.join(work, "big.bin")
    if os.path.exists(big_out):
        os.remove(big_out)

    env = dict(os.environ)
    env["WIN10XBOX_DEBUG"] = os.environ.get("REALCURL_DEBUG", "-all")

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port
    print("server: listening on %s" % base, flush=True)

    # A port nobody listens on, for the refused-connection case.
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    dead_port = s.getsockname()[1]
    s.close()

    fails = []

    def run(label, args, expect_rc, check=None):
        cmd = ([a.emulator] if a.emulator else []) + [a.host, "--machine", machine, "C:\\guest\\curl.exe"] + args
        try:
            p = subprocess.run(cmd, capture_output=True, timeout=a.timeout, env=env)
            rc, out, err = p.returncode, p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            rc, out, err = "timeout", e.stdout or b"", e.stderr or b""
        print("=== curl %s -> exit %s" % (" ".join(args), rc))
        print(out.decode("latin-1")[:4000])
        if err:
            print("--- stderr ---\n" + err.decode("latin-1")[:4000])
        ok = rc == expect_rc
        if not ok:
            fails.append("%s: exit %s, expected %s" % (label, rc, expect_rc))
        if check:
            msg = check(out)
            if msg:
                fails.append("%s: %s" % (label, msg))
                ok = False
        print("realcurl_http: %s %s" % (label, "ok" if ok else "FAILED"), flush=True)

    common = ["-sS", "--max-time", "45"]
    run("get", common + [base + "/hello.txt"], 0,
        lambda o: None if o == HELLO else "stdout %r, expected %r" % (o[:200], HELLO))

    def check_big(o):
        if o.strip() != b"size=%d" % len(BIG):
            return "stdout %r, expected size=%d" % (o[:200], len(BIG))
        if not os.path.exists(big_out):
            return "big.bin was not written"
        got = open(big_out, "rb").read()
        if hashlib.sha256(got).digest() != hashlib.sha256(BIG).digest():
            return "big.bin: %d bytes, SHA-256 mismatch" % len(got)
        return None
    run("get big -o", common + ["-o", "C:\\workcurl_http\\big.bin", "-w", "size=%{size_download}", base + "/big.bin"],
        0, check_big)

    payload = b"alpha=1&beta=two+words"
    run("post", common + ["-d", payload.decode(), base + "/echo"], 0,
        lambda o: None if o == b"received %d bytes: %s\n" % (len(payload), payload) else "stdout %r" % o[:200])
    run("redirect -L", common + ["-L", base + "/redirect"], 0,
        lambda o: None if o == HELLO else "stdout %r, expected %r" % (o[:200], HELLO))
    run("404 -f", common + ["-f", base + "/missing"], 22)
    run("head -I", common + ["-I", base + "/hello.txt"], 0,
        lambda o: None if (o.startswith(b"HTTP/1.1 200") and b"Content-Length: %d" % len(HELLO) in o)
        else "stdout %r" % o[:300])
    run("refused", ["-sS", "--max-time", "30", "http://127.0.0.1:%d/" % dead_port], 7)

    srv.shutdown()
    with SEEN_LOCK:
        seen = list(SEEN)
    for m, path, ua, body in seen:
        print("server saw: %s %s UA=%r body=%r" % (m, path, ua, body[:80]))
    if not any(m == "POST" and path == "/echo" and body == payload for m, path, ua, body in seen):
        fails.append("server never received the POST body")
    if not all(ua.startswith("curl/") for m, path, ua, body in seen):
        fails.append("a request without a curl/ User-Agent")

    for f in fails:
        print("FAIL: " + f)
    print("realcurl_http: done fails=%d" % len(fails), flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
