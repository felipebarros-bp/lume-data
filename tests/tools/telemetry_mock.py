# SPDX-License-Identifier: AGPL-3.0-only
# Loopback stand-in for the analytics endpoint: prints its port, then appends every request it
# receives (method, path, App-Key header, body) as one JSON line to the file given as argv[1].
import http.server, json, socketserver, sys
OUT = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = self.rfile.read(n).decode('utf-8', 'replace')
        with open(OUT, 'a') as f:
            f.write(json.dumps({"path": self.path, "app_key": self.headers.get('App-Key'), "content_type": self.headers.get('Content-Type'), "body": body}) + "\n")
        self.send_response(200); self.send_header('Content-Length', '0'); self.end_headers()
srv = socketserver.TCPServer(('127.0.0.1', 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
