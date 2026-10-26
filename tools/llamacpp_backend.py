#!/usr/bin/env python3
"""Managed-backend adapter that runs llama.cpp's `llama-server`.

Implements the gateway's startup contract: start the model server on a free
loopback port, wait until GET /health returns 200, print that port as one
line on stdout, then stay silent on stdout until terminated.

    llm-server --backend-cmd python3 \
        --backend-arg tools/llamacpp_backend.py \
        --backend-arg --model --backend-arg /path/to/model.gguf 4 5

Status: written against the llama.cpp server README (flags --host, --port,
-m/--model, -hf/--hf-repo; GET /health returns 503 while loading and 200 when
ready; POST /completion accepts prompt/n_predict/cache_prompt and returns
content/tokens_evaluated). It has NOT been run against a real model in this
project's verification; see docs/verification.md.

The gateway starts this adapter in its own process group and signals the
whole group, so llama-server is stopped together with the adapter.
"""
import argparse
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def healthy(port):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/health" % port, timeout=2) as resp:
            return resp.status == 200
    except (urllib.error.URLError, OSError):
        return False


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--model", help="path to a local GGUF model file")
    source.add_argument("--hf-repo", help="Hugging Face repo, downloaded by llama.cpp")
    parser.add_argument("--llama-server", default="llama-server")
    parser.add_argument("--ready-timeout", type=float, default=300.0)
    parser.add_argument("--log", default=os.devnull, help="llama-server output file")
    parser.add_argument("extra", nargs="*", help="extra llama-server arguments")
    args = parser.parse_args()

    port = free_port()
    cmd = [args.llama_server, "--host", "127.0.0.1", "--port", str(port)]
    cmd += ["-m", args.model] if args.model else ["-hf", args.hf_repo]
    cmd += args.extra
    with open(args.log, "ab") as log:
        child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=log, stderr=log)

    def stop(signum, _frame):
        child.terminate()
        try:
            child.wait(10)
        except subprocess.TimeoutExpired:
            child.kill()
        sys.exit(128 + signum)

    signal.signal(signal.SIGTERM, stop)
    deadline = time.monotonic() + args.ready_timeout
    while not healthy(port):
        if child.poll() is not None or time.monotonic() > deadline:
            child.kill()
            return 1  # EOF on stdout without a port line = startup failure
        time.sleep(0.25)
    sys.stdout.write("%d\n" % port)
    sys.stdout.flush()
    return child.wait()


if __name__ == "__main__":
    sys.exit(main())
