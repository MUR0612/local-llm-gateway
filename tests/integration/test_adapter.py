"""tools/llamacpp_backend.py wiring, using a fake llama-server (not a model)."""
import os
import shutil
import signal
import stat
import sys

from helpers import PROJECT, TESTS, GatewayTest, pid_gone, wait_until

ADAPTER = os.path.join(PROJECT, "tools", "llamacpp_backend.py")
FAKE = os.path.join(TESTS, "fake_llama_server.py")


class LlamaCppAdapter(GatewayTest):
    def test_adapter_handshake_completion_and_group_shutdown(self):
        fake = os.path.join(self.tmp, "llama-server")
        shutil.copy(FAKE, fake)
        os.chmod(fake, os.stat(fake).st_mode | stat.S_IXUSR)
        pidfile = os.path.join(self.tmp, "llama.pid")
        args = []
        for arg in [ADAPTER, "--llama-server", fake, "--model", "/models/none.gguf"]:
            args += ["--backend-arg", arg]
        server = self.server(mock=False, args=["--backend-cmd", sys.executable] + args,
                             env={"FAKE_LLAMA_PIDFILE": pidfile})
        conn = self.connect(server.port)
        resp = conn.post("hello adapter\n")
        self.assertEqual(resp.status, 200)
        self.assertEqual(resp.body, b"llama:hello adapter\n")
        with open(pidfile) as handle:
            llama_pid = int(handle.read())
        self.assertEqual(server.stop(signal.SIGTERM), 0)
        self.assertTrue(wait_until(lambda: pid_gone(llama_pid)),
                        "llama-server outlived the gateway")
