/**
 * index.js  –  Peleprompter PebbleKit JS
 *
 * 完整實作手錶端協定（與 Android PebbleCommManager 等效）：
 *
 *   Watch → JS  CMD_REQUEST_TEXT      手錶要求初始化，帶螢幕尺寸與文字大小
 *   JS → Watch  CMD_INIT_IMAGES       回傳總頁數與實際渲染尺寸
 *   Watch → JS  CMD_REQUEST_PAGE      手錶要求第 N 頁圖片資料
 *   JS → Watch  CMD_SEND_IMAGE_CHUNK  分塊傳送 1-bit 圖片（ACK 驅動）
 *
 * Config 頁以 data URI 形式嵌入本檔（buildConfigHtml），
 * 在 Pebble 設定介面的 WebView 中執行，完成渲染後透過
 * pebblejs://close# 將結果傳回。不需要網路或外部伺服器。
 * Android companion app 為選配。
 */

'use strict';


// ══════════════════════════════════════════════════════════════════
// 協定常數（與 constants.h 完全一致）
// ══════════════════════════════════════════════════════════════════
var KEY_COMMAND          = 0;
var KEY_SCROLL_SPEED     = 3;
var KEY_TEXT_SIZE        = 4;
var KEY_PAGE_NUM         = 5;
var KEY_CHUNK_INDEX      = 6;
var KEY_TOTAL_CHUNKS     = 7;
var KEY_IMAGE_DATA       = 8;
var KEY_TOTAL_PAGES      = 9;
var KEY_WATCH_WIDTH      = 10;
var KEY_WATCH_HEIGHT     = 11;
var KEY_START_PAGE       = 12;

var CMD_REQUEST_TEXT     = 0;
var CMD_SYNC_SETTINGS    = 2;
var CMD_INIT_IMAGES      = 3;
var CMD_REQUEST_PAGE     = 4;
var CMD_SEND_IMAGE_CHUNK = 5;

// 每個區塊最多傳送位元組數（與 constants.h 一致；1024 安全小於手錶 2048 inbox）
var IMAGE_CHUNK_DATA_SIZE = 1024;

// ══════════════════════════════════════════════════════════════════
// 狀態
// ══════════════════════════════════════════════════════════════════
var pages            = [];   // number[][]：每頁的 0-255 位元組陣列
var totalPages       = 0;
var watchWidth       = 144;
var watchHeight      = 168;
var textSize         = 2;    // TEXT_SIZE_MEDIUM（目前設定值）
var rawText          = '';   // 原始未渲染文字（供設定頁預先填入）
var rawHpad          = 0;    // 水平內距（與平台有關，矩形機種為 0）

// ACK 驅動發送佇列
var messageQueue = [];
var isSending    = false;

// ══════════════════════════════════════════════════════════════════
// 訊息佇列（ACK 驅動，防止超出手錶 inbox）
// ══════════════════════════════════════════════════════════════════
function enqueue(dict) {
  messageQueue.push(dict);
  processQueue();
}

function processQueue() {
  if (isSending || messageQueue.length === 0) return;
  isSending = true;
  var msg = messageQueue.shift();
  Pebble.sendAppMessage(
    msg,
    function() { isSending = false; processQueue(); },
    function(e) {
      console.log('[pkjs] sendAppMessage failed: ' + JSON.stringify(e));
      isSending = false;
      processQueue();
    }
  );
}

// ══════════════════════════════════════════════════════════════════
// AppMessage 事件
// ══════════════════════════════════════════════════════════════════
Pebble.addEventListener('ready', function() {
  console.log('[pkjs] Ready. Pages loaded: ' + pages.length);
  // 若 JS（重）啟動時已有頁面（從 localStorage 還原），主動通知手錶，
  // 涵蓋「手錶已在等待、但啟動時序錯過 CMD_REQUEST_TEXT」的情況。
  if (pages.length > 0 && totalPages > 0) notifyWatchReady();
});

