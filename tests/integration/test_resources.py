"""Resource stability, busy-wait checks, shutdown and fault injection
(category 21 plus portfolio reliability extensions)."""
import os
import signal
import time
import unittest

from helpers import (BASE_THREADS, FAULT_LIB, SANITIZER, ExternalMock, GatewayTest, cpu_ticks,
                     fd_count, pid_gone, proc_state, rss_kb, thread_count, wait_until)


class Stability(GatewayTest):
    def settle(self, server, threads):
        return wait_until(lambda: thread_count(server.pid) == threads, timeout=5)

    def test_one_thread_per_connected_client(self):
        server = self.server()
        conns = [self.connect(server.port) for _ in range(5)]
        for conn in conns:
            conn.post("t\n")
        self.assertEqual(thread_count(server.pid), BASE_THREADS + 5)
        for conn in conns:
            conn.close()
        self.assertTrue(self.settle(server, BASE_THREADS))

    def test_no_fd_thread_or_heap_growth(self):
        server = self.server(stats=True)
        base_threads = BASE_THREADS + 1

        def cycle(n):
            for i in range(n):
                conn = self.connect(server.port)
                conn.post("cycle %d\n" % i)
                conn.request("GET", "/")
                conn.read_response()
                conn.close()
                self.conns.remove(conn)
                stats = self.connect(server.stats_port)
                stats.request("GET", "/stats")
                stats.read_response()
                stats.close()
                self.conns.remove(stats)

        cycle(30)  # warm up allocator arenas and caches
        self.assertTrue(self.settle(server, base_threads))
        fds, rss = fd_count(server.pid), rss_kb(server.pid)
        cycle(150)
        self.assertTrue(self.settle(server, base_threads))
        self.assertEqual(fd_count(server.pid), fds)
        if not SANITIZER:  # sanitizers keep quarantined memory resident
            self.assertLess(rss_kb(server.pid) - rss, 1024)

    def test_fds_stable_across_restarts(self):
        server = self.server()
        conn = self.connect(server.port)
        conn.post("first\n")
        fds = fd_count(server.pid)
        for i in range(5):
            pid = server.backend_pids()[-1]
            os.kill(pid, signal.SIGKILL)
            self.assertTrue(wait_until(lambda: proc_state(pid) == "Z"))
            self.assertEqual(conn.post("r%d\n" % i).status, 200)
        self.assertEqual(fd_count(server.pid), fds)
        self.assertTrue(all(pid_gone(p) for p in server.backend_pids()[:-1]))

    def test_idle_server_does_not_busy_wait(self):
        server = self.server(stats=True, max_clients=1)
        idle_prompt = self.connect(server.port)
        idle_prompt.post("x\n")
        waiting = self.connect(server.port)  # blocked behind the limit
        idle_stats = self.connect(server.stats_port)
        before = cpu_ticks(server.pid)
        time.sleep(2)
        used = cpu_ticks(server.pid) - before
        self.assertLessEqual(used, 5, "%d CPU ticks while idle" % used)
        del waiting, idle_stats


class Shutdown(GatewayTest):
    def test_sigterm_with_active_clients_and_transaction(self):
        server = self.server(stats=True, plan={"actions": ["ok", "hang"]})
        idle = self.connect(server.port)
        idle.post("idle\n")
        busy = self.connect(server.port)
        busy.request("POST", "/", "in flight\n")
        self.assertTrue(server.wait_events("request", 2))
        stats = self.connect(server.stats_port)
        stats.request("GET", "/stats")
        stats.read_response()
        started = time.monotonic()
        self.assertEqual(server.stop(signal.SIGTERM), 0)
        self.assertLess(time.monotonic() - started, 6)
        for conn in (idle, busy, stats):
            self.assertTrue(conn.is_closed())
        self.assertTrue(pid_gone(server.backend_pids()[0]))

    def test_sigint_during_startup(self):
        server = self.server(plan={"startup": "hang"})
        self.assertTrue(wait_until(lambda: server.events("start")))
        self.assertEqual(server.stop(signal.SIGINT), 0)
        self.assertTrue(pid_gone(server.backend_pids()[0]))

    def test_waiting_client_released_on_shutdown(self):
        server = self.server(max_clients=1)
        holder = self.connect(server.port)
        holder.post("hold\n")
        waiter = self.connect(server.port)
        waiter.request("POST", "/", "wait\n")
        self.assertEqual(server.stop(), 0)
        self.assertIsNone(waiter.read_response())

    def test_external_backend_survives_shutdown(self):
        ext = ExternalMock(self)
        server = self.server(external_port=ext.port)
        self.connect(server.port).post("x\n")
        self.assertEqual(server.stop(), 0)
        self.assertIsNone(ext.proc.poll())


@unittest.skipIf(SANITIZER or not FAULT_LIB, "fault injection needs the plain build and --fault-lib")
class FaultInjection(GatewayTest):
    def test_handler_thread_creation_failure(self):
        # pthread_create calls: 1 reaper, 2 signal thread, 3 first client handler.
        server = self.server(max_clients=1, preload="pthread_create:3")
        dropped = self.connect(server.port)
        dropped.request("POST", "/", "dropped\n")
        self.assertIsNone(dropped.read_response())
        self.assertEqual(self.connect(server.port).post("next\n").status, 200)  # slot released

    def test_accept_failure_backs_off_and_recovers(self):
        server = self.server(preload="accept4:1")
        self.assertEqual(self.connect(server.port).post("hi\n").status, 200)

    def test_spawn_failure(self):
        server = self.server(preload="posix_spawnp:1", wait_port=False)
        self.assertEqual(server.wait_exit(), 17)
        self.assertTrue(server.stderr().endswith(b"llm-server: could not start the backend\n"))

    def test_spawn_pipe_failure(self):
        server = self.server(preload="pipe2:2", wait_port=False)
        self.assertEqual(server.wait_exit(), 17)
        self.assertEqual(server.events(), [])

    def test_restart_spawn_failure_is_fatal(self):
        server = self.server(preload="posix_spawnp:2")
        conn = self.connect(server.port)
        conn.post("x\n")
        pid = server.backend_pids()[0]
        os.kill(pid, signal.SIGKILL)
        self.assertTrue(wait_until(lambda: proc_state(pid) == "Z"))
        conn.request("POST", "/", "y\n")
        self.assertEqual(server.wait_exit(), 17)
        self.assertTrue(pid_gone(pid))
