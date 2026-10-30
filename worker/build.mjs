import {cp, mkdir, rm} from 'node:fs/promises';
await rm(new URL('./dist/', import.meta.url), {recursive:true, force:true});
await mkdir(new URL('./dist/companion/', import.meta.url), {recursive:true});
await cp(new URL('./public/', import.meta.url), new URL('./dist/', import.meta.url), {recursive:true});
// Only the public, source-controlled companion assets are deployed.
for (const file of ['index.html','style.css','app.mjs','connection.mjs','protocol.mjs','text-render.mjs','gallery.mjs']) {
  await cp(new URL(`../web/${file}`, import.meta.url), new URL(`./dist/companion/${file}`, import.meta.url));
}
