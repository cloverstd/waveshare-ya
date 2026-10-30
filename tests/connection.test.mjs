import test from 'node:test';import assert from 'node:assert/strict';
import {Connection,classify} from '../web/connection.mjs';import {UUID,encode,Frames} from '../web/protocol.mjs';
class Target extends EventTarget{value(bytes){this.value=new DataView(bytes.buffer);this.dispatchEvent(new Event('characteristicvaluechanged'));}}
function fixture({missing=false,auth=false,handshake=true,managementNotify=true,encryptionDelays=0,cachedData=false}={}){
 const subscriptions=[];const tx=new Target(),mt=new Target();let dataEnabled=false,platformEnabled=cachedData,registered=false,management='',lastReply=new DataView(new TextEncoder().encode('{"v":1}').buffer);const frames=new Frames();
 const mgRx={async writeValueWithResponse(bytes){management+=new TextDecoder().decode(bytes);if(management.endsWith('\n')){const j=JSON.parse(management);management='';lastReply=new DataView(new TextEncoder().encode(JSON.stringify({id:j.id,ok:true})+'\n').buffer);mt.value=lastReply;if(managementNotify)mt.dispatchEvent(new Event('characteristicvaluechanged'));}}};
 mt.readValue=async()=>{if(encryptionDelays-->0)throw Error('Insufficient Encryption');if(auth)throw Object.assign(Error('authentication failed'),{name:'SecurityError'});return lastReply;};mt.stopNotifications=async()=>{subscriptions.push('management stop');};mt.startNotifications=async()=>{subscriptions.push('management start');};tx.stopNotifications=async()=>{subscriptions.push('data stop');dataEnabled=false;if(registered)platformEnabled=false;registered=false;};tx.startNotifications=async()=>{subscriptions.push('data start');if(!platformEnabled){dataEnabled=true;platformEnabled=true;}registered=true;};
 const rx={async writeValueWithResponse(bytes){const m=frames.feed(bytes);if(m?.type==='hello'&&handshake&&dataEnabled){for(const obj of [{type:'ack',reply_to:m.id},{...m,id:'device-hello'}]){const b=encode(obj);tx.value=new DataView(b.buffer);tx.dispatchEvent(new Event('characteristicvaluechanged'));}}}};
 const device=new Target();device.gatt={connected:false,disconnect(){this.connected=false;},async connect(){this.connected=true;return {async getPrimaryService(id){if(id===UUID.service&&missing)throw Error('No Services matching UUID');return {async getCharacteristic(ch){return id===UUID.service?(ch===UUID.rx?rx:tx):(ch.startsWith('98f10002')?mgRx:mt);}};}};}};
 const states=[],logs=[];const c=new Connection({bluetooth:{requestDevice:async()=>device},onState:s=>states.push(s),onLog:s=>logs.push(s),hello:session=>({proto:'ya.data',v:1,id:crypto.randomUUID(),type:'hello',session,service:'x',scope:'y'}),onMessage:async m=>{if(m.type!=='ack')await c.channel.send({type:'ack',reply_to:m.id});}});
 return {c,states,logs,device,subscriptions,rx,mgRx};
}
test('chooser cancellation stays idle and does not retry',async()=>{const {c,states}=fixture();c.bluetooth.requestDevice=async()=>{throw Object.assign(Error('cancel'),{name:'NotFoundError'});};await c.select();assert.equal(states.at(-1),'未选择设备');assert.equal(c.timer,null);});
test('missing service stops retry and gives recovery guidance',async()=>{const {c}=fixture({missing:true});await c.select();assert.equal(c.stopped,true);assert.match(c.stage,/disconnected/);assert.equal(c.timer,null);});
test('authentication failure is terminal',async()=>{const {c}=fixture({auth:true});await c.select();assert.equal(c.stopped,true);assert.equal(c.timer,null);});
test('ready requires management response and both sides of hello',async()=>{const {c,states}=fixture();await c.select();assert.equal(c.stage,'ready');assert.equal(states.at(-1),'连接就绪');c.disconnect();assert.equal(c.listeners.length,0);});
test('temporary disconnect retries; manual disconnect cancels timer and old callback',async()=>{const {c,device}=fixture();await c.select();device.dispatchEvent(new Event('gattserverdisconnected'));assert.ok(c.timer);const generation=c.generation;c.disconnect();assert.equal(c.timer,null);assert.ok(c.generation>generation);device.dispatchEvent(new Event('gattserverdisconnected'));assert.equal(c.stage,'idle');});
test('protocol incompatibility stops retry',()=>assert.equal(classify(Error('协议不兼容'),'handshake').retry,false));
test('cancel in-flight connection cannot publish ready',async()=>{const {c,device}=fixture();let finish;const original=device.gatt.connect;device.gatt.connect=()=>new Promise(resolve=>finish=async()=>resolve(await original.call(device.gatt)));const pending=c.select();await new Promise(r=>setTimeout(r,0));c.disconnect();await finish();await pending;assert.equal(c.stage,'idle');});
test('hello response timeout cannot become ready',async()=>{
 const {c}=fixture({handshake:false});c.auto=()=>false;
 c.timers={setTimeout(fn,ms){if(ms===25000)queueMicrotask(fn);return 0;},clearTimeout(){}};
 await c.select();assert.equal(c.stopped,true);assert.equal(c.stage,'disconnected');assert.equal(c.channel,null);
});
test('unsupported cached device is terminal',()=>assert.equal(classify(Error('Unsupported device.'),'connecting').retry,false));
test('missed management notification recovers from authenticated TX read',async()=>{const {c}=fixture({managementNotify:false});await c.select();assert.equal(c.stage,'ready');c.disconnect();});
test('explicit unique name restricts chooser to the intended device',async()=>{const {c,device}=fixture();let requested;c.bluetooth.requestDevice=async options=>{requested=options;return device;};await c.select('Waveshare-YA-B022');assert.deepEqual(requested.filters,[{services:[UUID.service],name:'Waveshare-YA-B022'}]);c.disconnect();});
test('transport connection timeout does not hang the page',async()=>{const {c,device}=fixture();device.gatt.connect=()=>new Promise(()=>{});c.auto=()=>false;c.timers={setTimeout(fn,ms){if(ms===20000)queueMicrotask(fn);return 0;},clearTimeout(){}};await c.select();assert.equal(c.stage,'disconnected');assert.equal(c.stopped,true);});
test('waits for encryption after subscribing before management request',async()=>{const {c,states}=fixture({encryptionDelays:1});await c.select();assert.equal(c.stage,'ready');assert.ok(states.indexOf('正在验证安全连接')<states.indexOf('连接就绪'));c.disconnect();});
test('automatic reconnect preserves the device and does not disconnect an already lost link',async()=>{
 const {c,device}=fixture();await c.select();let disconnects=0;const disconnect=device.gatt.disconnect;device.gatt.disconnect=function(){disconnects++;disconnect.call(this);};
 let retry;c.timers={setTimeout(fn,ms){if(ms===1000)retry=fn;return 1;},clearTimeout(){}};
 device.gatt.connected=false;device.dispatchEvent(new Event('gattserverdisconnected'));assert.equal(disconnects,0);
 retry();await new Promise(r=>setTimeout(r,5));assert.equal(c.stage,'ready');assert.equal(disconnects,0);c.disconnect();
});
test('keeps the freshly selected device handle for connection',async()=>{
 const {c,device}=fixture();c.bluetooth.getDevices=async()=>{throw Error('must use the freshly selected handle');};await c.select();assert.equal(c.device,device);assert.equal(c.stage,'ready');c.disconnect();
});
test('reconnect waits for the same permitted device advertisement and stops watching afterwards',async()=>{
 const {c,device}=fixture();await c.select();device.gatt.connected=false;let signal,watched=false;
 device.watchAdvertisements=async options=>{watched=true;signal=options.signal;queueMicrotask(()=>device.dispatchEvent(new Event('advertisementreceived')));};
 await c.open(true);assert.equal(c.stage,'ready');assert.equal(watched,true);assert.equal(signal.aborted,true);c.disconnect();
});
test('manual disconnect cancels an advertisement wait',async()=>{
 const {c,device}=fixture();await c.select();device.gatt.connected=false;let signal;
 device.watchAdvertisements=async options=>{signal=options.signal;};const task=c.open(true);await new Promise(r=>setTimeout(r,1));c.disconnect();await task;assert.equal(signal.aborted,true);assert.equal(c.stage,'idle');
});
test('transport ACK timeout is retryable rather than protocol incompatibility',()=>assert.equal(classify(Error('ACK timeout'),'handshake').retry,true));
test('new session registers then renews the same data notification object',async()=>{
 const {c,subscriptions}=fixture();await c.select();assert.deepEqual(subscriptions,['management stop','management start','data start','data stop','data start']);c.disconnect();
});
test('stale platform notification cache is cleared after registering the current object',async()=>{
 const {c}=fixture({cachedData:true});c.auto=()=>false;await c.select();assert.equal(c.stage,'ready');c.disconnect();
});
test('resource save requires ready and remains blocked until a new session',async()=>{
 const {c,device,logs}=fixture();c.stage='handshake';await assert.rejects(c.configure('{}'),/连接就绪/);
 await assert.rejects(c.manage('set',{key:'brightness',value:50}),/连接就绪/);
 await c.select();await c.configure('{}');assert.equal(c.stage,'configuring');
 await assert.rejects(c.configure('{}'),/连接就绪/);
 device.gatt.connected=false;device.dispatchEvent(new Event('gattserverdisconnected'));
 assert.ok(logs.some(s=>s.includes('资源配置会话已结束')));assert.ok(!logs.some(s=>s.includes('configuring: Error')));
 c.disconnect();
});
test('management and data share one GATT operation queue',async()=>{
 const {c,rx,mgRx}=fixture();await c.select();let running=0,peak=0;
 for(const target of [rx,mgRx]){const original=target.writeValueWithResponse;target.writeValueWithResponse=async bytes=>{peak=Math.max(peak,++running);try{await new Promise(r=>setTimeout(r,1));return await original.call(target,bytes);}finally{running--;}};}
 await Promise.all([c.manage('set',{key:'brightness',value:100}),c.channel.send({type:'ack',reply_to:'probe'})]);
 assert.equal(peak,1);c.disconnect();
});
test('handshake identity is captured once even while the form is edited',async()=>{
 const {c}=fixture();let calls=0;const original=c.hello;
 c.hello=session=>{const m=original(session);if(++calls>1)m.service='edited';return m;};
 await c.select();assert.equal(c.stage,'ready');assert.equal(calls,1);c.disconnect();
});
