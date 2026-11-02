# SPDX-License-Identifier: AGPL-3.0-only
# Loopback stand-in for OpenAI (tests only):
#   /v1/responses   Responses API. An API key (sk-oa-test) gets one JSON answer; a ChatGPT plan token
#                   (issued below) must send stream:true and store:false, none of the fields the plan
#                   refuses, and gets SSE events.
#   /v1/models      data[] for API keys, models[] {slug, visibility} for plan tokens
#   /api/accounts/authorize          checks the OAuth request, 302 to the loopback redirect with code/state/client_id
#   /api/accounts/oauth/token        authorization_code (PKCE S256 checked) and refresh_token (rotation;
#                                    an old refresh token -> refresh_token_reused)
#   /ctl?expires=N|usage=1|revoke=1   test controls; /stats: counters as JSON
import base64, hashlib, json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs, urlencode

API_KEY = 'sk-oa-test'
CLIENT = 'oaiapp_lume_test'
FORBIDDEN = ('max_output_tokens', 'temperature', 'top_p', 'user', 'metadata', 'previous_response_id', 'truncation')
lock = threading.Lock()
st = {'codes': {}, 'access': set(), 'refresh_live': set(), 'refresh_used': set(), 'n': 0, 'expires': 3600, 'usage': False,
      'stats': {'authorize': 0, 'code': 0, 'refresh': 0, 'reused': 0, 'responses_key': 0, 'responses_plan': 0, 'rejected': 0, 'models': 0}}

def b64u(b): return base64.urlsafe_b64encode(b).rstrip(b'=').decode()
def jwt(claims): return b64u(b'{"alg":"none"}') + '.' + b64u(json.dumps(claims).encode()) + '.sig'

