/**
 * index.js  –  Peleprompter PebbleKit JS
 *
 * 完整實作手錶端協定（與 Android PebbleCommManager 等效）：
 *
 *   Watch → JS  CMD_REQUEST_TEXT    手錶要求初始化，帶螢幕尺寸與文字大小
 *   JS → Watch  CMD_INIT_IMAGES     回傳總頁數與實際渲染尺寸
 *   Watch → JS  CMD_REQUEST_PAGE    手錶要求第 N 頁圖片資料
 *   JS → Watch  CMD_SEND_IMAGE_CHUNK × M  分塊傳送 1-bit 圖片
 *
 * Config 頁（config.html）在 Pebble 設定介面中開啟，讓使用者貼上文字、
 * 選擇平台與文字大小，並在手機端用 Canvas 預先渲染全部頁面，
 * 完成後透過 pebblejs://close# 將結果傳回本檔。
 *
 * Android companion app 變為選配：使用 PebbleKit JS 時不需安裝。
 */

'use strict';

// ══════════════════════════════════════════════════════════════════
// 協定常數（與 constants.h 完全一致）
// ══════════════════════════════════════════════════════════════════
var KEY_COMMAND         = 0;
var KEY_SCROLL_SPEED    = 3;
var KEY_TEXT_SIZE       = 4;
var KEY_PAGE_NUM        = 5;
var KEY_CHUNK_INDEX     = 6;
var KEY_TOTAL_CHUNKS    = 7;
var KEY_IMAGE_DATA      = 8;
var KEY_TOTAL_PAGES     = 9;
var KEY_WATCH_WIDTH     = 10;
var KEY_WATCH_HEIGHT    = 11;

var CMD_REQUEST_TEXT    = 0;
var CMD_SYNC_SETTINGS   = 2;
var CMD_INIT_IMAGES     = 3;
var CMD_REQUEST_PAGE    = 4;
var CMD_SEND_IMAGE_CHUNK= 5;

/**
 * 每個區塊最多傳送的位元組數。
 * 設為 1800 而非 1900，為 PebbleKit JS AppMessage header 預留更多空間。
 * 每筆訊息含 4 個 Int32 欄位（44B） + 1 Bytes 欄位 (7 + N B)，共 51 + N B。
 * 1800 + 51 = 1851 < INBOX_SIZE 2048，安全。
 */
var IMAGE_CHUNK_DATA_SIZE = 1800;

// ══════════════════════════════════════════════════════════════════
// 狀態
// ══════════════════════════════════════════════════════════════════
/** 已解碼的頁面資料，每頁為 number[] (0-255)，對應 GBitmapFormat1Bit 位元組 */
var pages       = [];
var totalPages  = 0;
var watchWidth  = 144;
var watchHeight = 168;
var textSize    = 1;     // TEXT_SIZE_MEDIUM

/** ACK 驅動發送佇列（同 Android PebbleCommManager） */
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
    function() {
      isSending = false;
      processQueue();
    },
    function(e) {
      console.log('[pkjs] sendAppMessage failed: ' + JSON.stringify(e));
      isSending = false;
      processQueue();   // 繼續下一筆，不重試失敗的訊息
    }
  );
}

// ══════════════════════════════════════════════════════════════════
// AppMessage 事件
// ══════════════════════════════════════════════════════════════════
Pebble.addEventListener('ready', function() {
  console.log('[pkjs] PebbleKit JS ready. Pages loaded: ' + pages.length);
});

Pebble.addEventListener('appmessage', function(e) {
  var dict = e.payload;
  var cmd  = dict[KEY_COMMAND];
  if (cmd === undefined || cmd === null) return;

  switch (cmd) {
    case CMD_REQUEST_TEXT:
      handleInitRequest(dict);
      break;
    case CMD_REQUEST_PAGE:
      handlePageRequest(dict);
      break;
    case CMD_SYNC_SETTINGS:
      handleSettingsSync(dict);
      break;
    default:
      console.log('[pkjs] Unknown command: ' + cmd);
  }
});

