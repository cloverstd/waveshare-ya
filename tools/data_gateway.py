#!/usr/bin/env python3
"""Serve the Chrome companion and YA's shared data exchange (stdlib only)."""
import argparse
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import ssl
import threading
import time
import urllib.parse
import urllib.request
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1] / 'web'
LIMIT = 4096

def no_duplicates(items):
    out = {}
    for k, v in items:
        if k in out:
            raise ValueError('duplicate key')
        out[k] = v
    return out

def decode(raw):
    if not raw or len(raw) > LIMIT:
        raise ValueError('length')
    text = raw.decode('utf-8')
    depth = 0
    quoted = escape = False
    for c in text:
        if quoted:
            if escape: escape = False
            elif c == '\\': escape = True
            elif c == '"': quoted = False
        elif c == '"': quoted = True
        elif c in '{[':
            depth += 1
            if depth > 6: raise ValueError('depth')
        elif c in '}]': depth -= 1
    result = json.loads(text, object_pairs_hook=no_duplicates,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('number')))
    def finite(value):
        if isinstance(value,float) and not math.isfinite(value):raise ValueError('number')
        if isinstance(value,dict):
            for item in value.values():finite(item)
        elif isinstance(value,list):
            for item in value:finite(item)
    finite(result)
    return result

def fetch(url, payload=None, token=''):
    headers = {'User-Agent': 'Waveshare-YA/0.3', 'Content-Type': 'application/json'}
    if token: headers['Authorization'] = 'Bearer ' + token
    data = None if payload is None else json.dumps(payload).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data, headers), timeout=8) as response:
        return decode(response.read(LIMIT + 1))

