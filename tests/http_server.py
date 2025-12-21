import json
import os
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

print(f"DEBUG: Starting http_server.py with args: {sys.argv}", file=sys.stderr)
sys.stderr.flush()
print(f"DEBUG: Python executable: {sys.executable}", file=sys.stderr)
sys.stderr.flush()


class Handler(BaseHTTPRequestHandler):
    def _send(self, code, body, content_type="text/plain; charset=utf-8"):
        body_bytes = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", content_type)
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
        elif self.path == "/json":
            payload = json.dumps({"message": "hello", "count": 2})
            self._send(200, payload, "application/json; charset=utf-8")
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

    print(f"DEBUG: Binding to 127.0.0.1:{port}...", file=sys.stderr)
    sys.stderr.flush()
    try:
        server = TestServer(("127.0.0.1", port), Handler)
        print("DEBUG: Bind successful. Serving forever...", file=sys.stderr)
        sys.stderr.flush()
        server.serve_forever()
    except Exception as e:
        print(f"DEBUG: Exception in main: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc(file=sys.stderr)
        sys.stderr.flush()
        raise


if __name__ == "__main__":
    main()
