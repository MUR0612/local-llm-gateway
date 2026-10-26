#!/usr/bin/env python3
"""Controllable test backend for llm-server (standard library only).

This is a test fixture, not a language model. It speaks just enough HTTP to
answer POST /completion with JSON, and can be scripted to misbehave so the
gateway's startup, recovery and history logic can be tested offline.

Startup contract (managed mode): print the listening port as one decimal line
on stdout, flush, then never write to stdout again.

Usage: mock_backend.py --plan PLAN.json --log LOG.jsonl [--gen-file FILE]

Plan keys (a "generations" list may hold one plan per restart; the last entry
repeats):
  startup   ok | exit | hang | bad_port | dead_port | exit_after_port
  actions   list consumed one per request: ok | close | malformed | bad_json |
            status500 | no_content | crash | hang | delay:<ms> | raw:<body> |
            close_listener | slow_ok:<ms>
  default   action used once actions run out (default "ok")
  tokens    tokens_evaluated value (default 10)
  content   fixed content; default echoes the last user turn as "echo:<text>"
"""
import argparse
import fcntl
import json
import os
import socket
import sys
import threading
import time

USER_TAG = "<|im_start|>user\n"
END_TAG = "<|im_end|>"


class Logger:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()

    def write(self, **event):
        event["t"] = time.monotonic()
        event["pid"] = os.getpid()
        line = json.dumps(event) + "\n"
        with self.lock, open(self.path, "a", encoding="utf-8") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX)
            handle.write(line)
            handle.flush()
            fcntl.flock(handle, fcntl.LOCK_UN)


def next_generation(path):
    if path is None:
        return 0
    with open(path, "a+", encoding="utf-8") as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        handle.seek(0)
        text = handle.read().strip()
        gen = int(text) if text else 0
        handle.seek(0)
        handle.truncate()
        handle.write(str(gen + 1))
        handle.flush()
        fcntl.flock(handle, fcntl.LOCK_UN)
    return gen


