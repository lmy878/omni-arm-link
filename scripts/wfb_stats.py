#!/usr/bin/env python3
"""Snapshot one WFB-NG rx stream from the JSON stats API.

WFB-NG's api_port serves newline-delimited JSON (one message per stats
interval).  This helper connects, waits for the requested rx service and
writes a small key=value file that scripts/link_report.sh can diff.
"""

import argparse
import json
import socket
import sys
import time

COUNTERS = (
    "all",
    "all_bytes",
    "data",
    "uniq",
    "fec_rec",
    "lost",
    "bad",
    "dec_err",
    "out",
    "out_bytes",
    "frags",
    "frags_lost",
)


def total(value):
    """WFB-NG sends (window, total) pairs; tolerate plain integers."""
    if isinstance(value, list):
        return int(value[-1]) if value else 0
    if isinstance(value, (int, float)):
        return int(value)
    return 0


def fetch(host, port, service, timeout):
    deadline = time.monotonic() + timeout
    with socket.create_connection((host, port), timeout=timeout) as sock:
        buffer = b""
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            sock.settimeout(remaining)
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                return None
            if not chunk:
                return None
            buffer += chunk
            while b"\n" in buffer:
                line, buffer = buffer.split(b"\n", 1)
                if not line.strip():
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if msg.get("type") != "rx":
                    continue
                ident = str(msg.get("id", ""))
                if service and service not in (ident, ident.split()[0]):
                    continue
                return msg


def main():
    parser = argparse.ArgumentParser(description="Snapshot WFB-NG rx stats")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--service", default="", help="rx service name, e.g. test_up")
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--out", default="", help="write to FILE instead of stdout")
    args = parser.parse_args()

    try:
        msg = fetch(args.host, args.port, args.service, args.timeout)
    except OSError as exc:
        print("wfb_stats: connect to %s:%d failed: %s" % (args.host, args.port, exc),
              file=sys.stderr)
        return 2

    if msg is None:
        print("wfb_stats: no rx stats for service %r on %s:%d"
              % (args.service or "*", args.host, args.port), file=sys.stderr)
        return 3

    packets = msg.get("packets") or {}
    session = msg.get("session") or {}

    lines = [
        "timestamp=%.3f" % float(msg.get("timestamp", 0)),
        "id=%s" % msg.get("id", ""),
        "fec_k=%s" % session.get("fec_k", ""),
        "fec_n=%s" % session.get("fec_n", ""),
    ]
    for key in COUNTERS:
        lines.append("%s=%d" % (key, total(packets.get(key))))

    text = "\n".join(lines) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as handle:
            handle.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
