#!/usr/bin/env python3
"""TLS echo server for integration.guest_net2 (tests/guest/net2.c).

  net2_tls_server.py start <port> <state-dir>   generate a self-signed certificate (openssl, or the cryptography
                                                package when no openssl is found), start the server detached on
                                                127.0.0.1:<port>, return once it accepts connections
  net2_tls_server.py stop  <state-dir>          stop the server started in <state-dir>
  net2_tls_server.py serve <port> <state-dir>   (internal) run in the foreground

The certificate and key live in <state-dir> and exist only for the test run. The server echoes every byte it
receives and shuts itself down after ten minutes if nobody stops it.
"""
import os
import shutil
import hashlib
import socket
import ssl
import subprocess
import sys
import threading
import time

LIFETIME = 600


def find_openssl():
    found = shutil.which("openssl")
    if found:
        return found
    for base in (os.environ.get("ProgramFiles", r"C:\Program Files"), os.environ.get("ProgramW6432", "")):
        for sub in (r"Git\usr\bin", r"Git\mingw64\bin", r"OpenSSL-Win64\bin"):
            cand = os.path.join(base, sub, "openssl.exe")
            if base and os.path.exists(cand):
                return cand
    return None


def make_cert(state, cert, key):
    exe = find_openssl()
    if exe:
        env = dict(os.environ, MSYS2_ARG_CONV_EXCL="*")
        subprocess.run([exe, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha256", "-days", "2",
                        "-subj", "/CN=localhost", "-keyout", key, "-out", cert], check=True, env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    import datetime
    k = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(x509.oid.NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    c = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(k.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=5))
         .not_valid_after(now + datetime.timedelta(days=2)).sign(k, hashes.SHA256()))
    with open(cert, "wb") as f:
        f.write(c.public_bytes(serialization.Encoding.PEM))
    with open(key, "wb") as f:
        f.write(k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption()))


def handle(conn):
    try:
        with conn:
            while True:
                data = conn.recv(16384)
                if not data:
                    break
                conn.sendall(data)
    except (OSError, ssl.SSLError):
        pass


def tap(raw):
    """Diagnostic: put a logging relay between the client and the TLS engine and return the engine's end. Every chunk
    that crosses is logged with its direction (c2s read from the client, s2c written to it), length, sha256 and bytes."""
    a, b = socket.socketpair()

    def pump(src, dst, tag):
        try:
            while True:
                d = src.recv(16384)
                if not d:
                    break
                print("%s %d sha256=%s %s" % (tag, len(d), hashlib.sha256(d).hexdigest(), d[:400].hex()), flush=True)
                dst.sendall(d)
        except OSError:
            pass
        try:
            dst.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    threading.Thread(target=pump, args=(raw, a, "c2s"), daemon=True).start()
    threading.Thread(target=pump, args=(a, raw, "s2c"), daemon=True).start()
    return b


def serve(port, state):
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(os.path.join(state, "cert.pem"), os.path.join(state, "key.pem"))
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    srv.settimeout(1.0)
    deadline = time.time() + LIFETIME
    stop = os.path.join(state, "stop")
    while time.time() < deadline and not os.path.exists(stop):
        try:
            raw, _ = srv.accept()
        except socket.timeout:
            continue
        try:
            inner = tap(raw)
            conn = ctx.wrap_socket(inner, server_side=True)
        except (OSError, ssl.SSLError) as e:
            if not isinstance(e, (ssl.SSLEOFError, ConnectionError)) and "EOF" not in str(e):   # not the start probe
                print("handshake error: %r" % (e,), file=sys.stderr, flush=True)
            raw.close()
            inner.close()
            continue
        threading.Thread(target=handle, args=(conn,), daemon=True).start()


def start(port, state):
    os.makedirs(state, exist_ok=True)
    stop = os.path.join(state, "stop")
    if os.path.exists(stop):
        os.remove(stop)
    make_cert(state, os.path.join(state, "cert.pem"), os.path.join(state, "key.pem"))
    log = open(os.path.join(state, "server.log"), "ab")
    kw = {}
    if os.name == "nt":
        kw["creationflags"] = 0x00000008 | 0x00000200   # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
    else:
        kw["start_new_session"] = True
    subprocess.Popen([sys.executable, os.path.abspath(__file__), "serve", str(port), state], stdin=subprocess.DEVNULL,
                     stdout=log, stderr=log, close_fds=True, **kw)
    end = time.time() + 60
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            print("net2 tls server listening on 127.0.0.1:%d" % port)
            return 0
        except OSError:
            time.sleep(0.25)
    print("net2 tls server did not come up", file=sys.stderr)
    return 1


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "start" and len(sys.argv) == 4:
        return start(int(sys.argv[2]), sys.argv[3])
    if cmd == "serve" and len(sys.argv) == 4:
        serve(int(sys.argv[2]), sys.argv[3])
        return 0
    if cmd == "stop" and len(sys.argv) == 3:
        os.makedirs(sys.argv[2], exist_ok=True)
        open(os.path.join(sys.argv[2], "stop"), "w").close()
        time.sleep(1.5)
        try:
            text = open(os.path.join(sys.argv[2], "server.log"), "r", errors="replace").read()
        except OSError:
            text = ""
        print(text)    # the relay log (tap): kept in the test output for the handshake diagnosis
        return 1 if "handshake error" in text else 0   # a TLS handshake failed on the server: the guest test fails too
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
