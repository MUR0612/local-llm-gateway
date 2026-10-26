#!/usr/bin/env python3
"""Stand-in for `llama-server` used only to test tools/llamacpp_backend.py.

It mimics the CLI flags, the /health readiness behaviour (503 while
"loading", then 200) and the /completion JSON shape. It is not a model.
"""
import argparse
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

USER_TAG = "<|im_start|>user\n"


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    health_calls = 0
    lock = threading.Lock()

    def log_message(self, *args):
        pass

    def send(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        with Handler.lock:
            Handler.health_calls += 1
            ready = Handler.health_calls > 2
        if self.path == "/health" and ready:
            self.send(200, {"status": "ok"})
        else:
            self.send(503, {"error": {"message": "Loading model"}})

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(length))
        prompt = request["prompt"]
        user = prompt[prompt.rfind(USER_TAG) + len(USER_TAG):].split("<|im_end|>")[0]
        self.send(200, {"content": "llama:" + user.strip(), "tokens_evaluated": len(prompt) // 4,
                        "n_predict_seen": request.get("n_predict")})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("-m")
    parser.add_argument("-hf")
    args = parser.parse_args()
    pidfile = os.environ.get("FAKE_LLAMA_PIDFILE")
    if pidfile:
        with open(pidfile, "w") as handle:
            handle.write(str(os.getpid()))
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
