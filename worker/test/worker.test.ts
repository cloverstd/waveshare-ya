import {beforeEach, afterEach, expect, test, vi} from 'vitest';
import {createExecutionContext, waitOnExecutionContext} from 'cloudflare:test';
import worker, {validateManifest} from '../src/index';

const host = 'https://ya.cloverstd.com';
const path = '/firmware/waveshare-ya/0.5.1/waveshare-ya-0.5.1.bin';
const fixture = {
  product:'waveshare-ya', board:'esp32-s3-touch-amoled-2.16', layout:'ab6-v1',
  version:'0.5.1', url:host+path, size:512, sha256:'a'.repeat(64), notes:'测试更新',
};
const asset = {fetch:vi.fn(async () => new Response('asset'))} as unknown as Fetcher;
const contexts: ExecutionContext[] = [];
beforeEach(async () => {
  for (const key of ['/__cache/manifest/latest','/__cache/manifest/0.5.1',path]) {
    await caches.default.delete(host+key);
  }
});
afterEach(async () => {
  for (const ctx of contexts.splice(0)) await waitOnExecutionContext(ctx);
  vi.unstubAllGlobals();
});
async function request(pathname = '/api/v1/ota/manifest.json', method = 'GET') {
  const ctx = createExecutionContext(); contexts.push(ctx);
  return worker.fetch(new Request(host+pathname,{method}),{ASSETS:asset},ctx);
}
function mock(...responses: (Response | Error)[]) {
  const fn = vi.fn(async () => {
    const next = responses.shift();
    if (next instanceof Error) throw next;
    if (!next) throw new Error('Unexpected upstream request');
    return next;
  });
  vi.stubGlobal('fetch',fn); return fn;
}
function manifest(m = fixture) { return new Response(JSON.stringify(m)); }

test('latest manifest uses current device contract and a short internal cache', async () => {
  const fetch = mock(manifest());
  const response = await request();
  expect(response.status).toBe(200);
  expect(response.headers.get('Content-Type')).toContain('application/json');
  expect(response.headers.get('Cache-Control')).toContain('no-store');
  expect(await response.json()).toEqual(fixture);
  await waitOnExecutionContext(contexts[0]);
  expect(await (await request()).json()).toEqual(fixture);
  expect(fetch).toHaveBeenCalledTimes(1);
});
test.each([404,403,429,500])('upstream %s becomes the documented error', async status => {
  mock(new Response('error',{status}));
  const r = await request(); expect(r.status).toBe(status === 404 ? 404 : 503);
  expect(await r.json()).toHaveProperty('error');
});
test('network timeout fails without publishing an update', async () => {
  mock(new Error('timeout')); expect((await request()).status).toBe(503);
});
test('redirects stay inside the fixed GitHub hosts', async () => {
  mock(new Response(null,{status:302,headers:{Location:'https://example.com/firmware'}}));
  expect((await request()).status).toBe(503);
});
test('allowed attachment redirect is followed internally', async () => {
  mock(new Response(null,{status:302,headers:{Location:'https://release-assets.githubusercontent.com/asset'}}),manifest());
  const r = await request(); expect(r.status).toBe(200); expect(r.headers.has('Location')).toBe(false);
  expect(await r.json()).toEqual(fixture);
});
test('redirect loops are bounded', async () => {
  const responses = Array.from({length:5},() => new Response(null,{status:302,
    headers:{Location:'https://github.com/cloverstd/waveshare-ya/releases/latest/download/manifest.json'}}));
  const fetch = mock(...responses); expect((await request()).status).toBe(503); expect(fetch).toHaveBeenCalledTimes(5);
});
test.each([
  {...fixture,board:'other-board'}, {...fixture,layout:'other'}, {...fixture,size:512.5},
  {...fixture,version:'0.5.2'}, {...fixture,url:'https://example.com/app.bin'},
  {...fixture,sha256:'A'.repeat(64)}, {...fixture,notes:'字'.repeat(86)},
  {...fixture,notes:'a\0b'}, {...fixture,size:6291457}, {...fixture,extra:{nested:true}},
])('rejects incompatible or unsafe manifest %#', m => {
  expect(() => validateManifest(JSON.stringify(m))).toThrow();
});
test('duplicate keys and oversized manifests are rejected', () => {
  expect(() => validateManifest(JSON.stringify(fixture).replace('"size":512','"size":512,"size":512'))).toThrow();
  expect(() => validateManifest(' '.repeat(4096))).toThrow();
});
test('versioned downloads require matching manifest identity', async () => {
  mock(manifest({...fixture,version:'0.5.2',url:host+'/firmware/waveshare-ya/0.5.2/waveshare-ya-0.5.2.bin'}));
  expect((await request(path)).status).toBe(503);
});
test.each(['/api/unknown','/firmware/waveshare-ya/0.5.1/waveshare-ya-0.5.2.bin',
  '/firmware/waveshare-ya/%2e%2e/waveshare-ya-%2e%2e.bin','/firmware/arbitrary.bin'])('rejects unknown path %s',async path => {
  const fetch = mock(); expect((await request(path)).status).toBe(404); expect(fetch).not.toHaveBeenCalled();
});
test('API methods are read-only', async () => { expect((await request(undefined,'POST')).status).toBe(405); });
test('firmware stays byte-identical across cold and warm cache', async () => {
  const data = new Uint8Array(512).fill(0xe9);
  const fetch = mock(manifest(),new Response(data,{headers:{'Content-Length':'512'}}));
  const r = await request(path); expect(r.status).toBe(200);
  expect(r.headers.get('Content-Type')).toBe('application/octet-stream');
  expect(r.headers.get('Content-Encoding')).toBe('identity');
  expect(new Uint8Array(await r.arrayBuffer())).toEqual(data);
  await waitOnExecutionContext(contexts[0]);
  expect(new Uint8Array(await (await request(path)).arrayBuffer())).toEqual(data);
  expect(fetch).toHaveBeenCalledTimes(2);
});
test.each(['511','513',null])('rejects missing or mismatched upstream length %s',async length => {
  mock(manifest(),new Response(new Uint8Array(512),{headers:length ? {'Content-Length':length} : {}}));
  expect((await request(path)).status).toBe(503);
});
test('compressed upstream bytes are rejected',async () => {
  mock(new Response('{}',{headers:{'Content-Encoding':'gzip'}})); expect((await request()).status).toBe(503);
});
test.each([511,513])('aborts a stream whose actual length is %s',async size => {
  const body = new ReadableStream({start(c){c.enqueue(new Uint8Array(size));c.close();}});
  mock(manifest(),new Response(body,{headers:{'Content-Length':'512'}}));
  const r = await request(path);
  await expect(r.arrayBuffer()).rejects.toThrow();
});
test('static paths are delegated to the asset binding',async () => {
  expect(await (await request('/companion/')).text()).toBe('asset');
});