class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *a): pass
    def send(self, code, obj, ctype='application/json', extra=None):
        b = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code); self.send_header('content-type', ctype); self.send_header('content-length', str(len(b)))
        for k, v in (extra or {}).items(): self.send_header(k, v)
        self.end_headers(); self.wfile.write(b)
    def bearer(self):
        h = self.headers.get('authorization', '')
        return h[7:] if h.startswith('Bearer ') else ''
    def do_GET(self):
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == '/ctl':
            with lock:
                if 'expires' in q: st['expires'] = int(q['expires'])
                if 'usage' in q: st['usage'] = q['usage'] == '1'
                if 'revoke' in q: st['refresh_used'] |= st['refresh_live']; st['refresh_live'] = set(); st['access'] = set()
            return self.send(200, {'ok': True})
        if u.path == '/stats':
            return self.send(200, st['stats'])
        if u.path == '/api/accounts/authorize':
            st['stats']['authorize'] += 1
            need = ('client_id', 'response_type', 'redirect_uri', 'scope', 'resource', 'state', 'nonce', 'code_challenge_method', 'code_challenge', 'ext_agent_host_id')
            bad = [k for k in need if k not in q]
            if bad or q['response_type'] != 'code' or q['code_challenge_method'] != 'S256' or not q['redirect_uri'].startswith('http://127.0.0.1:') \
               or 'chatgpt.tokens.use.direct' not in q['scope'].split() or 'offline_access' not in q['scope'].split() \
               or q['client_id'] not in ('dynamic_agent_client', CLIENT) or not q['ext_agent_host_id'].startswith('urn:uuid:'):
                return self.send(400, {'error': 'invalid_request', 'missing': bad})
            with lock:
                st['n'] += 1; code = 'code-%d' % st['n']
                st['codes'][code] = {'challenge': q['code_challenge'], 'redirect': q['redirect_uri']}
            loc = q['redirect_uri'] + '?' + urlencode({'code': code, 'state': q['state'], 'client_id': CLIENT})
            self.send_response(302); self.send_header('location', loc); self.send_header('content-length', '0'); self.end_headers(); return
        if u.path == '/v1/models':
            st['stats']['models'] += 1
            t = self.bearer()
            if t == API_KEY: return self.send(200, {'object': 'list', 'data': [{'id': 'gpt-5.5'}, {'id': 'text-embedding-3-small'}, {'id': 'gpt-5.5-mini'}, {'id': 'gpt-realtime'}]})
            if t in st['access']: return self.send(200, {'models': [{'slug': 'gpt-5.5', 'display_name': 'GPT-5.5', 'visibility': 'list'}, {'slug': 'gpt-internal', 'visibility': 'hide'}, {'slug': 'gpt-5.5-mini', 'visibility': 'list'}]})
            return self.send(401, {'error': {'code': 'invalid_api_key', 'message': 'bad token'}})
        self.send(404, {'error': 'not found'})
    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get('content-length', 0)); raw = self.rfile.read(n)
        if u.path == '/api/accounts/oauth/token':
            f = {k: v[0] for k, v in parse_qs(raw.decode()).items()}
            if f.get('resource') != 'https://api.openai.com/v1': return self.send(400, {'error': 'invalid_target'})
            with lock:
                if f.get('grant_type') == 'authorization_code':
                    c = st['codes'].pop(f.get('code', ''), None)
                    if not c or f.get('client_id') != CLIENT or f.get('redirect_uri') != c['redirect'] \
                       or b64u(hashlib.sha256(f.get('code_verifier', '').encode()).digest()) != c['challenge']:
                        return self.send(400, {'error': 'invalid_grant', 'error_description': 'code / PKCE / client mismatch'})
                    st['stats']['code'] += 1
                elif f.get('grant_type') == 'refresh_token':
                    r = f.get('refresh_token', '')
                    if r in st['refresh_used']: st['stats']['reused'] += 1; return self.send(400, {'error': 'refresh_token_reused'})
                    if r not in st['refresh_live'] or f.get('client_id') != CLIENT: return self.send(400, {'error': 'invalid_grant'})
                    st['refresh_live'].discard(r); st['refresh_used'].add(r); st['stats']['refresh'] += 1
                else:
                    return self.send(400, {'error': 'unsupported_grant_type'})
                st['n'] += 1; a = 'tok-access-%d' % st['n']; rt = 'tok-refresh-%d' % st['n']
                st['access'].add(a); st['refresh_live'].add(rt)
                return self.send(200, {'access_token': a, 'refresh_token': rt, 'id_token': jwt({'email': 'isa@example.com', 'sub': 'u1'}), 'token_type': 'Bearer',
                                       'expires_in': st['expires'], 'scope': 'openid profile email offline_access resource.invoke chatgpt.tokens.use.direct'})
        if u.path == '/v1/responses':
            body = json.loads(raw or b'{}'); t = self.bearer()
            system = body.get('instructions', '')
            text = 'OK' if 'connection test' in system else json.dumps({'type': 'cannot_answer', 'reason': 'no metric fits'}) if 'Lume Report Copilot' in system else 'EVIDENCE\n#1 scans orders'
            if t == API_KEY:
                st['stats']['responses_key'] += 1
                if body.get('stream') or body.get('store') is not False or not isinstance(body.get('input'), list):
                    st['stats']['rejected'] += 1; return self.send(400, {'error': {'code': 'bad_request', 'message': 'stream / store / input'}})
                return self.send(200, {'id': 'resp_1', 'object': 'response', 'status': 'completed', 'model': body.get('model'),
                                       'output': [{'type': 'reasoning', 'summary': []}, {'type': 'message', 'role': 'assistant', 'content': [{'type': 'output_text', 'text': text}]}],
                                       'usage': {'input_tokens': 12, 'output_tokens': 3}})
            if t in st['access']:
                st['stats']['responses_plan'] += 1
                bad = [k for k in FORBIDDEN if k in body]
                if body.get('stream') is not True or body.get('store') is not False or bad or any(i.get('role') == 'system' for i in body.get('input', [])):
                    st['stats']['rejected'] += 1; return self.send(400, {'detail': 'Unsupported parameter(s): %s' % ','.join(bad or ['stream/store/system'])})
                ev = [{'type': 'response.created', 'response': {'id': 'resp_2'}}]
                if st['usage']:
                    ev.append({'type': 'response.failed', 'response': {'id': 'resp_2', 'error': {'code': 'subscription_sharing_usage_limit_exceeded', 'message': 'limit'}}})
                else:
                    for i in range(0, len(text), 5): ev.append({'type': 'response.output_text.delta', 'delta': text[i:i + 5]})
                    ev.append({'type': 'response.completed', 'response': {'id': 'resp_2', 'model': body.get('model'), 'usage': {'input_tokens': 12, 'output_tokens': 3}}})
                b = ''.join('event: %s\ndata: %s\n\n' % (e['type'], json.dumps(e)) for e in ev).encode()
                return self.send(200, b, 'text/event-stream')
            return self.send(401, {'error': {'code': 'invalid_api_key', 'message': 'Incorrect API key provided'}})
        self.send(404, {'error': 'not found'})

s = ThreadingHTTPServer(('127.0.0.1', 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
