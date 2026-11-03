# SPDX-License-Identifier: AGPL-3.0-only
# Loopback stand-in for GitHub's releases API: prints its port, then answers every GET with the
# releases described by argv[1] ("none" -> 404, "error" -> 500, "empty" -> [], "v0.1.0" -> only the
# current version, otherwise that tag plus v8.5.0, v0.1.0 and a newer pre-release, each with notes and installers). Each request (path, User-Agent, Accept) is appended to argv[2].
import http.server, json, socketserver, sys
MODE, OUT = sys.argv[1], sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        with open(OUT, 'a') as f:
            f.write(json.dumps({"path": self.path, "ua": self.headers.get('User-Agent'), "accept": self.headers.get('Accept')}) + "\n")
        if MODE in ("none", "error"):
            self.send_response(404 if MODE == "none" else 500); self.send_header('Content-Length', '0'); self.end_headers(); return
        def rel(tag, notes, pre=False):
            v = tag.lstrip('v'); base = "https://github.com/tessarotto21/lume-data/releases/download/" + tag + "/"
            names = ["Lume-Data-%s-macos-universal.dmg" % v, "lume-data-%s-linux-x86_64.tar.gz" % v,
                     "lume-data-%s-linux-aarch64.tar.gz" % v, "lume-data-%s-windows-x64.zip" % v]
            return {"tag_name": tag, "html_url": "https://github.com/tessarotto21/lume-data/releases/tag/" + tag, "draft": False, "prerelease": pre,
                    "body": notes, "assets": [{"name": n, "browser_download_url": base + n} for n in names]}
        if MODE == "empty": rels = []
        elif MODE == "v0.1.0": rels = [rel("v0.1.0", "First preview.")]
        else:   # newest first by date, like GitHub: a pre-release above, the current version below
            rels = [rel("v10.0.0-beta.1", "## Beta\n- Not for everyone", pre=True),
                    rel(MODE, "## What's new\n\n- **Faster** result grid\n- Fixed the [export](https://example.invalid/x) dialog\n\n![shot](https://example.invalid/s.png)\n<!-- hidden -->"),
                    rel("v8.5.0", "### Fixes\n* Saved `queries` reopen at the right line"),
                    rel("v0.1.0", "First preview.")]
        body = json.dumps(rels).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.send_header('Content-Length', str(len(body))); self.end_headers()
        self.wfile.write(body)
srv = socketserver.TCPServer(('127.0.0.1', 0), H)
print(srv.server_address[1], flush=True)
srv.serve_forever()
