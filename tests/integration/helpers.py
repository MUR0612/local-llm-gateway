"""Shared fixtures for the integration tests.

All ports are dynamic, every wait has a deadline, and readiness is detected
from explicit signals (the port line on stderr, mock-backend log events,
/proc state) rather than fixed sleeps or model output.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

TESTS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.dirname(TESTS)
BIN_DIR = os.environ.get("LLMGW_TEST_BIN_DIR", PROJECT)
SANITIZER = os.environ.get("LLMGW_TEST_SANITIZER", "")
FAULT_LIB = os.environ.get("LLMGW_TEST_FAULT_LIB")
CLIENT = os.path.join(BIN_DIR, "llm-client")
SERVER = os.path.join(BIN_DIR, "llm-server")
MOCK = os.path.join(TESTS, "mock_backend.py")
INDEX = os.path.join(PROJECT, "assets", "index.html")
# Sanitizers slow everything down; scale deadlines rather than skip.
SLOW = 4.0 if SANITIZER else 1.0
# main + reaper + signal thread (TSan adds one background thread).
BASE_THREADS = 3 + (1 if SANITIZER == "tsan" else 0)
USAGE = b"Usage: llm-server [options] maxclients historysize [port]\n"


def wait_until(predicate, timeout=5.0, interval=0.01):
    deadline = time.monotonic() + timeout * SLOW
    while True:
        value = predicate()
        if value:
            return value
        if time.monotonic() >= deadline:
            return value
        time.sleep(interval)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def proc_state(pid):
    """Returns the /proc state letter, or None if the PID does not exist."""
    try:
        with open("/proc/%d/stat" % pid) as handle:
            return handle.read().rsplit(")", 1)[1].split()[0]
    except (FileNotFoundError, ProcessLookupError, IndexError):
        return None


def pid_gone(pid):
    return proc_state(pid) is None


def fd_count(pid):
    return len(os.listdir("/proc/%d/fd" % pid))


def thread_count(pid):
    return len(os.listdir("/proc/%d/task" % pid))


def rss_kb(pid):
    with open("/proc/%d/status" % pid) as handle:
        for line in handle:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0


def cpu_ticks(pid):
    with open("/proc/%d/stat" % pid) as handle:
        fields = handle.read().rsplit(")", 1)[1].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def listening_ports(pid):
    """TCP ports in LISTEN state whose socket belongs to @pid."""
    inodes = set()
    for fd in os.listdir("/proc/%d/fd" % pid):
        try:
            target = os.readlink("/proc/%d/fd/%s" % (pid, fd))
        except OSError:
            continue
        if target.startswith("socket:["):
            inodes.add(target[8:-1])
    ports = set()
    with open("/proc/net/tcp") as handle:
        next(handle)
        for line in handle:
            parts = line.split()
            if parts[3] == "0A" and parts[9] in inodes:
                ports.add(int(parts[1].split(":")[1], 16))
    return ports


class Lines(threading.Thread):
    """Collects a pipe's output in a background thread."""

    def __init__(self, stream):
        super().__init__(daemon=True)
        self.stream = stream
        self.data = b""
        self.cond = threading.Condition()
        self.closed = False
        self.start()

    def run(self):
        while True:
            chunk = self.stream.read1(65536)
            with self.cond:
                if not chunk:
                    self.closed = True
                    self.cond.notify_all()
                    return
                self.data += chunk
                self.cond.notify_all()

    def text(self):
        with self.cond:
            return self.data

    def wait_for(self, predicate, timeout=5.0):
        deadline = time.monotonic() + timeout * SLOW
        with self.cond:
            while not predicate(self.data):
                left = deadline - time.monotonic()
                if left <= 0 or self.closed:
                    return predicate(self.data)
                self.cond.wait(left)
            return True


class Response:
    def __init__(self, status, reason, headers, body):
        self.status = status
        self.reason = reason
        self.headers = headers
        self.body = body

    def __repr__(self):
        return "Response(%d, %r)" % (self.status, self.body[:80])


