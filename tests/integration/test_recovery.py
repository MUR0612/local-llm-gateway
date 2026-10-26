"""Backend reconnect/resend and managed process restart (categories 19-20)."""
import os
import signal
import threading
import time

from helpers import ExternalMock, GatewayTest, expected_prompt, pid_gone, proc_state, wait_until


class Reconnect(GatewayTest):
    def check_single_resend(self, action):
        server = self.server(stats=True, plan={"actions": [action], "tokens": 4})
        conn = self.connect(server.port)
        resp = conn.post("retry me\n")
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.body, b"echo:retry me\n")
        requests = server.requests()
        self.assertEqual(len(requests), 2)
        self.assertEqual(requests[0]["json"], requests[1]["json"])  # same prompt resent
        self.assertNotEqual(requests[0]["conn"], requests[1]["conn"])  # new connection
        stats = server.stats()
        self.assertEqual(stats["Num prompts received"], 1)  # resend not double counted
        self.assertEqual(stats["Total tokens evaluated"], 4)
        self.assertEqual(len(server.events("start")), 1)  # EOF is not a restart
        conn.post("next\n")
        self.assertEqual(server.requests()[-1]["json"]["prompt"],
                         expected_prompt([("retry me\n", "echo:retry me")], "next\n"))

    def test_eof_before_response_resends(self):
        self.check_single_resend("close")

    def test_malformed_response_resends(self):
        self.check_single_resend("malformed")

    def test_persistent_garbage_hits_resend_budget(self):
        server = self.server(plan={"default": "malformed"},
                             env={"LLMGW_BACKEND_MAX_RESENDS": "3"})
        conn = self.connect(server.port)
        resp = conn.post("never works\n")
        self.assertEqual(resp.status, 502)
        self.assertEqual(resp.body, b"Backend request failed\n")
        self.assertEqual(len(server.requests()), 4)  # first try + 3 resends
        self.assertIsNone(server.proc.poll())

    def test_backend_timeout_then_recovery(self):
        server = self.server(plan={"actions": ["hang"]},
                             env={"LLMGW_BACKEND_TIMEOUT_MS": "400",
                                  "LLMGW_BACKEND_MAX_RESENDS": "0"})
        conn = self.connect(server.port)
        started = time.monotonic()
        self.assertEqual(conn.post("slow\n").status, 502)
        self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(conn.post("fast\n").body, b"echo:fast\n")
        requests = server.requests()
        self.assertNotEqual(requests[0]["conn"], requests[1]["conn"])

    def test_bad_replies_are_not_retried(self):
        for action in ["bad_json", "status500", "no_content",
                       'raw:{"content": 5}', 'raw:{"content":"x","tokens_evaluated":-3}']:
            with self.subTest(action=action):
                server = self.server(stats=True, plan={"actions": [action]})
                conn = self.connect(server.port)
                resp = conn.post("q\n")
                self.assertEqual((resp.status, resp.body), (502, b"Invalid backend response\n"))
                self.assertEqual(len(server.requests()), 1)
                self.assertEqual(server.stats()["Total tokens evaluated"], 0)
                self.assertEqual(conn.post("ok\n").status, 200)  # connection still fine

    def test_missing_tokens_count_as_zero(self):
        server = self.server(stats=True, plan={"actions": ['raw:{"content":"only"}']})
        conn = self.connect(server.port)
        self.assertEqual(conn.post("q\n").body, b"only\n")
        self.assertEqual(server.stats()["Total tokens evaluated"], 0)

    def test_reconnect_exhaustion_is_fatal_and_reaps_child(self):
        server = self.server(plan={"actions": ["close_listener"]}, env={"LLMGW_DEBUG": "1"})
        conn = self.connect(server.port)
        conn.request("POST", "/", "doomed\n")
        self.assertEqual(server.wait_exit(), 1)
        err = server.stderr()
        self.assertTrue(err.endswith(b"llm-server: could not connect to the backend\n"))
        self.assertEqual(err.count(b"connect attempt "), 26)
        self.assertTrue(pid_gone(server.backend_pids()[0]))

    def test_external_backend_is_never_signalled(self):
        ext = ExternalMock(self, plan={"actions": ["close", "ok", "close_listener"]})
        server = self.server(external_port=ext.port)
        conn = self.connect(server.port)
        self.assertEqual(conn.post("one\n").status, 200)  # close -> reconnect -> ok
        self.assertEqual(len(ext.events("accept")), 2)
        conn.request("POST", "/", "two\n")
        self.assertEqual(server.wait_exit(), 1)
        self.assertIsNone(ext.proc.poll(), "external backend must stay alive")

    def test_external_shutdown_leaves_backend_running(self):
        ext = ExternalMock(self)
        server = self.server(external_port=ext.port)
        self.assertEqual(self.connect(server.port).post("hi\n").status, 200)
        self.assertEqual(server.stop(signal.SIGINT), 0)
        self.assertIsNone(ext.proc.poll())


