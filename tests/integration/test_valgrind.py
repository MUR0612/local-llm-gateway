"""Valgrind Memcheck runs (category 5 and the leak part of 21).

Run via `make valgrind`; skipped automatically when valgrind is missing.
"""
import os
import re
import shutil
import signal
import subprocess

from helpers import CLIENT, SERVER, SLOW, GatewayTest, free_port, pid_gone, proc_state, wait_until

VALGRIND = shutil.which("valgrind")
BASE = ["--leak-check=full", "--show-leak-kinds=all", "--track-fds=yes",
        "--error-exitcode=99"]


def summary(path):
    with open(path) as handle:
        return handle.read()


class ValgrindTest(GatewayTest):
    def setUp(self):
        if VALGRIND is None:
            self.skipTest("valgrind not installed")
        super().setUp()

    def vg(self, name, kinds):
        log = os.path.join(self.tmp, name + ".vg.log")
        return [VALGRIND] + BASE + ["--errors-for-leak-kinds=" + kinds,
                                    "--log-file=" + log], log

    def assert_clean(self, log, all_freed):
        text = summary(log)
        self.assertIn("ERROR SUMMARY: 0 errors", text, text)
        if all_freed:
            self.assertIn("All heap blocks were freed -- no leaks are possible", text, text)
        else:
            self.assertRegex(text, r"definitely lost: 0 bytes|no leaks are possible")
        fds = re.findall(r"FILE DESCRIPTORS: (\d+) open", text)
        self.assertTrue(fds, text)
        self.assertEqual(fds[-1], "3", text)  # only stdin/stdout/stderr remain
        return text


class ClientMemory(ValgrindTest):
    def run_vg_client(self, port_args, stdin, expected_rc):
        prefix, log = self.vg("client", "all")
        result = subprocess.run(prefix + [CLIENT] + port_args, input=stdin,
                                capture_output=True, timeout=120)
        self.assertEqual(result.returncode, expected_rc, summary(log))
        self.assert_clean(log, all_freed=True)

    def test_client_paths_free_everything(self):
        server = self.server(plan={"actions": ["ok", "ok", "ok"]})
        port = str(server.port)
        with self.subTest("eof after prompts and errors"):
            self.run_vg_client([port], b"one\n" + b"x" * 3000 + b"\nthree\n", 0)
        with self.subTest("usage"):
            self.run_vg_client([], b"", 8)
        with self.subTest("empty port"):
            self.run_vg_client([""], b"", 9)
        with self.subTest("connect failure"):
            self.run_vg_client([str(free_port())], b"", 18)
        with self.subTest("communication error"):
            dying = self.server(plan={"default": "malformed"},
                                env={"LLMGW_BACKEND_MAX_RESENDS": "0"})
            self.run_vg_client([str(dying.port)], b"q\n", 0)  # 502 is a valid reply
            dying.stop()
            self.run_vg_client([str(dying.port)], b"", 18)


class ServerMemory(ValgrindTest):
    def test_server_workload_then_graceful_shutdown(self):
        prefix, log = self.vg("server", "definite,indirect,possible")
        server = self.server(command=prefix + [SERVER], stats=True, max_clients=3,
                             plan={"actions": ["ok", "close", "ok", "bad_json"]})
        a = self.connect(server.port, timeout=60)
        self.assertEqual(a.post("first\n").status, 200)
        self.assertEqual(a.post("resent\n").status, 200)
        self.assertEqual(a.post("bad\n").status, 502)
        self.assertEqual(a.post("x" * 3000).status, 413)
        a.request("GET", "/")
        self.assertEqual(a.read_response().status, 200)
        a.request("GET", "/missing")
        self.assertEqual(a.read_response().status, 404)
        junk = self.connect(server.port, timeout=60)
        junk.send(b"junk\r\n\r\n")
        self.assertTrue(junk.is_closed(timeout=30))
        for _ in range(5):
            c = self.connect(server.port, timeout=60)
            self.assertEqual(c.post("short\n").status, 200)
            c.close()
        pid = server.backend_pids()[-1]
        os.kill(pid, signal.SIGKILL)
        self.assertTrue(wait_until(lambda: proc_state(pid) == "Z", timeout=20))
        self.assertEqual(a.post("after restart\n").status, 200)
        stats = self.connect(server.stats_port, timeout=60)
        stats.request("GET", "/stats")
        self.assertEqual(stats.read_response().status, 200)
        self.assertEqual(server.stop(timeout=60 / SLOW), 0, summary(log))
        text = self.assert_clean(log, all_freed=False)
        self.assertTrue(all(pid_gone(p) for p in server.backend_pids()))
        print("\n    server leak summary:", " ".join(
            line.split("==")[-1].strip() for line in text.splitlines()
            if "lost:" in line or "reachable:" in line or "in use at exit" in line))
