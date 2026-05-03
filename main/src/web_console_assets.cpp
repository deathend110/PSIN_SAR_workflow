#include "workflow/web/web_console_server.hpp"

namespace workflow::web::assets
{
    // 返回 Web Console 的静态 HTML 骨架。
    // 页面本身很薄，真正的状态驱动逻辑在下方内嵌的 app.js 中。
    const std::string &IndexHtml()
    {
        static const std::string html = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>PSIN SAR Web Console</title>
  <link rel="stylesheet" href="/app.css" />
</head>
<body>
  <div class="page-shell">
    <header class="hero">
      <div>
        <div class="eyebrow">Edge AI Web Console</div>
        <h1>SAR Workflow Control Station</h1>
      </div>
      <div class="hero-meta">
        <span class="badge success">Single Device / Single Active Job</span>
        <span class="badge">HTTP + SSE</span>
        <button class="icon-button" id="settings-toggle" type="button" aria-expanded="false">&#9881; Settings</button>
        <button class="shutdown-button" id="shutdown-web" type="button" title="Shutdown Web Console" aria-label="Shutdown Web Console">&#x23FB;</button>
      </div>
    </header>

    <main class="grid">
      <section class="card controls">
        <h2>Mode Selection</h2>
        <div class="button-grid" id="workflow-buttons"></div>
        <h2>Patch / Flight Mode</h2>
        <div class="button-grid" id="patch-buttons"></div>
        <h2>Output Mode</h2>
        <div class="button-grid button-grid-2" id="output-buttons"></div>
        <h2>Run Control</h2>
        <div class="button-grid button-grid-2" id="command-buttons"></div>
      </section>

      <section class="card sources">
        <div class="section-header">
          <h2>Input Source Loader</h2>
          <button class="primary" id="reload-sources">Reload Sources</button>
        </div>
        <div class="sources-layout">
          <div class="source-list" id="source-list"></div>
          <div class="preview-panel">
            <div class="preview-title">Preview</div>
            <img id="source-preview" alt="source preview" />
            <div class="preview-empty" id="preview-empty">Preview is available for inference images only.</div>
          </div>
        </div>
      </section>

      <section class="card status">
        <h2>Live Status</h2>
        <div class="kv-grid" id="status-grid"></div>
      </section>

      <section class="card settings" id="settings-panel" hidden>
        <div class="section-header">
          <h2>Settings Workspace</h2>
          <button class="primary" id="save-settings">Apply In-Memory Settings</button>
        </div>
        <div class="settings-grid">
          <div class="settings-column">
            <h3>Inference Parameters</h3>
            <div id="infer-settings"></div>
          </div>
          <div class="settings-column">
            <h3>RD Parameters</h3>
            <div id="rd-settings"></div>
          </div>
          <div class="settings-column">
            <h3>Reserved Flight Parameters</h3>
            <div id="flight-settings"></div>
          </div>
        </div>
      </section>

      <section class="card logs">
        <div class="section-header">
          <h2>Event Stream</h2>
          <button class="secondary" id="clear-logs">Clear</button>
        </div>
        <pre id="log-box"></pre>
      </section>

      <section class="card reserved">
        <h2>Reserved Endpoints</h2>
        <div class="reserved-grid">
          <button class="reserved-item" data-manual-key="w">W / Forward</button>
          <button class="reserved-item" data-manual-key="a">A / Left</button>
          <button class="reserved-item" data-manual-key="s">S / Backward</button>
          <button class="reserved-item" data-manual-key="d">D / Right</button>
        </div>
      </section>
    </main>
  </div>
  <script src="/app.js"></script>
</body>
</html>)HTML";
        return html;
    }

    // 返回单文件 CSS 资源，控制台前端当前不依赖额外静态目录或打包产物。
    const std::string &AppCss()
    {
        static const std::string css = R"CSS(:root{
  --bg:#edf2f7;
  --card:#ffffff;
  --line:#d7dee7;
  --text:#0f172a;
  --muted:#5b6778;
  --accent:#0f766e;
  --accent-soft:#ccfbf1;
  --warn:#92400e;
  --warn-soft:#fef3c7;
  --danger:#991b1b;
  --danger-soft:#fee2e2;
}
*{box-sizing:border-box}
body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:var(--bg);color:var(--text)}
.page-shell{max-width:1680px;margin:0 auto;padding:24px}
.hero{display:flex;justify-content:space-between;align-items:center;gap:16px;background:var(--card);border:1px solid var(--line);border-radius:24px;padding:24px 28px;box-shadow:0 10px 26px rgba(15,23,42,.06)}
.hero h1{margin:6px 0 0;font-size:30px}
.eyebrow{text-transform:uppercase;letter-spacing:.28em;font-size:12px;color:var(--muted)}
.hero-meta{display:flex;gap:10px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;padding:10px 14px;border-radius:999px;border:1px solid var(--line);background:#f8fafc;color:#334155;font-size:13px}
.badge.success{background:#ecfdf5;border-color:#86efac;color:#166534}
.grid{display:grid;grid-template-columns:320px minmax(0,1fr) 360px;gap:18px;margin-top:18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:22px;padding:18px;box-shadow:0 6px 16px rgba(15,23,42,.04)}
.controls{display:flex;flex-direction:column;gap:12px}
.controls h2,.sources h2,.status h2,.settings h2,.logs h2,.reserved h2{margin:0 0 10px;font-size:17px}
.button-grid{display:grid;grid-template-columns:1fr;gap:10px}
.button-grid-2{grid-template-columns:1fr 1fr}
button{font:inherit;cursor:pointer}
.choice,.command,.secondary,.primary,.reserved-item,.icon-button,.shutdown-button{border-radius:14px;border:1px solid var(--line);background:#f8fafc;padding:12px 14px;color:#1f2937;transition:.15s}
.choice.active{border-color:#14b8a6;background:var(--accent-soft);color:#134e4a}
.command.start{background:#ecfdf5;border-color:#86efac;color:#166534}
.command.pause{background:#fffbeb;border-color:#fcd34d;color:#92400e}
.command.stop{background:#fef2f2;border-color:#fca5a5;color:#991b1b}
.primary{background:#0f766e;border-color:#0f766e;color:#fff}
.secondary{background:#fff}
.icon-button{background:#fff;color:#0f172a;white-space:nowrap}
.shutdown-button{width:48px;height:48px;padding:0;border-radius:999px;background:#facc15;border-color:#eab308;color:#713f12;display:inline-flex;align-items:center;justify-content:center;font-size:22px;line-height:1}
.shutdown-button:hover{background:#fde047}
.shutdown-button:disabled{cursor:default;opacity:.7}
.note{padding:12px;border-radius:14px;background:var(--warn-soft);color:var(--warn);font-size:13px}
.section-header{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:10px}
.sources{grid-column:2/3}
.sources-layout{display:grid;grid-template-columns:1fr 320px;gap:14px}
.source-list{display:grid;gap:10px;max-height:540px;overflow:auto}
.source-item{border:1px solid var(--line);border-radius:16px;padding:12px;background:#f8fafc}
.source-item.active{border-color:#14b8a6;background:var(--accent-soft)}
.source-item .meta{display:flex;justify-content:space-between;gap:10px;margin-bottom:6px}
.source-item .title{font-weight:600}
.source-item .detail{font-size:13px;color:var(--muted)}
.pill{display:inline-block;padding:4px 8px;border-radius:999px;background:#e2e8f0;font-size:11px;text-transform:uppercase;letter-spacing:.14em}
.preview-panel{border:1px dashed var(--line);border-radius:16px;padding:14px;display:flex;flex-direction:column;gap:10px;min-height:280px;background:#f8fafc}
.preview-title{font-weight:600}
#source-preview{width:100%;border-radius:14px;display:none;border:1px solid var(--line)}
.preview-empty{font-size:13px;color:var(--muted);line-height:1.5}
.status{grid-column:3/4}
.kv-grid{display:grid;grid-template-columns:minmax(128px,160px) minmax(0,1fr);gap:8px 12px;align-items:start;font-family:Consolas,monospace;font-size:13px}
.kv-grid div{padding:8px 0;border-bottom:1px solid var(--line)}
.kv-grid .status-key{color:var(--muted);font-weight:600;letter-spacing:.04em}
.kv-grid .status-value{text-align:right;white-space:normal;word-break:break-word;overflow-wrap:anywhere}
.settings{grid-column:2/4}
.settings[hidden]{display:none}
.settings-grid{display:grid;grid-template-columns:repeat(3, minmax(0,1fr));gap:16px}
.settings-column{border:1px solid var(--line);border-radius:16px;padding:14px;background:#f8fafc}
.settings-column h3{margin:0 0 12px;font-size:15px}
.form-field{display:grid;grid-template-columns:1fr;gap:6px;margin-bottom:10px}
.form-field label{font-size:12px;color:var(--muted)}
.form-field input{height:38px;border-radius:12px;border:1px solid var(--line);padding:0 12px;background:#fff}
.logs{grid-column:1/3}
#log-box{height:240px;margin:0;overflow:auto;background:#0f172a;color:#dbeafe;border-radius:14px;padding:14px;font:12px/1.5 Consolas,monospace}
.reserved{grid-column:3/4}
.reserved-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.reserved-item{background:#fff7ed;border-color:#fdba74;color:#9a3412}
@media (max-width: 1320px){
  .grid{grid-template-columns:1fr}
  .sources,.status,.settings,.logs,.reserved{grid-column:auto}
  .sources-layout,.settings-grid{grid-template-columns:1fr}
}
)CSS";
        return css;
    }

    // 返回整个控制台前端脚本。
    // 服务端会把这段字符串直接作为 `/app.js` 的响应体返回给浏览器。
    const std::string &AppJs()
    {
        static const std::string js = R"JS((function(){
const workflowChoices=[["rd","RD only"],["infer","Inference only"]];
const patchChoices=[["auto_snake","auto_snake"],["manual_flight","manual_flight"],["debug_raster","debug_raster"]];
const outputChoices=[["hdmi","hdmi"],["png","png"]];
const inferFields=[
  ["infer.sys.device","sys.device"],["infer.sys.run_backend","sys.run_backend"],["infer.sys.mmuMode","sys.mmuMode"],
  ["infer.sys.speedMode","sys.speedMode"],["infer.sys.compressFtmp","sys.compressFtmp"],["infer.sys.ocm_option","sys.ocm_option"],
  ["infer.sys.profile","sys.profile"],["infer.input.sar_img_dir","input.sar_img_dir"],["infer.input.sar_img_ext","input.sar_img_ext"],
  ["infer.input.recursive","input.recursive"],["infer.pipeline.patch.mode","pipeline.patch.mode"],["infer.pipeline.patch.patch_size","pipeline.patch.patch_size"],
  ["infer.pipeline.patch.stride","pipeline.patch.stride"],["infer.pipeline.debug.stride_x_px","pipeline.debug.stride_x_px"],["infer.pipeline.debug.stride_y_px","pipeline.debug.stride_y_px"],["infer.pipeline.output_wait_ms","pipeline.output_wait_ms"],["infer.display.width","display.width"],
  ["infer.display.height","display.height"],["infer.display.fps","display.fps"],["infer.output.mode","output.mode"],["infer.output.dir","output.dir"],["infer.output.overwrite","output.overwrite"]
];
const rdFields=[
  ["rd.execution_mode","rd.execution_mode"],["rd.echo_dir","rd.echo_dir"],["rd.echo_ext","rd.echo_ext"],["rd.output_dir","rd.output_dir"],
  ["rd.scratch_dir","rd.scratch_dir"],["rd.column_tile","rd.column_tile"],["rd.row_tile","rd.row_tile"],["rd.memory_limit_mb","rd.memory_limit_mb"],
  ["rd.prefer_memory_pipeline","rd.prefer_memory_pipeline"],["rd.keep_scratch","rd.keep_scratch"],["rd.overwrite","rd.overwrite"]
];
const flightFields=[
  ["flight.manual_step_px","manual_step_px (manual uses stride)"],["flight.boost_step_px","boost_step_px (ignored by cursor mode)"],["flight.trigger_distance_px","trigger_distance_px (ignored by cursor mode)"],
  ["flight.cache_grid_px","cache_grid_px (legacy)"],["flight.path_overlay","path_overlay"],["flight.control_bindings","control_bindings"]
];

// 整个前端的唯一全局状态容器：
// 保存后端状态快照、设置缓存、source 列表以及 SSE 连接状态。
const app={ state:null, settings:{}, sources:[], selectedSource:"", eventSource:null, settingsVisible:false, shutdownInProgress:false, connectionClosedNotified:false };

// 将一条日志写到页面底部的 Event Stream 面板。
function logLine(message){
  const box=document.getElementById("log-box");
  const stamp=new Date().toLocaleTimeString();
  box.textContent += `[${stamp}] ${message}\n`;
  box.scrollTop=box.scrollHeight;
}

// 发起 GET 请求并按 JSON 解析，供只读接口调用。
async function getJson(url){
  const response=await fetch(url);
  return response.json();
}

// 发起 POST JSON 请求，并把常见失败模式统一封装成标准返回对象。
async function postJson(url, body){
  const response=await fetch(url,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(body||{})});
  const statusSuffix=Number.isInteger(response.status) ? ` (HTTP ${response.status})` : "";
  let responseText="";
  try{
    responseText=await response.text();
  }catch(error){
    return {ok:false,code:"response_read_failed",message:`${url} response read failed${statusSuffix}: ${error.message}`};
  }
  if(!responseText.trim()){
    return {ok:false,code:"empty_response",message:`${url} returned an empty response body${statusSuffix}.`};
  }
  try{
    return JSON.parse(responseText);
  }catch(error){
    return {ok:false,code:"invalid_json_response",message:`${url} returned malformed JSON${statusSuffix}: ${error.message}`};
  }
}

// 渲染一组单选按钮，例如 workflow / patch / output 区域。
function renderButtons(containerId, choices, selected, handler, className){
  const root=document.getElementById(containerId);
  root.innerHTML="";
  choices.forEach(([value,label])=>{
    const btn=document.createElement("button");
    btn.className=`choice ${className||""} ${selected===value?"active":""}`;
    btn.textContent=label;
    btn.onclick=()=>handler(value);
    root.appendChild(btn);
  });
}

// 将 `/api/state` 的状态快照铺平成右侧状态键值面板。
function renderStatus(){
  if(!app.state){return;}
  const pairs=[
    ["CONTROL_STATE",app.state.control_state],
    ["WORKFLOW_MODE",app.state.workflow_mode],
    ["PATCH_MODE",app.state.patch_mode],
    ["OUTPUT_MODE",app.state.output_mode],
    ["CURRENT_STAGE",app.state.current_stage || "-"],
    ["CURRENT_ITEM",app.state.current_item || "-"],
    ["CURRENT_INDEX",String(app.state.current_index || 0)],
    ["TOTAL_COUNT",String(app.state.total_count || 0)],
    ["INFER_MS",String(app.state.infer_ms || 0)],
    ["TOTAL_MS",String(app.state.total_ms || 0)],
    ["FPS",String(app.state.fps || 0)],
    ["LAST_ERROR",app.state.last_error || "-"]
  ];
  if(app.state.patch_mode==="manual_flight"){
    pairs.push(["MANUAL_ACTIVE",String(app.state["manual.active"] || false)]);
    pairs.push(["MANUAL_PAUSED",String(app.state["manual.paused"] || false)]);
    pairs.push(["MANUAL_EDGE_BLOCK",String(app.state["manual.edge_blocked"] || false)]);
    pairs.push(["MANUAL_POS",`${app.state["manual.position_x"] || 0}, ${app.state["manual.position_y"] || 0}`]);
    pairs.push(["MANUAL_DIR",app.state["manual.current_direction"] || "-"]);
    pairs.push(["MANUAL_NEXT_DIR",app.state["manual.pending_direction"] || "-"]);
    pairs.push(["MANUAL_LAST",`${app.state["manual.last_inferred_center_x"] || 0}, ${app.state["manual.last_inferred_center_y"] || 0}`]);
    pairs.push(["MANUAL_PATCH_COUNT",String(app.state["manual.patch_count"] || 0)]);
    pairs.push(["MANUAL_PATH",String(app.state["manual.path_points"] || 0)]);
  }
  const grid=document.getElementById("status-grid");
  grid.innerHTML="";
  pairs.forEach(([k,v])=>{
    const key=document.createElement("div");
    key.className="status-key";
    key.textContent=k;
    const value=document.createElement("div");
    value.className="status-value";
    value.textContent=v;
    grid.appendChild(key);
    grid.appendChild(value);
  });
}

// 渲染可选输入源列表，并在点击后把选择同步回控制器。
function renderSources(){
  const list=document.getElementById("source-list");
  list.innerHTML="";
  app.sources.forEach((item)=>{
    const button=document.createElement("button");
    button.className=`source-item ${app.selectedSource===item.id?"active":""}`;
    button.onclick=async ()=>{
      const previousSelectedSource=app.selectedSource;
      app.selectedSource=item.id;
      const response=await pushSelection();
      if(!response.ok){
        app.selectedSource=(app.state && app.state.selected_source) || previousSelectedSource;
      }
      renderSources();
      renderPreview();
    };
    button.innerHTML=`<div class="meta"><div><div class="title">${item.name}</div><div class="detail">${item.type}</div></div><span class="pill">${item.status}</span></div><div class="detail">${item.detail}</div>`;
    list.appendChild(button);
  });
}

// 根据当前 source 和 workflow 决定是否显示预览图。
function renderPreview(){
  const preview=document.getElementById("source-preview");
  const empty=document.getElementById("preview-empty");
  const selected=app.sources.find((item)=>item.id===app.selectedSource);
  if(selected && selected.previewable && app.state.workflow_mode==="infer"){
    preview.style.display="block";
    empty.style.display="none";
    preview.src=`/api/source/preview?id=${encodeURIComponent(selected.id)}&t=${Date.now()}`;
  }else{
    preview.style.display="none";
    preview.removeAttribute("src");
    empty.style.display="block";
  }
}

// 根据字段清单批量生成设置面板输入框。
function renderSettingsSection(containerId, fields){
  const root=document.getElementById(containerId);
  root.innerHTML="";
  fields.forEach(([key,label])=>{
    const wrapper=document.createElement("div");
    wrapper.className="form-field";
    const input=document.createElement("input");
    input.value=app.settings[key] || "";
    input.dataset.key=key;
    wrapper.innerHTML=`<label>${label}</label>`;
    wrapper.appendChild(input);
    root.appendChild(wrapper);
  });
}

// 同步设置面板的显示/隐藏状态和按钮文案。
function syncSettingsVisibility(){
  const panel=document.getElementById("settings-panel");
  const toggle=document.getElementById("settings-toggle");
  panel.hidden=!app.settingsVisible;
  toggle.setAttribute("aria-expanded", app.settingsVisible ? "true" : "false");
  toggle.innerHTML=app.settingsVisible ? "&#9881; Hide Settings" : "&#9881; Settings";
}

// 在 shutdown 进行中禁用按钮，避免重复发关闭请求。
function syncShutdownButton(){
  const button=document.getElementById("shutdown-web");
  if(!button){return;}
  button.disabled=app.shutdownInProgress;
  button.title=app.shutdownInProgress ? "Web Console is shutting down" : "Shutdown Web Console";
}

// 向后端发送 W/A/S/D 输入，驱动 manual_flight 模式的 patch 移动。
async function sendManualKey(key){
  const normalizedKey=String(key || "").toLowerCase();
  if(!["w","a","s","d"].includes(normalizedKey)){
    return false;
  }
  if(!app.state || app.state.patch_mode!=="manual_flight"){
    return false;
  }
  try{
    const response=await postJson("/api/manual/key",{key:normalizedKey,action:"down"});
    if(!response.ok){
      logLine(`manual: ${response.message}`);
      return false;
    }
    return true;
  }catch(error){
    logLine(`manual request failed: ${error.message}`);
    return false;
  }
}

// 重绘所有依赖当前状态的页面区域。
function renderAll(){
  if(!app.state){return;}
  renderButtons("workflow-buttons", workflowChoices, app.state.workflow_mode, async (value)=>{
    if(app.state.workflow_mode===value){
      return;
    }
    app.state.workflow_mode=value;
    app.selectedSource="";
    await pushSelection();
    await reloadSources();
  });
  renderButtons("patch-buttons", patchChoices, app.state.patch_mode, async (value)=>{
    app.state.patch_mode=value;
    await pushSelection();
  });
  renderButtons("output-buttons", outputChoices, app.state.output_mode, async (value)=>{
    app.state.output_mode=value;
    await pushSelection();
  });
  renderCommandButtons();
  renderStatus();
  renderSources();
  renderPreview();
  renderSettingsSection("infer-settings", inferFields);
  renderSettingsSection("rd-settings", rdFields);
  renderSettingsSection("flight-settings", flightFields);
  syncSettingsVisibility();
  syncShutdownButton();
}

// 渲染 Start / Pause / Stop / Reset 控制按钮。
function renderCommandButtons(){
  const root=document.getElementById("command-buttons");
  root.innerHTML="";
  [["/api/command/start","Start","start"],["/api/command/pause","Pause","pause"],["/api/command/stop","Stop","stop"],["/api/command/reset","Reset",""]].forEach(([path,label,tone])=>{
    const btn=document.createElement("button");
    btn.className=`command ${tone}`;
    btn.textContent=label;
    btn.onclick=async ()=>{
      const response=await postJson(path,{});
      logLine(`${label}: ${response.message}`);
      await refreshState();
    };
    root.appendChild(btn);
  });
}

// 将 workflow / patch / output / source 当前选择一次性提交给后端。
async function pushSelection(){
  const response=await postJson("/api/selection",{
    workflow_mode:app.state.workflow_mode,
    patch_mode:app.state.patch_mode,
    output_mode:app.state.output_mode,
    selected_source:app.selectedSource || ""
  });
  logLine(`selection: ${response.message}`);
  await refreshState();
  return response;
}

// 主动刷新 `/api/state`，并驱动页面重绘。
async function refreshState(){
  app.state=await getJson("/api/state");
  app.selectedSource=app.state.selected_source || app.selectedSource;
  renderAll();
}

// 主动刷新 `/api/settings`，更新表单默认值。
async function refreshSettings(){
  app.settings=await getJson("/api/settings");
  renderAll();
}

// 重新加载 source 列表；若旧选择失效，会自动选第一个可用项。
async function reloadSources(){
  const workflow=(app.state && app.state.workflow_mode) || "infer";
  const payload=await getJson(`/api/sources?workflow=${encodeURIComponent(workflow)}`);
  app.sources=payload.items || [];
  let autoSelected=false;
  if(!app.sources.find((item)=>item.id===app.selectedSource) && app.sources.length){
    app.selectedSource=app.sources[0].id;
    autoSelected=true;
  }else if(!app.sources.length){
    app.selectedSource="";
  }
  if(autoSelected){
    const response=await pushSelection();
    if(!response.ok){
      renderSources();
      renderPreview();
    }
    return;
  }
  renderSources();
  renderPreview();
}

// 建立 SSE 连接，持续接收 state/log/error 三类事件。
function connectEvents(){
  app.eventSource=new EventSource("/events");
  app.eventSource.addEventListener("state",(event)=>{
    app.connectionClosedNotified=false;
    app.state=JSON.parse(event.data);
    app.selectedSource=app.state.selected_source || app.selectedSource;
    renderAll();
  });
  app.eventSource.addEventListener("log",(event)=>{
    app.connectionClosedNotified=false;
    const payload=JSON.parse(event.data);
    logLine(payload.message);
  });
  app.eventSource.addEventListener("error",(event)=>{
    if(typeof event.data === "string" && event.data.length){
      app.connectionClosedNotified=false;
      const payload=JSON.parse(event.data);
      logLine(`ERROR: ${payload.message}`);
      return;
    }
    if(app.connectionClosedNotified){
      return;
    }
    app.connectionClosedNotified=true;
    logLine(app.shutdownInProgress ? "Web Console connection closed." : "Event stream disconnected. Waiting for reconnect...");
    if(app.shutdownInProgress && app.eventSource){
      app.eventSource.close();
    }
  });
}

// 向后端发送 shutdown_web 命令，并切换到“等待连接断开”的前端状态。
async function shutdownWebConsole(){
  if(app.shutdownInProgress){
    return;
  }
  const confirmed=window.confirm("这会关闭板子上的 Web 控制台服务，当前页面将断开连接，并保存当前设置。是否继续？");
  if(!confirmed){
    return;
  }
  app.shutdownInProgress=true;
  syncShutdownButton();
  logLine("Web Console is shutting down...");
  try{
    const response=await postJson("/api/command/shutdown_web",{});
    logLine(`shutdown: ${response.message}`);
  }catch(error){
    logLine(`shutdown request interrupted: ${error.message}`);
  }
}

// 绑定页面上所有静态 DOM 事件。
function bindStaticActions(){
  document.getElementById("reload-sources").onclick=reloadSources;
  document.getElementById("save-settings").onclick=saveSettings;
  document.getElementById("settings-toggle").onclick=()=>{
    app.settingsVisible=!app.settingsVisible;
    syncSettingsVisibility();
  };
  document.getElementById("shutdown-web").onclick=shutdownWebConsole;
  document.getElementById("clear-logs").onclick=()=>{ document.getElementById("log-box").textContent=""; };
  document.querySelectorAll("[data-manual-key]").forEach((button)=>{
    const key=button.dataset.manualKey;
    button.onpointerdown=async (event)=>{
      event.preventDefault();
      await sendManualKey(key);
    };
  });
  window.addEventListener("keydown", async (event)=>{
    const key=event.key.toLowerCase();
    if(["w","a","s","d"].includes(key) && app.state && app.state.patch_mode==="manual_flight"){
      event.preventDefault();
      if(event.repeat){
        return;
      }
      await sendManualKey(key);
    }
  });
}

// 收集设置面板当前值并整体提交给 `/api/settings`。
async function saveSettings(){
  const payload={};
  document.querySelectorAll(".settings input").forEach((input)=>{
    payload[input.dataset.key]=input.value;
  });
  const response=await postJson("/api/settings", payload);
  logLine(`settings: ${response.message}`);
  await refreshSettings();
  await reloadSources();
}

// 页面初始化入口：先拿初始状态，再接上 SSE，之后主要依赖事件驱动刷新。
async function boot(){
  bindStaticActions();
  await refreshState();
  await refreshSettings();
  await reloadSources();
  connectEvents();
  logLine("Web console ready.");
}

boot().catch((error)=>{
  logLine(`boot error: ${error.message}`);
});
})();)JS";
        return js;
    }
}