class Restart(GatewayTest):
    def kill_backend(self, server):
        pid = server.backend_pids()[-1]
        os.kill(pid, signal.SIGKILL)
        # The gateway has not reaped it yet, so it must show up as a zombie.
        self.assertTrue(wait_until(lambda: proc_state(pid) == "Z"))
        return pid

    def test_dead_child_detected_before_send(self):
        server = self.server(stats=True, plan={"tokens": 5})
        conn = self.connect(server.port)
        self.assertEqual(conn.post("before\n").status, 200)
        old = self.kill_backend(server)
        resp = conn.post("after\n")
        self.assertEqual(resp.body, b"echo:after\n")
        self.assertTrue(pid_gone(old))  # reaped
        starts = server.events("start")
        self.assertEqual([e["gen"] for e in starts], [0, 1])
        stats = server.stats()
        self.assertEqual(stats["Total tokens evaluated"], 10)  # not reset by restart
        self.assertEqual(server.requests()[-1]["json"]["prompt"],
                         expected_prompt([("before\n", "echo:before")], "after\n"))

    def test_crash_mid_transaction_restarts_and_resends(self):
        server = self.server(plan={"generations": [{"actions": ["crash"]}, {}]})
        conn = self.connect(server.port)
        self.assertEqual(conn.post("crash it\n").body, b"echo:crash it\n")
        self.assertEqual(len(server.events("start")), 2)
        self.assertEqual(server.requests()[-1]["gen"], 1)

    def test_one_replacement_for_concurrent_clients(self):
        server = self.server()
        conns = [self.connect(server.port) for _ in range(6)]
        for conn in conns:
            self.assertEqual(conn.post("warm\n").status, 200)
        self.kill_backend(server)
        bodies = []
        lock = threading.Lock()

        def go(conn, i):
            body = conn.post("c%d\n" % i).body
            with lock:
                bodies.append(body)

        threads = [threading.Thread(target=go, args=(c, i)) for i, c in enumerate(conns)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(30)
        self.assertEqual(sorted(bodies), sorted(b"echo:c%d\n" % i for i in range(6)))
        self.assertEqual(len(server.events("start")), 2)

    def test_replacement_start_failure_is_fatal(self):
        server = self.server(plan={"generations": [{}, {"startup": "exit"}]})
        conn = self.connect(server.port)
        self.assertEqual(conn.post("x\n").status, 200)
        self.kill_backend(server)
        conn.request("POST", "/", "y\n")
        self.assertEqual(server.wait_exit(), 17)
        self.assertTrue(server.stderr().endswith(b"llm-server: could not start the backend\n"))
        self.assertTrue(all(pid_gone(p) for p in server.backend_pids()))

    def test_no_restart_without_child_exit(self):
        server = self.server(plan={"actions": ["close", "close"]})
        conn = self.connect(server.port)
        self.assertEqual(conn.post("x\n").status, 200)
        self.assertEqual(len(server.events("start")), 1)