Pebble.addEventListener('appmessage', function(e) {
  var dict = e.payload;
  var cmd  = dict[KEY_COMMAND];
  if (cmd === undefined || cmd === null) return;

  switch (cmd) {
    case CMD_REQUEST_TEXT:   handleInitRequest(dict);    break;
    case CMD_REQUEST_PAGE:   handlePageRequest(dict);    break;
    case CMD_SYNC_SETTINGS:  handleSettingsSync(dict);   break;
    default: console.log('[pkjs] Unknown command: ' + cmd);
  }
});

function handleInitRequest(dict) {
  var reqW    = parseInt(dict[KEY_WATCH_WIDTH])  || 144;
  var reqH    = parseInt(dict[KEY_WATCH_HEIGHT]) || 168;
  var reqSize = (dict[KEY_TEXT_SIZE] != null) ? parseInt(dict[KEY_TEXT_SIZE]) : textSize;

  console.log('[pkjs] Init request: ' + reqW + 'x' + reqH + ' textSize=' + reqSize +
              ' pages=' + pages.length);

  if (pages.length === 0) {
    console.log('[pkjs] No pages — open settings to paste text.');
    return;
  }

  // 注意：PebbleKit JS 環境沒有 <canvas>，無法重新渲染頁面。
  // 實際渲染只發生在設定頁的 WebView 中。因此若手錶要求的 textSize
  // 與目前頁面渲染時不同，這裡仍回傳既有頁面 —— 要套用新字型大小，
  // 需重新開啟設定頁渲染，或改用 Android companion app。
  if (reqSize !== textSize) {
    console.log('[pkjs] 注意：手錶要求 textSize=' + reqSize +
                ' 但 pkjs 無法重繪，沿用既有頁面（textSize=' + textSize + '）。');
  }

  var reply = {};
  reply[KEY_COMMAND]     = CMD_INIT_IMAGES;
  reply[KEY_TOTAL_PAGES] = totalPages;
  reply[KEY_WATCH_WIDTH] = watchWidth;
  reply[KEY_WATCH_HEIGHT]= watchHeight;
  reply[KEY_START_PAGE]  = 0;   // pkjs 不追蹤書籤，一律從第 0 頁開始
  enqueue(reply);

  console.log('[pkjs] Sent CMD_INIT_IMAGES: ' + totalPages + ' pages ' +
              watchWidth + 'x' + watchHeight);
}

function handlePageRequest(dict) {
  var pageNum = parseInt(dict[KEY_PAGE_NUM]);
  if (isNaN(pageNum) || pageNum < 0 || pageNum >= totalPages) {
    console.log('[pkjs] Invalid page: ' + pageNum + ' (total=' + totalPages + ')');
    return;
  }
  var pageData = pages[pageNum];
  if (!pageData || pageData.length === 0) {
    console.log('[pkjs] Page data missing: ' + pageNum);
    return;
  }
  sendPageInChunks(pageNum, pageData);
}

function handleSettingsSync(dict) {
  if (dict[KEY_TEXT_SIZE] != null) {
    var newSize = parseInt(dict[KEY_TEXT_SIZE]);
    if (newSize !== textSize) {
      textSize = newSize;
      // 僅記錄使用者偏好；pkjs 無 canvas 無法重繪，實際以新字型重渲染
      // 需重新開啟設定頁（或使用 Android companion app）。
      console.log('[pkjs] Text size preference updated to ' + newSize +
                  ' (re-open config to re-render at this size).');
    }
  }
}

function sendPageInChunks(pageNum, data) {
  var totalChunks = Math.ceil(data.length / IMAGE_CHUNK_DATA_SIZE);
  for (var i = 0; i < totalChunks; i++) {
    var start = i * IMAGE_CHUNK_DATA_SIZE;
    var end   = Math.min(start + IMAGE_CHUNK_DATA_SIZE, data.length);
    var chunk = [];
    for (var j = start; j < end; j++) chunk.push(data[j]);

    var msg = {};
    msg[KEY_COMMAND]      = CMD_SEND_IMAGE_CHUNK;
    msg[KEY_PAGE_NUM]     = pageNum;
    msg[KEY_CHUNK_INDEX]  = i;
    msg[KEY_TOTAL_CHUNKS] = totalChunks;
    msg[KEY_IMAGE_DATA]   = chunk;
    enqueue(msg);
  }
  console.log('[pkjs] Queued ' + totalChunks + ' chunks for page ' + pageNum +
              ' (' + data.length + 'B)');
}

