"""Connection limiting, SIGHUP and HTTP statistics (categories 14, 15, 17, 18)."""
import signal
import threading

from helpers import BASE_THREADS, STAT_KEYS, GatewayTest, parse_stats, thread_count, wait_until


class ConnectionLimit(GatewayTest):
    def test_waiting_client_is_not_accepted_or_counted(self):
        server = self.server(max_clients=2, stats=True)
        a = self.connect(server.port)
        b = self.connect(server.port)
        self.assertEqual(a.post("a\n").status, 200)
        self.assertEqual(b.post("b\n").status, 200)
        c = self.connect(server.port)  # connect() succeeds via the kernel backlog
        c.request("POST", "/", "c\n")
        self.assertTrue(c.no_data_within(0.7))
        stats = server.stats()
        self.assertEqual((stats["Clients connected"], stats["Total connected clients"]), (2, 2))
        self.assertEqual(stats["Num prompts received"], 2)
        a.close()
        resp = c.read_response()
        self.assertEqual(resp.body, b"echo:c\n")
        self.assertTrue(server.wait_stats(lambda s: s["Total connected clients"] == 3
                                          and s["Clients connected"] == 2))

    def test_slot_released_after_malformed_and_disconnects(self):
        server = self.server(max_clients=1, stats=True)
        for i in range(5):
            conn = self.connect(server.port)
            if i % 2:
                conn.send(b"garbage\r\n\r\n")
                self.assertTrue(conn.is_closed())
            else:
                self.assertEqual(conn.post("p\n").status, 200)
                conn.close()
        last = self.connect(server.port)
        self.assertEqual(last.post("last\n").status, 200)
        self.assertTrue(server.wait_stats(lambda s: s["Total sessions completed"] == 5))

    def test_zero_means_no_application_limit(self):
        server = self.server(max_clients=0, stats=True)
        conns = [self.connect(server.port) for _ in range(30)]
        for conn in conns:
            conn.request("GET", "/")
        for conn in conns:
            self.assertEqual(conn.read_response().status, 200)
        self.assertEqual(server.stats()["Clients connected"], 30)


class Sighup(GatewayTest):
    def read_blocks(self, server, count):
        ok = server.err.wait_for(lambda d: d.count(b"Total tokens evaluated:") >= count)
        self.assertTrue(ok, server.stderr())
        text = server.stderr().decode()
        lines = [l for l in text.split("\n") if ":" in l]
        blocks = [lines[i:i + 5] for i in range(0, len(lines), 5)]
        for block in blocks:
            self.assertEqual([l.split(":")[0] for l in block], STAT_KEYS)
        return [parse_stats("\n".join(b)) for b in blocks]

    def test_sighup_reports_five_counters(self):
        server = self.server(plan={"tokens": 7})
        a = self.connect(server.port)
        a.post("1\n")
        a.post("2\n")
        a.post("x" * 3000)  # counted as a prompt, no tokens
        a.request("GET", "/nope")
        a.read_response()
        b = self.connect(server.port)
        b.post("3\n")
        b.close()
        self.assertTrue(wait_until(lambda: thread_count(server.pid) == BASE_THREADS + 1))
        server.send_signal(signal.SIGHUP)
        stats = self.read_blocks(server, 1)[0]
        self.assertEqual(stats, {"Clients connected": 1, "Total connected clients": 2,
                                 "Total sessions completed": 1, "Num prompts received": 4,
                                 "Total tokens evaluated": 21})
        self.assertTrue(server.stderr().startswith(b"%d\n" % server.port))

    def test_repeated_sighup_under_load_is_consistent(self):
        server = self.server()
        stop = threading.Event()

        def churn():
            while not stop.is_set():
                conn = self.connect(server.port)
                conn.post("load\n")
                conn.close()

        workers = [threading.Thread(target=churn) for _ in range(4)]
        for w in workers:
            w.start()
        for _ in range(20):
            server.send_signal(signal.SIGHUP)
            wait_until(lambda: False, timeout=0.02)
        stop.set()
        for w in workers:
            w.join(30)
        blocks = self.read_blocks(server, 20)
        self.assertGreaterEqual(len(blocks), 20)
        previous = None
        for stats in blocks:
            self.assertEqual(stats["Clients connected"] + stats["Total sessions completed"],
                             stats["Total connected clients"])
            if previous:
                for key in STAT_KEYS[1:]:
                    self.assertGreaterEqual(stats[key], previous[key])
            previous = stats
        self.assertIsNone(server.proc.poll())


class StatsHttp(GatewayTest):
    def test_single_stats_client(self):
        server = self.server(stats=True, plan={"tokens": 3})
        p = self.connect(server.port)
        p.post("hello\n")
        s = self.connect(server.stats_port)
        s.request("GET", "/stats")
        resp = s.read_response()
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.headers["content-type"], "text/plain")
        self.assertEqual(resp.body, b"Clients connected: 1\nTotal connected clients: 1\n"
                                    b"Total sessions completed: 0\nNum prompts received: 1\n"
                                    b"Total tokens evaluated: 3\n")
        self.assertEqual(int(resp.headers["content-length"]), len(resp.body))
        for method, path, status in [("POST", "/stats", 405), ("PUT", "/x", 405),
                                     ("GET", "/", 404), ("GET", "/stats/", 404)]:
            with self.subTest(method=method, path=path):
                s.request(method, path, "body")
                r = s.read_response()
                self.assertEqual(r.status, status)
        s.send(b"GET /stats\r\n\r\n")
        self.assertTrue(s.is_closed())
        self.assertEqual(server.stats()["Total connected clients"], 1)

    def test_stats_disconnect_mid_request(self):
        server = self.server(stats=True)
        s = self.connect(server.stats_port)
        s.send(b"GET /stats HTTP/1.1\r\nHost:")
        s.close()
        self.assertEqual(server.stats()["Clients connected"], 0)

    def test_many_stats_clients_keepalive_and_no_quota(self):
        server = self.server(stats=True, max_clients=1)
        prompt = self.connect(server.port)
        prompt.post("holding the only slot\n")
        conns = [self.connect(server.stats_port) for _ in range(6)]
        for _ in range(3):
            for conn in conns:
                conn.request("GET", "/stats")
            for conn in conns:
                stats = parse_stats(conn.read_response().body.decode())
                self.assertEqual(stats["Clients connected"], 1)
                self.assertEqual(stats["Total connected clients"], 1)
        self.assertEqual(thread_count(server.pid), BASE_THREADS + 1 + 1 + 6)

    def test_stats_answer_while_backend_is_stuck(self):
        server = self.server(stats=True, plan={"actions": ["hang"]},
                             env={"LLMGW_BACKEND_TIMEOUT_MS": "4000",
                                  "LLMGW_BACKEND_MAX_RESENDS": "0"})
        stuck = self.connect(server.port)
        stuck.request("POST", "/", "stuck\n")
        self.assertTrue(server.wait_events("request", 1))
        stats = server.stats()
        self.assertEqual(stats["Num prompts received"], 1)
        conn = self.connect(server.stats_port)
        conn.request("GET", "/stats")
        self.assertEqual(conn.read_response().status, 200)
        self.assertEqual(stuck.read_response().status, 502)