// ── CMD_REQUEST_TEXT ─────────────────────────────────────────────
function handleInitRequest(dict) {
  var reqW    = parseInt(dict[KEY_WATCH_WIDTH])  || 144;
  var reqH    = parseInt(dict[KEY_WATCH_HEIGHT]) || 168;
  var reqSize = (dict[KEY_TEXT_SIZE] !== undefined && dict[KEY_TEXT_SIZE] !== null)
                  ? parseInt(dict[KEY_TEXT_SIZE])
                  : textSize;

  console.log('[pkjs] Init request: ' + reqW + 'x' + reqH +
              ', textSize=' + reqSize +
              ', pages loaded: ' + pages.length);

  if (pages.length === 0) {
    // 尚未載入文字 — 提示使用者開啟設定頁面
    console.log('[pkjs] No pages loaded. Open watch app settings to paste text.');
    return;
  }

  // 若文字大小與已渲染版本不符，記錄警告但仍繼續（使用者可重新開啟設定）
  if (reqSize !== textSize) {
    console.log('[pkjs] Text size mismatch (request=' + reqSize +
                ', rendered=' + textSize + '). Using rendered version.');
  }

  // 傳送已渲染頁面的尺寸（而非手錶回報的尺寸），讓 watch C code 依此設定 ImageManager
  var reply = {};
  reply[KEY_COMMAND]      = CMD_INIT_IMAGES;
  reply[KEY_TOTAL_PAGES]  = totalPages;
  reply[KEY_WATCH_WIDTH]  = watchWidth;
  reply[KEY_WATCH_HEIGHT] = watchHeight;
  enqueue(reply);

  console.log('[pkjs] Sent CMD_INIT_IMAGES: ' + totalPages + ' pages, ' +
              watchWidth + 'x' + watchHeight);
}

// ── CMD_REQUEST_PAGE ─────────────────────────────────────────────
function handlePageRequest(dict) {
  var pageNum = parseInt(dict[KEY_PAGE_NUM]);
  if (isNaN(pageNum) || pageNum < 0 || pageNum >= totalPages) {
    console.log('[pkjs] Invalid page request: ' + pageNum +
                ' (total=' + totalPages + ')');
    return;
  }

  var pageData = pages[pageNum];
  if (!pageData || pageData.length === 0) {
    console.log('[pkjs] Page data missing: ' + pageNum);
    return;
  }

  sendPageInChunks(pageNum, pageData);
}

// ── CMD_SYNC_SETTINGS ────────────────────────────────────────────
function handleSettingsSync(dict) {
  if (dict[KEY_TEXT_SIZE] !== undefined && dict[KEY_TEXT_SIZE] !== null) {
    var newSize = parseInt(dict[KEY_TEXT_SIZE]);
    if (newSize !== textSize) {
      console.log('[pkjs] Text size changed to ' + newSize +
                  '. Re-open settings to re-render.');
      textSize = newSize;
    }
  }
}

// ── 分塊發送 ─────────────────────────────────────────────────────
function sendPageInChunks(pageNum, data) {
  var totalChunks = Math.ceil(data.length / IMAGE_CHUNK_DATA_SIZE);

  for (var i = 0; i < totalChunks; i++) {
    var start = i * IMAGE_CHUNK_DATA_SIZE;
    var end   = Math.min(start + IMAGE_CHUNK_DATA_SIZE, data.length);

    // PebbleKit JS 的 Bytes 欄位使用純 Array，不接受 TypedArray
    var chunk = [];
    for (var j = start; j < end; j++) {
      chunk.push(data[j]);
    }

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
// 設定頁面
// ══════════════════════════════════════════════════════════════════
Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(buildConfigDataUri());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response || e.response === 'CANCELLED' || e.response === '') {
    console.log('[pkjs] Config closed without data');
    return;
  }

  try {
    var result = JSON.parse(decodeURIComponent(e.response));

    if (!result || !Array.isArray(result.pages) || result.pages.length === 0) {
      console.log('[pkjs] Config returned empty page set');
      return;
    }

    // 解碼 base64 頁面資料為 number[] 陣列
    pages       = result.pages.map(base64ToArray);
    totalPages  = result.totalPages  || pages.length;
    watchWidth  = result.watchWidth  || 144;
    watchHeight = result.watchHeight || 168;
    textSize    = result.textSize    !== undefined ? result.textSize : 1;

    console.log('[pkjs] Loaded ' + totalPages + ' pages (' +
                watchWidth + 'x' + watchHeight + ', textSize=' + textSize + ')');

    // 儲存到 localStorage 以便下次開啟 app 時恢復
    try {
      localStorage.setItem('pele_meta', JSON.stringify({
        totalPages: totalPages, watchWidth: watchWidth,
        watchHeight: watchHeight, textSize: textSize
      }));
      result.pages.forEach(function(b64, i) {
        localStorage.setItem('pele_page_' + i, b64);
      });
      console.log('[pkjs] Pages saved to localStorage');
    } catch (lsErr) {
      console.log('[pkjs] localStorage save failed: ' + lsErr);
    }

  } catch (ex) {
    console.log('[pkjs] Failed to parse config response: ' + ex.message);
  }
});

