"""llm-server arguments, listeners and backend startup (categories 6-8, 16)."""
import signal
import socket
import subprocess
import time

from helpers import (SERVER, USAGE, GatewayTest, Http, free_port, listening_ports,
                     pid_gone, wait_until)
from test_client import find_free_service


class ServerCli(GatewayTest):
    INVALID = [[], ["1"], ["1", "2", "3", "4"], ["", "1"], ["1", ""], ["1", "1", ""],
               ["-1", "1"], ["10001", "1"], ["1", "0"], ["1", "11"], ["+", "1"],
               [" 1", "1"], ["1 ", "1"], ["1x", "1"], ["1", "+-1"], ["0x10", "1"],
               ["99999999999999999999999", "1"], ["1", "-0"], ["--bogus", "1", "1"],
               ["--backend-cmd", "", "1", "1"]]

    def test_invalid_command_lines(self):
        for args in self.INVALID:
            with self.subTest(args=args):
                result = subprocess.run([SERVER] + args, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 16)
                self.assertEqual(result.stderr, USAGE)
                self.assertEqual(result.stdout, b"")

    def test_invalid_environment_value(self):
        result = subprocess.run([SERVER, "1", "1"], capture_output=True, timeout=10,
                                env={"LLMGW_BACKEND_MAX_RESENDS": "x", "PATH": "/usr/bin"})
        self.assertEqual(result.returncode, 16)
        self.assertIn(b"LLMGW_BACKEND_MAX_RESENDS", result.stderr)

    def test_plus_signs_and_limits_accepted(self):
        server = self.server(max_clients="+10000", history="+10")
        self.assertGreater(server.port, 0)
        self.assertEqual(server.stop(), 0)

    def test_help(self):
        result = subprocess.run([SERVER, "--help"], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertIn(b"maxclients", result.stdout)


class Listeners(GatewayTest):
    def test_ephemeral_port_reported_and_listening(self):
        server = self.server()
        self.assertNotEqual(server.port, 0)
        self.assertEqual(listening_ports(server.pid), {server.port})
        self.assertEqual(server.stop(), 0)

    def test_explicit_zero_is_ephemeral(self):
        server = self.server(port=0)
        self.assertNotEqual(server.port, 0)

    def test_specific_port(self):
        port = free_port()
        server = self.server(port=port)
        self.assertEqual(server.port, port)

    def test_service_name_port_reports_number(self):
        service = find_free_service()
        if service is None:
            self.skipTest("no free /etc/services entry >= 1024")
        server = self.server(port=service[0])
        self.assertEqual(server.port, service[1])

    def test_port_in_use(self):
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen(1)
            port = busy.getsockname()[1]
            server = self.server(port=port, wait_port=False)
            self.assertEqual(server.wait_exit(), 9)
        self.assertEqual(server.stderr(),
                         b'llm-server: unable to listen on given port "%d"\n' % port)

    def test_invalid_service_name(self):
        server = self.server(port="no-such-service-xyz", wait_port=False)
        self.assertEqual(server.wait_exit(), 9)
        self.assertEqual(server.stderr(),
                         b'llm-server: unable to listen on given port "no-such-service-xyz"\n')
        self.assertEqual(server.events(), [])  # backend never started

    def test_binds_loopback_only(self):
        server = self.server()
        with open("/proc/net/tcp") as handle:
            rows = [line.split() for line in handle][1:]
        local = [r[1] for r in rows if r[3] == "0A" and int(r[1].split(":")[1], 16) == server.port]
        self.assertEqual(local, ["0100007F:%04X" % server.port])


class StatsListener(GatewayTest):
    def test_unset_means_single_listener(self):
        server = self.server()
        server.wait_ready()
        self.assertEqual(listening_ports(server.pid), {server.port})

    def test_set_opens_second_listener(self):
        server = self.server(stats=True)
        self.assertEqual(listening_ports(server.pid), {server.port, server.stats_port})
        self.assertEqual(server.stats()["Clients connected"], 0)

    def test_stats_port_in_use(self):
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen(1)
            port = busy.getsockname()[1]
            server = self.server(stats=port, wait_port=False)
            self.assertEqual(server.wait_exit(), 4)
        self.assertEqual(server.stderr(),
                         b'llm-server: cannot listen on given statistics port "%d"\n' % port)
        self.assertEqual(server.events(), [])

    def test_stats_port_invalid_or_empty(self):
        for value in ["bogus-service-xyz", ""]:
            with self.subTest(value=value):
                server = self.server(env={"LLMGW_STATS_PORT": value}, wait_port=False)
                self.assertEqual(server.wait_exit(), 4)
                self.assertEqual(server.stderr(),
                                 b'llm-server: cannot listen on given statistics port "%s"\n'
                                 % value.encode())

    def test_main_port_checked_before_stats_port(self):
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen(1)
            port = busy.getsockname()[1]
            server = self.server(port=port, env={"LLMGW_STATS_PORT": "bogus-xyz"},
                                 wait_port=False)
            self.assertEqual(server.wait_exit(), 9)


class BackendStartup(GatewayTest):
    def test_success_connects_once(self):
        server = self.server()
        self.assertTrue(server.wait_ready())
        self.assertEqual(len(server.events("start")), 1)
        self.assertEqual(server.stop(), 0)
        self.assertTrue(pid_gone(server.backend_pids()[0]))

    def test_exec_failure(self):
        server = self.server(args=["--backend-cmd", "/nonexistent/backend"],
                             external_port=None, wait_port=False)
        # --backend-cmd given twice: the last one wins (getopt semantics).
        self.assertEqual(server.wait_exit(), 17)
        self.assertTrue(server.stderr().endswith(b"llm-server: could not start the backend\n"))

    def check_start_failure(self, startup, env=None):
        server = self.server(plan={"startup": startup}, env=env, wait_port=False)
        self.assertEqual(server.wait_exit(), 17)
        self.assertTrue(server.stderr().endswith(b"llm-server: could not start the backend\n"))
        pids = server.backend_pids()
        self.assertEqual(len(pids), 1)
        self.assertTrue(pid_gone(pids[0]), "backend child left running")
        return server

    def test_eof_before_ready_line(self):
        self.check_start_failure("exit")

    def test_invalid_ready_line(self):
        self.check_start_failure("bad_port")

    def test_startup_timeout(self):
        started = time.monotonic()
        self.check_start_failure("hang", env={"LLMGW_BACKEND_STARTUP_TIMEOUT_MS": "500"})
        self.assertLess(time.monotonic() - started, 8)

    def test_port_reported_before_backend_ready(self):
        server = self.server(plan={"startup": "hang"})  # port line already read
        self.assertTrue(wait_until(lambda: server.events("start")))
        self.assertEqual(server.events("ready"), [])
        self.assertEqual(server.stop(signal.SIGINT), 0)
        self.assertTrue(pid_gone(server.backend_pids()[0]))

    def test_connect_failure_after_26_attempts(self):
        server = self.server(plan={"startup": "dead_port"}, env={"LLMGW_DEBUG": "1"},
                             wait_port=False)
        self.assertEqual(server.wait_exit(), 1)
        err = server.stderr()
        self.assertTrue(err.endswith(b"llm-server: could not connect to the backend\n"))
        self.assertEqual(err.count(b"connect attempt "), 26)
        self.assertTrue(pid_gone(server.backend_pids()[0]))

    def test_external_connect_failure(self):
        server = self.server(external_port=free_port(), wait_port=False)
        self.assertEqual(server.wait_exit(), 1)
        self.assertTrue(server.stderr().endswith(b"llm-server: could not connect to the backend\n"))

    def test_no_prompt_client_accepted_before_backend_ready(self):
        server = self.server(plan={"startup": "hang"},
                             env={"LLMGW_BACKEND_STARTUP_TIMEOUT_MS": "1500"})
        conn = self.connect(server.port)
        conn.request("POST", "/", "hi\n")
        self.assertIsNone(conn.read_response())  # closed when the server exits
        self.assertEqual(server.wait_exit(), 17)