// ══════════════════════════════════════════════════════════════════
// 主動推送：頁面就緒後通知手錶，無須手錶重新發 CMD_REQUEST_TEXT
// ══════════════════════════════════════════════════════════════════
function notifyWatchReady() {
  if (totalPages === 0) return;
  var reply = {};
  reply[KEY_COMMAND]      = CMD_INIT_IMAGES;
  reply[KEY_TOTAL_PAGES]  = totalPages;
  reply[KEY_WATCH_WIDTH]  = watchWidth;
  reply[KEY_WATCH_HEIGHT] = watchHeight;
  reply[KEY_START_PAGE]   = 0;   // pkjs 不追蹤書籤，一律從第 0 頁開始
  enqueue(reply);
  console.log('[pkjs] Proactively sent CMD_INIT_IMAGES: ' + totalPages + ' pages');
}

// ══════════════════════════════════════════════════════════════════
// 輔助：根據螢幕尺寸推算預設水平內距
// ══════════════════════════════════════════════════════════════════
// 與 Android WatchImageRenderer.horizontalPadPx 一致，確保兩端分頁相同
function hpadForDimensions(w, h) {
  if (w === 180 && h === 180) return 20;   // Chalk (Time Round)
  if (w === 260 && h === 260) return 30;   // Gabbro (Time Round 2)
  return 0;                                // Aplite/Basalt/Emery
}

// ══════════════════════════════════════════════════════════════════
// localStorage 持久化（集中管理，含原始文字）
// ══════════════════════════════════════════════════════════════════
function saveToStorage() {
  try {
    localStorage.setItem('pele_meta', JSON.stringify({
      totalPages: totalPages, watchWidth: watchWidth,
      watchHeight: watchHeight, textSize: textSize
    }));
    pages.forEach(function(bytes, i) {
      localStorage.setItem('pele_page_' + i, arrayToBase64(bytes));
    });
    if (rawText) {
      localStorage.setItem('pele_text', rawText);
      localStorage.setItem('pele_hpad', String(rawHpad));
    }
  } catch (e) {
    console.log('[pkjs] localStorage save failed: ' + e);
  }
}

// ══════════════════════════════════════════════════════════════════
// 設定頁面
// ══════════════════════════════════════════════════════════════════
Pebble.addEventListener('showConfiguration', function() {
  var html = buildConfigHtml();
  Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(html));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response || e.response === 'CANCELLED' || e.response === '') {
    console.log('[pkjs] Config closed without data');
    return;
  }
  try {
    var result = JSON.parse(decodeURIComponent(e.response));

    watchWidth  = result.watchWidth  || 144;
    watchHeight = result.watchHeight || 168;
    textSize    = result.textSize    != null ? result.textSize : 2;

    if (result.text) {
      rawText = result.text;
      rawHpad = result.hpad != null ? result.hpad : hpadForDimensions(watchWidth, watchHeight);
    }

    if (Array.isArray(result.pages) && result.pages.length > 0) {
      pages      = result.pages.map(base64ToArray);
      totalPages = result.totalPages || pages.length;
    } else {
      console.log('[pkjs] Config returned no usable data');
      return;
    }



    console.log('[pkjs] Loaded ' + totalPages + ' pages (' +
                watchWidth + 'x' + watchHeight + ' textSize=' + textSize + ')');

    saveToStorage();

    notifyWatchReady();

  } catch (ex) {
    console.log('[pkjs] Failed to parse config response: ' + ex.message);
  }
});

