import {test} from 'node:test';import assert from 'node:assert/strict';
import {Frames,encode,parse,ReliableChannel} from '../web/protocol.mjs';
test('UTF-8 frame fragmented one byte at a time',()=>{const f=new Frames(),m={text:'天气预报',v:1};const b=encode(m);let out;for(const byte of b)out=f.feed(Uint8Array.of(byte));assert.deepEqual(out,m);});
test('bounds, duplicate decoded keys, malformed and depth',()=>{assert.throws(()=>parse('{"x":1,"\\u0078":2}'));assert.throws(()=>parse('{"a":[[[[[[1]]]]]]}'));assert.throws(()=>parse('{"n":1e999}'));assert.throws(()=>encode({text:'x'.repeat(4096)}));assert.throws(()=>new Frames().feed(Uint8Array.of(0,0,0,0)));assert.throws(()=>new Frames().feed(Uint8Array.of(0,0,16,1)));});
test('partial timeout resets connection assembly',()=>{const f=new Frames(),b=encode({a:1});f.feed(b.slice(0,3),100);assert.deepEqual(f.feed(b,5200),{a:1});});
test('ACK and simultaneous bidirectional messages do not deadlock',async()=>{let a,b,ar=[],br=[];a=new ReliableChannel(async bytes=>{b.receive(bytes);},async m=>{ar.push(m);await a.send({type:'ack',reply_to:m.id});});b=new ReliableChannel(async bytes=>{a.receive(bytes);},async m=>{br.push(m);await b.send({type:'ack',reply_to:m.id});});await Promise.all([a.send({id:'a',type:'request',text:'one'}),b.send({id:'b',type:'request',text:'two'})]);assert.equal(ar.length,1);assert.equal(br.length,1);a.close();b.close();});
test('closing rejects an outstanding request',async()=>{const ch=new ReliableChannel(async()=>{},()=>{});const task=ch.send({id:'a',type:'request'});await new Promise(r=>setTimeout(r,10));ch.close();await assert.rejects(task);});
test('ACK deadline excludes time spent writing slow fragments',async()=>{
 let a,b,received=0;
 a=new ReliableChannel(async bytes=>{await new Promise(r=>setTimeout(r,2));b.receive(bytes);},()=>{},()=>{},{ackTimeoutMs:20});
 b=new ReliableChannel(async bytes=>a.receive(bytes),async m=>{received++;await b.send({type:'ack',reply_to:m.id});});
 await a.send({type:'request',id:'slow',text:'x'.repeat(800)});assert.equal(received,1);a.close();b.close();
});