/** base64 字串 → number[] (0-255) */
function base64ToArray(b64) {
  var binary = atob(b64);
  var arr    = new Array(binary.length);
  for (var i = 0; i < binary.length; i++) {
    arr[i] = binary.charCodeAt(i);
  }
  return arr;
}

// ── 從 localStorage 恢復上次的頁面資料 ──────────────────────────
(function restoreFromStorage() {
  try {
    var metaStr = localStorage.getItem('pele_meta');
    if (!metaStr) return;

    var meta = JSON.parse(metaStr);
    var n    = meta.totalPages || 0;
    if (n === 0) return;

    var restored = [];
    for (var i = 0; i < n; i++) {
      var b64 = localStorage.getItem('pele_page_' + i);
      if (!b64) { restored = []; break; }
      restored.push(base64ToArray(b64));
    }

    if (restored.length === n) {
      pages       = restored;
      totalPages  = meta.totalPages;
      watchWidth  = meta.watchWidth  || 144;
      watchHeight = meta.watchHeight || 168;
      textSize    = meta.textSize    !== undefined ? meta.textSize : 1;
      console.log('[pkjs] Restored ' + totalPages + ' pages from localStorage');
    }
  } catch (e) {
    console.log('[pkjs] localStorage restore failed: ' + e);
  }
}());

// ══════════════════════════════════════════════════════════════════
// 嵌入式 Config 頁面（config.html 的 data URI 版本）
//
// 此函式將 config.html 的完整內容以 data URI 形式傳入 Pebble.openURL()。
// 優點：完全離線，不需要網路或伺服器。
// 若要改用外部 URL（例如 GitHub Pages），請將函式內容替換為：
//   return 'https://your-hosted-config-page-url';
// ══════════════════════════════════════════════════════════════════
function buildConfigDataUri() {
  return 'data:text/html;charset=utf-8,' + encodeURIComponent(CONFIG_HTML);
}

