"""Connectivity and latency test between host A and client B.

Uses only the standard library, so it works before requirements are installed.

On A (server):  python -m tools.ping_test --server
On B (client):  python -m tools.ping_test --host <A's IP>
"""

import argparse
import json
import socket
import statistics
import struct
import time
from pathlib import Path

CONFIG_PATH = Path(__file__).resolve().parent.parent / "config.json"
HEADER = struct.Struct("!I")


def load_config() -> dict:
    with open(CONFIG_PATH, encoding="utf-8") as f:
        return json.load(f)


def send_msg(sock: socket.socket, msg: dict) -> None:
    data = json.dumps(msg).encode("utf-8")
    sock.sendall(HEADER.pack(len(data)) + data)


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf.extend(chunk)
    return bytes(buf)


def recv_msg(sock: socket.socket) -> dict:
    (length,) = HEADER.unpack(recv_exact(sock, HEADER.size))
    return json.loads(recv_exact(sock, length))


def run_server(port: int) -> None:
    with socket.create_server(("0.0.0.0", port)) as srv:
        print(f"Listening on port {port}... (Ctrl+C to stop)")
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            print(f"Client connected: {addr[0]}")
            with conn:
                try:
                    while True:
                        msg = recv_msg(conn)
                        if msg.get("type") == "ping":
                            send_msg(conn, {"type": "pong", "t": msg["t"]})
                except ConnectionError:
                    print("Client disconnected")


def run_client(host: str, port: int, count: int) -> None:
    print(f"Connecting to {host}:{port}...")
    with socket.create_connection((host, port), timeout=5) as sock:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        rtts = []
        for _ in range(count):
            send_msg(sock, {"type": "ping", "t": time.perf_counter()})
            reply = recv_msg(sock)
            rtts.append((time.perf_counter() - reply["t"]) * 1000)
            time.sleep(0.05)
    print(f"{count} pings: min {min(rtts):.2f} ms, "
          f"avg {statistics.mean(rtts):.2f} ms, max {max(rtts):.2f} ms")


def main() -> None:
    cfg = load_config()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", action="store_true", help="run as server (on A)")
    parser.add_argument("--host", default=cfg["host_ip"], help="A's IP address")
    parser.add_argument("--port", type=int, default=cfg["control_port"])
    parser.add_argument("--count", type=int, default=50)
    args = parser.parse_args()

    if args.server:
        run_server(args.port)
    else:
        run_client(args.host, args.port, args.count)


if __name__ == "__main__":
    main()
