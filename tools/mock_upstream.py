#!/usr/bin/env python3
"""Small deterministic HTTP/1.1 upstream for PulseGate proxy experiments.

It deliberately uses only Python's standard library.  It is a local learning
tool, not a production HTTP server: it reads one request per connection and
offers failure modes that are awkward to reproduce with curl alone.
"""

from __future__ import annotations

import argparse
import asyncio
from itertools import count


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--name", default="mock-upstream")
    parser.add_argument("--status", type=int, default=200)
    parser.add_argument("--body", default="hello from mock upstream\n")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--chunked", action="store_true")
    parser.add_argument("--close-after-header", action="store_true")
    parser.add_argument("--split-header", action="store_true")
    parser.add_argument("--stall-body", action="store_true")
    parser.add_argument(
        "--keep-alive",
        action="store_true",
        help="serve multiple complete requests on one TCP connection",
    )
    return parser.parse_args()


def request_id(headers: str) -> str:
    for line in headers.split("\r\n"):
        if line.lower().startswith("x-request-id:"):
            return line.split(":", 1)[1].strip()
    return "-"


async def main() -> None:
    args = arguments()
    sequence = count(1)

    async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        connection_id = next(sequence)
        try:
            while True:
                raw = await reader.readuntil(b"\r\n\r\n")
                headers = raw.decode("iso-8859-1")
                for line in headers.split("\r\n"):
                    if line.lower().startswith("content-length:"):
                        await reader.readexactly(int(line.split(":", 1)[1].strip()))
                        break
                print(
                    f"{args.name} connection={connection_id} request_id={request_id(headers)}",
                    flush=True,
                )
                if args.delay_ms:
                    await asyncio.sleep(args.delay_ms / 1000)
                body = args.body.encode()
                reason = "OK" if args.status == 200 else "Mock Status"
                connection = "keep-alive" if args.keep_alive else "close"
                if args.chunked:
                    header = (
                        f"HTTP/1.1 {args.status} {reason}\r\n"
                        "Content-Type: text/plain\r\n"
                        f"Connection: {connection}\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"
                    ).encode()
                else:
                    header = (
                        f"HTTP/1.1 {args.status} {reason}\r\n"
                        "Content-Type: text/plain\r\n"
                        f"Connection: {connection}\r\n"
                        f"Content-Length: {len(body)}\r\n\r\n"
                    ).encode()
                if args.split_header:
                    midpoint = len(header) // 2
                    writer.write(header[:midpoint])
                    await writer.drain()
                    await asyncio.sleep(0.01)
                    writer.write(header[midpoint:])
                else:
                    writer.write(header)
                await writer.drain()
                if args.close_after_header:
                    return
                if args.stall_body:
                    await asyncio.Future()
                if args.chunked:
                    writer.write(f"{len(body):X}\r\n".encode() + body + b"\r\n0\r\n\r\n")
                else:
                    writer.write(body)
                await writer.drain()
                if not args.keep_alive:
                    return
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()
            await writer.wait_closed()

    server = await asyncio.start_server(handle, args.host, args.port)
    print(f"{args.name} listening on {args.host}:{args.port}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
