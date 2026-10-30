import {UUID,ReliableChannel} from './protocol.mjs';
const mgRx='98f10002-8f7c-4cb1-9e50-6bf589c91010', mgTx='98f10003-8f7c-4cb1-9e50-6bf589c91010';
export function classify(error,stage){
 if(stage==='selecting'&&error.name==='NotFoundError')return {retry:false,message:'未选择设备'};
 if(/No Services|service.*not found|matching UUID/i.test(error.message))return {retry:false,message:'设备服务不可用：请确认固件版本；升级后若仍出现，请在系统蓝牙中忘记本设备，再重新配对。'};
 if(/Unsupported device/i.test(error.message))return {retry:false,message:'系统中的设备记录已失效，请在系统蓝牙中忘记对应的 Waveshare 设备，再按屏幕上的唯一名称重新配对。'};
 if(stage==='security'&&/设备已断开|GATT operation failed|insufficient/i.test(error.message))return {retry:false,message:'安全配对未完成，请重新打开设备配对窗口并完成系统验证码输入。'};
 if(error.name==='SecurityError'||/authenticat|pairing|not authorized|insufficient.*encrypt/i.test(error.message))return {retry:false,message:'安全配对未完成：请打开设备配对窗口并输入屏幕验证码。'};
 if(/ACK timeout|数据握手超时/.test(error.message))return {retry:true,message:'蓝牙数据传输超时，正在准备重连。'};
 if(/握手|协议|管理请求超时/.test(error.message))return {retry:false,message:'数据握手未完成，请检查设备配对和数据服务身份后重新连接。'};
 return {retry:true,message:'蓝牙连接中断：'+error.message};
}
export class Connection {
 constructor({bluetooth,onState=()=>{},onLog=()=>{},onChannel=()=>{},onMessage=()=>{},onIdentity=()=>{},hello,auto=()=>true,timers=globalThis}){
  Object.assign(this,{bluetooth,onState,onLog,onChannel,onMessage,onIdentity,hello,auto,timers});
  this.generation=0;this.stopped=true;this.pending=null;this.tail=Promise.resolve();this.id=0;this.listeners=[];
 }
 state(stage,label){this.stage=stage;this.onState(label);}
 listen(obj,type,fn){obj.addEventListener(type,fn);this.listeners.push(()=>obj.removeEventListener(type,fn));}
 cleanup(disconnect=true){
  ++this.generation;this.tail=Promise.resolve();this.gattTail=Promise.resolve();this.reconfiguring=false;this.timers.clearTimeout(this.timer);this.timer=null;
  this.channel?.close();this.channel=null;this.management=null;
  for(const off of this.listeners.splice(0))off();
  if(this.pending){this.pending.reject(Error('蓝牙已断开'));this.pending=null;}
  this.cancelHello?.(Error('蓝牙已断开'));this.cancelHello=null;
  this.cancelWatch?.(Error('蓝牙已断开'));this.cancelWatch=null;this.watchAbort?.abort();this.watchAbort=null;
  if(disconnect&&this.device?.gatt.connected)this.device.gatt.disconnect();
 }
 async select(name=''){
  this.stopped=true;this.cleanup();this.state('selecting','请选择设备');
  try{this.device=await this.bluetooth.requestDevice({filters:[name?{services:[UUID.service],name}:{services:[UUID.service]}],optionalServices:[UUID.service,UUID.management]});}
  catch(e){const f=classify(e,'selecting');this.state('idle',f.message);this.onLog(f.message);return;}
  this.stopped=false;this.attempt=0;await this.open();
 }
 disconnect(){this.stopped=true;this.cleanup();this.state('idle','已断开');}
 async open(reconnect=false){
  this.cleanup(false);const generation=this.generation;
  const current=()=>generation===this.generation&&!this.stopped;
  const ensure=()=>{if(!current())throw Error('旧连接已取消');};
  this.session=crypto.randomUUID();this.reply='';this.decoder=new TextDecoder();
  try{
   this.state('connecting','正在连接设备');
   if(reconnect&&typeof this.device.watchAdvertisements==='function'){
    this.state('advertising','等待设备重新广播');
    const target=this.device,controller=new AbortController();this.watchAbort=controller;
    let timer,seenResolve;const seen=new Promise((resolve,reject)=>{this.cancelWatch=reject;seenResolve=resolve;});seen.catch(()=>{});
    const observed=()=>{if(current())seenResolve();};target.addEventListener('advertisementreceived',observed);
    try{
     await target.watchAdvertisements({signal:controller.signal});ensure();
     await Promise.race([seen,new Promise((_,reject)=>{timer=this.timers.setTimeout(()=>reject(Error('等待设备广播超时')),10000);})]);ensure();
    }catch(e){if(!current()||!['NotSupportedError','SecurityError'].includes(e.name))throw e;}
    finally{this.timers.clearTimeout(timer);target.removeEventListener('advertisementreceived',observed);if(this.watchAbort===controller){this.cancelWatch=null;this.watchAbort=null;}controller.abort();}
    this.state('connecting','正在重新连接设备');
   }
   const selected=this.device;let connectTimer;
   const connected=selected.gatt.connect().then(server=>{if(!current()){if(this.stopped||this.device!==selected)selected.gatt.disconnect();throw Error('旧连接已取消');}return server;});
   let server;try{server=await Promise.race([connected,new Promise((_,reject)=>{connectTimer=this.timers.setTimeout(()=>reject(Error('蓝牙连接超时')),20000);})]);}
   finally{this.timers.clearTimeout(connectTimer);}ensure();
   this.listen(this.device,'gattserverdisconnected',()=>{if(current())this.failed(Error('设备已断开'));});
   this.state('security','正在验证安全连接');
   const mg=await server.getPrimaryService(UUID.management);ensure();
   this.management=await mg.getCharacteristic(mgRx);ensure();
   const mt=await mg.getCharacteristic(mgTx);ensure();this.managementTx=mt;
   this.listen(mt,'characteristicvaluechanged',e=>{if(current())this.managementReply(e.target.value);});
   try{await mt.stopNotifications?.();}catch{}ensure();
   await mt.startNotifications();ensure();
   // The subscription starts SMP on the device after service discovery. Keep
   // the protected read pending while the user enters the displayed passkey.
   const authDeadline=Date.now()+110000;
   for(;;){try{await mt.readValue();ensure();break;}catch(e){
    ensure();if(!/insufficient.*(?:encrypt|authenticat)|GATT operation failed for unknown reason/i.test(e.message)||Date.now()>authDeadline)throw e;
    await new Promise(resolve=>this.timers.setTimeout(resolve,1000));ensure();
   }}
   const deviceStatus=await this.manage('status');ensure();
   if(deviceStatus.data_identity){const identity=JSON.parse(deviceStatus.data_identity);if(typeof identity.service==='string'&&typeof identity.scope==='string')this.onIdentity(identity);}
   this.state('services','正在发现数据服务');
   const svc=await server.getPrimaryService(UUID.service);ensure();
   const rx=await svc.getCharacteristic(UUID.rx);ensure();const tx=await svc.getCharacteristic(UUID.tx);ensure();
   const session=this.session,identity=this.hello(session);
   let helloResolve,helloReject;
   const received=new Promise((resolve,reject)=>{helloResolve=resolve;helloReject=reject;this.cancelHello=reject;});received.catch(()=>{});
   const ch=new ReliableChannel(bytes=>this.gatt(()=>rx.writeValueWithResponse(bytes),generation),m=>{
    if(!current())return;
    if(m.session!==session||m.proto!=='ya.data'||m.v!==1||m.service!==identity.service||m.scope!==identity.scope)return;
    if(m.type==='hello')helloResolve();
    return this.onMessage(m);
   },e=>{if(current())this.failed(e);});
   this.channel=ch;this.onChannel(ch,session,identity);
   this.listen(tx,'characteristicvaluechanged',e=>{if(current())ch.receive(new Uint8Array(e.target.value.buffer,e.target.value.byteOffset,e.target.value.byteLength));});
   this.state('subscribing','正在订阅数据通知');
   // First register this characteristic object with Chrome. A stop on a fresh
   // object may leave its cached platform subscription behind after reconnect.
   await tx.startNotifications();ensure();await tx.stopNotifications();ensure();await tx.startNotifications();ensure();
   this.state('handshake','正在协商数据连接');
   let timeout;const deadline=new Promise((_,reject)=>{timeout=this.timers.setTimeout(()=>reject(Error('数据握手超时')),25000);});
   try{await Promise.race([Promise.all([ch.send(identity),received]),deadline]);}finally{this.timers.clearTimeout(timeout);if(this.cancelHello===helloReject)this.cancelHello=null;}
   ensure();this.attempt=0;this.state('ready','连接就绪');this.onLog('安全连接与数据握手已完成');
  }catch(e){if(current())this.failed(e);}
 }
 failed(e){
  const expected=this.reconfiguring&&/设备已断开/.test(e.message);
  const f=expected?{retry:true,message:'资源配置会话已结束，正在重新连接设备。'}:classify(e,this.stage);
  if(this.stage==='handshake'&&this.attempt>=2){f.retry=false;f.message='多次数据握手失败，请检查设备连接和资源配置后重新连接。';}this.cleanup();
  if(!expected)this.onLog(`${this.stage}: ${e.name||'Error'}: ${e.message}`);this.onLog(f.message);this.state('disconnected',f.message);
  if(!f.retry||this.stopped||!this.auto()){this.stopped=true;return;}
  const generation=this.generation;const ms=Math.min(30000,1000*2**this.attempt++);
  this.onLog(`将在 ${ms/1000} 秒后重连`);
  this.timer=this.timers.setTimeout(()=>{if(generation===this.generation&&!this.stopped&&this.auto())this.open(true);},ms);
 }
 gatt(run,generation=this.generation){
  const task=(this.gattTail||Promise.resolve()).then(()=>{if(generation!==this.generation)throw Error('蓝牙已断开');return run();});
  this.gattTail=task.catch(()=>{});return task;
 }
 async configure(json){
  if(this.stage!=='ready')throw Error('请等待连接就绪后再保存资源');
  const generation=this.generation;this.reconfiguring=true;this.state('configuring','正在保存资源配置，等待设备重连');
  try{return await this.manage('data_config',{json});}
  catch(e){if(generation===this.generation){this.reconfiguring=false;this.state('ready','连接就绪');}throw e;}
 }
 managementReply(value){
  this.reply+=this.decoder.decode(value,{stream:true});if(this.reply.length>8192){this.reply='';return;}
  let cut;while((cut=this.reply.indexOf('\n'))>=0){const line=this.reply.slice(0,cut);this.reply=this.reply.slice(cut+1);try{const j=JSON.parse(line);if(this.pending&&j.id===this.pending.id){this.pending.resolve(j);this.pending=null;}}catch{}}
 }
 manage(op,extra={}){
  if(!['status','data_status'].includes(op)&&this.stage!=='ready'&&!(op==='data_config'&&this.reconfiguring))return Promise.reject(Error('请等待连接就绪后再修改设置'));
  const generation=this.generation;
  const run=async()=>{
   if(!this.management||generation!==this.generation)throw Error('请先连接设备');
   const id=++this.id;const bytes=new TextEncoder().encode(JSON.stringify({v:1,id,op,...extra})+'\n');
   if(bytes.length>1024)throw Error('配置超过管理协议 1024 字节');
   let fallback;let timer;const reply=new Promise((resolve,reject)=>{this.pending={id,resolve,reject};timer=this.timers.setTimeout(()=>reject(Error('管理请求超时')),15000);});reply.catch(()=>{});
   try{for(let i=0;i<bytes.length;i+=20){if(generation!==this.generation)throw Error('蓝牙已断开');await this.gatt(()=>this.management.writeValueWithResponse(bytes.slice(i,i+20)),generation);}
    // TX retains the last management response; recover a missed notification after reconnect.
    fallback=this.timers.setTimeout(async()=>{if(generation!==this.generation||this.pending?.id!==id)return;try{
      const v=await this.gatt(()=>this.managementTx.readValue(),generation);if(generation!==this.generation||this.pending?.id!==id||!v)return;
      const j=JSON.parse(new TextDecoder().decode(v));if(j.id===id){this.pending.resolve(j);this.pending=null;}
    }catch{}},1000);
    const r=await reply;if(!r.ok)throw Error(r.error||'设置失败');return r;}
   finally{this.timers.clearTimeout(timer);this.timers.clearTimeout(fallback);if(this.pending?.id===id)this.pending=null;}
  };
  const result=this.tail.then(run);this.tail=result.catch(()=>{});return result;
 }
}
