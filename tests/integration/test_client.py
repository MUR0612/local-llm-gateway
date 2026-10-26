"""llm-client behaviour against scripted fake servers (categories 1-4)."""
import socket
import time

from helpers import FakeServer, GatewayTest, free_port, http_reply

WELCOME = b"Welcome to llm-client.\n"
ENTER = b"Enter your prompt:\n"


def reply_each(*replies):
    """Handler that answers successive requests with the given bytes."""
    def handler(server, conn):
        state = {}
        for reply in replies:
            if server.read_request(conn, state) is None:
                return
            conn.sendall(reply)
        while server.read_request(conn, state) is not None:
            pass
    return handler


def find_free_service():
    """A /etc/services name (port >= 1024) whose port is currently free."""
    try:
        with open("/etc/services") as handle:
            lines = handle.readlines()
    except OSError:
        return None
    for line in lines:
        parts = line.split()
        if len(parts) < 2 or not parts[1].endswith("/tcp") or line.startswith("#"):
            continue
        port = int(parts[1].split("/")[0])
        if port < 1024:
            continue
        try:
            if socket.getservbyname(parts[0], "tcp") != port:
                continue
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", port))
            return parts[0], port
        except OSError:
            continue
    return None


class ClientCli(GatewayTest):
    def test_missing_port_is_usage_error(self):
        result = self.run_client(None, args=[])
        self.assertEqual(result.returncode, 8)
        self.assertEqual(result.stderr, b"Usage: llm-client port\n")
        self.assertEqual(result.stdout, b"")

    def test_extra_argument_is_usage_error(self):
        result = self.run_client(None, args=["1234", "extra"])
        self.assertEqual(result.returncode, 8)
        self.assertEqual(result.stderr, b"Usage: llm-client port\n")

    def test_empty_port(self):
        result = self.run_client(None, args=[""])
        self.assertEqual(result.returncode, 9)
        self.assertEqual(result.stderr, b"llm-client: invalid port\n")

    def test_invalid_timeout_env(self):
        result = self.run_client(1, env={"LLMGW_CLIENT_TIMEOUT_MS": "abc"})
        self.assertEqual(result.returncode, 8)

    def test_connect_failure(self):
        result = self.run_client(free_port())
        self.assertEqual(result.returncode, 18)
        self.assertEqual(result.stderr, b"llm-client: unable to connect to the server\n")
        self.assertEqual(result.stdout, b"")

    def test_unknown_service_name(self):
        result = self.run_client(None, args=["no-such-service-xyz"])
        self.assertEqual(result.returncode, 18)

    def test_connect_by_service_name(self):
        service = find_free_service()
        if service is None:
            self.skipTest("no free /etc/services entry >= 1024")
        name, port = service
        fake = FakeServer(reply_each(http_reply(200, b"ok\n")), port=port)
        self.addCleanup(fake.close)
        result = self.run_client(None, args=[name], stdin=b"hi\n")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"ok\n", result.stdout)