function base64ToArray(b64) {
  if (typeof Buffer !== 'undefined') {
    var buf = Buffer.from(b64, 'base64');
    var arr = new Array(buf.length);
    for (var i = 0; i < buf.length; i++) arr[i] = buf[i];
    return arr;
  }
  var binary = atob(b64);
  var arr = new Array(binary.length);
  for (var i = 0; i < binary.length; i++) arr[i] = binary.charCodeAt(i);
  return arr;
}

function arrayToBase64(arr) {
  if (typeof Buffer !== 'undefined') {
    return Buffer.from(arr).toString('base64');
  }
  var s = '';
  for (var i = 0; i < arr.length; i++) s += String.fromCharCode(arr[i]);
  return btoa(s);
}

// ── 從 localStorage 恢復上次的頁面資料 ──────────────────────────
(function restoreFromStorage() {
  try {
    var metaStr = localStorage.getItem('pele_meta');
    if (!metaStr) return;
    var meta = JSON.parse(metaStr);
    var n = meta.totalPages || 0;
    if (n === 0) return;

    var restored = [];
    for (var i = 0; i < n; i++) {
      var b64 = localStorage.getItem('pele_page_' + i);
      if (!b64) { restored = []; break; }
      restored.push(base64ToArray(b64));
    }
    if (restored.length === n) {
      pages            = restored;
      totalPages       = meta.totalPages;
      watchWidth       = meta.watchWidth  || 144;
      watchHeight      = meta.watchHeight || 168;
      textSize         = meta.textSize    != null ? meta.textSize : 2;

      rawText          = localStorage.getItem('pele_text') || '';
      rawHpad          = parseInt(localStorage.getItem('pele_hpad') || '0');
      console.log('[pkjs] Restored ' + totalPages + ' pages from localStorage' +
                  (rawText ? ' (raw text available for config prefill)' : ''));
    }
  } catch (e) {
    console.log('[pkjs] localStorage restore failed: ' + e);
  }
}());

