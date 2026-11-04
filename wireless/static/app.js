'use strict';
const $ = selector => document.querySelector(selector);
let mode = 'login', user = null, devices = [], selected = null, live = null;
let source = null, rotation = 0, pendingReady = null, reconnectTimer = null, uploading = false;
const canvas = $('#preview'), ctx = canvas.getContext('2d', {willReadFrequently:true});
const states = {none:'尚未上传',queued:'等待设备接收',sent:'已发送，等待设备确认',applied:'已保存并显示',failed:'设备处理失败'};

function toast(text, error=false) {
  $('#toast').textContent = text; $('#toast').classList.toggle('error', error); $('#toast').hidden = false;
  clearTimeout(toast.timer); toast.timer = setTimeout(() => $('#toast').hidden = true, 6000);
}
async function api(path, data) {
  const response = await fetch(path, {method:data === undefined?'GET':'POST',headers:data===undefined?{}:{'Content-Type':'application/json'},body:data===undefined?undefined:JSON.stringify(data)});
  const result = await response.json(); if (!response.ok) throw new Error(result.error || '请求失败'); return result;
}
function draw() {
  ctx.fillStyle='#101c16';ctx.fillRect(0,0,240,280);
  if(!source){ctx.textAlign='center';ctx.fillStyle='#b7f5b3';ctx.font='600 23px system-ui';ctx.fillText('Hello, lineless.',120,122);ctx.fillStyle='#91a79b';ctx.font='11px system-ui';ctx.fillText('YOUR NEXT CARD STARTS HERE',120,149);return;}
  const quarter=rotation%180!==0,w=quarter?source.height:source.width,h=quarter?source.width:source.height;
  const zoom=Number($('#zoom').value),scale=Math.max(240/w,280/h)*zoom;
  ctx.save();ctx.translate(120+Number($('#offsetX').value),140+Number($('#offsetY').value));ctx.rotate(rotation*Math.PI/180);ctx.scale(scale,scale);ctx.drawImage(source,-source.width/2,-source.height/2);ctx.restore();$('#zoomValue').textContent=zoom.toFixed(1)+'×';
}
function rgb565() {
  const rgba=ctx.getImageData(0,0,240,280).data, output=new Uint8Array(134400);
  for(let i=0,j=0;i<rgba.length;i+=4){const pixel=((rgba[i]>>3)<<11)|((rgba[i+1]>>2)<<5)|(rgba[i+2]>>3);output[j++]=pixel>>8;output[j++]=pixel&255;}return output;
}
function crc32(bytes){let c=0xffffffff;for(const b of bytes){c^=b;for(let bit=0;bit<8;bit++)c=(c>>>1)^((c&1)?0xedb88320:0);}return(c^0xffffffff)>>>0;}
function renderDevices() {
  $('#devices').replaceChildren();$('#deviceCount').textContent=String(devices.length);$('#emptyDevices').hidden=devices.length>0;
  if(!devices.some(d=>d.id===selected))selected=devices[0]?.id||null;
  for(const device of devices){const button=document.createElement('button');button.className='device-item'+(device.id===selected?' selected':'');const name=document.createElement('b');name.textContent=device.id;const status=document.createElement('span');status.textContent=(device.model==='simulator'?'模拟设备 · ':'')+(device.online?'● 在线 · ':'○ 离线 · ')+(states[device.image_state]||device.image_state);button.append(name,status);button.addEventListener('click',()=>{selected=device.id;renderDevices();});$('#devices').append(button);}
  const current=devices.find(d=>d.id===selected);$('#selectedName').textContent=current?current.id:'先在左侧选择一台设备';$('#upload').disabled=uploading||!source||!current||live?.readyState!==WebSocket.OPEN;
  $('#deliveryTitle').textContent=current?(states[current.image_state]||current.image_state):'等待选择设备';$('#deliveryDetail').textContent=current?(current.online?'设备已建立图片传输连接。':'设备离线时仍可保存图片，重连后自动发送。'):'图片会保存在设备中，断电重启也能恢复。';
}
function connectLive(){
  clearTimeout(reconnectTimer);if(!user)return;
  live=new WebSocket(`${location.protocol==='https:'?'wss':'ws'}://${location.host}/ws/browser`);
  live.onopen=()=>{$('#liveState').textContent='● 实时连接';renderDevices();};
  live.onmessage=event=>{const data=JSON.parse(event.data);if(data.type==='devices'){devices=data.devices;renderDevices();}else if(data.type==='image_ready'){pendingReady?.resolve(data.request_id);pendingReady=null;}else if(data.type==='image_queued'){toast('图片已保存，正在等待设备确认显示。');}else if(data.type==='error'){pendingReady?.reject(new Error(data.message));pendingReady=null;toast(data.message,true);}};
  live.onclose=event=>{$('#liveState').textContent='连接已断开';pendingReady?.reject(new Error('连接已断开，请重新上传'));pendingReady=null;renderDevices();if(user&&event.code!==1008)reconnectTimer=setTimeout(connectLive,2500);if(event.code===1008){user=null;showWorkspace();toast('登录已过期，请重新登录',true);}};
}
function showWorkspace(){
  $('#auth').hidden=!!user;$('#dashboard').hidden=!user;$('#logout').hidden=!user;$('#workspace').textContent=user?user.tenant_name:'让内容，自由抵达。';
  if(user)connectLive();else{clearTimeout(reconnectTimer);live?.close();devices=[];selected=null;}
}
document.querySelectorAll('[data-mode]').forEach(button=>button.addEventListener('click',()=>{mode=button.dataset.mode;document.querySelectorAll('[data-mode]').forEach(b=>b.classList.toggle('active',b===button));$('#tenantField').hidden=mode!=='signup';$('#tenantName').required=mode==='signup';$('#authTitle').textContent=mode==='signup'?'为你的名片留一个位置':'欢迎回来';$('#authSubmit').textContent=mode==='signup'?'创建工作空间 →':'进入工作空间 →';$('#password').autocomplete=mode==='signup'?'new-password':'current-password';}));
$('#authForm').addEventListener('submit',async event=>{event.preventDefault();$('#authSubmit').disabled=true;try{const data=await api('/api/auth/'+mode,{email:$('#email').value,password:$('#password').value,tenant_name:$('#tenantName').value});user=data.user;$('#password').value='';showWorkspace();}catch(error){toast(error.message,true);}finally{$('#authSubmit').disabled=false;}});
$('#logout').addEventListener('click',async()=>{try{await api('/api/auth/logout',{});user=null;showWorkspace();}catch(error){toast(error.message,true);}});
$('#claimForm').addEventListener('submit',async event=>{event.preventDefault();try{const result=await api('/api/devices/claim',{device_id:$('#deviceId').value,claim_code:$('#claimCode').value});devices=result.devices;selected=$('#deviceId').value.trim().toLowerCase();$('#claimCode').value='';renderDevices();toast('设备已认领，正在通过 MQTT 分配工作空间。');}catch(error){toast(error.message,true);}});
$('#imageFile').addEventListener('change',async event=>{const file=event.target.files[0];if(!file)return;if(file.size>20*1024*1024){toast('请选择小于 20 MB 的图片',true);return;}try{source?.close();source=await createImageBitmap(file);draw();renderDevices();}catch{toast('无法读取这张图片',true);}});
['zoom','offsetX','offsetY'].forEach(id=>$('#'+id).addEventListener('input',draw));
$('#rotate').addEventListener('click',()=>{rotation=(rotation+90)%360;draw();});
$('#resetCrop').addEventListener('click',()=>{rotation=0;$('#zoom').value='1';$('#offsetX').value='0';$('#offsetY').value='0';draw();});
$('#upload').addEventListener('click',async()=>{
  if(uploading||!selected||!source||live?.readyState!==WebSocket.OPEN)return;
  uploading=true;$('#upload').disabled=true;$('#progress').hidden=false;$('#progress').value=0;
  const socket=live,deviceId=selected;
  try{const bytes=rgb565();const ready=new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pendingReady=null;reject(new Error('上传初始化超时'));},10000);pendingReady={resolve:id=>{clearTimeout(timer);resolve(id);},reject:error=>{clearTimeout(timer);reject(error);}};});socket.send(JSON.stringify({type:'image_begin',device_id:deviceId,size:bytes.length,crc32:crc32(bytes)}));const requestId=await ready;for(let offset=0;offset<bytes.length;offset+=2048){if(socket.readyState!==WebSocket.OPEN)throw new Error('传输连接已断开');while(socket.bufferedAmount>65536){await new Promise(resolve=>setTimeout(resolve,20));if(socket.readyState!==WebSocket.OPEN)throw new Error('传输连接已断开');}socket.send(bytes.subarray(offset,offset+2048));$('#progress').value=Math.min(100,100*(offset+2048)/bytes.length);}socket.send(JSON.stringify({type:'image_commit',request_id:requestId}));}catch(error){toast(error.message,true);}finally{uploading=false;$('#progress').hidden=true;renderDevices();}
});
draw();
Promise.all([api('/api/auth/session'),api('/api/health')]).then(([session,health])=>{user=session.user;$('#serverAddress').textContent=health.public_base_url;showWorkspace();}).catch(error=>toast(error.message,true));
