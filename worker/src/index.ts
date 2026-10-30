const ORIGIN = 'https://ya.cloverstd.com';
const RELEASES = 'https://github.com/cloverstd/waveshare-ya/releases';
const VERSION = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
const HOSTS = new Set(['github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com']);
const encoder = new TextEncoder();
interface Env { ASSETS: Fetcher }
interface Manifest {
  product: 'waveshare-ya'; board: 'esp32-s3-touch-amoled-2.16'; layout: 'ab6-v1';
  version: string; url: string; size: number; sha256: string; notes?: string;
}
class Failure extends Error {
  constructor(public status: number, public code: string) { super(code); }
}
function firmwarePath(version: string): string {
  return `/firmware/waveshare-ya/${version}/waveshare-ya-${version}.bin`;
}
function json(value: unknown, status = 200): Response {
  return new Response(encoder.encode(JSON.stringify(value)), {status, headers: {
    'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store, no-transform',
    'Content-Encoding': 'identity', 'X-Content-Type-Options': 'nosniff',
  }, encodeBody: 'manual'});
}
export function validateManifest(raw: string, expected?: string): Manifest {
  if (!raw || encoder.encode(raw).byteLength >= 4096) throw new Failure(503, 'invalid_manifest');
  let value: unknown;
  try { value = JSON.parse(raw); } catch { throw new Failure(503, 'invalid_manifest'); }
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Failure(503, 'invalid_manifest');
  const entries = Object.entries(value);
  if (entries.some(([k,v]) => k.includes('\0') || (v !== null && typeof v === 'object') ||
    (typeof v === 'string' && v.includes('\0')))) throw new Failure(503, 'invalid_manifest');
  // Tokenize whole strings so punctuation inside notes cannot be mistaken for keys.
  const tokens = raw.match(/"(?:\\.|[^"\\])*"|[{}:,]|[^{}\s:,]+/g) ?? [];
  const keys = tokens.filter((_,i) => tokens[i+1] === ':').map(s => JSON.parse(s));
  if (keys.length !== new Set(keys).size) throw new Failure(503, 'invalid_manifest');
  const m = value as Manifest;
  if (m.product !== 'waveshare-ya' || m.board !== 'esp32-s3-touch-amoled-2.16' || m.layout !== 'ab6-v1' ||
      typeof m.version !== 'string' || !VERSION.test(m.version) || m.version.length > 31 ||
      (expected !== undefined && m.version !== expected) || m.url !== ORIGIN + firmwarePath(m.version) ||
      !Number.isInteger(m.size) || m.size < 512 || m.size > 6291456 ||
      typeof m.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(m.sha256) ||
      (m.notes !== undefined && (typeof m.notes !== 'string' || encoder.encode(m.notes).length > 255))) {
    throw new Failure(503, 'invalid_manifest');
  }
  return m;
}
async function upstream(url: string): Promise<Response> {
  for (let hop = 0; hop < 5; hop++) {
    const target = new URL(url);
    if (target.protocol !== 'https:' || target.username || target.password || target.port ||
        !HOSTS.has(target.hostname) ||
        (target.hostname === 'github.com' && !target.pathname.startsWith('/cloverstd/waveshare-ya/releases/'))) {
      throw new Failure(503, 'invalid_upstream');
    }
    let response: Response;
    try {
      response = await fetch(target, {redirect:'manual', headers:{'Accept-Encoding':'identity'},
        signal:AbortSignal.timeout(60000)});
    } catch { throw new Failure(503, 'upstream_unavailable'); }
    if ([301,302,303,307,308].includes(response.status)) {
      const location = response.headers.get('Location');
      await response.body?.cancel();
      if (!location) throw new Failure(503, 'invalid_upstream');
      url = new URL(location, url).href;
      continue;
    }
    if (response.status !== 200) {
      await response.body?.cancel();
      throw new Failure(response.status === 404 ? 404 : 503,
        response.status === 404 ? 'not_published' : 'upstream_unavailable');
    }
    const encoding = response.headers.get('Content-Encoding');
    if (encoding && encoding.toLowerCase() !== 'identity') {
      await response.body?.cancel();
      throw new Failure(503, 'encoded_upstream');
    }
    return response;
  }
  throw new Failure(503, 'too_many_redirects');
}
async function boundedText(response: Response): Promise<string> {
  if (!response.body) throw new Failure(503, 'invalid_manifest');
  const reader = response.body.getReader();
  const chunks: Uint8Array[] = [];
  let size = 0;
  try {
    while (true) {
      const {value,done} = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size >= 4096) throw new Failure(503, 'invalid_manifest');
      chunks.push(value);
    }
  } catch (error) {
    await reader.cancel().catch(() => {});
    throw error;
  } finally { reader.releaseLock(); }
  const data = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) { data.set(chunk,offset); offset += chunk.length; }
  return new TextDecoder('utf-8',{fatal:true,ignoreBOM:false}).decode(data);
}
async function manifest(version: string | undefined, ctx: ExecutionContext): Promise<Manifest> {
  const key = new Request(`${ORIGIN}/__cache/manifest/${version ?? 'latest'}`);
  const cached = await caches.default.match(key);
  if (cached) return validateManifest(await cached.text(), version);
  const path = version ? `download/v${version}` : 'latest/download';
  const response = await upstream(`${RELEASES}/${path}/manifest.json`);
  const m = validateManifest(await boundedText(response), version);
  ctx.waitUntil(caches.default.put(key, new Response(JSON.stringify(m), {
    headers:{'Cache-Control':`public, max-age=${version ? 31536000 : 60}`},
  })));
  return m;
}
function fixed(body: ReadableStream, size: number, ctx: ExecutionContext): Response {
  const stream = new FixedLengthStream(size);
  ctx.waitUntil(body.pipeTo(stream.writable).catch(() => {
    // Do not log signed upstream URLs. The aborted output remains a failed download.
    console.warn('firmware_stream_failed');
  }));
  return new Response(stream.readable, {headers:{
    'Content-Type':'application/octet-stream', 'Content-Encoding':'identity',
    'Cache-Control':'public, max-age=31536000, immutable, no-transform',
    'X-Content-Type-Options':'nosniff',
  }, encodeBody:'manual'});
}
async function firmware(version: string, ctx: ExecutionContext): Promise<Response> {
  const m = await manifest(version,ctx);
  const key = new Request(ORIGIN + firmwarePath(version));
  const cached = await caches.default.match(key);
  if (cached?.body) return fixed(cached.body,m.size,ctx);
  const response = await upstream(`${RELEASES}/download/v${version}/waveshare-ya-${version}.bin`);
  if (!response.body || response.headers.get('Content-Length') !== String(m.size)) {
    await response.body?.cancel();
    throw new Failure(503,'invalid_firmware_length');
  }
  const result = fixed(response.body,m.size,ctx);
  ctx.waitUntil(caches.default.put(key,result.clone()).catch(() => { console.warn('firmware_cache_failed'); }));
  return result;
}
export default {
  async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    const path = new URL(request.url).pathname;
    if (!path.startsWith('/api/') && !path.startsWith('/firmware/')) return env.ASSETS.fetch(request);
    if (request.method !== 'GET') return new Response(null,{status:405,headers:{Allow:'GET'}});
    try {
      if (path === '/api/v1/ota/manifest.json') return json(await manifest(undefined,ctx));
      const match = path.match(/^\/firmware\/waveshare-ya\/([^/]+)\/waveshare-ya-([^/]+)\.bin$/);
      if (match && VERSION.test(match[1]) && match[1] === match[2] && match[1].length <= 31) {
        return await firmware(match[1],ctx);
      }
      return json({error:'not_found'},404);
    } catch (error) {
      const failure = error instanceof Failure ? error : new Failure(503,'upstream_unavailable');
      return json({error:failure.code},failure.status);
    }
  },
} satisfies ExportedHandler<Env>;
