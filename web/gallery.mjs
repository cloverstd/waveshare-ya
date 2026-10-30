// Album manager uses the authenticated data station channel; photos stay local.
export const LIMITS={albums:8,local:9,online:90,packed:128*1024};
export async function jobRequest(request,resource,op,data={},options={}){
 const {job_id}=await request(resource,op,data);if(!Number.isInteger(job_id))throw Error('设备没有返回任务编号');
 const started=Date.now(),wait=options.wait||((ms)=>new Promise(r=>setTimeout(r,ms)));
 for(;;){if(options.signal?.aborted&&op!=='cancel'){await request('gallery.jobs','cancel',{job_id});throw Error('已取消');}
  const job=await request('gallery.jobs','get',{job_id});if(job.state==='done')return job.result||{};if(job.state==='failed')throw Error(job.error||'处理失败');
  if(Date.now()-started>90000){await request('gallery.jobs','cancel',{job_id});throw Error('设备处理超时');}await wait(200);
 }
}
export async function listAll(run,resource,key,data={}){
 const items=[];for(let offset=0;offset<90;){const page=await run(resource,'list',{...data,offset});const part=page[key]||[];items.push(...part);if(items.length>=page.total)return items;if(!part.length)throw Error('分页没有前进');offset+=part.length;}return items;
}
export async function packPhoto(file,{bitmap=createImageBitmap,canvas=()=>document.createElement('canvas')}={}){
 if(!['image/jpeg','image/png'].includes(file.type))throw Error('请选择 JPG 或 PNG 图片');
 const source=await bitmap(file,{imageOrientation:'from-image'});
 try{const scale=Math.min(1,432/source.width,384/source.height),c=canvas();c.width=Math.max(1,Math.floor(source.width*scale));c.height=Math.max(1,Math.floor(source.height*scale));const ctx=c.getContext('2d');ctx.fillStyle='#000';ctx.fillRect(0,0,c.width,c.height);ctx.drawImage(source,0,0,c.width,c.height);
  for(let quality=.85;quality>=.39;quality-=.15){const blob=await new Promise(r=>c.toBlob(r,'image/jpeg',quality));if(blob&&blob.size<=LIMITS.packed){const bytes=new Uint8Array(await blob.arrayBuffer());const digest=new Uint8Array(await crypto.subtle.digest('SHA-256',bytes));return {bytes,sha256:Array.from(digest,x=>x.toString(16).padStart(2,'0')).join('')};}}
  throw Error('压缩后仍超过 128 KiB，请换一张图片');
 }finally{source.close?.();}
}
export async function uploadPhoto(run,file,album,photo='',{signal,onProgress=()=>{},pack=packPhoto}={}){
 const {bytes,sha256}=await pack(file),transfer=crypto.randomUUID();let committed=false;
 try{await run('gallery.transfer','begin',{transfer,album,photo,size:bytes.length,sha256});for(let offset=0;offset<bytes.length;offset+=1536){if(signal?.aborted)throw Error('已取消');const chunk=bytes.slice(offset,offset+1536);const data=btoa(String.fromCharCode(...chunk));await run('gallery.transfer','chunk',{transfer,offset,bytes:data});onProgress(Math.round((offset+chunk.length)/bytes.length*100));}if(signal?.aborted)throw Error('已取消');await run('gallery.transfer','commit',{transfer});committed=true;
 }finally{if(!committed)try{await run('gallery.transfer','cancel',{transfer});}catch{}}
}
const errors={busy:'设备正忙，请稍后重试',storage_unavailable:'内置存储未挂载',catalog_corrupt:'相册目录校验失败，请保留设备数据并检查日志',storage_full_or_error:'内置存储空间不足或写入失败，已有照片保留',album_limit:'最多创建 8 个相册',photo_limit:'每个本地相册最多 9 张',invalid_or_limit:'内容无效或超过相册／在线照片上限',sd_unavailable:'TF 卡未挂载，请先在设备设置中挂载',sd_read_failed:'TF 卡读取失败',manifest_failed:'远程清单加载失败或超过上限，保留原清单',unsupported_image:'图片格式不支持：请通过网页上传转换后的照片',cancelled:'已取消'};
export function initGallery({request,connected,host=document.getElementById('gallery-manager')}){
 if(!host)return {state(){}};
 host.innerHTML=`<div class="section-title"><span>03 / 相册</span><h2>管理相册与照片</h2></div><p class="hint">最多 8 个相册；每个本地相册最多 9 张，在线合计最多 90 张。本地容量以设备剩余空间为准。</p><p id="gallery-status" role="status">连接支持相册的设备后管理。</p><div class="toolbar"><button id="gallery-refresh">刷新相册</button><button id="gallery-cancel" class="secondary" disabled>取消当前操作</button></div><div class="toolbar"><label>相册名称<input id="gallery-name" maxlength="32" placeholder="例如：旅行"></label><label>照片来源<select id="gallery-source"><option value="local">本地照片</option><option value="urls">图片 URL 列表</option><option value="manifest">远程 JSON 清单</option></select></label><label>清单 URL<input id="gallery-manifest" type="url" placeholder="https://example.com/album.json"></label><button id="gallery-create">创建相册</button></div><div id="gallery-albums"></div><div id="gallery-detail" hidden><h3 id="gallery-title"></h3><div class="toolbar"><input id="gallery-rename" maxlength="32" aria-label="相册新名称"><input id="gallery-edit-manifest" type="url" aria-label="清单 URL"><button id="gallery-save">保存名称／清单</button><button id="gallery-sync">刷新远程清单</button><button id="gallery-delete" class="secondary">删除相册</button></div><div id="gallery-local"><label>选择 JPG／PNG 照片<input id="gallery-files" type="file" accept="image/jpeg,image/png" multiple></label><button id="gallery-upload">上传照片</button><progress id="gallery-progress" max="100" value="0"></progress><p class="hint">自动处理方向、缩放和压缩。替换照片时只选择一张。</p><div class="toolbar"><input id="gallery-sd-path" value="photos" aria-label="TF 卡目录"><button id="gallery-sd-list">读取 TF 卡目录</button></div><div id="gallery-sd-files"></div><button id="gallery-sd-next" class="secondary">下一批 TF 文件</button></div><div id="gallery-urls"><label>图片 URL<input id="gallery-url" type="url"></label><label>缩略图 URL（可选）<input id="gallery-thumb" type="url"></label><button id="gallery-add-url">保存图片 URL</button></div><p id="gallery-editing" class="hint"></p><div id="gallery-photos"></div></div>`;
 const $=id=>host.querySelector(`#gallery-${id}`);let albums=[],chosen=null,editing='',active=false,capable=false,abort=null,sdOffset=0,epoch=0;
 const message=s=>$('status').textContent=s;
 const run=(resource,op,data={},opts={})=>jobRequest(request,resource,op,data,{signal:abort?.signal,...opts});
 function controls(){for(const el of host.querySelectorAll('button,input,select'))el.disabled=active||!connected()||!capable;$('cancel').disabled=!active;}
 function button(parent,title,action){const b=document.createElement('button');b.className='secondary';b.textContent=title;b.onclick=()=>operate(action);parent.append(b);return b;}
 async function operate(fn){if(active||!connected()||!capable)return;active=true;abort=new AbortController();const started=epoch;controls();try{await fn();if(started===epoch)message('操作已完成');}catch(e){message(errors[e.message]||e.message);}finally{active=false;abort=null;controls();}}
 function edit(p){editing=p.id;$('editing').textContent=`正在替换图片 ${p.id}；保存后生效。`;$('url').value=p.url||'';$('thumb').value=p.thumbnail_url||'';}
 async function detail(){if(!chosen)return;$('detail').hidden=false;$('title').textContent=chosen.name;$('rename').value=chosen.name;$('edit-manifest').value=chosen.manifest||'';$('edit-manifest').hidden=chosen.source!=='manifest';$('sync').hidden=chosen.source!=='manifest';$('local').hidden=chosen.source!=='local';$('urls').hidden=chosen.source!=='urls';$('editing').textContent='';editing='';
  const photos=await listAll(run,'gallery.photos','photos',{album:chosen.id});$('photos').replaceChildren();photos.forEach((p,i)=>{const row=document.createElement('div');row.className='album-row';const label=document.createElement('span');label.textContent=`${i+1}. ${p.url||p.id}`;row.append(label);if(chosen.source!=='manifest'){button(row,'替换',async()=>edit(p));button(row,'上移',async()=>{await run('gallery.photos','move',{album:chosen.id,photo:p.id,position:Math.max(0,i-1)});await detail();});button(row,'下移',async()=>{await run('gallery.photos','move',{album:chosen.id,photo:p.id,position:Math.min(photos.length-1,i+1)});await detail();});button(row,'删除',async()=>{if(!confirm('删除这张设备照片？TF 卡原文件会保留。'))return;await run('gallery.photos','delete',{album:chosen.id,photo:p.id});await reload();});}$('photos').append(row);});
 }
 async function reload(){const selected=chosen?.id;albums=await listAll(run,'gallery.albums','albums');$('albums').replaceChildren();albums.forEach((a,i)=>{const row=document.createElement('div');row.className='album-row';button(row,`${a.name} · ${a.source==='local'?'本地':'在线'} · ${a.count} 张`,async()=>{chosen=a;await detail();});button(row,'上移',async()=>{await run('gallery.albums','move',{album:a.id,position:Math.max(0,i-1)});await reload();});button(row,'下移',async()=>{await run('gallery.albums','move',{album:a.id,position:Math.min(albums.length-1,i+1)});await reload();});$('albums').append(row);});chosen=albums.find(a=>a.id===selected)||null;if(chosen)await detail();else $('detail').hidden=true;}
 function action(id,fn){$(id).onclick=()=>operate(fn);}
 action('refresh',reload);action('create',async()=>{await run('gallery.albums','create',{name:$('name').value.trim(),source:$('source').value,manifest:$('manifest').value.trim()});await reload();});
 action('save',async()=>{await run('gallery.albums','update',{album:chosen.id,name:$('rename').value.trim(),manifest:$('edit-manifest').value.trim()});await reload();});
 action('delete',async()=>{if(!confirm(`删除相册“${chosen.name}”及其中的设备照片？`))return;await run('gallery.albums','delete',{album:chosen.id});chosen=null;await reload();});
 action('sync',async()=>{await run('gallery.albums','refresh',{album:chosen.id});await reload();});
 action('add-url',async()=>{await run('gallery.photos',editing?'update':'add',{album:chosen.id,photo:editing,url:$('url').value.trim(),thumbnail_url:$('thumb').value.trim()});$('url').value='';$('thumb').value='';await reload();});
 action('upload',async()=>{const files=[...$('files').files];if(!files.length)throw Error('请先选择照片');if(editing&&files.length!==1)throw Error('替换时请选择一张照片');for(const file of files){message(`正在上传 ${file.name}，传输完成后等待设备保存…`);await uploadPhoto(run,file,chosen.id,editing,{signal:abort.signal,onProgress:n=>$('progress').value=n});}await reload();});
 async function sd(){const dir=$('sd-path').value.replace(/\/$/,'');const {files}=await run('gallery.sd','list',{path:dir,offset:sdOffset});$('sd-files').replaceChildren();for(const name of files||[]){const path=`${dir}/${name}`;button($('sd-files'),`导入 ${name}`,async()=>{await run('gallery.sd','import',{album:chosen.id,photo:editing,path});await reload();});}message(files?.length?'选择照片导入；目录可在上方输入路径。':'本批没有文件');}
 action('sd-list',async()=>{sdOffset=0;await sd();});action('sd-next',async()=>{sdOffset+=8;await sd();});$('cancel').onclick=()=>abort?.abort();
 controls();return {state(ready){capable=ready;if(!connected()){epoch++;abort?.abort();}controls();}};
}
