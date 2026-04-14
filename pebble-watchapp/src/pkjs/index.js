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

// CloudPebble 模擬器偵測（platform === 'pypkjs' 表示在 CloudPebble 中執行）
var EMULATOR = (typeof Pebble === 'undefined') || (Pebble.platform === 'pypkjs');
// 代理頁面負責在 CloudPebble 中開啟 config 頁面（需啟用 GitHub Pages）
var PROXY_URL = 'https://ping840907.github.io/Peleprompter/config-proxy.html#';

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

var CMD_REQUEST_TEXT     = 0;
var CMD_SYNC_SETTINGS    = 2;
var CMD_INIT_IMAGES      = 3;
var CMD_REQUEST_PAGE     = 4;
var CMD_SEND_IMAGE_CHUNK = 5;

// 每個區塊最多傳送位元組數（留出 AppMessage header 空間）
var IMAGE_CHUNK_DATA_SIZE = 1800;

// ══════════════════════════════════════════════════════════════════
// 狀態
// ══════════════════════════════════════════════════════════════════
var pages            = [];   // number[][]：每頁的 0-255 位元組陣列
var totalPages       = 0;
var watchWidth       = 144;
var watchHeight      = 168;
var textSize         = 2;    // TEXT_SIZE_MEDIUM（目前設定值）
var renderedTextSize = 2;    // 實際渲染頁面時使用的文字大小
var rawText          = '';   // 原始未渲染文字（供重繪使用）
var rawHpad          = 2;    // 水平內距（與平台有關）

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
  var reqSize = (dict[KEY_TEXT_SIZE] != null) ? parseInt(dict[KEY_TEXT_SIZE]) : renderedTextSize;

  console.log('[pkjs] Init request: ' + reqW + 'x' + reqH + ' textSize=' + reqSize +
              ' pages=' + pages.length);

  if (pages.length === 0) {
    console.log('[pkjs] No pages — open settings to paste text.');
    return;
  }

  // 文字大小與實際渲染不符時自動重繪（需要有原始文字且環境支援 canvas）
  if (reqSize !== renderedTextSize && rawText) {
    console.log('[pkjs] Text size changed (' + renderedTextSize + '→' + reqSize + '), re-rendering...');
    var rerendered = renderAllPagesFromText(rawText, reqW, reqH, rawHpad, reqSize);
    if (rerendered && rerendered.length > 0) {
      pages            = rerendered;
      totalPages       = pages.length;
      watchWidth       = reqW;
      watchHeight      = reqH;
      textSize         = reqSize;
      renderedTextSize = reqSize;
      saveToStorage();
      console.log('[pkjs] Re-rendered ' + totalPages + ' pages at textSize=' + reqSize);
    } else {
      console.log('[pkjs] Re-render failed (canvas not available?). Open settings to re-render manually.');
    }
  }

  var reply = {};
  reply[KEY_COMMAND]     = CMD_INIT_IMAGES;
  reply[KEY_TOTAL_PAGES] = totalPages;
  reply[KEY_WATCH_WIDTH] = watchWidth;
  reply[KEY_WATCH_HEIGHT]= watchHeight;
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
      // 實際重繪在 handleInitRequest 執行（手錶在發送 SYNC_SETTINGS 後會發送 REQUEST_TEXT）
      console.log('[pkjs] Text size setting updated to ' + newSize + '. Awaiting init request to re-render.');
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
  enqueue(reply);
  console.log('[pkjs] Proactively sent CMD_INIT_IMAGES: ' + totalPages + ' pages');
}

// ══════════════════════════════════════════════════════════════════
// 輔助：根據螢幕尺寸推算預設水平內距
// ══════════════════════════════════════════════════════════════════
function hpadForDimensions(w, h) {
  if (w === 180 && h === 180) return 20;   // Chalk (Time Round)
  if (w === 260 && h === 260) return 30;   // Gabbro (Time Round 2)
  return 2;                                // Aplite/Basalt/Emery
}

