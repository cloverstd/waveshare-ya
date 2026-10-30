export const WIDTH=432, MAX_HEIGHT=2048, MAX_PACKED=128*1024;
export function wrapText(text,measure,maxWidth=400){
 if(!text||Array.from(text).length>1000||text.includes('\0'))throw Error('请输入 1–1000 个字符');
 const lines=[];
 for(const paragraph of text.replace(/\r\n?/g,'\n').split('\n')){
  let line='';
  const segments=new Intl.Segmenter(undefined,{granularity:'grapheme'}).segment(paragraph);
  for(const {segment}of segments){if(line&&measure(line+segment)>maxWidth){lines.push(line);line='';}line+=segment;}
  lines.push(line);
 }
 if(lines.length*30+32>MAX_HEIGHT)throw Error('排版超过 2048 像素，请减少文字或空行');
 return lines;
}
export async function renderText(text,canvas){
 await document.fonts?.ready;
 let ctx=canvas.getContext('2d',{willReadFrequently:true});
 const font='22px "PingFang SC", "Microsoft YaHei", "Noto Sans CJK SC", "Apple Color Emoji", "Segoe UI Emoji", sans-serif';
 ctx.font=font;const lines=wrapText(text,s=>ctx.measureText(s).width);
 canvas.width=WIDTH;canvas.height=lines.length*30+32;ctx=canvas.getContext('2d',{willReadFrequently:true});
 ctx.fillStyle='#000';ctx.fillRect(0,0,canvas.width,canvas.height);ctx.font=font;ctx.fillStyle='#e5ebf2';ctx.textBaseline='top';
 lines.forEach((line,i)=>ctx.fillText(line,16,16+i*30));
 return {text,height:canvas.height,rgba:ctx.getImageData(0,0,WIDTH,canvas.height).data};
}
export async function packText(rendered){
 const text=new TextEncoder().encode(rendered.text),raw=new Uint8Array(text.length+WIDTH*rendered.height*2);
 raw.set(text);for(let i=0;i<rendered.rgba.length;i+=4){const p=rendered.rgba;const rgb=((p[i]>>3)<<11)|((p[i+1]>>2)<<5)|(p[i+2]>>3),j=text.length+i/2;raw[j]=rgb&255;raw[j+1]=rgb>>8;}
 const stream=new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'));
 const bytes=new Uint8Array(await new Response(stream).arrayBuffer());
 if(bytes.length>MAX_PACKED)throw Error('排版数据超过 128 KiB，请缩短内容');
 const hash=new Uint8Array(await crypto.subtle.digest('SHA-256',bytes));
 return {bytes,height:rendered.height,text_bytes:text.length,sha256:Array.from(hash,x=>x.toString(16).padStart(2,'0')).join('')};
}
export function base64(bytes){return btoa(String.fromCharCode(...bytes));}