class Http:
    """Raw HTTP/1.1 client over one TCP connection."""

    def __init__(self, port, timeout=10.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout * SLOW)
        self.buf = b""

    def send(self, data):
        self.sock.sendall(data)

    def request(self, method, path, body=b"", headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        lines = ["%s %s HTTP/1.1" % (method, path)]
        for name, value in (headers or {"Content-Length": str(len(body))}).items():
            lines.append("%s: %s" % (name, value))
        self.send(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body)

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise EOFError()
        self.buf += chunk

    def read_response(self):
        """Returns a Response, or None if the server closed the connection."""
        try:
            while b"\r\n\r\n" not in self.buf:
                self._fill()
            head, self.buf = self.buf.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            version, status, reason = (lines[0].split(" ", 2) + [""])[:3]
            assert version == "HTTP/1.1", lines[0]
            headers = {}
            for line in lines[1:]:
                name, _, value = line.partition(":")
                headers[name.strip().lower()] = value.strip()
            length = int(headers["content-length"])
            while len(self.buf) < length:
                self._fill()
            body, self.buf = self.buf[:length], self.buf[length:]
            return Response(int(status), reason, headers, body)
        except (EOFError, ConnectionResetError):
            return None

    def post(self, text, path="/"):
        self.request("POST", path, text)
        return self.read_response()

    def is_closed(self, timeout=3.0):
        """True when the peer closes without sending anything more."""
        self.sock.settimeout(timeout * SLOW)
        try:
            return self.sock.recv(1) == b""
        except ConnectionResetError:
            return True
        except socket.timeout:
            return False

    def no_data_within(self, seconds):
        self.sock.settimeout(seconds)
        try:
            data = self.sock.recv(1)
            self.buf += data
            return False
        except socket.timeout:
            return True
        finally:
            self.sock.settimeout(10.0 * SLOW)

    def close(self):
        self.sock.close()


def expected_prompt(pairs, current):
    text = "<|im_start|>system\nYou are a helpful assistant.<|im_end|>\n"
    for prompt, reply in pairs:
        text += ("<|im_start|>user\n" + prompt + "<|im_end|>\n<|im_start|>assistant\n"
                 + reply + "<|im_end|>\n")
    return text + "<|im_start|>user\n" + current + "<|im_end|>\n<|im_start|>assistant\n"


def parse_stats(text):
    values = {}
    for line in text.strip().split("\n"):
        name, _, value = line.rpartition(": ")
        values[name] = int(value)
    return values


STAT_KEYS = ["Clients connected", "Total connected clients", "Total sessions completed",
             "Num prompts received", "Total tokens evaluated"]


class Server:
    """Runs llm-server with the mock backend (managed by default)."""

    def __init__(self, case, max_clients=0, history=5, port=None, plan=None,
                 external_port=None, env=None, args=None, stats=False,
                 wait_port=True, preload=None, command=None, mock=True):
        self.case = case
        self.log_path = os.path.join(case.tmp, "backend-%d.jsonl" % len(case.servers))
        self.gen_path = self.log_path + ".gen"
        plan_path = self.log_path + ".plan.json"
        with open(plan_path, "w") as handle:
            json.dump(plan or {}, handle)
        cmd = list(command or [SERVER])
        if external_port is not None:
            cmd += ["--backend-port", str(external_port)]
        elif mock:
            # With fault injection the shim must not leak into the backend.
            launcher = ["env", "-u", "LD_PRELOAD", "-u", "FI_FAIL", sys.executable] \
                if preload else [sys.executable]
            cmd += ["--backend-cmd", launcher[0]]
            for arg in launcher[1:] + ["-u", MOCK, "--plan", plan_path, "--log", self.log_path,
                                       "--gen-file", self.gen_path]:
                cmd += ["--backend-arg", arg]
        cmd += list(args or [])
        cmd += [str(max_clients), str(history)]
        if port is not None:
            cmd.append(str(port))
        full_env = dict(os.environ)
        # Sanitizer builds live in build/*/, away from the default asset path.
        full_env.setdefault("LLMGW_HOME_PAGE", INDEX)
        self.stats_port = None
        if stats:
            explicit = isinstance(stats, int) and not isinstance(stats, bool)
            self.stats_port = stats if explicit else free_port()
            full_env["LLMGW_STATS_PORT"] = str(self.stats_port)
        if preload:
            full_env["LD_PRELOAD"] = FAULT_LIB
            full_env["FI_FAIL"] = preload
        full_env.update(env or {})
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=full_env)
        case.servers.append(self)
        self.out = Lines(self.proc.stdout)
        self.err = Lines(self.proc.stderr)
        self.port = None
        if wait_port:
            ok = self.err.wait_for(lambda d: b"\n" in d, timeout=10)
            first = self.err.text().split(b"\n", 1)[0]
            case.assertTrue(ok and first.isdigit(), "no port line: %r" % self.err.text())
            self.port = int(first)

    @property
    def pid(self):
        return self.proc.pid

    def events(self, kind=None):
        try:
            with open(self.log_path) as handle:
                items = [json.loads(line) for line in handle if line.strip()]
        except FileNotFoundError:
            return []
        return [e for e in items if kind is None or e["event"] == kind]

    def requests(self):
        return self.events("request")

    def wait_events(self, kind, count, timeout=5.0):
        return wait_until(lambda: len(self.events(kind)) >= count, timeout)

    def backend_pids(self):
        return [e["pid"] for e in self.events("start")]

    def wait_ready(self, timeout=10.0):
        """Waits until the gateway has connected to the backend."""
        return wait_until(lambda: self.events("accept"), timeout)

    def wait_exit(self, timeout=10.0):
        try:
            return self.proc.wait(timeout * SLOW)
        except subprocess.TimeoutExpired:
            return None

    def send_signal(self, sig):
        self.proc.send_signal(sig)

    def stop(self, sig=signal.SIGTERM, timeout=10.0):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
        return self.wait_exit(timeout)

    def stderr(self):
        if self.proc.poll() is not None:
            self.err.join(5)
        return self.err.text()

    def stats(self):
        conn = Http(self.stats_port)
        conn.request("GET", "/stats")
        resp = conn.read_response()
        conn.close()
        return parse_stats(resp.body.decode())

    def wait_stats(self, predicate, timeout=5.0):
        return wait_until(lambda: predicate(self.stats()), timeout)

    def cleanup(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)
        for pid in set(self.backend_pids()):
            if not pid_gone(pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
        self.out.join(5)
        self.err.join(5)
        self.proc.stdout.close()
        self.proc.stderr.close()


class ExternalMock:
    """Runs the mock backend as a user-owned external service."""

    def __init__(self, case, plan=None):
        self.log_path = os.path.join(case.tmp, "external-%d.jsonl" % len(case.externals))
        plan_path = self.log_path + ".plan.json"
        with open(plan_path, "w") as handle:
            json.dump(plan or {}, handle)
        self.proc = subprocess.Popen([sys.executable, "-u", MOCK, "--plan", plan_path,
                                      "--log", self.log_path],
                                     stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.port = int(self.proc.stdout.readline())
        case.externals.append(self)

    def events(self, kind=None):
        with open(self.log_path) as handle:
            items = [json.loads(line) for line in handle if line.strip()]
        return [e for e in items if kind is None or e["event"] == kind]

    def cleanup(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(10)
        self.proc.stdout.close()


class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="llmgw-test-")
        self.servers = []
        self.externals = []
        self.conns = []

    SANITIZER_MARKERS = (b"ThreadSanitizer", b"AddressSanitizer", b"LeakSanitizer",
                         b"runtime error:")

    def tearDown(self):
        for conn in self.conns:
            conn.close()
        reports = []
        for server in self.servers:
            server.cleanup()
            err = server.err.text()
            if any(marker in err for marker in self.SANITIZER_MARKERS):
                reports.append(err.decode("utf-8", "replace"))
        for ext in self.externals:
            ext.cleanup()
        shutil.rmtree(self.tmp, ignore_errors=True)
        self.assertEqual(reports, [], "sanitizer report in server stderr")

    def connect(self, port, timeout=10.0):
        conn = Http(port, timeout)
        self.conns.append(conn)
        return conn

    def server(self, **kwargs):
        return Server(self, **kwargs)

    def run_client(self, port, stdin=b"", env=None, timeout=20.0, args=None, stdout=None):
        full_env = dict(os.environ)
        full_env.update(env or {})
        cmd = [CLIENT] + (list(args) if args is not None else [str(port)])
        return subprocess.run(cmd, input=stdin, stdout=stdout or subprocess.PIPE,
                              stderr=subprocess.PIPE, env=full_env, timeout=timeout * SLOW)


class FakeServer:
    """Scripted TCP server used to test llm-client in isolation."""

    def __init__(self, handler, port=0):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", port))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self.handler = handler
        self.connections = 0
        self.requests = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            self.connections += 1
            threading.Thread(target=self.run_handler, args=(conn,), daemon=True).start()

    def run_handler(self, conn):
        with conn:
            try:
                self.handler(self, conn)
            except OSError:
                pass

    def read_request(self, conn, state):
        """Reads one request; returns (head, body) or None on EOF."""
        buf = state.get("buf", b"")
        while b"\r\n\r\n" not in buf:
            chunk = conn.recv(65536)
            if not chunk:
                return None
            buf += chunk
        head, buf = buf.split(b"\r\n\r\n", 1)
        length = 0
        for line in head.split(b"\r\n")[1:]:
            name, _, value = line.partition(b":")
            if name.strip().lower() == b"content-length":
                length = int(value.strip())
        while len(buf) < length:
            chunk = conn.recv(65536)
            if not chunk:
                return None
            buf += chunk
        body, state["buf"] = buf[:length], buf[length:]
        self.requests.append((head, body))
        return head, body

    def close(self):
        self.listener.close()


def http_reply(status, body, reason="X", extra=b""):
    return (b"HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n%s\r\n"
            % (status, reason.encode(), len(body), extra) + body)
