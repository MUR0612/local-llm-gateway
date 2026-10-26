"""Single, sequential and concurrent prompt clients (categories 9-13)."""
import socket
import threading
import time

from helpers import INDEX, GatewayTest, expected_prompt, wait_until


class SingleClient(GatewayTest):
    def test_home_page(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.request("GET", "/", headers={"X-Ignored": "1"})
        resp = conn.read_response()
        with open(INDEX, "rb") as handle:
            page = handle.read()
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.headers["content-type"], "text/html")
        self.assertEqual(int(resp.headers["content-length"]), len(page))
        self.assertEqual(resp.body, page)

    def test_missing_home_page_is_500_not_hang(self):
        server = self.server(args=["--home-page", "/nonexistent/index.html"])
        conn = self.connect(server.port)
        conn.request("GET", "/")
        resp = conn.read_response()
        self.assertEqual(resp.status, 500)
        self.assertEqual(resp.body, b"Home page unavailable\n")
        self.assertEqual(conn.post("still ok\n").status, 200)

    def test_prompt_round_trip_and_backend_request_format(self):
        server = self.server()
        conn = self.connect(server.port)
        resp = conn.post("What are bananas?\n")
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.headers["content-type"], "text/plain")
        self.assertEqual(resp.body, b"echo:What are bananas?\n")
        request = server.requests()[0]
        self.assertEqual(request["path"], "/completion")
        self.assertEqual(request["content_type"], "application/json")
        port = server.events("ready")[0]["port"]
        self.assertEqual(request["host"], "localhost:%d" % port)
        self.assertEqual(request["json"], {
            "prompt": expected_prompt([], "What are bananas?\n"),
            "n_predict": 256, "cache_prompt": False})
        self.assertTrue(request["raw"].endswith('","n_predict": 256,"cache_prompt": false}'))
        self.assertTrue(request["raw"].startswith(
            '{"prompt": "<|im_start|>system\\nYou are a helpful assistant.<|im_end|>\\n'))

    def test_many_requests_one_connection(self):
        server = self.server()
        conn = self.connect(server.port)
        for i in range(5):
            self.assertEqual(conn.post("p%d\n" % i).body, b"echo:p%d\n" % i)
        conn.request("GET", "/")
        self.assertEqual(conn.read_response().status, 200)
        self.assertEqual(conn.post("again\n").status, 200)

    def test_pipelined_requests_in_one_write(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.send(b"POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\na\n"
                  b"GET /nope HTTP/1.1\r\n\r\n"
                  b"POST / HTTP/1.1\r\ncontent-length: 2\r\n\r\nb\n")
        self.assertEqual(conn.read_response().body, b"echo:a\n")
        self.assertEqual(conn.read_response().status, 404)
        self.assertEqual(conn.read_response().body, b"echo:b\n")

    def test_request_split_across_writes(self):
        server = self.server()
        conn = self.connect(server.port)
        message = b"POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nslow!\n"
        for i in range(len(message)):
            conn.send(message[i:i + 1])
            time.sleep(0.002)
        self.assertEqual(conn.read_response().body, b"echo:slow!\n")

    def test_special_characters_reach_backend_exactly(self):
        server = self.server()
        conn = self.connect(server.port)
        text = 'q " \\ \t \r tab \\\\ end 中文 😀 \x01 /\n'
        resp = conn.post(text)
        self.assertEqual(resp.status, 200)
        self.assertEqual(server.requests()[0]["json"]["prompt"], expected_prompt([], text))
        self.assertEqual(resp.body, ("echo:" + text.rstrip("\n") + "\n").encode("utf-8"))

    def test_nul_and_invalid_utf8_policy(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.request("POST", "/", b"a\x00b\xffc\n")
        self.assertEqual(conn.read_response().status, 200)
        self.assertEqual(server.requests()[0]["json"]["prompt"],
                         expected_prompt([], "a\x00b�c\n"))

    def test_backend_unicode_content_returned_as_utf8(self):
        server = self.server(plan={"content": "回答 \"quoted\" \\ é\n😀"})
        conn = self.connect(server.port)
        resp = conn.post("x\n")
        self.assertEqual(resp.body, "回答 \"quoted\" \\ é\n😀\n".encode("utf-8"))
        self.assertEqual(int(resp.headers["content-length"]), len(resp.body))

    def test_connection_close_header(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.request("POST", "/", "bye\n", headers={"Content-Length": "4", "Connection": "close"})
        resp = conn.read_response()
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.headers.get("connection"), "close")
        self.assertTrue(conn.is_closed())

    def test_post_without_content_length_is_empty_prompt(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.request("POST", "/", headers={})
        self.assertEqual(conn.read_response().body, b"echo:\n")


class InvalidRequests(GatewayTest):
    def test_405_404_order(self):
        server = self.server()
        conn = self.connect(server.port)
        cases = [("PUT", "/", 405, b"Invalid HTTP method\n"),
                 ("DELETE", "/elsewhere", 405, b"Invalid HTTP method\n"),
                 ("get", "/", 405, b"Invalid HTTP method\n"),
                 ("GET", "/x", 404, b"Invalid address on request line\n"),
                 ("POST", "/x", 404, b"Invalid address on request line\n"),
                 ("GET", "//", 404, b"Invalid address on request line\n")]
        for method, path, status, body in cases:
            with self.subTest(method=method, path=path):
                conn.request(method, path, "abc")
                resp = conn.read_response()
                self.assertEqual((resp.status, resp.body), (status, body))
                self.assertEqual(resp.headers["content-type"], "text/plain")
                self.assertEqual(int(resp.headers["content-length"]), len(body))
        self.assertEqual(conn.post("alive\n").status, 200)

    def test_413_boundaries_include_trailing_newline(self):
        server = self.server(stats=True)
        conn = self.connect(server.port)
        for size, status in [(2047, 200), (2048, 200), (2049, 413)]:
            with self.subTest(size=size):
                resp = conn.post("x" * (size - 1) + "\n")
                self.assertEqual(resp.status, status)
        self.assertEqual(resp.body, b"Input is too large: 2049 bytes\n")
        self.assertEqual(conn.post("after\n").status, 200)  # body was drained
        self.assertEqual(len(server.requests()), 3)
        self.assertEqual(server.stats()["Num prompts received"], 4)

    def test_404_wins_over_413(self):
        server = self.server()
        conn = self.connect(server.port)
        resp = conn.post("y" * 5000, path="/other")
        self.assertEqual(resp.status, 404)
        resp = self.connect(server.port)
        resp.request("PATCH", "/", "z" * 5000)
        self.assertEqual(resp.read_response().status, 405)

    def test_huge_content_length_is_not_allocated(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.send(b"POST / HTTP/1.1\r\nContent-Length: 1000000000000\r\n\r\n")
        resp = conn.read_response()
        self.assertEqual(resp.status, 413)
        self.assertEqual(resp.body, b"Input is too large: 1000000000000 bytes\n")
        self.assertEqual(resp.headers.get("connection"), "close")
        self.assertTrue(conn.is_closed())

    MALFORMED = [b"GET / HTTP/1.1\n\n",
                 b"GET /\r\n\r\n",
                 b"GET / HTTP/9.9\r\n\r\n",
                 b"GET / HTTP/1.1\r\nNoColon\r\n\r\n",
                 b"GET / HTTP/1.1\r\nA: b\r\n folded\r\n\r\n",
                 b"POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nab",
                 b"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
                 b"POST / HTTP/1.1\r\nContent-Length: 99999999999999999999999\r\n\r\n",
                 b"POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n0\r\n\r\n",
                 b"GET / HTTP/1.1\r\nX: " + b"a" * 9000 + b"\r\n\r\n",
                 b"GET / HTTP/1.1\r\n" + b"".join(b"H%d: v\r\n" % i for i in range(101)) + b"\r\n",
                 b"\x00\x01\x02\r\n\r\n"]

    def test_malformed_requests_close_silently(self):
        server = self.server(stats=True)
        for raw in self.MALFORMED:
            with self.subTest(raw=raw[:40]):
                conn = self.connect(server.port)
                conn.send(raw)
                self.assertTrue(conn.is_closed(), "connection left open")
                self.assertEqual(conn.buf, b"")
        good = self.connect(server.port)
        self.assertEqual(good.post("fine\n").status, 200)
        stats = server.stats()
        self.assertEqual(stats["Num prompts received"], 1)
        self.assertEqual(server.requests()[0]["json"]["prompt"], expected_prompt([], "fine\n"))

    def test_early_eof_in_body(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.send(b"POST / HTTP/1.1\r\nContent-Length: 50\r\n\r\npartial")
        conn.sock.shutdown(socket.SHUT_WR)
        self.assertTrue(conn.is_closed())
        self.assertEqual(self.connect(server.port).post("next\n").status, 200)
        self.assertEqual(server.requests()[0]["json"]["prompt"], expected_prompt([], "next\n"))


class History(GatewayTest):
    def run_history(self, size, prompts):
        server = self.server(history=size)
        conn = self.connect(server.port)
        for i in range(prompts):
            self.assertEqual(conn.post("p%d\n" % i).status, 200)
        sent = [r["json"]["prompt"] for r in server.requests()]
        for i in range(prompts):
            keep = list(range(max(0, i - size), i))
            pairs = [("p%d\n" % j, "echo:p%d" % j) for j in keep]
            self.assertEqual(sent[i], expected_prompt(pairs, "p%d\n" % i), "request %d" % i)

    def test_history_size_1(self):
        self.run_history(1, 4)

    def test_history_size_5(self):
        self.run_history(5, 8)

    def test_history_size_10(self):
        self.run_history(10, 13)

    def test_failed_transactions_are_not_recorded(self):
        server = self.server(history=5, plan={"actions": ["ok", "bad_json", "ok"]})
        conn = self.connect(server.port)
        self.assertEqual(conn.post("one\n").status, 200)
        self.assertEqual(conn.post("two\n").status, 502)
        self.assertEqual(conn.post("x" * 3000).status, 413)
        self.assertEqual(conn.post("three\n").status, 200)
        sent = [r["json"]["prompt"] for r in server.requests()]
        self.assertEqual(sent[2], expected_prompt([("one\n", "echo:one")], "three\n"))

    def test_history_is_per_connection(self):
        server = self.server(history=3)
        a = self.connect(server.port)
        b = self.connect(server.port)
        a.post("a1\n")
        b.post("b1\n")
        a.post("a2\n")
        sent = [r["json"]["prompt"] for r in server.requests()]
        self.assertEqual(sent[1], expected_prompt([], "b1\n"))
        self.assertEqual(sent[2], expected_prompt([("a1\n", "echo:a1")], "a2\n"))
        a.close()
        c = self.connect(server.port)
        c.post("c1\n")
        self.assertEqual(server.requests()[3]["json"]["prompt"], expected_prompt([], "c1\n"))


class MultipleClients(GatewayTest):
    def test_sequential_clients(self):
        server = self.server(stats=True)
        for i in range(5):
            conn = self.connect(server.port)
            self.assertEqual(conn.post("s%d-a\n" % i).body, b"echo:s%d-a\n" % i)
            self.assertEqual(conn.post("s%d-b\n" % i).status, 200)
            conn.close()
        self.assertTrue(server.wait_stats(lambda s: s["Total sessions completed"] == 5))
        sent = [r["json"]["prompt"] for r in server.requests()]
        self.assertEqual(sent[8], expected_prompt([], "s4-a\n"))

    def test_concurrent_clients_share_one_serialised_backend(self):
        server = self.server(plan={"default": "delay:40"}, stats=True)
        results = {}
        errors = []

        def worker(name):
            try:
                conn = self.connect(server.port)
                got = []
                for i in range(3):
                    got.append(conn.post("%s-%d\n" % (name, i)).body)
                results[name] = got
            except Exception as exc:  # noqa: BLE001 - reported below
                errors.append(exc)

        threads = [threading.Thread(target=worker, args=("c%d" % n,)) for n in range(8)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
        self.assertEqual(errors, [])
        for name, got in results.items():
            self.assertEqual(got, [b"echo:%s-%d\n" % (name.encode(), i) for i in range(3)])
        requests = server.requests()
        self.assertEqual(len(requests), 24)
        self.assertEqual(max(r["inflight"] for r in requests), 1)
        self.assertEqual(len({r["conn"] for r in requests}), 1)  # one persistent connection
        for r in requests:  # history never leaks between clients
            turns = r["json"]["prompt"].split("<|im_start|>user\n")[1:]
            owners = {turn.split("-")[0] for turn in turns}
            self.assertEqual(len(owners), 1, r["json"]["prompt"])
            self.assertEqual(len(turns), int(turns[-1].split("-")[1][0]) + 1)
        self.assertEqual(server.stats()["Num prompts received"], 24)

    def test_client_disconnect_mid_transaction(self):
        server = self.server(plan={"default": "delay:300"}, stats=True)
        conn = self.connect(server.port)
        conn.request("POST", "/", "gone\n")
        self.assertTrue(server.wait_events("request", 1))
        conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        conn.close()
        self.assertTrue(server.wait_stats(lambda s: s["Total sessions completed"] == 1))
        other = self.connect(server.port)
        self.assertEqual(other.post("still here\n").status, 200)
        stats = server.stats()
        self.assertEqual(stats["Total tokens evaluated"], 20)  # work done for both
        self.assertIsNone(server.proc.poll())

    def test_client_that_never_reads_does_not_block_others(self):
        server = self.server()
        hog = self.connect(server.port)
        hog.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        for _ in range(50):
            hog.send(b"GET / HTTP/1.1\r\n\r\n")
        other = self.connect(server.port)
        self.assertEqual(other.post("ok\n").status, 200)
        self.assertTrue(wait_until(lambda: server.proc.poll() is None))