class Provider:
    def __init__(self):
        self.service = os.environ.get('YA_DATA_SERVICE', 'ya-local')
        self.scope = os.environ.get('YA_DATA_SCOPE', 'default')
        self.token = os.environ.get('YA_DATA_TOKEN', '')
        self.resources = os.environ.get('YA_RESOURCES_FILE', '')
        self.upstream = os.environ.get('YA_UPSTREAM_URL', '')
        if self.upstream and not self.upstream.startswith('https://'):
            raise ValueError('YA_UPSTREAM_URL must use HTTPS')
        self.cache = {}
        self.lock = threading.Lock()

    def exchange(self, req):
        if not isinstance(req, dict) or req.get('proto') != 'ya.data' or req.get('v') != 1:
            raise ValueError('protocol')
        for k in ('id', 'session'):
            uuid.UUID(req[k])
        if req.get('type') != 'request' or req.get('schema') != 1 or not isinstance(req.get('params'), dict):
            raise ValueError('request')
        if req.get('service') != self.service or req.get('scope') != self.scope:
            raise PermissionError('scope')
        out = dict(proto='ya.data', v=1, type='response', id=str(uuid.uuid4()),
                   session=req['session'], reply_to=req['id'], service=self.service, scope=self.scope,
                   resource=req['resource'], schema=1, params=req['params'],
                   updated_at=None, ttl_s=0)
        # An optional authoritative upstream implements command idempotency/status.
        # This bridge never retries commands or pretends to be their execution authority.
        if self.upstream:
            result = fetch(self.upstream, req, os.environ.get('YA_UPSTREAM_TOKEN', ''))
            for k in ('proto','v','session','reply_to','service','scope','resource','schema','params'):
                if result.get(k) != (req['id'] if k == 'reply_to' else req.get(k)):
                    raise ValueError('upstream identity')
            return result
        if req.get('op') not in ('get', 'subscribe'):
            out.update(status='failed', error={'code':'unsupported','retryable':False})
            return out
        try:
            resource = req['resource']
            if resource == 'system.health':
                value = dict(data={'online':True, 'commands':False}, updated_at=int(time.time()), ttl_s=30)
            elif resource == 'weather.current':
                p = req['params']
                lat, lon = float(p['latitude']), float(p['longitude'])
                if not math.isfinite(lat) or not math.isfinite(lon) or abs(lat)>90 or abs(lon)>180:
                    raise ValueError('coordinates')
                key = (lat, lon)
                with self.lock:
                    cached = self.cache.get(key)
                if cached and time.monotonic() - cached[0] < 60:
                    value = cached[1]
                else:
                    q = urllib.parse.urlencode(dict(latitude=lat, longitude=lon,
                        current='temperature_2m,relative_humidity_2m,weather_code',
                        daily='temperature_2m_max,temperature_2m_min,weather_code',
                        forecast_days=3, timezone='UTC', timeformat='unixtime'))
                    j = fetch('https://api.open-meteo.com/v1/forecast?' + q)
                    c, d = j['current'], j['daily']
                    value = dict(data=dict(temperature_c=c['temperature_2m'],humidity_pct=c['relative_humidity_2m'],
                        condition=str(c['weather_code']),timezone='UTC',source='Open-Meteo',forecast=[
                        dict(time=t,min_c=d['temperature_2m_min'][i],max_c=d['temperature_2m_max'][i],code=d['weather_code'][i])
                        for i,t in enumerate(d['time'])]),updated_at=c['time'],ttl_s=1800)
                    with self.lock:
                        if len(self.cache)>=32:self.cache.clear()
                        self.cache[key] = (time.monotonic(), value)
            elif resource == 'market.quote' and os.environ.get('TWELVEDATA_API_KEY'):
                p = req['params']
                symbol, market = p['symbol'], p['market']
                if not isinstance(symbol,str) or not isinstance(market,str) or len(symbol)>40 or len(market)>40:
                    raise ValueError('instrument')
                cache_key = ('quote',market,symbol)
                with self.lock:
                    cached = self.cache.get(cache_key)
                if cached and time.monotonic()-cached[0] < 60:
                    value = cached[1]
                else:
                    q = urllib.parse.urlencode(dict(symbol=symbol,mic_code=market,
                        apikey=os.environ['TWELVEDATA_API_KEY']))
                    j = fetch('https://api.twelvedata.com/quote?' + q)
                    if j.get('status')=='error' or not j.get('close') or j.get('mic_code') != market:
                        raise ValueError('quote unavailable or market mismatch')
                    value = dict(data=dict(price=str(j['close']),currency=j.get('currency'),
                        change_pct=str(j['percent_change']) if j.get('percent_change') is not None else None,
                        market_state='open' if j.get('is_market_open') else 'closed',
                        delay_s=None,source='Twelve Data'),updated_at=int(j['timestamp']),ttl_s=60)
                    with self.lock:
                        if len(self.cache)>=32:self.cache.clear()
                        self.cache[cache_key] = (time.monotonic(),value)
            else:
                # User-maintained snapshots use the same identity/params and retain source timestamps.
                values = decode(Path(self.resources).read_bytes()) if self.resources else {}
                key = json.dumps([resource, req['params']],sort_keys=True,separators=(',',':'),ensure_ascii=False)
                value = values.get(key)
                if value is None:
                    out.update(status='failed',error={'code':'missing','retryable':False})
                    return out
                if not isinstance(value.get('data'),dict) or not isinstance(value.get('ttl_s'),int):
                    raise ValueError('snapshot')
            out.update(value, status='succeeded')
            out['revision'] = hashlib.sha256(json.dumps(value,sort_keys=True).encode()).hexdigest()[:16]
        except (OSError, KeyError, ValueError, TypeError):
            out.update(status='failed',error={'code':'offline','retryable':True})
        return out

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_): pass  # Never log tokens or request payloads.
    def respond(self, status, data, mime='application/json'):
        self.send_response(status)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(data)))
        self.send_header('Cache-Control','no-store')
        self.send_header('X-Content-Type-Options','nosniff')
        self.end_headers()
        self.wfile.write(data)
    def do_GET(self):
        files = {'/':'index.html','/index.html':'index.html','/app.mjs':'app.mjs','/protocol.mjs':'protocol.mjs','/text-render.mjs':'text-render.mjs','/gallery.mjs':'gallery.mjs','/connection.mjs':'connection.mjs','/style.css':'style.css'}
        name = files.get(urllib.parse.urlsplit(self.path).path)
        if not name: return self.respond(404,b'{}')
        mime = 'text/html; charset=utf-8' if name.endswith('.html') else 'text/css' if name.endswith('.css') else 'text/javascript'
        self.respond(200,(ROOT/name).read_bytes(),mime)
    def do_POST(self):
        if self.path != '/api/v1/data/exchange':return self.respond(404,b'{}')
        provider = self.server.provider
        supplied = self.headers.get('Authorization','')
        if provider.token and not hmac.compare_digest(supplied,'Bearer '+provider.token):
            return self.respond(401,b'{"error":"unauthorized"}')
        try:
            n = int(self.headers.get('Content-Length','0'))
            if n<=0 or n>LIMIT:raise ValueError('length')
            self.connection.settimeout(10)
            req = decode(self.rfile.read(n))
            out = provider.exchange(req)
            raw = json.dumps(out,ensure_ascii=False,allow_nan=False,separators=(',',':')).encode()
            if len(raw)>LIMIT:raise ValueError('response length')
            self.respond(200,raw)
        except PermissionError:self.respond(403,b'{"error":"unauthorized"}')
        except (ValueError,KeyError,TypeError):self.respond(400,b'{"error":"protocol_error"}')
        except Exception:self.respond(502,b'{"error":"upstream_failed"}')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host',default='127.0.0.1')
    parser.add_argument('--port',type=int,default=8765)
    parser.add_argument('--cert');parser.add_argument('--key')
    args=parser.parse_args()
    provider=Provider()
    if args.host not in ('127.0.0.1','localhost','::1') and not provider.token:
        parser.error('A non-loopback listener requires YA_DATA_TOKEN')
    server=ThreadingHTTPServer((args.host,args.port),Handler);server.provider=provider
    if args.cert or args.key:
        if not args.cert or not args.key:parser.error('--cert and --key must be provided together')
        ctx=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);ctx.load_cert_chain(args.cert,args.key)
        server.socket=ctx.wrap_socket(server.socket,server_side=True)
    print(f'YA gateway: {"https" if args.cert else "http"}://{args.host}:{args.port}',flush=True)
    server.serve_forever()

if __name__=='__main__':main()
