export const UUID = {service:'98f20001-8f7c-4cb1-9e50-6bf589c91010',rx:'98f20002-8f7c-4cb1-9e50-6bf589c91010',tx:'98f20003-8f7c-4cb1-9e50-6bf589c91010',management:'98f10001-8f7c-4cb1-9e50-6bf589c91010'};
const encoder=new TextEncoder(), decoder=new TextDecoder('utf-8',{fatal:true});
export function validate(value,depth=1){
 if(depth>6)throw Error('JSON exceeds depth 6');
 if(typeof value==='number'&&!Number.isFinite(value))throw Error('Non-finite number');
 if(value&&typeof value==='object')for(const v of Object.values(value))validate(v,depth+(v&&typeof v==='object'?1:0));
}
// Reject duplicate decoded keys before JSON.parse can silently overwrite them.
export function parse(text){
 let i=0;const ws=()=>{while(/\s/.test(text[i]??'')&&i<text.length)i++;};
 const string=()=>{const start=i++;while(i<text.length){if(text[i]==='\\'){i+=2;continue;}if(text[i++]==='"')return JSON.parse(text.slice(start,i));}throw Error('Unterminated string');};
 function value(depth){ws();const c=text[i];if(c==='"'){string();return;}if(c==='{'||c==='['){if(depth>6)throw Error('Depth');i++;const end=c==='{'?'}':']',keys=new Set();ws();if(text[i]===end){i++;return;}for(;;){if(c==='{'){ws();if(text[i]!=='"')throw Error('Key');const k=string();if(keys.has(k))throw Error('Duplicate key');keys.add(k);ws();if(text[i++]!==':')throw Error('Colon');}value(depth+1);ws();if(text[i]===end){i++;break;}if(text[i++]!==',')throw Error('Comma');}return;}const m=text.slice(i).match(/^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/);if(!m)throw Error('Value');i+=m[0].length;}
 value(1);ws();if(i!==text.length)throw Error('Trailing JSON');const j=JSON.parse(text);validate(j);return j;
}
export function encode(message){validate(message);const body=encoder.encode(JSON.stringify(message));if(!body.length||body.length>4096)throw Error('Message exceeds 4096 bytes');const out=new Uint8Array(body.length+4);new DataView(out.buffer).setUint32(0,body.length);out.set(body,4);return out;}
export class Frames{
 constructor(){this.reset();}
 reset(){this.bytes=new Uint8Array();this.at=0;}
 feed(chunk,now=Date.now()){
  if(now-this.at>5000)this.reset();this.at=now;
  const joined=new Uint8Array(this.bytes.length+chunk.length);joined.set(this.bytes);joined.set(chunk,this.bytes.length);this.bytes=joined;
  if(joined.length>4100){this.reset();throw Error('Frame overflow');}
  if(joined.length<4)return null;const n=new DataView(joined.buffer).getUint32(0);
  if(!n||n>4096||joined.length>n+4){this.reset();throw Error('Invalid frame length');}
  if(joined.length!==n+4)return null;
  this.reset();return parse(decoder.decode(joined.subarray(4)));
 }
}
export class ReliableChannel{
 constructor(write,onMessage,onError=()=>{},{ackTimeoutMs=6000}={}){this.ackTimeoutMs=ackTimeoutMs;this.write=write;this.onMessage=onMessage;this.onError=onError;this.frames=new Frames();this.writes=Promise.resolve();this.messages=Promise.resolve();this.pending=new Map();this.closed=false;}
 raw(message){const bytes=encode(message);const run=async()=>{if(this.closed)throw Error('Disconnected');for(let i=0;i<bytes.length;i+=20){if(this.closed)throw Error('Disconnected');await this.write(bytes.slice(i,i+20));}};const task=this.writes.then(run);this.writes=task.catch(()=>{});return task;}
 send(message){if(message.type==='ack')return this.raw(message);const run=async()=>{for(let attempt=0;attempt<3;attempt++){
  let timer,rejectReceipt;const receipt=new Promise((resolve,reject)=>{this.pending.set(message.id,{resolve,reject});rejectReceipt=reject;});receipt.catch(()=>{});
  // Keep the receipt registered for a fast ACK, but start its deadline only
  // after the complete frame has left the serialized GATT write queue.
  try{await this.raw(message);timer=setTimeout(()=>rejectReceipt(Error('ACK timeout')),this.ackTimeoutMs);await receipt;return;}catch(e){if(this.closed||attempt===2)throw e;}finally{clearTimeout(timer);this.pending.delete(message.id);}
 }};const task=this.messages.then(run);this.messages=task.catch(()=>{});return task;}
 receive(chunk){try{const m=this.frames.feed(chunk);if(!m)return;if(m.type==='ack'){this.pending.get(m.reply_to)?.resolve();return;}Promise.resolve(this.onMessage(m)).catch(this.onError);}catch(e){this.onError(e);}}
 close(){this.closed=true;for(const p of this.pending.values())p.reject(Error('Disconnected'));this.pending.clear();this.frames.reset();}
}