class ClientExchange(GatewayTest):
    def test_success_and_error_responses_on_one_connection(self):
        fake = FakeServer(reply_each(http_reply(200, b"first reply\n", "OK"),
                                     http_reply(404, b"not here\n")))
        self.addCleanup(fake.close)
        result = self.run_client(fake.port, stdin=b"hello world\nsecond\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, WELCOME + ENTER + b"Response:\nfirst reply\n"
                         + ENTER + ENTER)
        self.assertEqual(result.stderr, b"Response:\nnot here\n")
        self.assertEqual(fake.connections, 1)
        head, body = fake.requests[0]
        self.assertTrue(head.startswith(b"POST / HTTP/1.1\r\n"))
        self.assertIn(b"\r\nContent-Length: 12", head)
        self.assertEqual(body, b"hello world\n")
        self.assertEqual(fake.requests[1][1], b"second\n")

    def test_waits_for_reply_before_next_prompt(self):
        order = []

        def handler(server, conn):
            state = {}
            server.read_request(conn, state)
            order.append("first")
            conn.settimeout(0.3)
            try:
                early = conn.recv(1)
            except socket.timeout:
                early = b""
            order.append("early" if early else "quiet")
            conn.settimeout(None)
            conn.sendall(http_reply(200, b"a\n"))
            server.read_request(conn, state)
            conn.sendall(http_reply(200, b"b\n"))

        fake = FakeServer(handler)
        self.addCleanup(fake.close)
        result = self.run_client(fake.port, stdin=b"one\ntwo\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(order, ["first", "quiet"])

    def test_last_line_without_newline_and_byte_lengths(self):
        text = "中文 prompt".encode("utf-8")
        fake = FakeServer(reply_each(http_reply(200, "回覆\n".encode("utf-8")),
                                     http_reply(200, b"x\n")))
        self.addCleanup(fake.close)
        result = self.run_client(fake.port, stdin=text + b"\nnul\x00byte")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(fake.requests[0][1], text + b"\n")
        self.assertIn(b"Content-Length: %d" % (len(text) + 1), fake.requests[0][0])
        self.assertEqual(fake.requests[1][1], b"nul\x00byte\n")
        self.assertIn("回覆\n".encode("utf-8"), result.stdout)

    def test_immediate_eof(self):
        fake = FakeServer(reply_each())
        self.addCleanup(fake.close)
        result = self.run_client(fake.port, stdin=b"")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, WELCOME + ENTER)

    def test_extra_response_headers_are_ignored(self):
        fake = FakeServer(reply_each(http_reply(200, b"ok\n", extra=b"X-Thing: 1\r\n")))
        self.addCleanup(fake.close)
        result = self.run_client(fake.port, stdin=b"q\n")
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"Response:\nok\n", result.stdout)


class ClientFailures(GatewayTest):
    def expect_comm_error(self, handler, stdin=b"prompt\n", env=None):
        fake = FakeServer(handler)
        self.addCleanup(fake.close)
        started = time.monotonic()
        result = self.run_client(fake.port, stdin=stdin, env=env)
        self.assertEqual(result.returncode, 13, result)  # never -SIGPIPE
        self.assertTrue(result.stderr.endswith(b"llm-client: unexpected communication error\n"))
        return result, time.monotonic() - started

    def test_malformed_response(self):
        self.expect_comm_error(reply_each(b"garbage\r\n\r\n"))

    def test_response_without_content_length(self):
        self.expect_comm_error(reply_each(b"HTTP/1.1 200 OK\r\n\r\nbody"))

    def test_truncated_body(self):
        def handler(server, conn):
            server.read_request(conn, {})
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort")
        self.expect_comm_error(handler)

    def test_server_closes_before_reply(self):
        def handler(server, conn):
            server.read_request(conn, {})
        self.expect_comm_error(handler)

    def test_broken_pipe_on_next_prompt(self):
        def handler(server, conn):
            server.read_request(conn, {})
            conn.sendall(http_reply(200, b"bye\n"))
            conn.shutdown(socket.SHUT_RDWR)
        result, _ = self.expect_comm_error(handler, stdin=b"a\n" + b"b" * 200000 + b"\n")
        self.assertIn(b"Response:\nbye\n", result.stdout)

    def test_response_timeout(self):
        def handler(server, conn):
            server.read_request(conn, {})
            time.sleep(5)
        _, elapsed = self.expect_comm_error(handler, env={"LLMGW_CLIENT_TIMEOUT_MS": "300"})
        self.assertLess(elapsed, 4)

    def test_output_write_failure(self):
        fake = FakeServer(reply_each(http_reply(200, b"x\n")))
        self.addCleanup(fake.close)
        with open("/dev/full", "wb") as full:
            result = self.run_client(fake.port, stdin=b"q\n", stdout=full)
        self.assertEqual(result.returncode, 14)
        self.assertEqual(result.stderr, b"llm-client: unable to write output\n")
