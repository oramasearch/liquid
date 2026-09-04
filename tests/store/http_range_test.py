#!/usr/bin/env python3
import http.server
import pathlib
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import urllib.request


class RangeHandler(http.server.BaseHTTPRequestHandler):
    pack = pathlib.Path()

    def do_GET(self):
        value = self.headers.get("Range", "")
        if not value.startswith("bytes=") or "-" not in value:
            self.send_error(400)
            return
        first, last = (int(part) for part in value[6:].split("-", 1))
        data = self.pack.read_bytes()[first:last + 1]
        self.send_response(206)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Content-Range", f"bytes {first}-{last}/{self.pack.stat().st_size}")
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, _format, *_args):
        pass


def main():
    with tempfile.TemporaryDirectory(prefix="liquid-http-range-") as temporary:
        root = pathlib.Path(temporary)
        subprocess.run([sys.argv[1], str(root)], check=True)
        table = (root / "packs" / "0.table").read_bytes()
        if table[:8] != b"LQTABLE1" or len(table) < 57:
            raise RuntimeError("pack sidecar is malformed")
        offset = struct.unpack_from("<Q", table, 41)[0]
        length = struct.unpack_from("<I", table, 49)[0]
        RangeHandler.pack = root / "packs" / "0.pack"
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), RangeHandler)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            request = urllib.request.Request(
                f"http://127.0.0.1:{server.server_port}/0.pack",
                headers={"Range": f"bytes={offset}-{offset + length - 1}"},
            )
            with urllib.request.urlopen(request) as response:
                fetched = response.read()
                if response.status != 206:
                    raise RuntimeError("server did not return a partial response")
            expected = RangeHandler.pack.read_bytes()[offset:offset + length]
            if fetched != expected:
                raise RuntimeError("HTTP range does not match the indexed pack record")
        finally:
            server.shutdown()
            thread.join()
            server.server_close()


if __name__ == "__main__":
    main()