// ══════════════════════════════════════════════════════════════════
// localStorage 持久化（集中管理，含原始文字）
// ══════════════════════════════════════════════════════════════════
function saveToStorage() {
  try {
    localStorage.setItem('pele_meta', JSON.stringify({
      totalPages: totalPages, watchWidth: watchWidth,
      watchHeight: watchHeight, textSize: renderedTextSize
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
  if (EMULATOR) {
    // CloudPebble: 透過 GitHub Pages 代理頁面開啟
    // $$RETURN_TO$$ 由代理替換成 CloudPebble 注入的 return_to URL
    Pebble.openURL(PROXY_URL + encodeURIComponent(html));
  } else {
    // 實機：將 $$RETURN_TO$$ 替換為 pebblejs://close# 後以 data URI 開啟
    var deviceHtml = html.split('$$RETURN_TO$$').join('pebblejs://close#');
    Pebble.openURL('data:text/html;charset=utf-8,' + encodeURIComponent(deviceHtml));
  }
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

    // 儲存原始文字供後續重繪使用（實機與 CloudPebble 路徑均嘗試保存）
    if (result.text) {
      rawText = result.text;
      rawHpad = result.hpad != null ? result.hpad : hpadForDimensions(watchWidth, watchHeight);
    }

    if (result.text && !(Array.isArray(result.pages) && result.pages.length > 0)) {
      // CloudPebble path: URL length limit prevents passing bitmaps via redirect.
      // Render pages locally using the Node.js canvas module (available in pypkjs).
      var rendered = renderAllPagesFromText(rawText, watchWidth, watchHeight, rawHpad, textSize);
      if (!rendered || rendered.length === 0) {
        console.log('[pkjs] Rendering failed – canvas may not be available in this environment');
        return;
      }
      pages = rendered;
      totalPages = pages.length;
    } else if (Array.isArray(result.pages) && result.pages.length > 0) {
      // Real device path: config page pre-rendered pages and passed them via URL.
      pages      = result.pages.map(base64ToArray);
      totalPages = result.totalPages || pages.length;
    } else {
      console.log('[pkjs] Config returned no usable data');
      return;
    }

    renderedTextSize = textSize;

    console.log('[pkjs] Loaded ' + totalPages + ' pages (' +
                watchWidth + 'x' + watchHeight + ' textSize=' + textSize + ')');

    saveToStorage();

    // 主動通知手錶頁面已就緒，不等手錶重新發 CMD_REQUEST_TEXT
    // （手錶 app 可能已在前景等待，直接推送讓它立刻開始拉頁面）
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

// ── On-device renderer (used in CloudPebble / pypkjs via require('canvas')) ──
var RENDER_FONT_PX   = [9, 11, 14, 18, 22];
var RENDER_FONT_WT   = [200, 300, 400, 400, 400];
var RENDER_PADDING   = 50;
var RENDER_MAX_PAGES = 60;

function renderAllPagesFromText(text, w, h, hpad, szLevel) {
  var Canvas;
  try { Canvas = require('canvas'); } catch (e) {
    console.log('[pkjs] canvas not available: ' + e);
    return null;
  }
  var createFn = (typeof Canvas.createCanvas === 'function')
    ? Canvas.createCanvas
    : function(ww, hh) { return new Canvas(ww, hh); };

  var fpx = RENDER_FONT_PX[szLevel] || 14;
  var fwt = RENDER_FONT_WT[szLevel] || 400;
  var lh  = Math.floor(fpx * 1.4);
  var fontStr = fwt + ' ' + fpx + 'px sans-serif';

  var mc   = createFn(w, h);
  var mctx = mc.getContext('2d');
  mctx.font = fontStr;
  var lines  = renderWrapText(mctx, text, w - hpad * 2);
  var totalH = RENDER_PADDING + lines.length * lh + RENDER_PADDING;
  var numPages = Math.max(1, Math.min(RENDER_MAX_PAGES, Math.ceil(totalH / h)));
  console.log('[pkjs] Rendering ' + numPages + ' page(s) ' + w + 'x' + h + ' sz=' + szLevel);

  var out = [];
  for (var pageN = 0; pageN < numPages; pageN++) {
    var pc  = createFn(w, h);
    var ctx = pc.getContext('2d');
    ctx.fillStyle = '#000000';
    ctx.fillRect(0, 0, w, h);
    ctx.fillStyle = '#ffffff';
    ctx.font = fontStr;
    var offsetY = RENDER_PADDING - pageN * h;
    for (var li = 0; li < lines.length; li++) {
      var y = offsetY + li * lh + fpx;
      if (y > h + lh) break;
      if (y + lh < 0) continue;
      ctx.fillText(lines[li], hpad, y);
    }
    out.push(renderEncode1Bit(ctx.getImageData(0, 0, w, h), w, h));
  }
  console.log('[pkjs] Done: ' + out.length + ' page(s)');
  return out;
}

function renderWrapText(ctx, text, maxW) {
  var lines = [], paras = text.split('\n');
  for (var p = 0; p < paras.length; p++) {
    var para = paras[p];
    if (!para.trim()) { lines.push(''); continue; }
    var words = para.split(' '), line = '';
    for (var i = 0; i < words.length; i++) {
      var test = line ? line + ' ' + words[i] : words[i];
      if (ctx.measureText(test).width <= maxW) { line = test; }
      else { if (line) lines.push(line); line = words[i]; }
    }
    if (line) lines.push(line);
  }
  return lines;
}

function renderEncode1Bit(imageData, w, h) {
  var stride = Math.ceil(w / 8);
  var result = new Array(stride * h);
  for (var k = 0; k < result.length; k++) result[k] = 0;
  var d = imageData.data;
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      var idx  = (y * w + x) * 4;
      var luma = 0.299 * d[idx] + 0.587 * d[idx + 1] + 0.114 * d[idx + 2];
      if (luma > 64) result[y * stride + (x >> 3)] |= (0x80 >> (x & 7));
    }
  }
  return result;
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
      renderedTextSize = textSize;
      rawText          = localStorage.getItem('pele_text') || '';
      rawHpad          = parseInt(localStorage.getItem('pele_hpad') || '2');
      console.log('[pkjs] Restored ' + totalPages + ' pages from localStorage' +
                  (rawText ? ' (raw text available for re-render)' : ''));
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
    'body{background:#121212;color:#fff;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;padding:16px;max-width:480px;margin:0 auto;font-size:15px;-webkit-text-size-adjust:100%}',
    'h2{color:#BB86FC;font-size:20px;margin-bottom:18px;font-weight:700}',
    '.label{display:block;color:#b0b0b0;font-size:12px;text-transform:uppercase;letter-spacing:.05em;margin-bottom:6px;margin-top:16px}',
    'textarea{width:100%;height:160px;background:#1e1e1e;border:1px solid #333;border-radius:8px;color:#fff;padding:12px;font-size:14px;line-height:1.5;resize:vertical;-webkit-appearance:none}',
    'textarea:focus{outline:none;border-color:#BB86FC}',
    'textarea::placeholder{color:#555}',
    '.btn-row{display:flex;gap:8px}',
    '.seg-btn{flex:1;padding:10px 4px;background:#1e1e1e;border:1px solid #444;border-radius:6px;color:#fff;font-size:12px;text-align:center;cursor:pointer;-webkit-tap-highlight-color:transparent;line-height:1.4}',
    '.seg-btn.active{background:#BB86FC;border-color:#BB86FC;color:#000;font-weight:600}',
    '#btnRender{display:block;width:100%;margin-top:20px;padding:14px;background:#03DAC5;border:none;border-radius:8px;color:#000;font-size:16px;font-weight:700;cursor:pointer;-webkit-tap-highlight-color:transparent}',
    '#btnRender:disabled{opacity:.4;cursor:not-allowed}',
    '#progressWrap{margin-top:12px;display:none}',
    'progress{width:100%;height:6px;-webkit-appearance:none;appearance:none;border:none;border-radius:3px;background:#333}',
    'progress::-webkit-progress-bar{background:#333;border-radius:3px}',
    'progress::-webkit-progress-value{background:#03DAC5;border-radius:3px}',
    'progress::-moz-progress-bar{background:#03DAC5;border-radius:3px}',
    '#status{margin-top:10px;font-size:13px;color:#888;min-height:18px;line-height:1.4}',
    '#status.warn{color:#FFB74D}#status.error{color:#EF9A9A}#status.ok{color:#81C784}',
    'canvas{display:none}',
    '.info-box{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:10px 12px;margin-top:12px;font-size:12px;color:#888;display:none}',
    '.info-box span{color:#BB86FC;font-weight:600}',
    '</style>',
    '</head>',
    '<body>',
    '<h2>&#x1F4FA; Peleprompter</h2>',

    // ── Text input ──────────────────────────────────────────────
    '<label class="label">Text Content</label>',
    '<div style="display:flex;gap:8px;align-items:center;margin-bottom:8px">',
    '  <button class="seg-btn" style="flex:none;padding:8px 16px;font-size:13px" onclick="triggerFileInput()">&#x1F4C2; Import file</button>',
    '  <span id="fileLabel" style="color:#888;font-size:12px">.txt &amp; .epub supported</span>',
    '</div>',
    '<input type="file" id="fileIn" accept=".txt,.epub,text/plain,application/epub+zip" style="display:none" onchange="onFileSelected(this)">',
    '<textarea id="txt" placeholder="Or paste your script here..." oninput="onTextChange()"></textarea>',

    // ── Platform selector ────────────────────────────────────────
    '<label class="label">Platform</label>',
    '<div class="btn-row" id="platRow">',
    '  <button class="seg-btn active" data-w="144" data-h="168" data-hpad="2"  onclick="setPlatform(this)">Aplite/Basalt<br><span style="color:#888;font-size:10px">Pebble&#xB7;Time</span><br>144&#xD7;168</button>',
    '  <button class="seg-btn"        data-w="180" data-h="180" data-hpad="20" onclick="setPlatform(this)">Chalk<br><span style="color:#888;font-size:10px">Time Round</span><br>180&#xD7;180</button>',
    '  <button class="seg-btn"        data-w="200" data-h="228" data-hpad="2"  onclick="setPlatform(this)">Emery<br><span style="color:#888;font-size:10px">Time 2</span><br>200&#xD7;228</button>',
    '  <button class="seg-btn"        data-w="260" data-h="260" data-hpad="30" onclick="setPlatform(this)">Gabbro<br><span style="color:#888;font-size:10px">Round 2</span><br>260&#xD7;260</button>',
    '</div>',

    // ── Text size selector ───────────────────────────────────────
    '<label class="label">Text Size</label>',
    '<div class="btn-row" id="sizeRow">',
    '  <button class="seg-btn"        data-sz="0" onclick="setSize(this)">Tiny</button>',
    '  <button class="seg-btn"        data-sz="1" onclick="setSize(this)">Small</button>',
    '  <button class="seg-btn active" data-sz="2" onclick="setSize(this)">Medium</button>',
    '  <button class="seg-btn"        data-sz="3" onclick="setSize(this)">Large</button>',
    '  <button class="seg-btn"        data-sz="4" onclick="setSize(this)">XLarge</button>',
    '</div>',

    // ── Estimate info ────────────────────────────────────────────
    '<div class="info-box" id="infoBox">',
    '  Estimated <span id="estPages">?</span> pages &nbsp;&bull;&nbsp; <span id="estKb">?</span> KB',
    '</div>',

    // ── Render button + progress ─────────────────────────────────
    '<button id="btnRender" onclick="doRender()">Render &amp; Connect to Watch</button>',
    '<div id="progressWrap"><progress id="prog" max="100" value="0"></progress></div>',
    '<div id="status">Enter your script above and tap Render.</div>',
    '<canvas id="c"></canvas>',

    // ── Script ───────────────────────────────────────────────────
    '<script>',
    '"use strict";',

    // State — injected from pkjs at build time
    'var WW=' + watchWidth + ', WH=' + watchHeight + ', SZ=' + renderedTextSize + ', HPAD=' + rawHpad + ';',
    'var PADDING=50, FONT_PX=[9,11,14,18,22], FONT_WT=[200,300,400,400,400], MAX_PAGES=60;',
    'var STORED_TEXT=' + JSON.stringify(rawText) + ';',

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

    // ── File import (.txt and .epub) ─────────────────────────────
    'function triggerFileInput(){document.getElementById("fileIn").click();}',
    'function onFileSelected(input){',
    '  var file=input.files&&input.files[0]; if(!file)return;',
    '  document.getElementById("fileLabel").textContent=file.name;',
    '  if(file.name.toLowerCase().slice(-5)===".epub"||file.type==="application/epub+zip"){',
    '    if(typeof DecompressionStream==="undefined"){',
    '      setStatus("EPUB import requires Chrome 80+ / Firefox 113+ / Safari 16.4+. Use the Android app on older browsers.","warn");',
    '      return;',
    '    }',
    '    setStatus("Parsing EPUB...","");',
    '    readEpub(file).then(function(text){',
    '      document.getElementById("txt").value=text;',
    '      onTextChange();',
    '      setStatus("Imported "+file.name+" ("+text.length+" chars).","ok");',
    '    }).catch(function(err){setStatus("EPUB error: "+err.message,"error");});',
    '  } else {',
    '    var fr=new FileReader();',
    '    fr.onload=function(ev){document.getElementById("txt").value=ev.target.result;onTextChange();setStatus("Imported "+file.name+".","ok");};',
    '    fr.onerror=function(){setStatus("Failed to read file.","error");};',
    '    fr.readAsText(file);',
    '  }',
    '}',
    // readEpub: parses EPUB (ZIP) entirely in the browser.
    // Reads ZIP central directory → decompresses with DecompressionStream →
    // extracts OPF spine → strips HTML from each chapter.
    'function readEpub(file){',
    '  return new Promise(function(res,rej){',
    '    var fr=new FileReader();',
    '    fr.onload=function(e){res(e.target.result);};',
    '    fr.onerror=function(){rej(new Error("Read failed"));};',
    '    fr.readAsArrayBuffer(file);',
    '  }).then(function(buf){',
    '    var data=new Uint8Array(buf),view=new DataView(buf),eocd=-1;',
    '    for(var i=data.length-22;i>=Math.max(0,data.length-65578);i--)',
    '      if(view.getUint32(i,true)===0x06054b50){eocd=i;break;}',
    '    if(eocd<0)throw new Error("Not a valid ZIP/EPUB file");',
    '    var cdOff=view.getUint32(eocd+16,true),cdN=view.getUint16(eocd+8,true),ent={},pos=cdOff;',
    '    for(var i=0;i<cdN;i++){',
    '      if(view.getUint32(pos,true)!==0x02014b50)break;',
    '      var mth=view.getUint16(pos+10,true),cs=view.getUint32(pos+20,true);',
    '      var nl=view.getUint16(pos+28,true),xl=view.getUint16(pos+30,true),cl=view.getUint16(pos+32,true);',
    '      var lhO=view.getUint32(pos+42,true);',
    '      var nm=new TextDecoder().decode(data.subarray(pos+46,pos+46+nl));',
    '      var lhx=view.getUint16(lhO+28,true),lhn=view.getUint16(lhO+26,true),ds=lhO+30+lhn+lhx;',
    '      ent[nm]={m:mth,b:data.subarray(ds,ds+cs)};',
    '      pos+=46+nl+xl+cl;',
    '    }',
    '    function dc(e){',
    '      if(e.m===0)return Promise.resolve(e.b);',
    '      var s=new DecompressionStream("deflate-raw"),w=s.writable.getWriter();',
    '      w.write(e.b);w.close();',
    '      var r=s.readable.getReader(),ch=[];',
    '      function pump(){return r.read().then(function(x){',
    '        if(x.done){var t=0;ch.forEach(function(c){t+=c.length;});',
    '          var o=new Uint8Array(t),f=0;ch.forEach(function(c){o.set(c,f);f+=c.length;});return o;}',
    '        ch.push(x.value);return pump();',
    '      });}',
    '      return pump();',
    '    }',
    '    function ge(n){var e=ent[n];return e?dc(e):Promise.resolve(null);}',
    '    return ge("META-INF/container.xml").then(function(b){',
    '      if(!b)throw new Error("Missing META-INF/container.xml");',
    '      var cd=new DOMParser().parseFromString(new TextDecoder().decode(b),"text/xml");',
    '      var rf=cd.querySelector("rootfile"),opf=rf&&rf.getAttribute("full-path");',
    '      if(!opf)throw new Error("Cannot find OPF path");',
    '      var od=opf.lastIndexOf("/"),opfDir=od>=0?opf.slice(0,od):"";',
    '      return ge(opf).then(function(ob){',
    '        if(!ob)throw new Error("Cannot read OPF");',
    '        var od2=new DOMParser().parseFromString(new TextDecoder().decode(ob),"text/xml");',
    '        var mf={};',
    '        od2.querySelectorAll("manifest item").forEach(function(it){',
    '          var id=it.getAttribute("id"),hr=it.getAttribute("href"),mt=it.getAttribute("media-type")||"";',
    '          if(id&&hr&&(mt.indexOf("html")>=0||hr.toLowerCase().slice(-5)===".html"||hr.toLowerCase().slice(-6)===".xhtml"))mf[id]=hr;',
    '        });',
    '        var sp=[];od2.querySelectorAll("spine itemref").forEach(function(r){var v=r.getAttribute("idref");if(v)sp.push(v);});',
    '        var idx=0,parts=[];',
    '        function next(){',
    '          if(idx>=sp.length)return Promise.resolve(parts.join("\\n\\n").trim());',
    '          var hr=mf[sp[idx++]];if(!hr)return next();',
    '          var dec=decodeURIComponent(hr),fp=opfDir?opfDir+"/"+dec:dec;',
    '          return ge(fp).then(function(b2){return b2||ge(dec);}).then(function(b2){',
    '            if(b2){',
    '              var doc=new DOMParser().parseFromString(new TextDecoder().decode(b2),"text/html");',
    '              doc.querySelectorAll("script,style").forEach(function(e){e.remove();});',
    '              var t=((doc.body||doc.documentElement).textContent||"").replace(/[\\t ]+/g," ").replace(/(\\s*\\n){3,}/g,"\\n\\n").trim();',
    '              if(t)parts.push(t);',
    '            }',
    '            return next();',
    '          });',
    '        }',
    '        return next();',
    '      });',
    '    });',
    '  });',
    '}',

    // Live estimate
    'function updateEstimate() {',
    '  var text=document.getElementById("txt").value.trim();',
    '  if (!text){document.getElementById("infoBox").style.display="none";return;}',
    '  var canvas=document.getElementById("c");',
    '  canvas.width=WW; canvas.height=WH;',
    '  var ctx=canvas.getContext("2d"), fpx=FONT_PX[SZ];',
    '  ctx.font=FONT_WT[SZ]+" "+fpx+"px sans-serif";',
    '  var lines=wrapText(ctx,text,WW-HPAD*2), lh=Math.floor(fpx*1.4);',
    '  var n=Math.max(1,Math.ceil((PADDING+lines.length*lh+PADDING)/WH));',
    '  var kb=Math.round(n*Math.ceil(WW/8)*WH/1024);',
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

    // 1-bit encoder (matches WatchImageRenderer.kt: BT.601, MSB-first, threshold 64)
    'function encode1Bit(imageData,w,h) {',
    '  var stride=Math.ceil(w/8), result=new Uint8Array(stride*h), d=imageData.data;',
    '  for (var y=0;y<h;y++) for (var x=0;x<w;x++) {',
    '    var i=(y*w+x)*4, luma=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2];',
    '    if (luma>64) result[y*stride+(x>>3)]|=(0x80>>(x&7));',
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
    '  if (RETURN_TO.indexOf("pebblejs://") !== 0) {',
    '    // CloudPebble: URL length limit prevents passing bitmap data back via redirect.',
    '    // Send text + settings only; index.js renders locally using require("canvas").',
    '    setStatus("Sending script to CloudPebble emulator...","ok");',
    '    finish(null,0);',
    '    return;',
    '  }',
    '  var btn=document.getElementById("btnRender"), prog=document.getElementById("prog");',
    '  btn.disabled=true;',
    '  document.getElementById("progressWrap").style.display="block";',
    '  prog.value=0;',
    '  var canvas=document.getElementById("c");',
    '  canvas.width=WW; canvas.height=WH;',
    '  var ctx=canvas.getContext("2d"), fpx=FONT_PX[SZ], lh=Math.floor(fpx*1.4);',
    '  ctx.font=FONT_WT[SZ]+" "+fpx+"px sans-serif";',
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
    '    ctx.fillStyle="#ffffff"; ctx.font=FONT_WT[SZ]+" "+fpx+"px sans-serif";',
    '    var offsetY=PADDING-pageN*WH;',
    '    for (var li=0;li<lines.length;li++) {',
    '      var y=offsetY+li*lh+fpx;',
    '      if (y>WH+lh) break;',
    '      if (y+lh<0)  continue;',
    '      ctx.fillText(lines[li],HPAD,y);',
    '    }',
    '    var id=ctx.getImageData(0,0,WW,WH);',
    '    pagesOut.push(toBase64(encode1Bit(id,WW,WH)));',
    '    prog.value=Math.round((pageN+1)/numPages*100);',
    '    setStatus("Rendering page "+(pageN+1)+" / "+numPages+"...");',
    '    pageN++;',
    '    setTimeout(renderNext,0);',
    '  }',
    '  renderNext();',
    '}',

    // Send result back to PebbleKit JS
    // $$RETURN_TO$$ is replaced at runtime:
    //   real device  → "pebblejs://close#"  (via data URI in showConfiguration)
    //   CloudPebble  → injected return URL   (via config-proxy.html)
    'var RETURN_TO="$$RETURN_TO$$";',
    'function finish(pagesOut,n) {',
    '  var text=document.getElementById("txt").value.trim();',
    '  var result;',
    '  if (RETURN_TO.indexOf("pebblejs://") === 0) {',
    '    // Real device: pass pre-rendered pages + original text via URL.',
    '    // Including text allows pkjs to re-render when font size changes without re-opening settings.',
    '    setStatus("Done! "+n+(n>1?" pages":" page")+" rendered. Connecting...","ok");',
    '    result={pages:pagesOut,totalPages:n,watchWidth:WW,watchHeight:WH,textSize:SZ,text:text,hpad:HPAD};',
    '  } else {',
    '    // CloudPebble: pass text + settings only; rendering done in index.js.',
    '    setStatus("Script sent. Rendering on CloudPebble emulator...","ok");',
    '    result={text:text,watchWidth:WW,watchHeight:WH,textSize:SZ,hpad:HPAD};',
    '  }',
    '  try {',
    '    document.location=RETURN_TO+encodeURIComponent(JSON.stringify(result));',
    '  } catch(e) {',
    '    setStatus("Error: "+e.message,"error");',
    '    document.getElementById("btnRender").disabled=false;',
    '  }',
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
    '    setStatus("Previous text loaded. Tap Render to refresh pages.","");',
    '  }',
    '}());',

    '<\/script>',
    '</body>',
    '</html>'
  ];

  return lines.join('\n');
}