const deltaPath='/firmware/waveshare-ya/0.5.1/delta-index.json';
const patchPath='/firmware/waveshare-ya/0.5.1/waveshare-ya-0.5.0-to-0.5.1.patch';
async function deltaFixture() {
  const index={schema:1,product:fixture.product,board:fixture.board,layout:fixture.layout,
    version:fixture.version,size:fixture.size,sha256:fixture.sha256,patches:[{
      format:'detools-heatshrink-v1',base_version:'0.5.0',base_size:512,base_sha256:'b'.repeat(64),
      url:host+patchPath,size:100,sha256:'c'.repeat(64)}]};
  const raw=JSON.stringify(index)+'\n';
  const digest=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(raw))),
    b=>b.toString(16).padStart(2,'0')).join('');
  const m={...fixture,delta_index_url:host+deltaPath,delta_index_sha256:digest};
  return {index,raw,m};
}
beforeEach(async()=>{
  for(const p of [deltaPath,patchPath]) await caches.default.delete(host+p);
});
test('legacy manifest stays flat with optional delta pointers',async()=>{
  const {m}=await deltaFixture();
  mock(new Response(JSON.stringify(m)));
  expect(await (await request()).json()).toEqual(m);
});
test('delta index retains exact hashed bytes',async()=>{
  const {m,raw}=await deltaFixture();
  mock(new Response(JSON.stringify(m)),new Response(raw));
  const response=await request(deltaPath);
  expect(response.status).toBe(200);
  expect(response.headers.get('Content-Length')).toBe(String(new TextEncoder().encode(raw).length));
  expect(await response.text()).toBe(raw);
});
test('patch cold and warm downloads retain length and bytes',async()=>{
  const {m,raw}=await deltaFixture(); const bytes=new Uint8Array(100).fill(0x55);
  const fetch=mock(new Response(JSON.stringify(m)),new Response(raw),new Response(bytes,{headers:{'Content-Length':'100'}}));
  const response=await request(patchPath);
  expect(response.status).toBe(200);expect(response.headers.get('Content-Length')).toBe('100');
  expect(new Uint8Array(await response.arrayBuffer())).toEqual(bytes);
  await waitOnExecutionContext(contexts[0]);
  expect(new Uint8Array(await (await request(patchPath)).arrayBuffer())).toEqual(bytes);
  expect(fetch).toHaveBeenCalledTimes(3);
});
test('missing delta index leaves full manifest usable',async()=>{
  mock(manifest(),manifest());expect((await request(deltaPath)).status).toBe(404);
  expect((await request()).status).toBe(200);
});
test('tampered delta index is rejected',async()=>{
  const {m,raw}=await deltaFixture();
  mock(new Response(JSON.stringify(m)),new Response(raw+' '));
  expect((await request(deltaPath)).status).toBe(503);
});
test('patches not in index cannot be downloaded',async()=>{
  const {m,raw}=await deltaFixture();
  mock(new Response(JSON.stringify(m)),new Response(raw));
  expect((await request(patchPath.replace('0.5.0-to','0.4.0-to'))).status).toBe(404);
});
test('wrong patch Content-Length is rejected',async()=>{
  const {m,raw}=await deltaFixture();
  mock(new Response(JSON.stringify(m)),new Response(raw),new Response(new Uint8Array(100),{headers:{'Content-Length':'101'}}));
  expect((await request(patchPath)).status).toBe(503);
});
test('valid hash cannot hide an invalid target or duplicate index keys',async()=>{
  const {validateDeltaIndex}=await import('../src/index');
  const {raw,m,index}=await deltaFixture();
  expect(()=>validateDeltaIndex(raw,m)).not.toThrow();
  expect(()=>validateDeltaIndex(raw.replace('"schema":1','"schema":1,"schema":1'),m)).toThrow();
  expect(()=>validateDeltaIndex(JSON.stringify({...index,sha256:'e'.repeat(64)}),m)).toThrow();
  expect(()=>validateDeltaIndex(JSON.stringify({...index,patches:[...index.patches,...index.patches]}),m)).toThrow();
});
