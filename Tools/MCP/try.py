#!/usr/bin/env python
"""Calls one tool on the server over stdio, for checking it by hand."""

import json
import subprocess
import sys
import threading

name = sys.argv[1]
arguments = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}

proc = subprocess.Popen(
    [sys.executable, "server.py"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    text=True,
    bufsize=1,
)


def send(message):
    proc.stdin.write(json.dumps(message) + "\n")
    proc.stdin.flush()


send({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
        "protocolVersion": "2024-11-05",
        "capabilities": {},
        "clientInfo": {"name": "try", "version": "1"},
    },
})

proc.stdout.readline()

send({"jsonrpc": "2.0", "method": "notifications/initialized"})
send({"jsonrpc": "2.0", "id": 2, "method": "tools/call", "params": {"name": name, "arguments": arguments}})

result = {}


def read():
    global result

    for line in proc.stdout:
        line = line.strip()

        if not line.startswith("{"):
            continue

        message = json.loads(line)

        if message.get("id") == 2:
            result = message
            return


reader = threading.Thread(target=read, daemon=True)
reader.start()
reader.join(timeout=900)

proc.kill()

if not result:
    print("(no answer in time)")
    sys.exit(1)

body = result.get("result") or result.get("error") or {}
content = body.get("content") if isinstance(body, dict) else None

if content:
    for part in content:
        print(part.get("text", ""))
else:
    print(json.dumps(body, indent=1)[:2000])
