import test from 'node:test';
import assert from 'node:assert/strict';
import {wrapText,packText} from '../web/text-render.mjs';
import {inflateSync} from 'node:zlib';
let fixtureId=0;
async function fixture({failure='',deferCommit=false}={}){
 const elements=new Map();
 const context={measureText:s=>({width:Array.from(s).length*12}),fillRect(){},fillText(){},getImageData:(x,y,w,h)=>({data:new Uint8ClampedArray(w*h*4).fill(255)})};
 globalThis.document={fonts:{ready:Promise.resolve()},getElementById(id){if(!elements.has(id))elements.set(id,{value:id==='note'?'测试𠮷🙂\ntext':'',textContent:'',disabled:false,checked:id==='auto',getContext:()=>context});return elements.get(id);}};
 Object.defineProperty(globalThis,'navigator',{configurable:true,value:{bluetooth:{}}});
 const {connection:c}=await import(`../web/app.mjs?fixture=${++fixtureId}`),sent=[];let commit;
 const inbound=async extra=>c.onMessage({proto:'ya.data',v:1,id:crypto.randomUUID(),session:'session',service:'ya-local',scope:'default',...extra});
 const response=async m=>inbound({type:'response',reply_to:m.id,status:m.op===failure?'failed':'succeeded',error:{code:'storage_error'},data:m.resource==='system.logs'?{boot:1,entries:[{sequence:1,text:'W (1) wifi: body'}],next:1,more:false,overwritten:0,dropped:0}:{}});
 const ch={async send(m){sent.push(m);if(m.type==='request'){if(m.op==='commit'&&deferCommit)commit=()=>response(m);else queueMicrotask(()=>response(m));}}};
 const connect=async()=>{c.onChannel(ch,'session',{service:'ya-local',scope:'default'});await inbound({type:'hello',companion:'text-image-v1,logs-v1'});c.state('ready','连接就绪');};
 const settle=()=>new Promise(r=>setTimeout(r,10));
 return {c,el:id=>elements.get(id),sent,connect,settle,inbound,commit:()=>commit?.()};
}
test('wrap preserves whitespace, empty lines and emoji graphemes',()=>{
 assert.deepEqual(wrapText(' a\n\n👩‍💻X',s=>[...new Intl.Segmenter(undefined,{granularity:'grapheme'}).segment(s)].length,2),[' a','','👩‍💻X']);
 assert.throws(()=>wrapText('x'.repeat(1001),()=>1),/1000/);
 assert.throws(()=>wrapText('\n'.repeat(100),()=>1),/2048/);
 assert.throws(()=>wrapText('a\0b',()=>1),/1000/);
});
test('packet contains original UTF8 and little endian RGB565 after deflate',async()=>{
 const p=await packText({text:'𠮷🙂',height:1,rgba:new Uint8ClampedArray(432*4).fill(255)}),raw=inflateSync(p.bytes);
 assert.equal(raw.subarray(0,p.text_bytes).toString(),'𠮷🙂');assert.equal(raw.length,p.text_bytes+864);assert.equal(raw.at(-1),255);assert.equal(p.sha256.length,64);
});
test('offline text waits for capable session then uses begin chunk commit',async()=>{
 const f=await fixture();await f.el('save-note').onclick();assert.match(f.el('note-state').textContent,/暂存/);assert.equal(f.sent.length,0);
 await f.connect();await f.settle();assert.deepEqual(f.sent.filter(m=>m.type==='request').map(m=>m.op),['begin','chunk','commit']);assert.match(f.el('note-state').textContent,/已接收并保存/);
});
test('transport receipt alone does not mark text delivered',async()=>{
 const f=await fixture({deferCommit:true});await f.connect();const send=f.el('save-note').onclick();await f.settle();
 assert.doesNotMatch(f.el('note-state').textContent,/已接收并保存/);await f.commit();await send;assert.match(f.el('note-state').textContent,/已接收并保存/);
});
test('storage failure retains input and cancels incomplete transfer',async()=>{
 const f=await fixture({failure:'commit'});await f.connect();await f.el('save-note').onclick();assert.match(f.el('note-state').textContent,/storage_error/);assert.equal(f.el('note').value,'测试𠮷🙂\ntext');assert.equal(f.sent.at(-2)?.op||f.sent.at(-1)?.op,'cancel');
});
test('log pull and live events deduplicate by boot and sequence',async()=>{
 const f=await fixture();await f.connect();await f.el('pull').onclick();assert.match(f.el('device-log').textContent,/wifi: body/);
 await f.inbound({type:'event',resource:'system.logs',data:{boot:1,entries:[{sequence:1,text:'W (1) wifi: body'}],overwritten:0,dropped:0}});
 assert.equal(f.el('device-log').textContent.split('\n').length,1);
 await f.inbound({type:'event',resource:'system.logs',data:{boot:2,entries:[{sequence:1,text:'new boot'}],overwritten:0,dropped:0}});assert.match(f.el('device-log').textContent,/new boot/);assert.doesNotMatch(f.el('device-log').textContent,/wifi/);
});
test('disconnect during commit never reports delivery and keeps original input',async()=>{
 const f=await fixture({deferCommit:true});await f.connect();const send=f.el('save-note').onclick();await f.settle();
 f.c.state('disconnected','已断开');await send;
 assert.match(f.el('note-state').textContent,/连接已断开/);assert.doesNotMatch(f.el('note-state').textContent,/已接收并保存/);assert.equal(f.el('note').value,'测试𠮷🙂\ntext');
});
test('old firmware capabilities cannot start a text upload',async()=>{
 const f=await fixture();await f.connect();await f.inbound({type:'hello'});await f.el('save-note').onclick();
 assert.equal(f.sent.filter(m=>m.type==='request').length,0);assert.match(f.el('activity').textContent,/升级/);
});