/* eslint-disable */
// CONFIG_HTML 是 config.html 的嵌入式副本，透過建置腳本產生或手動更新。
// 如需修改設定頁面的 UI，請先修改 src/pkjs/config.html，再更新此常數。
var CONFIG_HTML = '<!DOCTYPE html>\n<html lang="en">\n<head>\n  <meta charset="utf-8">\n  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">\n  <title>Peleprompter</title>\n  <style>\n    *, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }\n    body { background: #121212; color: #ffffff; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; padding: 16px; max-width: 480px; margin: 0 auto; font-size: 15px; -webkit-text-size-adjust: 100%; }\n    h2 { color: #BB86FC; font-size: 20px; margin-bottom: 18px; font-weight: 700; }\n    .label { display: block; color: #b0b0b0; font-size: 12px; text-transform: uppercase; letter-spacing: 0.05em; margin-bottom: 6px; margin-top: 16px; }\n    textarea { width: 100%; height: 160px; background: #1e1e1e; border: 1px solid #333333; border-radius: 8px; color: #ffffff; padding: 12px; font-size: 14px; line-height: 1.5; resize: vertical; -webkit-appearance: none; }\n    textarea:focus { outline: none; border-color: #BB86FC; }\n    textarea::placeholder { color: #555555; }\n    .btn-row { display: flex; gap: 8px; margin-top: 0; }\n    .seg-btn { flex: 1; padding: 10px 4px; background: #1e1e1e; border: 1px solid #444444; border-radius: 6px; color: #ffffff; font-size: 12px; text-align: center; cursor: pointer; -webkit-tap-highlight-color: transparent; line-height: 1.4; }\n    .seg-btn.active { background: #BB86FC; border-color: #BB86FC; color: #000000; font-weight: 600; }\n    #btnRender { display: block; width: 100%; margin-top: 20px; padding: 14px; background: #03DAC5; border: none; border-radius: 8px; color: #000000; font-size: 16px; font-weight: 700; cursor: pointer; -webkit-tap-highlight-color: transparent; }\n    #btnRender:disabled { opacity: 0.4; cursor: not-allowed; }\n    #progressWrap { margin-top: 12px; display: none; }\n    progress { width: 100%; height: 6px; -webkit-appearance: none; appearance: none; border: none; border-radius: 3px; background: #333333; }\n    progress::-webkit-progress-bar { background: #333333; border-radius: 3px; }\n    progress::-webkit-progress-value { background: #03DAC5; border-radius: 3px; }\n    progress::-moz-progress-bar { background: #03DAC5; border-radius: 3px; }\n    #status { margin-top: 10px; font-size: 13px; color: #888888; min-height: 18px; line-height: 1.4; }\n    #status.warn { color: #FFB74D; } #status.error { color: #EF9A9A; } #status.ok { color: #81C784; }\n    canvas { display: none; }\n    .info-box { background: #1a1a1a; border: 1px solid #333; border-radius: 8px; padding: 10px 12px; margin-top: 12px; font-size: 12px; color: #888888; display: none; }\n    .info-box span { color: #BB86FC; font-weight: 600; }\n  </style>\n</head>\n<body>\n  <h2>&#x1F4FA; Peleprompter</h2>\n  <label class="label">Text Content</label>\n  <textarea id="txt" placeholder="Paste your script here..." oninput="onTextChange()"></textarea>\n  <label class="label">Platform</label>\n  <div class="btn-row" id="platRow">\n    <button class="seg-btn active" data-w="144" data-h="168" onclick="setPlatform(this)">Aplite / Basalt<br>144 &times; 168</button>\n    <button class="seg-btn" data-w="180" data-h="180" onclick="setPlatform(this)">Chalk<br>180 &times; 180</button>\n    <button class="seg-btn" data-w="200" data-h="228" onclick="setPlatform(this)">Emery<br>200 &times; 228</button>\n    <button class="seg-btn" data-w="260" data-h="260" onclick="setPlatform(this)">Gabbro<br>260 &times; 260</button>\n  </div>\n  <label class="label">Text Size</label>\n  <div class="btn-row" id="sizeRow">\n    <button class="seg-btn" data-sz="0" onclick="setSize(this)">Small</button>\n    <button class="seg-btn active" data-sz="1" onclick="setSize(this)">Medium</button>\n    <button class="seg-btn" data-sz="2" onclick="setSize(this)">Large</button>\n  </div>\n  <div class="info-box" id="infoBox">Estimated <span id="estPages">?</span> pages &nbsp;&bull;&nbsp; <span id="estKb">?</span> KB image data</div>\n  <button id="btnRender" onclick="doRender()">Render &amp; Connect to Watch</button>\n  <div id="progressWrap"><progress id="prog" max="100" value="0"></progress></div>\n  <div id="status">Enter your script above and tap Render.</div>\n  <canvas id="c"></canvas>\n  <script>\n  "use strict";\n  var WW=144,WH=168,SZ=1,PADDING=50,FONT_PX=[14,18,22],MAX_PAGES=60;\n  function setPlatform(btn){document.querySelectorAll("#platRow .seg-btn").forEach(function(b){b.className="seg-btn";});btn.className="seg-btn active";WW=parseInt(btn.getAttribute("data-w"));WH=parseInt(btn.getAttribute("data-h"));updateEstimate();}\n  function setSize(btn){document.querySelectorAll("#sizeRow .seg-btn").forEach(function(b){b.className="seg-btn";});btn.className="seg-btn active";SZ=parseInt(btn.getAttribute("data-sz"));updateEstimate();}\n  function setStatus(msg,cls){var el=document.getElementById("status");el.textContent=msg;el.className=cls||"";}\n  function onTextChange(){updateEstimate();}\n  function updateEstimate(){var text=document.getElementById("txt").value.trim();if(!text){document.getElementById("infoBox").style.display="none";return;}var canvas=document.getElementById("c");canvas.width=WW;canvas.height=WH;var ctx=canvas.getContext("2d");var fpx=FONT_PX[SZ];ctx.font=fpx+"px sans-serif";var lines=wrapText(ctx,text,WW-4);var lh=Math.floor(fpx*1.4);var totalH=PADDING+lines.length*lh+PADDING;var n=Math.max(1,Math.ceil(totalH/WH));var stride=Math.ceil(WW/8);var kb=Math.round(n*stride*WH/1024);document.getElementById("estPages").textContent=n;document.getElementById("estKb").textContent=kb;document.getElementById("infoBox").style.display="block";}\n  function wrapText(ctx,text,maxW){var lines=[],paras=text.split("\\n");for(var p=0;p<paras.length;p++){var para=paras[p];if(!para.trim()){lines.push("");continue;}var words=para.split(" "),line="";for(var i=0;i<words.length;i++){var test=line?line+" "+words[i]:words[i];if(ctx.measureText(test).width<=maxW){line=test;}else{if(line)lines.push(line);line=words[i];}}if(line)lines.push(line);}return lines;}\n  function encode1Bit(imageData,w,h){var stride=Math.ceil(w/8),result=new Uint8Array(stride*h),d=imageData.data;for(var y=0;y<h;y++)for(var x=0;x<w;x++){var i=(y*w+x)*4,luma=0.299*d[i]+0.587*d[i+1]+0.114*d[i+2];if(luma>64){result[y*stride+(x>>3)]|=(0x80>>(x&7));}}return result;}\n  function toBase64(bytes){var s="",CHUNK=8192;for(var i=0;i<bytes.length;i+=CHUNK){s+=String.fromCharCode.apply(null,bytes.subarray(i,i+CHUNK));}return btoa(s);}\n  function doRender(){var text=document.getElementById("txt").value.trim();if(!text){setStatus("Please enter some text first.","warn");return;}var btn=document.getElementById("btnRender");var prog=document.getElementById("prog");btn.disabled=true;document.getElementById("progressWrap").style.display="block";prog.value=0;var canvas=document.getElementById("c");canvas.width=WW;canvas.height=WH;var ctx=canvas.getContext("2d");var fpx=FONT_PX[SZ],lh=Math.floor(fpx*1.4);ctx.font=fpx+"px sans-serif";var lines=wrapText(ctx,text,WW-4);var totalH=PADDING+lines.length*lh+PADDING;var numPages=Math.max(1,Math.min(MAX_PAGES,Math.ceil(totalH/WH)));if(Math.ceil(totalH/WH)>MAX_PAGES){setStatus("Text truncated to "+MAX_PAGES+" pages.","warn");}else{setStatus("Rendering "+numPages+" page"+(numPages>1?"s":"")+"...");}var pages=[],pageN=0;function renderNext(){if(pageN>=numPages){finish(pages,numPages);return;}ctx.fillStyle="#000000";ctx.fillRect(0,0,WW,WH);ctx.fillStyle="#ffffff";ctx.font=fpx+"px sans-serif";var offsetY=PADDING-pageN*WH;for(var li=0;li<lines.length;li++){var y=offsetY+li*lh+fpx;if(y>WH+lh)break;if(y+lh<0)continue;ctx.fillText(lines[li],2,y);}var id=ctx.getImageData(0,0,WW,WH);pages.push(toBase64(encode1Bit(id,WW,WH)));prog.value=Math.round((pageN+1)/numPages*100);setStatus("Rendering page "+(pageN+1)+" / "+numPages+"...");pageN++;setTimeout(renderNext,0);}renderNext();}\n  function finish(pages,n){setStatus("Done! "+n+" page"+(n>1?"s":"")+" rendered. Sending to watch...","ok");var result={pages:pages,totalPages:n,watchWidth:WW,watchHeight:WH,textSize:SZ};try{document.location="pebblejs://close#"+encodeURIComponent(JSON.stringify(result));}catch(e){setStatus("Error: "+e.message,"error");document.getElementById("btnRender").disabled=false;}}\n  </script>\n</body>\n</html>';
/* eslint-enable */
