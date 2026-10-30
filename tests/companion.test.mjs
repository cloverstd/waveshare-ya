import test from 'node:test';
import assert from 'node:assert/strict';

let fixtureId=0;
async function fixture(){
 const defaults={service:'ya-local',scope:'default',mode:'direct',lat:'39.9042',lon:'116.4074',market:'XSHG',symbol:'600000',note:'test'};
 const elements=new Map();
 globalThis.document={getElementById(id){if(!elements.has(id))elements.set(id,{value:defaults[id]||'',textContent:'',disabled:false,checked:true});return elements.get(id);}};
 Object.defineProperty(globalThis,'navigator',{configurable:true,value:{bluetooth:{}}});
 const {connection:c}=await import(`../web/app.mjs?fixture=${++fixtureId}`);
 const el=id=>document.getElementById(id),sent=[];
 let failure=false;
 const channel={async send(m){if(failure&&m.type==='event')throw Error('Disconnected');sent.push(m);}};
 const connect=()=>{c.onChannel(channel,'session',{service:'ya-local',scope:'default'});c.state('ready','连接就绪');};
 const request=resource=>c.onMessage({proto:'ya.data',v:1,type:'request',id:crypto.randomUUID(),session:'session',service:'ya-local',scope:'default',resource,schema:1,op:'get',params:resource==='market.quote'?{market:'XSHG',symbol:'600000'}:{}});
 const settle=()=>new Promise(r=>setTimeout(r,0));
 return {c,el,sent,connect,request,settle,fail(value){failure=value;}};
}
test('offline text is pending and is delivered once the device selects this gateway',async()=>{
 const f=await fixture();await f.el('save-note').onclick();
 assert.match(f.el('note-state').textContent,/暂存/);assert.equal(f.sent.length,0);
 f.connect();await f.request('system.health');assert.equal(f.sent.filter(m=>m.type==='event').length,0);
 await f.request('market.quote');await f.settle();
 assert.equal(f.sent.filter(m=>m.type==='event').length,1);assert.match(f.el('note-state').textContent,/设备已确认接收/);
});
test('resource reconnect disables saving and text remains pending until the next session',async()=>{
 const f=await fixture();f.connect();f.c.state('configuring','等待重连');
 assert.equal(f.el('configure').disabled,true);assert.equal(f.el('save-power').disabled,true);
 await f.el('save-note').onclick();assert.match(f.el('note-state').textContent,/暂存/);
 f.connect();assert.equal(f.el('configure').disabled,false);await f.request('info.text');
 const response=f.sent.find(m=>m.type==='response');assert.equal(response.data.text,'test');
 assert.match(f.el('note-state').textContent,/设备已确认接收/);
 assert.equal(f.sent.filter(m=>m.type==='event').length,0);
});
test('failed text delivery stays pending and is retried after reconnection',async()=>{
 const f=await fixture();f.connect();await f.request('market.quote');f.fail(true);
 await f.el('save-note').onclick();assert.match(f.el('note-state').textContent,/尚未送达/);
 f.c.state('disconnected','断开');f.fail(false);f.connect();await f.request('market.quote');await f.settle();
 assert.match(f.el('note-state').textContent,/设备已确认接收/);
});
test('editing resource identity does not silently discard the live session',async()=>{
 const f=await fixture();f.connect();f.el('service').value='next-service';f.el('scope').value='next-scope';
 await f.el('save-note').onclick();await f.request('info.text');
 assert.equal(f.sent.find(m=>m.type==='response').service,'ya-local');assert.match(f.el('note-state').textContent,/设备已确认接收/);
});
test('active gateway announcement delivers pending text without waiting for a sleeping plan',async()=>{
 const f=await fixture();await f.el('save-note').onclick();f.connect();await f.request('system.health');
 await f.c.onMessage({proto:'ya.data',v:1,type:'event',id:crypto.randomUUID(),session:'session',service:'ya-local',scope:'default',resource:'system.gateway',schema:1,params:{},data:{active:true}});
 await f.settle();assert.equal(f.sent.filter(m=>m.type==='event').length,1);assert.match(f.el('note-state').textContent,/设备已确认接收/);
});
