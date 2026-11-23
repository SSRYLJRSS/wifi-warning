/* WiFiWarning v2 — SPA controller */
const $ = (s,r)=> (r||document).querySelector(s);
const $$ = (s,r)=> Array.from((r||document).querySelectorAll(s));
const state = { lang:'zh-CN', view:'overview', config:null, summary:null, stats:null };
window.__LANG='zh-CN';

/* ---------- mock backend (preview when server absent) ---------- */
const MOCK = (function(){
  var s = {
    config:{ version:'2.0.0', settings:{protection_enabled:true, auto_start:true, dark_mode:true, http_port:18765, bypass_timeout_minutes:30, language:'zh-CN' },
      app_groups:[ {id:'g1', name:'娱乐软件', apps:[{name:'Steam',path:'C:/Games/steam.exe'},{name:'哔哩哔哩',path:'C:/Apps/bili.exe'}]},
                    {id:'g2', name:'社交通讯', apps:[{name:'QQ',path:'C:/Program Files/QQ/QQ.exe'}]} ],
      rules:[ {id:'r1', network_type:'wifi', network_id:'Home-5G', network_name:'Home-5G', app_group_id:'g1'},
              {id:'r2', network_type:'wired', network_id:'以太网', network_name:'以太网', app_group_id:'g2'} ] },
    net:{ wifi:{available:true,connected:true,enabled:true,ssid:'Home-5G',signal:4,iface:'Intel Wi-Fi 6E AX211'},
          ethernet:[{id:'以太网',name:'以太网',connected:true,enabled:true}], current:{type:'wifi',id:'Home-5G',name:'Home-5G'} },
    stats:{ blocked_today:12, blocked_week:47, top_app:'Steam', events:[
      {time:'10:24', network:'Home-5G', app:'Steam', result:'blocked'},
      {time:'09:50', network:'Home-5G', app:'QQ', result:'bypass'},
      {time:'09:12', network:'Home-5G', app:'哔哩哔哩', result:'blocked'} ] },
    intercept:{ active:true, mode:'ifeo', elevated:true, desired:3, missing:0, managed_exes:['steam.exe','QQ.exe','bili.exe'], missing_exes:[] }
  };
  function ok(o){ o=o||{}; o.ok=true; return o; }
  return { route:function(m,p,b){
    if(p==='/api/config'&&m==='GET') return ok({config:JSON.parse(JSON.stringify(s.config)), config_path:'C:/WiFiWarning/config.json', backup_path:'C:/WiFiWarning/backups'});
    if(p==='/api/config'&&m==='POST'){ if(b&&b.config) s.config=b.config; return ok({config:s.config}); }
    if(p==='/api/network/summary') return ok(JSON.parse(JSON.stringify(s.net)));
    if(p==='/api/stats') return ok({stats:JSON.parse(JSON.stringify(s.stats))});
    if(p==='/api/intercept/status') return ok(JSON.parse(JSON.stringify(s.intercept)));
    if(p==='/api/network/restore-last'){ s.net.ethernet.forEach(function(a){a.enabled=true;}); return ok({restored:{type:'wifi',name:'Home-5G'}}); }
    if(p==='/api/network/adapter/disable'||p==='/api/network/adapter/enable'){
      var en = p.indexOf('enable')>=0; if(b&&b.type==='wired'){ s.net.ethernet.forEach(function(a){ if(a.id===b.id||!b.id){a.enabled=en; a.connected=en&&a.connected;} }); }
      else { s.net.wifi.enabled=en; if(!en) s.net.wifi.connected=false; } return ok({}); }
    if(p==='/api/intercept/apply') return ok({ok:true, applied:s.intercept.desired||0, already:0, removed:0, error:'', status:JSON.parse(JSON.stringify(s.intercept))});
    if(p==='/api/intercept/restore'){ s.intercept.active=false; s.intercept.desired=0; s.intercept.missing=0; s.intercept.managed_exes=[]; s.intercept.missing_exes=[]; return ok({ok:true, removed:3, status:JSON.parse(JSON.stringify(s.intercept))}); }
    if(p==='/api/apps/browse') return ok({apps:[{name:'新应用',path:'C:/Apps/new.exe'}]});
    return ok({}); } };
})();