def read_request(conn, pending):
    """Returns (path, headers, body, pending) or None on EOF/garbage."""
    while b"\r\n\r\n" not in pending:
        chunk = conn.recv(65536)
        if not chunk:
            return None
        pending += chunk
    head, pending = pending.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split(" ")
    if len(parts) != 3:
        return None
    headers = {}
    for line in lines[1:]:
        name, _, value = line.partition(":")
        headers[name.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0"))
    while len(pending) < length:
        chunk = conn.recv(65536)
        if not chunk:
            return None
        pending += chunk
    return parts[1], headers, pending[:length], pending[length:]


def last_user_text(prompt):
    start = prompt.rfind(USER_TAG)
    if start < 0:
        return ""
    text = prompt[start + len(USER_TAG):]
    end = text.find(END_TAG)
    return (text if end < 0 else text[:end]).rstrip("\n")


class Backend:
    def __init__(self, plan, log, gen):
        self.plan = plan
        self.log = log
        self.gen = gen
        self.actions = list(plan.get("actions", []))
        self.lock = threading.Lock()
        self.inflight = 0
        self.conn_ids = 0
        self.listener = None

    def next_action(self):
        with self.lock:
            if self.actions:
                return self.actions.pop(0)
        return self.plan.get("default", "ok")

    def reply(self, conn, status, body):
        reason = {200: "OK", 500: "Internal Server Error"}.get(status, "X")
        head = (f"HTTP/1.1 {status} {reason}\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                f"Content-Length: {len(body)}\r\nX-Extra: ignored\r\n\r\n")
        conn.sendall(head.encode("ascii") + body)

    def ok_body(self, request_json):
        content = self.plan.get("content")
        if content is None:
            content = "echo:" + last_user_text(request_json.get("prompt", ""))
        payload = {"index": 0, "content": content,
                   "generation_settings": {"content": "nested-ignored",
                                           "tokens_evaluated": 999999},
                   "tokens_evaluated": self.plan.get("tokens", 10),
                   "model": "mock", "stop": True}
        return json.dumps(payload, ensure_ascii=False).encode("utf-8")

    def perform(self, conn, action, request_json):
        """Returns False when the connection must be closed."""
        if action.startswith("delay:") or action.startswith("slow_ok:"):
            time.sleep(int(action.split(":", 1)[1]) / 1000.0)
            action = "ok"
        if action == "ok":
            self.reply(conn, 200, self.ok_body(request_json))
        elif action == "close":
            return False
        elif action == "malformed":
            conn.sendall(b"NOT-HTTP garbage\r\n\r\n")
            return False
        elif action == "bad_json":
            self.reply(conn, 200, b"{\"content\": \"unterminated")
        elif action == "status500":
            self.reply(conn, 500, self.ok_body(request_json))
        elif action == "no_content":
            self.reply(conn, 200, b"{\"tokens_evaluated\": 5}")
        elif action.startswith("raw:"):
            self.reply(conn, 200, action[4:].encode("utf-8"))
        elif action == "crash":
            self.log.write(event="crash", gen=self.gen)
            os._exit(3)
        elif action == "hang":
            time.sleep(3600)
        elif action == "close_listener":
            # close() alone does not stop listening while another thread is
            # blocked in accept(); shutdown() wakes it and refuses new peers.
            try:
                self.listener.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.listener.close()
            return False
        return True

    def handle(self, conn, conn_id):
        pending = b""
        with conn:
            while True:
                try:
                    parsed = read_request(conn, pending)
                except OSError:
                    parsed = None
                if parsed is None:
                    self.log.write(event="disconnect", conn=conn_id, gen=self.gen)
                    return
                path, headers, body, pending = parsed
                try:
                    request_json = json.loads(body.decode("utf-8"))
                except ValueError:
                    request_json = {"invalid": True}
                with self.lock:
                    self.inflight += 1
                    inflight = self.inflight
                action = self.next_action()
                self.log.write(event="request", conn=conn_id, gen=self.gen,
                               path=path, host=headers.get("host"),
                               content_type=headers.get("content-type"),
                               json=request_json, inflight=inflight,
                               raw=body.decode("utf-8", "replace"),
                               action=action)
                try:
                    keep = self.perform(conn, action, request_json)
                except OSError:
                    keep = False
                with self.lock:
                    self.inflight -= 1
                if not keep:
                    self.log.write(event="closed", conn=conn_id, gen=self.gen)
                    return

    def serve(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            with self.lock:
                self.conn_ids += 1
                conn_id = self.conn_ids
            self.log.write(event="accept", conn=conn_id, gen=self.gen)
            threading.Thread(target=self.handle, args=(conn, conn_id),
                             daemon=True).start()


def announce(text):
    sys.stdout.write(text + "\n")
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--plan", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--gen-file")
    args = parser.parse_args()
    with open(args.plan, encoding="utf-8") as handle:
        plan = json.load(handle)
    gen = next_generation(args.gen_file)
    if "generations" in plan:
        gens = plan["generations"]
        plan = gens[min(gen, len(gens) - 1)]
    log = Logger(args.log)
    startup = plan.get("startup", "ok")
    log.write(event="start", gen=gen, startup=startup)
    if startup == "exit":
        sys.exit(3)
    if startup == "hang":
        time.sleep(3600)
        return
    if startup == "bad_port":
        announce("not-a-port")
        time.sleep(3600)
        return
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(64)
    port = listener.getsockname()[1]
    if startup == "dead_port":
        listener.close()
        announce(str(port))
        time.sleep(3600)
        return
    announce(str(port))
    log.write(event="ready", gen=gen, port=port)
    if startup == "exit_after_port":
        os._exit(4)
    backend = Backend(plan, log, gen)
    backend.listener = listener
    backend.serve()
    time.sleep(3600)


if __name__ == "__main__":
    main()
