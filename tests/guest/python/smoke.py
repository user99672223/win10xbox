# The official CPython embeddable package through the layer (tests/integration: integration.real_python).
# Every check prints "py: <name> ok" or "py: <name> FAILED <why>"; each "ok" line is its own ctest assertion.
# Exit code 42 when every check passed, 1 otherwise. Arguments: alpha "beta gamma".
import sys

fails = 0


def check(name, fn):
    global fails
    try:
        r = fn()
        if r is False:
            raise AssertionError("check returned False")
        print("py: %s ok" % name, flush=True)
    except BaseException as e:  # report everything, SystemExit included
        fails += 1
        print("py: %s FAILED %s: %s" % (name, type(e).__name__, e), flush=True)


def t_argv():
    assert sys.argv[1:] == ["alpha", "beta gamma"], repr(sys.argv)


check("arith", lambda: 1 + 1 == 2)
check("argv", t_argv)
check("version", lambda: sys.version_info[:2] == (3, 12) and sys.platform == "win32")


def t_json():
    import json
    doc = {"a": [1, 2.5, None, True], "b": "café", "c": {"d": "e"}}
    s = json.dumps(doc, sort_keys=True)
    assert json.loads(s) == doc, s
    assert json.dumps([1, "x"]) == '[1, "x"]'


check("json", t_json)


def t_hashlib():
    import hashlib
    assert hashlib.sha256(b"abc").hexdigest() == \
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    assert hashlib.md5(b"").hexdigest() == "d41d8cd98f00b204e9800998ecf8427e"


check("hashlib", t_hashlib)


def t_hashlib_openssl():
    # _hashlib.pyd -> libcrypto-3.dll (OpenSSL), not the builtin fallback.
    import _hashlib
    h = _hashlib.new("sha256", b"abc")
    assert h.hexdigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"


check("hashlib.openssl", t_hashlib_openssl)


def t_zlib():
    import zlib
    data = b"win10xbox " * 1000
    c = zlib.compress(data, 9)
    assert len(c) < len(data) // 10, len(c)
    assert zlib.decompress(c) == data
    assert zlib.crc32(b"hello") == 0x3610A686


check("zlib", t_zlib)


def t_sqlite3():
    import sqlite3
    db = sqlite3.connect(":memory:")
    db.execute("create table t (k integer primary key, v text)")
    db.executemany("insert into t (v) values (?)", [("one",), ("two",), ("three",)])
    rows = db.execute("select k, v from t order by k").fetchall()
    assert rows == [(1, "one"), (2, "two"), (3, "three")], rows
    assert db.execute("select count(*), sum(k) from t").fetchone() == (3, 6)
    db.close()


check("sqlite3", t_sqlite3)


def t_ctypes():
    import ctypes
    import os
    assert ctypes.windll.kernel32.GetCurrentProcessId() == os.getpid()
    buf = ctypes.create_string_buffer(b"hello", 16)
    assert ctypes.cdll.msvcrt.strlen(buf) == 5


check("ctypes", t_ctypes)


def t_ctypes_callback():
    # A Python function called back through a libffi closure (executable trampoline memory).
    import ctypes
    proto = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_int)
    cb = proto(lambda x: x * 2)
    assert cb(21) == 42


check("ctypes.callback", t_ctypes_callback)


def _tmp_path():
    import os
    import tempfile
    return os.path.join(tempfile.gettempdir(), "w10x_py_%d.txt" % os.getpid())


def t_file_write():
    with open(_tmp_path(), "w", encoding="utf-8") as f:
        f.write("line one\n")
        f.write("zwei ü\n")
        for i in range(100):
            f.write("row %d\n" % i)


check("file.write", t_file_write)


def t_file_read():
    import os
    with open(_tmp_path(), "r", encoding="utf-8") as f:
        lines = f.read().splitlines()
    assert lines[0] == "line one" and lines[1] == "zwei ü" and lines[-1] == "row 99", lines[:3]
    assert len(lines) == 102, len(lines)
    assert os.path.getsize(_tmp_path()) > 700


check("file.read", t_file_read)


def t_file_rename_remove():
    import os
    src = _tmp_path()
    dst = src + ".renamed"
    os.replace(src, dst)
    assert not os.path.exists(src) and os.path.exists(dst)
    os.remove(dst)
    assert not os.path.exists(dst)


check("file.rename", t_file_rename_remove)


def t_listdir():
    import os
    names = os.listdir(os.path.dirname(sys.executable))
    assert "python.exe" in names and "python312.zip" in names, names


check("os.listdir", t_listdir)


def t_threading():
    import threading
    lock = threading.Lock()
    total = [0]
    seen = set()

    def work():
        for _ in range(1000):
            with lock:
                total[0] += 1
        with lock:
            seen.add(threading.get_ident())

    threads = [threading.Thread(target=work) for _ in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join(30)
        assert not t.is_alive()
    assert total[0] == 4000, total[0]
    assert len(seen) == 4, seen


check("threading", t_threading)


def t_threading_queue():
    import queue
    import threading
    q = queue.Queue()
    ev = threading.Event()

    def producer():
        for i in range(50):
            q.put(i)
        ev.set()

    t = threading.Thread(target=producer)
    t.start()
    assert ev.wait(30)
    t.join(30)
    got = [q.get(timeout=5) for _ in range(50)]
    assert got == list(range(50))


check("threading.queue", t_threading_queue)


def t_time():
    import datetime
    import time
    t0 = time.monotonic()
    time.sleep(0.05)
    assert time.monotonic() - t0 >= 0.04
    assert datetime.datetime.now().year >= 2024


check("time", t_time)

print("py: done fails=%d" % fails, flush=True)
sys.exit(42 if fails == 0 else 1)