const API = {
  req:function(method,path,body){
    return fetch(path,{method:method,headers:{'Content-Type':'application/json'},body:body?JSON.stringify(body):undefined})
      .then(function(r){ if(!r.ok) throw new Error('HTTP '+r.status); return r.json(); })
      .catch(function(e){
        var msg = String((e && e.message) || e);
        var offline = (e && e.name === 'TypeError') || /Failed to fetch|NetworkError|Load failed/i.test(msg);
        if (offline || location.port === '18999' || /[?&]mock=1/.test(location.search)) return MOCK.route(method,path,body);
        throw e;
      });
  },
  get:function(p){ return this.req('GET',p); },
  post:function(p,b){ return this.req('POST',p,b); }
};

/* ---------- utils ---------- */
function toast(msg,type){ var el=$('#toast'); el.textContent=msg; el.className='toast show '+(type||''); clearTimeout(el._t); el._t=setTimeout(function(){ el.className='toast'; },2600); }
function esc(s){ return String(s==null?'':s).replace(/[&<>\"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;'}[c];}); }
function applyI18n(){ $$('[data-i18n]').forEach(function(n){ var k=n.getAttribute('data-i18n'); n.textContent=t(k); });
  var vt=t('v'+state.view.charAt(0).toUpperCase()+state.view.slice(1)); if(Array.isArray(vt)){ $('#viewTitle').textContent=vt[0]; $('#viewSub').textContent=vt[1]; } }
function setLang(l){ state.lang=l; window.__LANG=l; document.documentElement.lang=l; applyI18n(); render(); }

/* ---------- navigation ---------- */
function setView(v){ state.view=v; $$('.nav-item').forEach(function(b){ b.classList.toggle('active', b.getAttribute('data-view')===v); });
  $$('.view').forEach(function(s){ s.classList.toggle('active', s.getAttribute('data-view')===v); });
  var title=t('v'+v.charAt(0).toUpperCase()+v.slice(1)); if(Array.isArray(title)){ $('#viewTitle').textContent=title[0]; $('#viewSub').textContent=title[1]; }
  if(v==='overview'||v==='activity') loadSummary(); if((v==='rules'||v==='groups')&&!state.config) loadConfig(); if(v==='settings') loadConfig(); if(v==='overview'||v==='settings') loadInterceptStatus(); render(); }

/* ---------- data ---------- */
function loadSummary(){ return API.get('/api/network/summary').then(function(r){ state.summary=normalize(r); renderChips(); if(state.view==='overview') renderOverview(); return state.stats?API.get('/api/stats').then(function(s){ state.stats=s.stats||s; renderStats(); }):null; }); }
function normalize(r){ r=r||{}; if(r.wifi||r.ethernet) return {wifi:r.wifi||{}, ethernet:r.ethernet||[], current:r.current||{}}; 
  /* adapt old /api/network/current shape */
  var wifi={available:!!r.wifi_available, connected:r.type==='wifi', enabled:true, ssid:r.type==='wifi'?r.id:'', signal:3, iface:r.interface||''};
  var eth=[]; if(r.wired_available) eth=[{id:r.id||'以太网',name:r.name||r.id||'以太网',connected:r.type==='wired',enabled:true}];
  return {wifi:wifi, ethernet:eth, current:{type:r.type,id:r.id,name:r.name}}; }
function loadConfig(){ return API.get('/api/config').then(function(r){ state.config=r.config||r; renderSettings(); }); }
function loadStats(){ return API.get('/api/stats').then(function(r){ state.stats=r.stats||r; renderStats(); }); }
function loadInterceptStatus(){ return API.get('/api/intercept/status').then(function(r){ state.intercept=r; renderInterceptBadges(); return r; }).catch(function(){ return null; }); }
function interceptState(){ var s=state.intercept||{}; var active=s.active===true; var missing=(s.missing!=null?s.missing:(s.missing_exes?s.missing_exes.length:0)); if(!active) return 'off'; return missing>0?'repair':'on'; }
function interceptApplyMsg(r){ if(!r) return t('opFail'); if(r.ok===false||r.error){ var e=String(r.error||''); if(!e||/cancel|取消|elevat|admin|uac|权限/i.test(e)) return t('interceptCancelled'); return t('interceptApplyFailed',{error:e}); } return t('interceptApplyOk',{count:(r.applied||0)+(r.already||0)}); }
function renderInterceptBadges(){ var st=interceptState(); var label=st==='on'?t('interceptActiveOn'):(st==='repair'?t('interceptNeedsRepair'):t('interceptActiveOff')); var cls='intercept-badge '+(st==='on'?'ok':(st==='repair'?'warn':'off')); var ov=$('#ovInterceptBadge'); if(ov){ ov.textContent=label; ov.className=cls; } var el=$('#interceptStatus'); if(el){ var s=state.intercept||{}; var desired=(s.desired!=null?s.desired:(s.managed_exes?s.managed_exes.length:0)); el.textContent=label+' · '+desired+' exe'; } }

/* ---------- chips + overview ---------- */
function renderChips(){ var n=state.summary||{}; var w=n.wifi||{}; var e=(n.ethernet||[])[0]||null;
  var cw=$('#chipWifi'); cw.className='chip '+(w.connected?'ok':'off'); cw.querySelector('b').textContent = w.connected?w.ssid:(w.available?t('disconnected'):t('noWifi'));
  var ce=$('#chipEth'); ce.className='chip '+((e&&e.connected)?'ok':'off'); ce.querySelector('b').textContent = e?e.name:t('noEth'); }
function setSignal(level){ var box=$('#wifiSignal'); box.innerHTML=''; for(var i=0;i<4;i++){ var b=document.createElement('i'); if(i<level) b.className='on'; box.appendChild(b); } }
function renderOverview(){ var n=state.summary||{}; var w=n.wifi||{}; var eth=n.ethernet||[];
  $('#wifiSsid').textContent = w.connected?w.ssid:(w.available?t('disconnected'):t('noWifi'));
  var wd=$('#wifiDot'); wd.className='dot '+(w.connected?'on':(w.available?'warn':'off'));
  setSignal(w.connected?(w.signal||3):0);
  $('#wifiMeta').textContent = (w.iface||'') + (w.enabled===false?(' · '+t('disabled')):'');
  var wt=$('#wifiToggle'); var wifiOn = w.enabled!==false; wt.textContent = wifiOn?t('disableWifi'):t('enableWifi'); wt.className='btn lg block '+(wifiOn?'danger':''); wt.disabled=!w.available; wt.dataset.enabled=wifiOn?'1':'0';
  var e0=eth[0]; $('#ethName').textContent = e0?e0.name:t('noEth');
  var ed=$('#ethDot'); ed.className='dot '+((e0&&e0.connected)?'on':'off');
  $('#ethDetail').innerHTML = eth.length? eth.map(function(a){ return '<p class="net-meta">'+esc(a.name)+' · '+(a.enabled===false?t('disabled'):(a.connected?t('connected'):t('disconnected')))+'</p>'; }).join('') : '<p class="net-meta">'+t('noEth')+'</p>';
  var et=$('#ethToggle'); var ethOn = e0? e0.enabled!==false : false; et.textContent = ethOn?t('disableEth'):t('enableEth'); et.className='btn lg block '+(ethOn?'danger':''); et.disabled=!e0; et.dataset.enabled=ethOn?'1':'0';
  $('#restoreHint').textContent=''; }
function renderStats(){ if(!state.stats) return; var s=state.stats;
  ['#todayBlocked','#todayBlocked2'].forEach(function(x){ if($(x)) $(x).textContent=s.blocked_today||0; });
  ['#weekBlocked','#weekBlocked2'].forEach(function(x){ if($(x)) $(x).textContent=s.blocked_week||0; });
  ['#topApp','#topApp2'].forEach(function(x){ if($(x)) $(x).textContent=s.top_app||'—'; });
  var rows=$('#logRows'); if(rows&&s.events){ rows.innerHTML = s.events.map(function(e){ var cls = e.result==='blocked'?'blocked':'bypass'; var label = e.result==='blocked'?t('blocked'):(e.result==='bypass'?t('bypassed'):t('netChange')); return '<tr><td>'+esc(e.time)+'</td><td>'+esc(e.network)+'</td><td>'+esc(e.app)+'</td><td><span class="tag '+cls+'">'+label+'</span></td></tr>'; }).join('') || '<tr><td colspan="4" class="muted" style="text-align:center;padding:26px">—</td></tr>'; } }

/* ---------- rules ---------- */
function groupName(id){ var c=state.config||{}; var g=(c.app_groups||[]).filter(function(x){return x.id===id;})[0]; return g?g.name:id; }
function renderRules(){ var c=state.config||{}; var rules=c.rules||[]; var box=$('#rulesList');
  if(!rules.length){ box.innerHTML='<div class="empty">'+t('rulesHint')+'</div>'; return; }
  box.innerHTML = rules.map(function(r){ var cls=r.network_type==='wired'?'eth':'wifi'; var icon=r.network_type==='wired'?'i-cable':'i-wifi';
    return '<div class="rule-card"><div class="rule-flow"><span class="pill '+cls+'"><svg><use href="#'+icon+'"/></svg>'+esc(r.network_name||r.network_id)+'</span><span class="arrow">→</span><span class="pill"><svg><use href="#i-box"/></svg>'+esc(groupName(r.app_group_id))+'</span></div>' +
      '<div class="rule-actions"><span class="tag net">'+t('enabled')+'</span><button class="icon-btn r-del" data-id="'+r.id+'" title="'+t('deleteTxt')+'"><svg><use href="#i-x"/></svg></button></div></div>'; }).join('');
  $$('.r-del').forEach(function(b){ b.onclick=function(){ if(!confirm(t('confirmDel'))) return; state.config.rules=state.config.rules.filter(function(x){return x.id!==b.dataset.id;}); API.post('/api/config',{config:state.config}).then(function(){ toast(t('saveOk'),'ok'); renderRules(); }); }; }); }

/* ---------- groups ---------- */
function renderGroups(){ var c=state.config||{}; var gs=c.app_groups||[]; var box=$('#groupsList');
  if(!gs.length){ box.innerHTML='<div class="empty">'+t('groupsHint')+'</div>'; return; }
  box.innerHTML = gs.map(function(g){ var chips=(g.apps||[]).map(function(a){ return '<span class="app-chip"><span class="ai">'+esc((a.name||'?').slice(0,1).toUpperCase())+'</span>'+esc(a.name)+'<button class="a-del" data-g="'+g.id+'" data-a="'+esc(a.name)+'"><svg><use href="#i-x"/></svg></button></span>'; }).join('');
    return '<div class="card group-card"><div class="group-head"><span class="gname">'+esc(g.name)+'</span><span class="rule-actions"><button class="btn ghost add-app" data-g="'+g.id+'" style="padding:6px 12px">+ '+t('addApp')+'</button><button class="icon-btn g-del" data-g="'+g.id+'"><svg><use href="#i-x"/></svg></button></span></div><div class="app-chips">'+(chips||'<span class="muted">—</span>')+'</div></div>'; }).join('');
  $$('.g-del').forEach(function(b){ b.onclick=function(){ if(!confirm(t('confirmDel'))) return; state.config.app_groups=state.config.app_groups.filter(function(x){return x.id!==b.dataset.g;}); API.post('/api/config',{config:state.config}).then(function(){ toast(t('saveOk'),'ok'); renderGroups(); }); }; });
  $$('.a-del').forEach(function(b){ b.onclick=function(){ var g=state.config.app_groups.filter(function(x){return x.id===b.dataset.g;})[0]; g.apps=g.apps.filter(function(a){return a.name!==b.dataset.a;}); API.post('/api/config',{config:state.config}).then(function(){ renderGroups(); }); }; });
  $$('.add-app').forEach(function(b){ b.onclick=function(){ API.post('/api/apps/browse',{}).then(function(r){ var g=state.config.app_groups.filter(function(x){return x.id===b.dataset.g;})[0]; (r.apps||[]).forEach(function(a){ g.apps.push({name:a.name,original_path:a.original_path||a.path,icon_path:a.icon_path||a.original_path||a.path}); }); API.post('/api/config',{config:state.config}).then(function(){ toast(t('saveOk'),'ok'); renderGroups(); }); }); }; }); }

/* ---------- settings ---------- */
function renderSettings(){ var c=state.config||{}; var s=c.settings||{};
  $('#autoStartSwitch').checked=!!s.auto_start; $('#darkSwitch').checked=s.dark_mode!==false; $('#portInput').value=s.http_port||18765; $('#protectionSwitch').checked=s.protection_enabled!==false;
  $('#pwdMin').value=s.bypass_timeout_minutes||30;
  $('#configPath').textContent='配置: '+(state.configPath||'—'); $('#backupPath').textContent='备份: '+(state.backupPath||'—');
  renderInterceptBadges(); }
function saveConfig(patch){ var c=state.config; Object.keys(patch).forEach(function(k){ if(k==='settings'){ Object.assign(c.settings,patch.settings); } else c[k]=patch[k]; }); return API.post('/api/config',{config:c}).then(function(r){ toast(t('saveOk'),'ok'); }); }

/* ---------- modals ---------- */
function modal(title,bodyHTML,onOk){ var root=$('#modalRoot');
  root.innerHTML='<div class="modal-mask"><div class="modal"><h2>'+esc(title)+'</h2><div class="m-body">'+bodyHTML+'</div><div class="m-actions"><button class="btn ghost m-cancel">'+t('cancel')+'</button><button class="btn primary m-ok">'+t('confirm')+'</button></div></div></div>';
  function close(){ root.innerHTML=''; }
  root.querySelector('.m-cancel').onclick=close; root.querySelector('.modal-mask').onclick=function(e){ if(e.target===root.querySelector('.modal-mask')) close(); };
  root.querySelector('.m-ok').onclick=function(){ if(onOk) onOk(root)===false?0:close(); }; return {close:close,root:root}; }
function openAddRule(){ var n=state.summary||{}; var nets=[]; var w=n.wifi||{}; if(w.connected&&w.ssid) nets.push({type:'wifi',id:w.ssid,name:w.ssid}); (n.ethernet||[]).forEach(function(e){ nets.push({type:'wired',id:e.id,name:e.name}); });
  var groups=(state.config.app_groups||[]);
  var netOpts=nets.map(function(x){ return '<option value="'+x.type+'|'+esc(x.id)+'">'+(x.type==='wifi'?'WiFi · ':'以太网 · ')+esc(x.name)+'</option>'; }).join('') + '<option value="manual|">'+t('manualNet')+'</option>';
  var gOpts=groups.map(function(g){ return '<option value="'+g.id+'">'+esc(g.name)+'</option>'; }).join('');
  modal(t('addRule'), '<label>'+t('pickNetwork')+'</label><select id="mNet">'+netOpts+'</select><input id="mNetManual" placeholder="'+t('manualNet')+'" style="display:none"><label>'+t('pickGroup')+'</label><select id="mGroup">'+gOpts+'</select>', function(root){
    var v=root.querySelector('#mNet').value.split('|'); var type=v[0], id=v[1], name=id; if(id===''){ id=root.querySelector('#mNetManual').value.trim(); name=id; type='wifi'; } if(!id) return false;
    var gid=root.querySelector('#mGroup').value; if(!gid){ toast('请先创建软件组','err'); return false; }
    state.config.rules.push({id:'r'+Date.now(),network_type:type,network_id:id,network_name:name,app_group_id:gid});
    API.post('/api/config',{config:state.config}).then(function(){ toast(t('saveOk'),'ok'); renderRules(); }); });
  var sel=$('#mNet'); sel.onchange=function(){ $('#mNetManual').style.display = sel.value==='manual|'?'block':'none'; }; }
function openAddGroup(){ modal(t('newGroup'), '<label>'+t('groupName')+'</label><input id="mGName" placeholder="例如 游戏">', function(root){ var name=root.querySelector('#mGName').value.trim(); if(!name) return false; state.config.app_groups.push({id:'g'+Date.now(),name:name,apps:[]}); API.post('/api/config',{config:state.config}).then(function(){ toast(t('saveOk'),'ok'); renderGroups(); }); }); }

/* ---------- global render + bindings ---------- */
function render(){ if(state.view==='overview') renderOverview(); else if(state.view==='rules') renderRules(); else if(state.view==='groups') renderGroups(); else if(state.view==='activity') renderStats(); else if(state.view==='settings') renderSettings(); }
function bind(){ $('#nav').addEventListener('click',function(e){ var b=e.target.closest('.nav-item'); if(b) setView(b.getAttribute('data-view')); });
  $('#refreshBtn').onclick=function(){ this.classList.add('spin'); loadSummary().then(loadStats).then(function(){ setTimeout(function(){ $('#refreshBtn').classList.remove('spin'); },400); }); };
  $('#wifiToggle').onclick=function(){ var on=this.dataset.enabled==='1'; var p=on?'/api/network/adapter/disable':'/api/network/adapter/enable'; API.post(p,{type:'wifi'}).then(function(r){ toast(r.ok?t('opOk'):t('opFail'), r.ok?'ok':'err'); loadSummary(); }); };
  $('#ethToggle').onclick=function(){ var on=this.dataset.enabled==='1'; var e=(state.summary.ethernet||[])[0]; var p=on?'/api/network/adapter/disable':'/api/network/adapter/enable'; API.post(p,{type:'wired',id:e?e.id:''}).then(function(r){ toast(r.ok?t('opOk'):t('opFail'), r.ok?'ok':'err'); loadSummary(); }); };
  $('#restoreLastBtn').onclick=function(){ API.post('/api/network/restore-last',{}).then(function(r){ toast(r.ok?t('opOk'):t('opFail'), r.ok?'ok':'err'); loadSummary(); }); };
  $('#protectionSwitch').onchange=function(){ var on=this.checked; saveConfig({settings:{protection_enabled:on}}).then(function(){ API.post(on?'/api/intercept/apply':'/api/intercept/restore',{}).then(function(r){ if(r&&r.status) state.intercept=r.status; else loadInterceptStatus(); renderInterceptBadges(); toast(interceptApplyMsg(r),'ok'); }).catch(function(e){ toast(interceptApplyMsg({ok:false,error:String(e&&e.message||e)}),'err'); }); }); };
  $('#addRuleBtn').onclick=openAddRule; $('#addGroupBtn').onclick=openAddGroup;
  $('#saveSecurity').onclick=function(){ var pwd=$('#pwdInput').value; var patch={settings:{bypass_timeout_minutes:parseInt($('#pwdMin').value)||30}}; if(pwd) patch.settings.bypass_password_hash=pwd; saveConfig(patch); $('#pwdInput').value=''; };
  $('#autoStartSwitch').onchange=function(){ saveConfig({settings:{auto_start:this.checked}}); };
  $('#portInput').onchange=function(){ saveConfig({settings:{http_port:parseInt(this.value)||18765}}); };
  $('#darkSwitch').onchange=function(){ var d=this.checked; document.documentElement.setAttribute('data-theme', d?'dark':'light'); saveConfig({settings:{dark_mode:d}}); };
  $('#applyInterceptBtn').onclick=function(){ var btn=this; btn.disabled=true; API.post('/api/intercept/apply',{}).then(function(r){ if(r&&r.status) state.intercept=r.status; else return loadInterceptStatus(); renderInterceptBadges(); toast(interceptApplyMsg(r),'ok'); }).catch(function(e){ toast(interceptApplyMsg({ok:false,error:String(e&&e.message||e)}),'err'); }).then(function(){ btn.disabled=false; }); };
  $('#restoreAllBtn').onclick=function(){ if(!confirm(t('interceptRestoreConfirm'))) return; API.post('/api/intercept/restore',{}).then(function(r){ if(r&&r.status) state.intercept=r.status; else return loadInterceptStatus(); renderInterceptBadges(); toast(t('opOk'),'ok'); }).catch(function(e){ toast(String(e&&e.message||e),'err'); }); };
  $('#refreshIntercept').onclick=function(){ loadInterceptStatus().then(function(){ toast('已刷新','ok'); }); };
  $('#langToggle').onclick=function(){ setLang(state.lang==='zh-CN'?'en-US':'zh-CN'); }; }

function init(){ API.get('/api/config').then(function(r){ state.config=r.config||r; state.configPath=r.config_path; state.backupPath=r.backup_path;
  if(state.config.settings){ window.__LANG=state.config.settings.language||'zh-CN'; state.lang=window.__LANG; var dark=state.config.settings.dark_mode!==false; document.documentElement.setAttribute('data-theme',dark?'dark':'light'); }
  applyI18n(); bind(); renderSettings(); return loadSummary().then(loadStats); }).then(function(){ render(); var qv=null; try{ qv=new URLSearchParams(location.search).get('view'); }catch(e){} setView(qv||'overview'); }); }
document.addEventListener('DOMContentLoaded', init);