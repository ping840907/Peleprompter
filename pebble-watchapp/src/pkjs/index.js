/**
 * index.js - PebbleKit JS 橋接層
 *
 * 此檔案為 Pebble 建置系統所必需。
 * 實際通訊由 Android 端的 PebbleKit Android SDK 直接處理。
 * 此 JS 層僅作為備用訊息轉發通道。
 */

Pebble.addEventListener('ready', function() {
    console.log('Peleprompter JS bridge ready');
});

// 將手錶的 AppMessage 轉發給 Android companion app (若需要)
Pebble.addEventListener('appmessage', function(e) {
    console.log('Received appmessage from watch: ' + JSON.stringify(e.payload));
});
