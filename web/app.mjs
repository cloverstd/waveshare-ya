import {initGallery} from './gallery.mjs';
import {Connection} from './connection.mjs';
import {renderText,packText,base64} from './text-render.mjs';
const $=id=>document.getElementById(id);
let galleryCapable=false;
let channel,session='',identity={service:'ya-local',scope:'default'},epoch=0,capable=false;
let pendingText=null,sending=false,stopped=false,transfer='',sequence=0,cursor=0,boot=null;
const requests=new Map(),entries=new Map();
function log(message){$('log').textContent=`${new Date().toLocaleTimeString()}  ${message}\n`+$('log').textContent.slice(0,6000);}
function controls(){const ready=connection.stage==='ready'&&capable;$('pull').disabled=!ready||sending;$('live').disabled=!ready||sending;$('cancel').disabled=!sending;$('save-note').disabled=sending;}
function envelope(type,extra={}){return {proto:'ya.data',v:1,type,id:crypto.randomUUID(),session,...identity,...extra};}
function rejectRequests(){for(const r of requests.values()){clearTimeout(r.timer);r.reject(Error('连接已断开'));}requests.clear();}
async function request(resource,op,extra={}){
 const m=envelope('request',{resource,schema:1,params:{},op,boot,...extra});const ch=channel;
 let timer;const result=new Promise((resolve,reject)=>{timer=setTimeout(()=>{requests.delete(m.id);reject(Error('设备处理超时'));},45000);requests.set(m.id,{resolve,reject,timer});});result.catch(()=>{});
 try{await ch.send(m);return await result;}finally{clearTimeout(timer);requests.delete(m.id);}
}
function receiveLogs(d){
 if(boot!==d.boot){boot=d.boot;cursor=0;entries.clear();}
 for(const e of d.entries||[]){entries.set(e.sequence,e.text);cursor=Math.max(cursor,e.sequence);}
 while(entries.size>1000)entries.delete(entries.keys().next().value);
 $('device-log').textContent=[...entries.entries()].sort((a,b)=>a[0]-b[0]).map(([id,text])=>`${id}  ${text}`).join('\n');
 $('log-state').textContent=`本次开机 ${d.boot} · 已显示 ${entries.size} 条 · 覆盖 ${d.overwritten} · 采集丢弃 ${d.dropped} · 本批缺失 ${d.missed||0}`;
}
async function message(m){
 if(m.session!==session||m.service!==identity.service||m.scope!==identity.scope)return;
 const receivedEpoch=epoch,ch=channel;
 await ch.send(envelope('ack',{reply_to:m.id}));if(receivedEpoch!==epoch)return;
 if(m.type==='hello'){galleryCapable=!!m.companion?.includes('gallery-v1');gallery.state(galleryCapable);capable=m.companion?.includes('text-image-v1')&&m.companion?.includes('logs-v1');if(!capable)$('activity').textContent='此固件不支持新数据站，请升级设备';controls();return;}
 if(m.type==='response'||m.type==='error'){
  const r=requests.get(m.reply_to);if(r){clearTimeout(r.timer);requests.delete(m.reply_to);m.status==='succeeded'?r.resolve(m.data||{}):r.reject(Error(m.error?.code||'设备未接受请求'));}return;
 }
 if(m.type==='event'&&m.resource==='system.logs'){receiveLogs(m.data);return;}
 if(m.type==='request'){
  const healthy=m.resource==='system.health';await ch.send(envelope('response',{reply_to:m.id,resource:m.resource,schema:1,params:m.params,status:healthy?'succeeded':'failed',data:healthy?{online:true,commands:false}:{},error:healthy?undefined:{code:'unsupported',retryable:false},updated_at:Math.floor(Date.now()/1000),ttl_s:30}));
 }
}
export const connection=new Connection({bluetooth:navigator.bluetooth,onLog:log,
 auto:()=>$('auto').checked,
 hello:s=>({proto:'ya.data',v:1,type:'hello',id:crypto.randomUUID(),session:s,...identity,max_bytes:4096,commands:false,companion:'text-image-v1,logs-v1,gallery-v1'}),
 onIdentity:j=>{identity=j;},
 onChannel:(ch,s,hello)=>{rejectRequests();channel=ch;session=s;identity={service:hello.service,scope:hello.scope};epoch++;capable=false;galleryCapable=false;gallery.state(false);},
 onMessage:message,
 onState:state=>{$('connection').textContent=state;gallery.state(galleryCapable);if(connection.stage!=='ready'){$('live').checked=false;rejectRequests();}controls();if(connection.stage==='ready')void flushText();}
});
const gallery=initGallery({request,connected:()=>connection.stage==='ready',host:document.getElementById('gallery-manager')?.querySelector?document.getElementById('gallery-manager'):null});
async function flushText(){
 if(!pendingText||sending||connection.stage!=='ready'||!capable)return;
 const text=pendingText,startedEpoch=epoch;let committed=false;
 sending=true;stopped=false;transfer=crypto.randomUUID();controls();
 try{
  const rendered=await renderText(text,$('preview')),pack=await packText(rendered);
  if(stopped||epoch!==startedEpoch)throw Error('发送已取消');
  $('note-state').textContent='正在发送排版内容……';
  await request('info.text','begin',{transfer,height:pack.height,text_bytes:pack.text_bytes,packed_bytes:pack.bytes.length,sha256:pack.sha256});
  for(let offset=0;offset<pack.bytes.length;offset+=1536){
   if(stopped||epoch!==startedEpoch)throw Error('发送已取消');
   const part=pack.bytes.slice(offset,offset+1536);await request('info.text','chunk',{transfer,offset,bytes:base64(part)});
   $('progress').value=Math.round((offset+part.length)/pack.bytes.length*100);
  }
  if(stopped||epoch!==startedEpoch)throw Error('发送已取消');
  await request('info.text','commit',{transfer});committed=true;
  if(pendingText===text)pendingText=null;
  $('note-state').textContent='设备已接收并保存，可在信息应用中查看';
 }catch(e){$('note-state').textContent=`${e.message}；文字保留在输入框中，可重新发送`;log(e.message);}
 finally{
  if(!committed&&startedEpoch===epoch&&connection.stage==='ready')try{await request('info.text','cancel',{transfer});}catch{}
  sending=false;controls();
  if(pendingText&&(pendingText!==text||epoch!==startedEpoch)&&connection.stage==='ready')void flushText();
 }
}
function action(id,run){$(id).onclick=async()=>{try{await run();}catch(e){log(e.message);$('activity').textContent=e.message;}};}
action('connect',async()=>{if(!navigator.bluetooth||!isSecureContext)throw Error('请在 Chrome 中通过 HTTPS 或 localhost 打开');await connection.select($('device-name').value.trim());});
action('disconnect',()=>{stopped=true;connection.disconnect();epoch++;channel=null;});
action('save-note',async()=>{const text=$('note').value;await renderText(text,$('preview'));pendingText=text;$('progress').value=0;$('note-state').textContent='文字已暂存，等待设备连接';await flushText();});
action('cancel',()=>{stopped=true;pendingText=null;$('note-state').textContent='正在取消，等待当前分块结束';});
action('pull',async()=>{let after=0;for(let page=0;page<100;page++){const d=await request('system.logs','get',{after});receiveLogs(d);if(!d.more)break;if(d.next<=after)throw Error('日志分页未前进');after=d.next;}});
$('live').onchange=async()=>{try{const d=await request('system.logs',$('live').checked?'subscribe':'unsubscribe',{after:cursor});if(d.entries)receiveLogs(d);}catch(e){$('live').checked=false;log(e.message);}};
action('download',()=>{const blob=new Blob([$('device-log').textContent],{type:'text/plain;charset=utf-8'}),url=URL.createObjectURL(blob),a=document.createElement('a');a.href=url;a.download=`ya-logs-${boot??'unknown'}.txt`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);});
let previewGeneration=0;$('note').oninput=async()=>{const generation=++previewGeneration;try{await renderText($('note').value,$('preview'));if(generation===previewGeneration)$('note-state').textContent='预览已更新';}catch(e){if(generation===previewGeneration)$('note-state').textContent=e.message;}};
if(!navigator.bluetooth)$('activity').textContent='当前浏览器不支持 Web Bluetooth，请使用 Chrome。';
controls();
