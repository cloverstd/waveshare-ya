import {Connection} from './connection.mjs';
const $=id=>document.getElementById(id), val=id=>$(id).value.trim();

let channel,session='',epoch=0,identity=null,gatewayActive=false;
let manual=null,note=null,notePending=false,noteSending=false,weather=null,weatherKey='';const responses=new Map(),busy=new Set();let sequence=0;
function log(message){$('log').textContent=`${new Date().toLocaleTimeString()}  ${message}\n`+$('log').textContent.slice(0,6000);}
function controls(){for(const id of ['configure','save-power'])$(id).disabled=busy.has(id)||connection.stage!=='ready';}
function status(message){$('connection').textContent=message;if(connection.stage!=='ready')gatewayActive=false;controls();}
function noteStatus(message){$('note-state').textContent=message;log(message);}
function envelope(type,extra={}){return {proto:'ya.data',v:1,type,id:crypto.randomUUID(),session,...(identity||{service:val('service'),scope:val('scope')}),...extra};}
async function flushNote(){
 if(!notePending||noteSending||!gatewayActive||connection.stage!=='ready')return;
 const sent=note,ch=channel,connectionEpoch=epoch,sending={};noteSending=sending;
 try{await ch.send(envelope('event',{resource:'info.text',schema:1,params:{},sequence:++sequence,notify:true,updated_at:sent.at,ttl_s:86400,data:{text:sent.text,source:'manual'}}));
  if(connectionEpoch===epoch&&note===sent){notePending=false;noteStatus('文字已发送，设备已确认接收');}
 }catch{if(note===sent)noteStatus('文字尚未送达，已暂存，等待数据连接恢复');}
 finally{if(noteSending===sending)noteSending=false;if(connectionEpoch===epoch&&note!==sent)void flushNote();}
}
function coordinates(){let lat=Number(val('lat')),lon=Number(val('lon'));if(!Number.isFinite(lat)||!Number.isFinite(lon)||lat<-90||lat>90||lon<-180||lon>180)throw Error('经纬度无效');return {latitude:lat,longitude:lon};}
async function weatherData(params){
 const {latitude,longitude}=params;if(!Number.isFinite(latitude)||!Number.isFinite(longitude)||Math.abs(latitude)>90||Math.abs(longitude)>180)throw Error('天气坐标无效');
 const k=JSON.stringify(params);if(weather&&k===weatherKey&&Date.now()-weather.fetched<60000)return weather;
 const q=new URLSearchParams({latitude,longitude,current:'temperature_2m,relative_humidity_2m,weather_code',daily:'temperature_2m_max,temperature_2m_min,weather_code',forecast_days:'3',timezone:'UTC',timeformat:'unixtime'});
 const res=await fetch('https://api.open-meteo.com/v1/forecast?'+q,{signal:AbortSignal.timeout(8000)});if(!res.ok)throw Error('天气服务 HTTP '+res.status);const j=await res.json();
 if(!j.current||!Number.isFinite(j.current.temperature_2m)||!j.daily)throw Error('天气数据不完整');
 weather={fetched:Date.now(),updated_at:j.current.time,ttl_s:1800,data:{temperature_c:j.current.temperature_2m,humidity_pct:j.current.relative_humidity_2m,condition:String(j.current.weather_code),timezone:'UTC',source:'Open-Meteo',forecast:j.daily.time.map((t,i)=>({time:t,min_c:j.daily.temperature_2m_min[i],max_c:j.daily.temperature_2m_max[i],code:j.daily.weather_code[i]}))}};weatherKey=k;
 $('temperature').textContent=j.current.temperature_2m+' °C';$('weather-detail').textContent=`湿度 ${j.current.relative_humidity_2m}% · ${new Date(j.current.time*1000).toLocaleString()} · 未来三天已获取`;
 return weather;
}
async function provide(m){
 if(val('mode')==='server'){
  const endpoint=val('endpoint');const u=new URL(endpoint);if(u.protocol!=='https:'&&!(u.protocol==='http:'&&['localhost','127.0.0.1'].includes(u.hostname)))throw Error('服务需要 HTTPS');
  const res=await fetch(u,{method:'POST',headers:{'Content-Type':'application/json',...(val('token')?{Authorization:'Bearer '+val('token')}:{})},body:JSON.stringify(m),signal:AbortSignal.timeout(8000)});
  if(!res.ok)throw Error('数据服务 HTTP '+res.status);const j=await res.json();if(j.proto!=='ya.data'||j.v!==1||j.session!==m.session||j.reply_to!==m.id||j.service!==m.service||j.scope!==m.scope)throw Error('服务响应身份不匹配');return j;
 }
 let resource;
 if(m.op!=='get'&&m.op!=='subscribe')return fail(m,'unsupported');
 if(m.resource==='system.health')resource={data:{online:true,commands:false},ttl_s:30,updated_at:Math.floor(Date.now()/1000)};
 else if(m.resource==='weather.current')resource=await weatherData(m.params);
 else if(m.resource==='market.quote'){
  if(!manual||manual.market!==m.params.market||manual.symbol!==m.params.symbol)return fail(m,'missing');
  resource={data:manual.data,updated_at:manual.at,ttl_s:60};
 }else if(m.resource==='info.text'){
  if(!note)return fail(m,'missing');resource={data:{text:note.text,source:'manual'},updated_at:note.at,ttl_s:86400};
 }else return fail(m,'unsupported');
 return envelope('response',{reply_to:m.id,resource:m.resource,schema:1,params:m.params,status:'succeeded',data:resource.data,updated_at:resource.updated_at,ttl_s:resource.ttl_s,revision:String(resource.updated_at)});
}
function fail(m,code){return envelope('response',{reply_to:m.id,resource:m.resource,schema:m.schema,params:m.params,status:'failed',updated_at:null,ttl_s:0,error:{code,retryable:false}});}
async function message(m,connectionEpoch){
 if(connectionEpoch!==epoch||m.proto!=='ya.data'||m.v!==1||m.session!==session||m.service!==identity.service||m.scope!==identity.scope)return;
 const ch=channel;await ch.send(envelope('ack',{reply_to:m.id}));if(connectionEpoch!==epoch)return;
 if(m.type==='hello'){log('数据协议已协商；等待系统选择活动网关');return;}
 if(m.type==='event'&&m.resource==='system.gateway'&&m.schema===1&&m.data?.active===true){gatewayActive=true;log('设备已选择本网页为活动网关');void flushNote();return;}
 if(m.type!=='request')return;
 if(m.resource!=='system.health'){gatewayActive=true;if(m.resource!=='info.text')void flushNote();}
 if(responses.has(m.id)){await ch.send(responses.get(m.id));return;}
 const providedNote=note;
 let response;try{response=await provide(m);}catch(e){log(e.message);response=fail(m,'offline');}
 if(connectionEpoch!==epoch)return;
 responses.set(m.id,response);if(responses.size>24)responses.delete(responses.keys().next().value);
 await ch.send(response);if(connectionEpoch!==epoch)return;
 if(m.resource==='info.text'&&response.status==='succeeded'&&note===providedNote&&notePending){notePending=false;noteStatus('文字已发送，设备已确认接收');}
 $('activity').textContent='最近提供：'+m.resource;log(m.resource+' → '+response.status);
 if(gatewayActive)void flushNote();
}
export const connection=new Connection({bluetooth:navigator.bluetooth,onState:status,onLog:log,
 auto:()=>$('auto').checked,
 hello:s=>({...envelope('hello',{max_bytes:4096,commands:false}),session:s,service:val('service'),scope:val('scope')}),
 onChannel:(ch,s,hello)=>{channel=ch;session=s;identity={service:hello.service,scope:hello.scope};gatewayActive=false;noteSending=false;epoch++;sequence=0;responses.clear();},
 onMessage:m=>message(m,epoch)});