// ══════════════════════════════════════════════════════════════════
// Config 頁面 HTML（完整嵌入，以 data URI 傳入 Pebble.openURL）
// ══════════════════════════════════════════════════════════════════
function buildConfigHtml() {
  // 使用陣列 join 維持可讀性，避免單一壓縮字串難以維護。
  // HTML 屬性使用雙引號；<script> 內的字串字面量亦使用雙引號，
  // 確保不與外層 JS 字串的單引號衝突。
  var lines = [
    '<!DOCTYPE html>',
    '<html lang="en">',
    '<head>',
    '<meta charset="utf-8">',
    '<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1">',
    '<title>Peleprompter</title>',
    '<style>',
    '*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}',
    'body{background:#0f0f10;color:#fff;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;padding:20px 16px 32px;max-width:480px;margin:0 auto;font-size:15px;-webkit-text-size-adjust:100%;-webkit-font-smoothing:antialiased}',
    'h2{color:#fff;font-size:24px;font-weight:800;letter-spacing:-.02em}',
    '.sub{color:#8a8a90;font-size:13px;margin-top:4px;margin-bottom:8px}',
    '.card{background:#1c1c1e;border:1px solid #2c2c2e;border-radius:16px;padding:16px;margin-top:14px}',
    '.label{display:block;color:#8a8a90;font-size:12px;font-weight:700;text-transform:uppercase;letter-spacing:.08em;margin-bottom:10px}',
    'textarea{width:100%;height:170px;background:#2a2a2c;border:1px solid #404044;border-radius:12px;color:#fff;padding:14px;font-size:14px;line-height:1.55;resize:vertical;-webkit-appearance:none}',
    'textarea:focus{outline:none;border-color:#BB86FC}',
    'textarea::placeholder{color:#5a5a60}',
    '.btn-row{display:flex;gap:8px}',
    '.seg-btn{flex:1;padding:11px 4px;background:#2a2a2c;border:1px solid #404044;border-radius:10px;color:#e8e8ea;font-size:12px;text-align:center;cursor:pointer;-webkit-tap-highlight-color:transparent;line-height:1.4;transition:background .12s,border-color .12s}',
    '.seg-btn.active{background:#BB86FC;border-color:#BB86FC;color:#000;font-weight:700}',
    '.ghost-btn{background:#2a2a2c;border:1px solid #404044;border-radius:10px;color:#BB86FC;padding:9px 16px;font-size:13px;font-weight:600;cursor:pointer;-webkit-tap-highlight-color:transparent}',
    '#btnRender{display:block;width:100%;margin-top:20px;padding:15px;background:#03DAC5;border:none;border-radius:12px;color:#000;font-size:16px;font-weight:800;cursor:pointer;-webkit-tap-highlight-color:transparent}',
    '#btnRender:disabled{opacity:.35;cursor:not-allowed}',
    '#btnRender:active{background:#02b5a4}',
    '#progressWrap{margin-top:14px;display:none}',
    'progress{width:100%;height:6px;-webkit-appearance:none;appearance:none;border:none;border-radius:3px;background:#2c2c2e}',
    'progress::-webkit-progress-bar{background:#2c2c2e;border-radius:3px}',
    'progress::-webkit-progress-value{background:#03DAC5;border-radius:3px}',
    'progress::-moz-progress-bar{background:#03DAC5;border-radius:3px}',
    '#status{margin-top:12px;font-size:13px;color:#8a8a90;min-height:18px;line-height:1.45;text-align:center}',
    '#status.warn{color:#FFB74D}#status.error{color:#EF9A9A}#status.ok{color:#03DAC5}',
    // 渲染用 canvas：保持可見並在正常流中，確保 WebView 實際繪製它。
    // （display:none 或離屏 + 透明的 canvas，在部分舊版 WebView 上 getImageData
    //  會回傳全 0，導致手錶頁面全黑。搭配 willReadFrequently 強制 CPU 後備緩衝，
    //  讓 getImageData 可靠。）此 canvas 同時作為渲染預覽。
    '#c{display:block;margin:14px auto 0;border:1px solid #2c2c2e;border-radius:8px;background:#000;image-rendering:pixelated}',
    '.info-box{background:#161617;border:1px solid #2c2c2e;border-radius:12px;padding:12px 14px;margin-top:14px;font-size:13px;color:#8a8a90;display:none;text-align:center}',
    '.info-box span{color:#BB86FC;font-weight:700}',
    '</style>',
    '</head>',
    '<body>',
    '<h2>&#x1F4FA; Peleprompter</h2>',
    '<div class="sub">Render your script and send it to your Pebble.</div>',

    // ── Script card ─────────────────────────────────────────────
    '<div class="card">',
    '<label class="label">Script</label>',
    '<textarea id="txt" placeholder="Paste your script here…" oninput="onTextChange()"></textarea>',
    '<div class="sub" style="margin-top:8px;margin-bottom:0">Tip: to import a .txt or .epub file, use the Peleprompter Android app.</div>',
    '</div>',

    // ── Platform card ───────────────────────────────────────────
    '<div class="card">',
    '<label class="label">Watch model</label>',
    '<div class="btn-row" id="platRow">',
    '  <button class="seg-btn active" data-w="144" data-h="168" data-hpad="0"  onclick="setPlatform(this)">Aplite/Basalt<br><span style="color:#888;font-size:10px">Pebble&#xB7;Time</span><br>144&#xD7;168</button>',
    '  <button class="seg-btn"        data-w="180" data-h="180" data-hpad="20" onclick="setPlatform(this)">Chalk<br><span style="color:#888;font-size:10px">Time Round</span><br>180&#xD7;180</button>',
    '  <button class="seg-btn"        data-w="200" data-h="228" data-hpad="0"  onclick="setPlatform(this)">Emery<br><span style="color:#888;font-size:10px">Time 2</span><br>200&#xD7;228</button>',
    '  <button class="seg-btn"        data-w="260" data-h="260" data-hpad="30" onclick="setPlatform(this)">Gabbro<br><span style="color:#888;font-size:10px">Round 2</span><br>260&#xD7;260</button>',
    '</div>',
    '</div>',

    // ── Text size card ──────────────────────────────────────────
    '<div class="card">',
    '<label class="label">Text size</label>',
    '<div class="btn-row" id="sizeRow">',
    '  <button class="seg-btn"        data-sz="0" onclick="setSize(this)">Tiny</button>',
    '  <button class="seg-btn"        data-sz="1" onclick="setSize(this)">Small</button>',
    '  <button class="seg-btn active" data-sz="2" onclick="setSize(this)">Medium</button>',
    '  <button class="seg-btn"        data-sz="3" onclick="setSize(this)">Large</button>',
    '  <button class="seg-btn"        data-sz="4" onclick="setSize(this)">XLarge</button>',
    '</div>',
    '</div>',

    // ── Estimate info ────────────────────────────────────────────
    '<div class="info-box" id="infoBox">',
    '  Estimated <span id="estPages">?</span> pages &nbsp;&bull;&nbsp; <span id="estKb">?</span> KB',
    '</div>',

    // ── Render button + progress ─────────────────────────────────
    '<button id="btnRender" onclick="doRender()">Send to watch</button>',
    '<div id="progressWrap"><progress id="prog" max="100" value="0"></progress></div>',
    '<div id="status">Add your script, choose your watch, then tap Send.</div>',
    '<canvas id="c"></canvas>',

    // ── Script ───────────────────────────────────────────────────
    '<script>',
    '"use strict";',

    // State — injected from pkjs at build time
    'var WW=' + watchWidth + ', WH=' + watchHeight + ', SZ=' + textSize + ', HPAD=' + rawHpad + ';',
    // 渲染參數須與 Android WatchImageRenderer.kt 一致，確保兩端分頁相同：
    //   字型像素 [10,13,16,22,28]、行距 1.15、Tiny/Small 用細體、其餘用等寬字。
    'var PADDING=50, FONT_PX=[10,13,16,22,28], LINE_MULT=1.15, MAX_PAGES=60;',
    'var STORED_TEXT=' + JSON.stringify(rawText) + ';',
    // 字型字串：Tiny/Small (0/1) 以 sans-serif 細體近似 Android 的 sans-serif-light，
    // 其餘等級使用等寬字 monospace 對應 Android 的 MONOSPACE。
    'function fontStr(sz,px){return sz<=1?("300 "+px+"px sans-serif"):(px+"px monospace");}',

    // UI helpers
    'function setPlatform(btn) {',
    '  document.querySelectorAll("#platRow .seg-btn").forEach(function(b){b.className="seg-btn";});',
    '  btn.className="seg-btn active";',
    '  WW=parseInt(btn.getAttribute("data-w"));',
    '  WH=parseInt(btn.getAttribute("data-h"));',
    '  HPAD=parseInt(btn.getAttribute("data-hpad"));',
    '  updateEstimate();',
    '}',
    'function setSize(btn) {',
    '  document.querySelectorAll("#sizeRow .seg-btn").forEach(function(b){b.className="seg-btn";});',
    '  btn.className="seg-btn active";',
    '  SZ=parseInt(btn.getAttribute("data-sz"));',
    '  updateEstimate();',
    '}',
    'function setStatus(msg,cls){var el=document.getElementById("status");el.textContent=msg;el.className=cls||"";}',
    'function onTextChange(){updateEstimate();}',


    // Live estimate
    'function updateEstimate() {',
    '  var text=document.getElementById("txt").value.trim();',
    '  if (!text){document.getElementById("infoBox").style.display="none";return;}',
    '  var canvas=document.getElementById("c");',
    '  canvas.width=WW; canvas.height=WH;',
    '  var ctx=canvas.getContext("2d",{willReadFrequently:true}), fpx=FONT_PX[SZ];',
    '  ctx.font=fontStr(SZ,fpx);',
    '  var lines=wrapText(ctx,text,WW-HPAD*2), lh=Math.floor(fpx*LINE_MULT);',
    '  var n=Math.max(1,Math.ceil((PADDING+lines.length*lh+PADDING)/WH));',
    '  var kb=Math.round(n*Math.ceil(WW/32)*4*WH/1024);',
    '  document.getElementById("estPages").textContent=n;',
    '  document.getElementById("estKb").textContent=kb;',
    '  document.getElementById("infoBox").style.display="block";',
    '}',

    // Text wrapping
    'function wrapText(ctx,text,maxW) {',
    '  var lines=[],paras=text.split("\\n");',
    '  for (var p=0;p<paras.length;p++) {',
    '    var para=paras[p];',
    '    if (!para.trim()){lines.push("");continue;}',
    '    var words=para.split(" "),line="";',
    '    for (var i=0;i<words.length;i++) {',
    '      var test=line?line+" "+words[i]:words[i];',
    '      if (ctx.measureText(test).width<=maxW){line=test;}',
    '      else{if(line)lines.push(line);line=words[i];}',
    '    }',
    '    if (line) lines.push(line);',
    '  }',
    '  return lines;',
    '}',

    // 1-bit encoder — 必須與 Pebble 原生 GBitmapFormat1Bit 完全一致：
    //   列對齊 4 bytes、LSB-first（位元 0 = 最左像素）、1=白、BT.601、閾值 64。
    'function encode1Bit(imageData,w,h) {',
    '  var stride=Math.ceil(w/32)*4, result=new Uint8Array(stride*h), d=imageData.data;',
    '  for (var y=0;y<h;y++) for (var x=0;x<w;x++) {',
    '    var i=(y*w+x)*4, luma=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2];',
    '    if (luma>64) result[y*stride+(x>>3)]|=(1<<(x&7));',
    '  }',
    '  return result;',
    '}',

    // Base64 encode (chunk to avoid call stack overflow on large arrays)
    'function toBase64(bytes) {',
    '  var s="", CHUNK=8192;',
    '  for (var i=0;i<bytes.length;i+=CHUNK)',
    '    s+=String.fromCharCode.apply(null,bytes.subarray(i,i+CHUNK));',
    '  return btoa(s);',
    '}',

    // Render all pages async (one page per setTimeout tick)
    'function doRender() {',
    '  var text=document.getElementById("txt").value.trim();',
    '  if (!text){setStatus("Please enter some text first.","warn");return;}',
    '  var btn=document.getElementById("btnRender"), prog=document.getElementById("prog");',
    '  btn.disabled=true;',
    '  document.getElementById("progressWrap").style.display="block";',
    '  prog.value=0;',
    '  var canvas=document.getElementById("c");',
    '  canvas.width=WW; canvas.height=WH;',
    '  var ctx=canvas.getContext("2d",{willReadFrequently:true}), fpx=FONT_PX[SZ], lh=Math.floor(fpx*LINE_MULT);',
    '  ctx.font=fontStr(SZ,fpx);',
    '  var lines=wrapText(ctx,text,WW-HPAD*2);',
    '  var totalH=PADDING+lines.length*lh+PADDING;',
    '  var numPages=Math.max(1,Math.min(MAX_PAGES,Math.ceil(totalH/WH)));',
    '  if (Math.ceil(totalH/WH)>MAX_PAGES)',
    '    setStatus("Text truncated to "+MAX_PAGES+" pages.","warn");',
    '  else',
    '    setStatus("Rendering "+numPages+(numPages>1?" pages":" page")+"...");',
    '  var pagesOut=[], pageN=0;',
    '  function renderNext() {',
    '    if (pageN>=numPages){finish(pagesOut,numPages);return;}',
    '    ctx.fillStyle="#000000"; ctx.fillRect(0,0,WW,WH);',
    '    ctx.fillStyle="#ffffff"; ctx.font=fontStr(SZ,fpx);',
    '    var offsetY=PADDING-pageN*WH;',
    '    for (var li=0;li<lines.length;li++) {',
    '      var y=offsetY+li*lh+fpx;',
    '      if (y>WH+lh) break;',
    '      if (y+lh<0)  continue;',
    '      ctx.fillText(lines[li],HPAD,y);',
    '    }',
    '    var id=ctx.getImageData(0,0,WW,WH);',
    '    var enc=encode1Bit(id,WW,WH);',
    // 偵測「全黑」頁面：若有文字卻渲染出全 0，多半是此裝置的設定瀏覽器
    // 不支援 canvas 像素讀取。直接中止並提示，避免送出空白頁讓手錶一片空白。
    '    if (pageN===0 && lines.length>0) {',
    '      var any=false; for (var k=0;k<enc.length;k++){ if(enc[k]!==0){any=true;break;} }',
    '      if (!any) {',
    '        setStatus("This phone\\u2019s config browser could not render the text (blank image). Please use the Peleprompter Android app to send your script.","error");',
    '        btn.disabled=false;',
    '        document.getElementById("progressWrap").style.display="none";',
    '        return;',
    '      }',
    '    }',
    '    pagesOut.push(toBase64(enc));',
    '    prog.value=Math.round((pageN+1)/numPages*100);',
    '    setStatus("Rendering page "+(pageN+1)+" / "+numPages+"...");',
    '    pageN++;',
    '    setTimeout(renderNext,0);',
    '  }',
    '  renderNext();',
    '}',

    // Send result back to PebbleKit JS.
    // 注意：不再把整份原始文字 (text) 放進回傳 payload，只送渲染後的頁面，
    // 大幅縮小 pebblejs://close 的資料量，避免大稿件被截斷導致手錶收不到頁面。
    'function finish(pagesOut,n) {',
    '  var bytes0 = pagesOut.length ? atob(pagesOut[0]).length : 0;',
    '  setStatus("Rendered "+n+" page(s); first page = "+bytes0+" bytes. Preview shown above. Sending to watch\\u2026","ok");',
    '  var result={pages:pagesOut,totalPages:n,watchWidth:WW,watchHeight:WH,textSize:SZ,hpad:HPAD};',
    // 延遲關閉，讓使用者能看到上方預覽與位元組數（便於診斷）
    '  setTimeout(function(){',
    '    try { document.location="pebblejs://close#"+encodeURIComponent(JSON.stringify(result)); }',
    '    catch(e){ setStatus("Error: "+e.message,"error"); document.getElementById("btnRender").disabled=false; }',
    '  }, 1800);',
    '}',

    // Page init: auto-select the correct platform + size buttons, pre-fill stored text
    '(function initPage(){',
    '  // 選擇符合目前手錶尺寸的平台按鈕',
    '  var pb=document.querySelectorAll("#platRow .seg-btn");',
    '  pb.forEach(function(b){b.className="seg-btn";});',
    '  var matched=false;',
    '  pb.forEach(function(b){',
    '    if(!matched && parseInt(b.getAttribute("data-w"))===WW && parseInt(b.getAttribute("data-h"))===WH){',
    '      b.className="seg-btn active"; HPAD=parseInt(b.getAttribute("data-hpad")); matched=true;',
    '    }',
    '  });',
    '  if(!matched && pb.length>0){pb[0].className="seg-btn active";}',
    '  // 選擇符合目前文字大小的尺寸按鈕',
    '  var sb=document.querySelectorAll("#sizeRow .seg-btn");',
    '  sb.forEach(function(b){b.className="seg-btn";});',
    '  sb.forEach(function(b){if(parseInt(b.getAttribute("data-sz"))===SZ)b.className="seg-btn active";});',
    '  // 若有儲存的原始文字，預先填入文字框',
    '  if(STORED_TEXT){',
    '    document.getElementById("txt").value=STORED_TEXT;',
    '    updateEstimate();',
    '    setStatus("Previous script loaded. Tap Send to update your watch.","");',
    '  }',
    '}());',

    '<\/script>',
    '</body>',
    '</html>'
  ];

  return lines.join('\n');
}
