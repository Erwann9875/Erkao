import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body):
        body_bytes = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body_bytes)))
        self.end_headers()
        self.wfile.write(body_bytes)

    def _handle_echo(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length > 0:
            body = self.rfile.read(length).decode("utf-8")
        else:
            body = ""
        self._send(200, "echo: " + body)

    def do_GET(self):
        if self.path == "/hello":
            self._send(200, "hello")
        else:
            self._send(404, "not found")

    def do_POST(self):
        if self.path == "/echo":
            self._handle_echo()
        else:
            self._send(404, "not found")

    def do_PUT(self):
        if self.path == "/echo":
            self._handle_echo()
        else:
            self._send(404, "not found")

    def log_message(self, format, *args):
        return


class TestServer(HTTPServer):
    allow_reuse_address = True


def main():
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    else:
        port = int(os.environ.get("ERKAO_HTTP_TEST_PORT", "0"))
    if port <= 0:
        raise SystemExit("Missing ERKAO_HTTP_TEST_PORT")

    server = TestServer(("127.0.0.1", port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