const manage=(op,extra={})=>connection.manage(op,extra);
function action(id,fn){$(id).onclick=async()=>{busy.add(id);$(id).disabled=true;try{await fn();}catch(e){log(e.message);$('activity').textContent=e.message;}finally{busy.delete(id);$(id).disabled=false;controls();}};}
action('connect',async()=>{if(!navigator.bluetooth||!isSecureContext)throw Error('请用支持蓝牙的 Chrome 打开 HTTPS 或 localhost');await connection.select(val('device-name'));});
action('disconnect',async()=>{connection.disconnect();channel=null;epoch++;});
action('refresh',async()=>{weather=null;await weatherData(coordinates());});
action('manual',async()=>{if(!/^-?\d+(\.\d+)?$/.test(val('price'))||Number(val('price'))<0)throw Error('请输入有效价格');if(val('change')&&!/^-?\d+(\.\d+)?$/.test(val('change')))throw Error('涨跌幅无效');manual={market:val('market'),symbol:val('symbol'),at:Math.floor(Date.now()/1000),data:{price:val('price'),currency:val('currency'),change_pct:val('change')||null,market_state:'unknown',delay_s:null,source:'manual'}};$('quote-state').textContent='手动报价更新时间：'+new Date().toLocaleString()+'，有效期 60 秒';});
action('save-note',async()=>{note={text:val('note'),at:Math.floor(Date.now()/1000)};notePending=true;noteStatus('文字已暂存，等待设备数据连接');await flushNote();});
action('configure',async()=>{
 const plans=[{resource:'weather.current',params:coordinates(),seconds:600},{resource:'market.quote',params:{market:val('market'),symbol:val('symbol')},seconds:15},{resource:'info.text',params:{},seconds:30}];
 const endpoint=val('endpoint');if(endpoint&&!endpoint.startsWith('https://'))throw Error('设备 Wi-Fi 服务地址必须是 HTTPS');
 const cfg={service:val('service'),scope:val('scope'),endpoint,token:val('token'),plans,wake_resources:$('notify').checked?['info.text']:[]};
 if(note){notePending=true;noteStatus('文字已暂存，资源保存并重连后自动发送');}
 await connection.configure(JSON.stringify(cfg));log('配置已入队，设备保存后将自动断开；请等待重连');
});
action('save-power',async()=>{const p=val('profile');const values={mode:Number(val('sleep-mode')),light:Number(val('light')),sync:Number(val('sync')),route:Number(val('route')),ble:$('ble-wake').checked?1:0};for(const [key,value]of Object.entries(values)){if(!Number.isInteger(value))throw Error('设置须为整数');await manage('set',{key:`power_${p}_${key}`,value});}log('休眠设置已保存');});
$('mode').onchange=()=>{$('source-state').textContent=val('mode')==='direct'?'浏览器提供天气和手动数据；无控制能力。':'数据经统一服务取得，与设备 Wi-Fi 使用同一来源。';};$('mode').onchange();
if(!navigator.bluetooth)$('activity').textContent='当前浏览器不支持 Web Bluetooth，请使用 Chrome。';
controls();
