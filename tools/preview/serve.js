// Preview server for checking a rendered video: copy an .mp4 here as test.mp4, then
//   node tools/preview/serve.js   ->   http://localhost:8765 shows frames at 1/4/7/11 s.
const http=require('http'),fs=require('fs'),path=require('path');
const root=__dirname;
http.createServer((q,r)=>{const f=path.join(root,decodeURIComponent(q.url.split('?')[0]==='/'?'/index.html':q.url.split('?')[0]));
fs.stat(f,(e,st)=>{if(e){r.writeHead(404);return r.end();}
const type=f.endsWith('.mp4')?'video/mp4':'text/html';const range=q.headers.range;
if(range){const m=/bytes=(\d+)-(\d*)/.exec(range);const s=+m[1],en=m[2]?+m[2]:st.size-1;r.writeHead(206,{'Content-Type':type,'Content-Range':`bytes ${s}-${en}/${st.size}`,'Accept-Ranges':'bytes','Content-Length':en-s+1});fs.createReadStream(f,{start:s,end:en}).pipe(r);}
else{r.writeHead(200,{'Content-Type':type,'Content-Length':st.size,'Accept-Ranges':'bytes'});fs.createReadStream(f).pipe(r);}});}).listen(8765);
