#!/usr/bin/env python3
"""Stop the development daemon through its private single-byte control."""

import os
import socket
import sys


def main() -> int:
    path = sys.argv[1] if len(sys.argv) == 2 else f"/tmp/frame-{os.getuid()}.sock"
    peer = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        peer.connect(path)
    except FileNotFoundError:
        return 0
    try:
        peer.sendall(b"S")
        while peer.recv(4096):
            pass
    finally:
        peer.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
