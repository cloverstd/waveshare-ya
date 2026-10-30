const ORIGIN = 'https://ya.cloverstd.com';
const RELEASES = 'https://github.com/cloverstd/waveshare-ya/releases';
const VERSION = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$/;
const HOSTS = new Set(['github.com', 'release-assets.githubusercontent.com', 'objects.githubusercontent.com']);
const encoder = new TextEncoder();
interface Env { ASSETS: Fetcher }
interface Manifest {
  product: 'waveshare-ya'; board: 'esp32-s3-touch-amoled-2.16'; layout: 'ab6-v1';
  version: string; url: string; size: number; sha256: string; notes?: string;
  delta_index_url?: string; delta_index_sha256?: string;
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
  if ((m.delta_index_url === undefined) !== (m.delta_index_sha256 === undefined) ||
      (m.delta_index_url !== undefined && (m.delta_index_url !== ORIGIN + deltaPath(m.version) ||
       typeof m.delta_index_sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(m.delta_index_sha256))))
    throw new Failure(503,'invalid_manifest');
  return m;
}
interface Patch {
  format: 'detools-heatshrink-v1'; base_version: string; base_size: number; base_sha256: string;
  url: string; size: number; sha256: string;
}
interface DeltaIndex {
  schema: number; product: string; board: string; layout: string;
  version: string; size: number; sha256: string; patches: Patch[];
}
function deltaPath(version: string): string {
  return `/firmware/waveshare-ya/${version}/delta-index.json`;
}
function patchName(base: string, version: string): string {
  return `waveshare-ya-${base}-to-${version}.patch`;
}
function strictObject(raw: string): unknown {
  const value: unknown = JSON.parse(raw);
  const stack: Set<string>[] = [];
  const tokens=raw.match(/"(?:\\.|[^"\\])*"|[{}:,\[\]]|[^{}\s:,\[\]]+/g) ?? [];
  for (let i=0;i<tokens.length;i++) {
    if(tokens[i]==='{') stack.push(new Set());
    else if(tokens[i]==='}') stack.pop();
    else if(tokens[i+1]===':') {
      const key=JSON.parse(tokens[i]) as string;
      const keys=stack[stack.length-1];
      if(!keys || keys.has(key) || key.includes('\0')) throw new Failure(503,'invalid_delta_index');
      keys.add(key);
    }
  }
  return value;
}
export function validateDeltaIndex(raw: string, m: Manifest): DeltaIndex {
  if (!raw || encoder.encode(raw).length>=4096) throw new Failure(503,'invalid_delta_index');
  let x: DeltaIndex;
  try { x=strictObject(raw) as DeltaIndex; } catch { throw new Failure(503,'invalid_delta_index'); }
  if (!x || x.schema!==1 || !Array.isArray(x.patches) || x.patches.length>3 ||
      ['product','board','layout','version','size','sha256'].some(k=>
        x[k as keyof DeltaIndex]!==m[k as keyof Manifest])) throw new Failure(503,'invalid_delta_index');
  const bases=new Set<string>();
  for (const p of x.patches) {
    if (!p || typeof p.base_version!=='string' || !VERSION.test(p.base_version) || p.base_version.length>31 ||
        bases.has(p.base_version) || p.format!=='detools-heatshrink-v1' ||
        !Number.isInteger(p.base_size) || p.base_size<512 || p.base_size>6291456 ||
        !Number.isInteger(p.size) || p.size<1 || p.size>=m.size ||
        typeof p.base_sha256!=='string' || !/^[a-f0-9]{64}$/.test(p.base_sha256) ||
        typeof p.sha256!=='string' || !/^[a-f0-9]{64}$/.test(p.sha256) ||
        p.url!==`${ORIGIN}/firmware/waveshare-ya/${m.version}/${patchName(p.base_version,m.version)}`)
      throw new Failure(503,'invalid_delta_index');
    bases.add(p.base_version);
  }
  return x;
}
async function sha256(raw: string): Promise<string> {
  const bytes=new Uint8Array(await crypto.subtle.digest('SHA-256',encoder.encode(raw)));
  return Array.from(bytes,b=>b.toString(16).padStart(2,'0')).join('');
}
async function deltaIndex(m: Manifest, ctx: ExecutionContext): Promise<{raw:string,index:DeltaIndex}> {
  if (!m.delta_index_url) throw new Failure(404,'delta_unavailable');
  const key=new Request(ORIGIN+deltaPath(m.version));
  const cached=await caches.default.match(key);
  const raw=await boundedText(cached ?? await upstream(`${RELEASES}/download/v${m.version}/delta-index.json`));
  if(await sha256(raw)!==m.delta_index_sha256) throw new Failure(503,'invalid_delta_digest');
  const index=validateDeltaIndex(raw,m);
  if(!cached) ctx.waitUntil(caches.default.put(key,new Response(raw,{headers:{'Cache-Control':'public, max-age=31536000'}})));
  return {raw,index};
}
async function deltaDownload(version: string, name: string, ctx: ExecutionContext): Promise<Response> {
  const m=await manifest(version,ctx);
  const {raw,index}=await deltaIndex(m,ctx);
  if (name==='delta-index.json') return new Response(encoder.encode(raw),{headers:{
    'Content-Type':'application/json', 'Content-Length':String(encoder.encode(raw).length), 'Cache-Control':'public, max-age=31536000, immutable, no-transform',
    'Content-Encoding':'identity', 'X-Content-Type-Options':'nosniff'},encodeBody:'manual'});
  const p=index.patches.find(p=>patchName(p.base_version,version)===name);
  if(!p) throw new Failure(404,'delta_unavailable');
  const key=new Request(p.url); const cached=await caches.default.match(key);
  if(cached?.body) return fixed(cached.body,p.size,ctx);
  const upstreamResponse=await upstream(`${RELEASES}/download/v${version}/${name}`);
  if(!upstreamResponse.body || upstreamResponse.headers.get('Content-Length')!==String(p.size)) {
    await upstreamResponse.body?.cancel(); throw new Failure(503,'invalid_delta_length');
  }
  const result=fixed(upstreamResponse.body,p.size,ctx);
  ctx.waitUntil(caches.default.put(key,result.clone()).catch(()=>{console.warn('patch_cache_failed');}));
  return result;
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
    'Content-Type':'application/octet-stream', 'Content-Encoding':'identity', 'Content-Length':String(size),
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
      const delta=path.match(/^\/firmware\/waveshare-ya\/([^/]+)\/(delta-index\.json|waveshare-ya-[0-9.]+-to-[0-9.]+\.patch)$/);
      if(delta && VERSION.test(delta[1]) && delta[1].length<=31) return await deltaDownload(delta[1],delta[2],ctx);
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
