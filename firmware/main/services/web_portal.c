#include "web_portal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "audio/audio_passthrough.h"
#include "esp_check.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "services/config_store.h"
#include "services/fmo_activate.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_discovery.h"
#include "services/fmo_link.h"
#include "services/fmo_qso.h"
#include "services/fmo_station_beacon.h"
#include "services/aprs_service.h"
#include "services/ota_service.h"
#include "services/radio_at.h"
#include "audio/nrl_audio_codec.h"
#include "app/driver/status_io.h"
#include "app/driver/es8311_codec.h"
#include "services/network_manager.h"
#include "services/net_radio.h"
#include "services/nrl_link.h"
#include "services/server_directory.h"
#include "version.h"

static const char *TAG = "web";
static httpd_handle_t s_server;

static const char k_index_html[] __attribute__((unused)) =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Open FMO</title><style>body{font:16px sans-serif;max-width:760px;margin:auto;padding:16px;background:#111;color:#eee}"
    "fieldset{margin:12px 0;border:1px solid #f80}label{display:grid;grid-template-columns:1fr 1.5fr;margin:7px}"
    "input,select,button{font:inherit;padding:7px}button{background:#f80;border:0;font-weight:bold;width:100%}.hint{color:#aaa}</style></head>"
    "<body><h1>Open FMO</h1><p><a href=/update style='color:#f80'>Firmware update / OTA</a></p>"
    "<p class=hint>配置热点免密码，Wi-Fi 修改在重启后生效。</p>"
    "<form method=post action=/save>"
    "<fieldset><legend>Identity / NRL</legend><label>Callsign<input name=callsign></label><label>Callsign SSID<input name=callsign_ssid type=number min=0 max=15></label><label>NRL host<input name=nrl_host></label>"
    "<label>NRL port<input name=nrl_port type=number></label></fieldset>"
    "<fieldset><legend>Wi-Fi 配网</legend><label>附近 Wi-Fi<select id=wifi_list onchange=\"wifi_ssid.value=this.value\"><option>正在扫描...</option></select></label>"
    "<button type=button onclick=scanWifi()>重新扫描</button><label>SSID<input id=wifi_ssid name=wifi_ssid></label>"
    "<label>密码<input name=wifi_password type=password placeholder='留空则保留原密码'></label></fieldset>"
    "<fieldset><legend>RF Audio</legend><label>RX MHz<input name=rx_mhz type=number step=.0001></label>"
    "<label>TX MHz<input name=tx_mhz type=number step=.0001></label><label>RX CTCSS Hz<input name=rx_ctcss type=number step=.1></label>"
    "<label>TX CTCSS Hz<input name=tx_ctcss type=number step=.1></label><label>Squelch 0-10<input name=squelch type=number min=0 max=10></label>"
    "<label>Power<select name=tx_power><option value=0>LOW</option><option value=1>MID</option><option value=2>HIGH</option></select></label>"
    "<label>RF enable<input name=rf_enabled type=checkbox value=1></label></fieldset><button>Save and apply</button></form>"
    "<pre id=status></pre><script>fetch('/api/config').then(r=>r.json()).then(c=>{for(const[k,v]of Object.entries(c)){let e=document.querySelector('[name='+k+']');"
    "if(!e||k==='wifi_password')continue;if(e.type==='checkbox')e.checked=!!v;else e.value=v}});"
    "fetch('/api/status').then(r=>r.text()).then(t=>status.textContent=t);"
    "async function scanWifi(){wifi_list.innerHTML='<option>正在扫描...</option>';try{let a=await(await fetch('/scan',{cache:'no-store'})).json();wifi_list.innerHTML='<option value=\"\">请选择热点</option>';a.forEach(x=>{let o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' ('+x.rssi+' dBm)';wifi_list.appendChild(o)})}catch(e){wifi_list.innerHTML='<option>扫描失败，请重试</option>'}}scanWifi()</script></body></html>";

static const char k_nav_html[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Open FMO</title><style>body{font:16px sans-serif;max-width:760px;margin:auto;padding:18px;background:#111;color:#eee}"
    "h1{margin-bottom:4px}.hint{color:#aaa;margin-top:0}.nav{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;margin-top:24px}"
    ".nav a{display:flex;min-height:76px;align-items:center;justify-content:center;padding:14px;border:2px solid #f80;border-radius:10px;background:#241600;color:#fff;text-decoration:none;font-size:19px;font-weight:bold;text-align:center}"
    ".nav a:hover,.nav a:focus{background:#f80;color:#000}@media(max-width:560px){.nav{grid-template-columns:1fr}.nav a{min-height:60px}}</style></head>"
    "<body><h1>Open FMO</h1><p class=hint>&#35831;&#36873;&#25321;&#35201;&#37197;&#32622;&#30340;&#39033;&#30446;&#65292;&#27599;&#39033;&#21333;&#29420;&#20445;&#23384;&#12290;</p><nav class=nav>"
    "<a href=/config/identity>NRL &#21628;&#21495;&#19982;&#26381;&#21153;&#22120;</a>"
    "<a href=/servers>FMO &#26381;&#21153;&#22120;&#19982;&#35777;&#20070;</a>"
    "<a href=/config/wifi>Wi-Fi &#37197;&#32593;</a>"
    "<a href=/config/radio>&#23556;&#39057;&#26495;&#35774;&#32622;</a>"
    "<a href=/config/audio>&#38899;&#39057;&#35774;&#32622;</a>"
    "<a href=/config/aprs>APRS-IS &#35774;&#32622;</a>"
    "<a href=/config/netradio>&#32593;&#32476;&#30005;&#21488;</a>"
    "<a href=/update>&#22266;&#20214;&#21319;&#32423; / OTA</a>"
    "</nav></body></html>";

/* Keep provisioning independent from the full settings page.  The full page
 * queries services which are deliberately not started while BLE provisioning
 * is active, and a JavaScript error there used to leave every form hidden. */
static const char k_wifi_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Open FMO Wi-Fi</title><style>"
    "body{font:16px sans-serif;max-width:620px;margin:auto;padding:18px;background:#111;color:#eee}"
    "fieldset{border:1px solid #f80;border-radius:8px;padding:14px}"
    "label{display:block;margin:12px 0}input,select,button{box-sizing:border-box;font:inherit;padding:10px;width:100%}"
    "button{margin:8px 0;background:#f80;border:0;font-weight:bold}a{color:#f80}.hint{color:#aaa}"
    ".saved{display:flex;gap:8px;align-items:center;margin:8px 0}.saved span{flex:1}.saved button{width:auto;margin:0}"
    "#msg{min-height:24px}</style></head><body>"
    "<p><a href=/>&lt; &#36820;&#22238;</a></p><h1>Wi-Fi &#37197;&#32593;</h1>"
    "<p class=hint>&#36830;&#25509; OpenFMO-xxxx &#28909;&#28857;&#21518;&#65292;&#22312;&#26412;&#39029;&#28155;&#21152;&#23478;&#37324;&#30340; Wi-Fi&#12290;</p>"
    "<fieldset><legend>&#24050;&#20445;&#23384;&#30340;&#32593;&#32476;</legend><div id=saved>&#21152;&#36733;&#20013;...</div></fieldset>"
    "<form id=form><fieldset><legend>&#28155;&#21152; Wi-Fi</legend>"
    "<button type=button id=scanbtn>&#37325;&#26032;&#25195;&#25551;&#38468;&#36817; Wi-Fi</button>"
    "<label>&#36873;&#25321;&#28909;&#28857;<select id=list><option value=''>-- &#27491;&#22312;&#35835;&#21462;&#28909;&#28857; --</option></select></label>"
    "<label>SSID<input id=ssid name=wifi_ssid maxlength=32 required></label>"
    "<label>&#23494;&#30721;<input name=wifi_password type=password maxlength=64></label>"
    "<input type=hidden name=section value=wifi><button id=savebtn>&#20445;&#23384;&#24182;&#36830;&#25509;</button>"
    "</fieldset></form><p id=msg></p><script>"
    "const q=s=>document.querySelector(s),msg=q('#msg'),saved=q('#saved'),list=q('#list'),ssid=q('#ssid');"
    "function esc(v){let d=document.createElement('div');d.textContent=v;return d.innerHTML}"
    "async function load(){try{let r=await fetch('/api/wifi',{cache:'no-store'});if(!r.ok)throw Error(await r.text());"
    "let d=await r.json();saved.innerHTML=d.profiles.length?'':'<span class=hint>\u6682\u65e0</span>';"
    "d.profiles.forEach(x=>saved.innerHTML+='<div class=saved><span>'+esc(x.ssid)+'</span><button onclick=del('+x.index+')>\u5220\u9664</button></div>')}"
    "catch(e){saved.textContent='\u52a0\u8f7d\u5931\u8d25: '+e.message}}"
    "async function scan(url){let b=q('#scanbtn');b.disabled=true;b.textContent='\u626b\u63cf\u4e2d...';msg.textContent='';"
    "try{let r=await fetch(url,{cache:'no-store'});if(!r.ok)throw Error(await r.text());let a=await r.json();"
    "list.innerHTML='<option value=\"\">-- \u8bf7\u9009\u62e9 --</option>';a.forEach(x=>{let o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' ('+x.rssi+' dBm)';list.appendChild(o)});"
    "msg.textContent='\u626b\u63cf\u5b8c\u6210\uff0c\u5171 '+a.length+' \u4e2a\u70ed\u70b9'}catch(e){msg.textContent='\u626b\u63cf\u5931\u8d25: '+e.message}"
    "b.disabled=false;b.textContent='\u91cd\u65b0\u626b\u63cf\u9644\u8fd1 Wi-Fi'}"
    "q('#scanbtn').onclick=()=>scan('/scan');"
    "list.onchange=()=>{if(list.value)ssid.value=list.value};"
    "q('#form').onsubmit=async e=>{e.preventDefault();let b=q('#savebtn');b.disabled=true;msg.textContent='\u4fdd\u5b58\u4e2d...';"
    "try{let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))});"
    "if(!r.ok)throw Error(await r.text());msg.textContent='\u5df2\u4fdd\u5b58\uff0c\u6b63\u5728\u8fde\u63a5 Wi-Fi...';await load()}"
    "catch(x){msg.textContent='\u4fdd\u5b58\u5931\u8d25: '+x.message}b.disabled=false};"
    "async function del(i){if(!confirm('\u5220\u9664\u8fd9\u4e2a Wi-Fi\uff1f'))return;let r=await fetch('/wifi/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'index='+i});if(r.ok)load();else msg.textContent=await r.text()}"
    "load();scan('/scan/cached')</script></body></html>";

static const char k_index_html_v2[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Open FMO</title><style>body{font:16px sans-serif;max-width:760px;margin:auto;padding:16px;background:#111;color:#eee}"
    "fieldset{margin:12px 0;border:1px solid #f80}label{display:grid;grid-template-columns:1fr 1.5fr;margin:7px}"
    "input,select,button{font:inherit;padding:7px}button{background:#f80;border:0;font-weight:bold;cursor:pointer}.wide{width:100%}"
    "button:disabled{opacity:.5;cursor:wait}.saved{display:flex;gap:8px;align-items:center;margin:7px}.saved span{flex:1}.hint{color:#aaa}a{color:#f80}.page,#netradio,#status{display:none}"
    "table{width:100%;border-collapse:collapse}th,td{padding:7px;border-bottom:1px solid #333;text-align:left}tr.sel{background:#352000}.pick{white-space:nowrap}"
    "#toast{position:fixed;top:12px;left:50%;transform:translateX(-50%);padding:10px 24px;border-radius:6px;font-weight:bold;display:none;z-index:99}"
    "#toast.ok{display:block;background:#0a0;color:#fff}#toast.err{display:block;background:#d00;color:#fff}"
    "#tone_ov{display:none;position:fixed;inset:0;background:rgba(0,0,0,.75);z-index:50;justify-content:center;align-items:center}"
    "#tone_ov.open{display:flex}#tone_box{background:#1c1c1c;border:1px solid #f80;border-radius:8px;padding:14px;max-width:440px;width:92%;max-height:80vh;overflow-y:auto}"
    "#tone_box h3{margin:0 0 10px;color:#f80}#tone_grid{display:grid;grid-template-columns:repeat(5,1fr);gap:6px}"
    ".tn{padding:9px 2px;background:#2a2a2a;border:1px solid #444;color:#eee;cursor:pointer;border-radius:4px;font-size:13px;text-align:center}"
    ".tn:hover{border-color:#f80;background:#333}.tn.cur{background:#f80;color:#000;font-weight:bold;border-color:#f80}"
    "#status{background:#1a1a1a;padding:10px;border-radius:6px;font-size:13px;white-space:pre-wrap}</style></head>"
    "<body><div id=toast></div><p><a href=/>&lt; &#37197;&#32622;&#23548;&#33322;</a></p><h1 id=page_title>Open FMO</h1>"
    "<p class=hint>&#37197;&#32622;&#28909;&#28857;&#20813;&#23494;&#30721;&#65306;&#36830;&#25509; OpenFMO-xxxx &#21518;&#35775;&#38382; 192.168.4.1&#12290;</p>"
    "<form class=page data-page=identity onsubmit='event.preventDefault();saveForm(this)'><input type=hidden name=section value=identity>"
    "<fieldset id=identity><legend>NRL &#36523;&#20221;</legend><label>NRL &#21628;&#21495;<input name=callsign></label><label>NRL SSID<input name=callsign_ssid type=number min=0 max=255></label>"
    "<label>&#24403;&#21069; NRL &#26381;&#21153;&#22120;<input name=nrl_host></label><label>&#24403;&#21069; NRL &#31471;&#21475;<input name=nrl_port type=number></label>"
    "<button class=wide>&#20445;&#23384; NRL &#37197;&#32622;</button></fieldset>"
    "<fieldset><legend>NRL &#26381;&#21153;&#22120;&#21015;&#34920;</legend><p class=hint>&#36873;&#25321;&#21518;&#20250;&#31435;&#21363;&#26356;&#26032;&#19978;&#26041;&#30340;&#24403;&#21069;&#26381;&#21153;&#22120;&#12290;</p>"
    "<table><thead><tr><th>&#21517;&#31216;</th><th>&#22320;&#22336;</th><th>&#22312;&#32447;</th><th></th></tr></thead><tbody id=nrl_servers><tr><td colspan=4>&#21152;&#36733;&#20013;...</td></tr></tbody></table></fieldset></form>"
    "<form class=page data-page=wifi onsubmit='event.preventDefault();saveForm(this)'><input type=hidden name=section value=wifi>"
    "<fieldset id=wifi><legend>Wi-Fi &#37197;&#32593;&#65288;&#26368;&#22810; 5 &#20010;&#65289;</legend><div id=saved_wifi></div>"
    "<label>&#38468;&#36817; Wi-Fi<select id=wifi_list onchange=\"wifi_ssid.value=this.value\"><option>&#27491;&#22312;&#25195;&#25551;...</option></select></label>"
    "<button class=wide type=button onclick=scanWifi()>&#37325;&#26032;&#25195;&#25551;</button><label>SSID<input id=wifi_ssid name=wifi_ssid></label>"
    "<label>&#23494;&#30721;<input name=wifi_password type=password placeholder='&#30041;&#31354;&#21017;&#20445;&#30041;&#24050;&#23384;&#23494;&#30721;'></label>"
    "<button class=wide>&#28155;&#21152;&#32593;&#32476;</button></fieldset></form>"
    "<form class=page data-page=radio onsubmit='event.preventDefault();saveForm(this)'><input type=hidden name=section value=radio>"
    "<fieldset id=radio><legend>&#23556;&#39057;&#26495;&#35774;&#32622;</legend>"
    "<label>&#21551;&#29992;&#23556;&#39057;&#26495;<input name=rf_enabled type=checkbox value=1></label>"
    "<label>&#25509;&#25910; MHz<input name=rx_mhz type=number step=.0001></label>"
    "<label>&#21457;&#23556; MHz<input name=tx_mhz type=number step=.0001></label>"
    "<label>&#25509;&#25910;&#20122;&#38899;<button type=button id=rx_ctcss_btn onclick=\"tonePick('rx_ctcss')\">OFF</button><input type=hidden name=rx_ctcss id=rx_ctcss value=0></label>"
    "<label>&#21457;&#23556;&#20122;&#38899;<button type=button id=tx_ctcss_btn onclick=\"tonePick('tx_ctcss')\">OFF</button><input type=hidden name=tx_ctcss id=tx_ctcss value=0></label>"
    "<label>&#38745;&#22122; 0-10<input name=squelch type=number min=0 max=10></label>"
    "<label>&#21151;&#29575;<select name=tx_power><option value=0>&#20302;</option><option value=1>&#20013;</option><option value=2>&#39640;</option></select></label>"
    "<label>&#23556;&#39057;&#26495; RX &#38899;&#37327; 0-10<input name=rx_volume type=number min=0 max=10></label>"
    "<label>&#23556;&#39057;&#26495; TX &#38899;&#37327; 0-10<input name=tx_volume type=number min=0 max=10></label>"
    "<label>&#39057;&#29575;&#20559;&#31227; Hz<input name=freq_tune type=number min=-5000 max=5000 step=10 placeholder=0></label>"
    "<button class=wide>&#21333;&#29420;&#20445;&#23384;&#23556;&#39057;&#37197;&#32622;</button></fieldset></form>"
    "<form class=page data-page=audio onsubmit='event.preventDefault();saveForm(this)'><input type=hidden name=section value=audio>"
    "<fieldset id=audio><legend>&#38899;&#39057;&#35774;&#32622;</legend>"
    "<label>NRL &#35821;&#38899;&#32534;&#30721;<select name=voice_codec><option value=0>G.711 8kHz (&#20860;&#23481;)</option><option value=1>Opus 16kHz (&#23485;&#24102;)</option></select></label>"
    "<label>ES8311 &#25196;&#22768;&#22120; 0-255<input name=es8311_dac_vol type=number min=0 max=255></label>"
    "<label>ES8311 &#40614;&#20811;&#39118; 0-170<input name=es8311_adc_vol type=number min=0 max=170></label>"
    "<label>ES8311 &#32819;&#26426;&#36755;&#20986;&#39537;&#21160; (REG13 HPSW)<input name=es8311_hp_drive type=checkbox value=1></label>"
    "<label>&#36719;&#20214; MIC &#22686;&#30410; 1-5<input name=mic_gain type=number min=1 max=5></label>"
    "<label>&#40664;&#35748;&#21457;&#36865;&#32593;&#32476;<select name=tx_network><option value=0>NRL</option><option value=1>FMO (Opus)</option></select></label>"
    "<label>NRL / FMO &#21516;&#26102;&#26469;&#35805;<select name=audio_policy><option value=0>&#28151;&#38899;</option><option value=1>&#20808;&#26469;&#20248;&#20808;</option></select></label>"
    "<button class=wide>&#21333;&#29420;&#20445;&#23384;&#38899;&#39057;&#37197;&#32622;</button></fieldset></form>"
    "<form class=page data-page=aprs onsubmit='event.preventDefault();saveForm(this)'><input type=hidden name=section value=aprs>"
    "<fieldset id=aprs><legend>APRS-IS (&#22266;&#23450;&#22352;&#26631;)</legend><label>&#21551;&#29992; APRS<input name=aprs_enabled type=checkbox value=1></label>"
    "<label>&#20351;&#29992;&#22266;&#23450;&#22352;&#26631;<input name=aprs_position_set type=checkbox value=1></label><label>APRS SSID<input name=aprs_ssid type=number min=0 max=15></label>"
    "<label>&#32428;&#24230;<input name=aprs_latitude type=text placeholder='31.8885 / 3153.3100N'></label><label>&#32463;&#24230;<input name=aprs_longitude type=text placeholder='118.8141 / 11848.8460E'></label>"
    "<label>&#20449;&#26631;&#38388;&#38548; (&#31186;)<input name=aprs_interval type=number min=30 max=3600></label><label>APRS-IS &#26381;&#21153;&#22120;<input name=aprs_host></label>"
    "<label>APRS-IS &#31471;&#21475;<input name=aprs_port type=number min=1 max=65535></label><label>&#20449;&#26631;&#22791;&#27880;<input name=aprs_comment maxlength=80></label>"
        "<p class=hint>AFSK 网关（1200 Bd AFSK 语音解码）</p>"
        "<label>RF 接收解码（麦克风）<input name=aprs_rf_rx type=checkbox value=1></label>"
        "<label>RF AFSK 发射（喇叭→电台）<input name=aprs_rf_tx type=checkbox value=1></label>"
        "<label>NRL 接收解码（网络下行）<input name=aprs_nrl_rx type=checkbox value=1></label>"
        "<label>NRL AFSK 发射（语音上行）<input name=aprs_nrl_tx type=checkbox value=1></label>"
        "<p class=hint>转发开关（RF=射频 NRL=网络 IS=APRS-IS）</p>"
        "<label>RF→IS<input name=fwd_rf_is type=checkbox value=1></label>"
        "<label>IS→RF<input name=fwd_is_rf type=checkbox value=1></label>"
        "<label>NRL→IS<input name=fwd_nrl_is type=checkbox value=1></label>"
        "<label>IS→NRL<input name=fwd_is_nrl type=checkbox value=1></label>"
        "<label>RF→NRL<input name=fwd_rf_nrl type=checkbox value=1></label>"
        "<label>NRL→RF<input name=fwd_nrl_rf type=checkbox value=1></label>"
        "<p class=hint>FMO 服务器广播（FMO-V4 STATION）：需 MQTT 已连接、以 super 角色登录自己的服务器、APRS-IS 已验证；"
        "host/port 取自当前选定的 FMO 服务器，坐标使用上方纬度/经度字段。</p>"
        "<p class=hint id=station_state></p>"
        "<label>启用服务器广播<input name=fmo_station_enabled type=checkbox value=1></label>"
        "<label>广播周期<select name=fmo_station_interval><option value=5>5 分钟</option><option value=10>10 分钟</option><option value=60>60 分钟</option></select></label>"
        "<label>国家码（2 位字母）<input name=fmo_country maxlength=2 autocapitalize=characters></label>"
        "<label>台站名称（线上 UTF-8，最大 32 字符）<input name=fmo_station_name maxlength=32></label>"
        "<label>覆盖半径 km<input name=fmo_coverage_km type=number min=0 max=5000></label>"
        "<label>在线人数<input name=fmo_station_online type=number min=0 max=65535></label>"
        "<label>峰值人数<input name=fmo_station_peak type=number min=0 max=65535></label>"
        "<p class=hint id=station_counters>在线/峰值填 0 使用自动统计</p>"
        "<p class=hint>FMO 个人信标（FMO-V4 BEACON，固定 10 分钟周期，整条帧 ≤512 字符）："
        "需 APRS-IS 已验证且证书就绪，不要求连接 FMO 服务器；频率必填（20-500 MHz），坐标同上。</p>"
        "<p class=hint id=beacon_state></p>"
        "<label>启用个人信标<input name=fmo_beacon_enabled type=checkbox value=1></label>"
        "<label>信标频率 MHz<input name=fmo_freq type=number step=.0001 min=20 max=500 placeholder='439.8250'></label>"
        "<label>天线高度 m（0=不报）<input name=fmo_height type=number min=0 max=65535></label>"
        "<label>电台装备 RIG（最大 16 字符）<input name=fmo_rig maxlength=16></label>"
        "<label>天线 ANT（最大 16 字符）<input name=fmo_ant maxlength=16></label>"
        "<label>APRS 个性化消息（APFMO2，信标后跟发，最大 64 字符）<input name=fmo_aprs_msg maxlength=64></label>"
        "<label>服务器登录公告（APFMO1，STATION 广播后跟发，最大 128 字符）<input name=fmo_notice maxlength=128></label>"
        "<label>QSO 消息（仅存储暂不发送，最大 128 字符）<input name=fmo_qso_msg maxlength=128></label>"
    "<p class=hint>&#25903;&#25345; WGS-84 &#21313;&#36827;&#21046;&#24230; dd.dddd &#21644; APRS/NMEA ddmm.mmmmN / dddmm.mmmmE&#65307;&#33258;&#21160;&#36716;&#25442;&#12290;</p>"
    "<button class=wide>&#21333;&#29420;&#20445;&#23384;</button></fieldset></form>"
    "<fieldset id=netradio><legend>网络电台</legend><div id=radio_state class=hint></div><div id=radio_list></div>"
    "<label>名称<input id=rname maxlength=47></label><label>流地址<input id=rurl placeholder='http://...'></label>"
    "<button class=wide type=button onclick=radioAdd()>添加电台</button>"
    "<button class=wide type=button onclick=radioStop()>停止播放</button></fieldset>"
    "<pre id=status>&#21152;&#36733;&#20013;...</pre>"
    "<div id=tone_ov onclick=\"if(event.target===this)this.classList.remove('open')\"><div id=tone_box><h3 id=tone_title></h3><div id=tone_grid></div></div></div>"
    "<script>"
    "const PAGE=location.pathname.split('/').pop();const TITLES={identity:'NRL \u547c\u53f7\u4e0e\u670d\u52a1\u5668',wifi:'Wi-Fi \u914d\u7f51',radio:'\u5c04\u9891\u677f\u8bbe\u7f6e',audio:'\u97f3\u9891\u8bbe\u7f6e',aprs:'APRS-IS \u8bbe\u7f6e',netradio:'\u7f51\u7edc\u7535\u53f0'};"
    "let panel=PAGE==='netradio'?document.getElementById('netradio'):document.querySelector('[data-page='+PAGE+']');if(panel)panel.style.display='block';page_title.textContent=TITLES[PAGE]||'Open FMO';"
    "let toastTimer;function toast(msg,ok){let t=document.getElementById('toast');t.textContent=msg;t.className=ok?'ok':'err';"
    "clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.className='',2500)}"
    "const TONES=[0,67,69.3,71.9,74.4,77,79.7,82.5,85.4,88.5,91.5,94.8,97.4,100,103.5,107.2,110.9,114.8,118.8,123,127.3,131.8,136.5,141.3,146.2,151.4,156.7,159.8,162.2,165.5,167.9,171.3,173.8,177.3,179.9,183.5,186.2,189.9,192.8,196.6,199.5,203.5,206.5,210.7,218.1,225.7,229.1,233.6,241.8,250.3,254.1];"
    "function toneLbl(t){return t==0?'OFF':t+' Hz'}"
    "let toneTarget=null;function tonePick(id){toneTarget=id;let cur=document.getElementById(id).value;"
    "tone_title.textContent=(id==='rx_ctcss'?'\u63a5\u6536':'\u53d1\u5c04')+'\u4e9a\u97f3';"
    "tone_grid.innerHTML='';TONES.forEach(t=>{let b=document.createElement('button');b.type='button';b.className='tn'+(String(t)===cur?' cur':'');"
    "b.textContent=toneLbl(t);b.onclick=()=>{document.getElementById(id).value=t;document.getElementById(id+'_btn').textContent=toneLbl(t);tone_ov.classList.remove('open')};tone_grid.appendChild(b)});tone_ov.classList.add('open')}"
    "async function saveForm(f){let btn=f.querySelector('button[type=submit],button:not([type=button])');btn.disabled=true;btn.textContent='\u4fdd\u5b58\u4e2d...';"
    "try{let r=await fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))});"
    "if(r.ok){toast('\u2713 \u5df2\u4fdd\u5b58',true)}else{let e=await r.text();toast('\u2717 '+e,false)}}"
    "catch(e){toast('\u2717 '+e.message,false)}"
    "btn.disabled=false;btn.textContent=btn.dataset.label||'\u4fdd\u5b58';return false}"
    "document.querySelectorAll('form button:not([type=button])').forEach(b=>b.dataset.label=b.textContent);"
    "function esc(s){let d=document.createElement('div');d.textContent=s;return d.innerHTML}"
    "let nrlData=[];async function loadNrlServers(){let body=document.getElementById('nrl_servers');if(!body)return;try{let r=await fetch('/api/servers',{cache:'no-store'});if(!r.ok)throw Error(await r.text());let d=await r.json();nrlData=d.nrl||[];body.innerHTML='';nrlData.forEach((x,i)=>{let row=body.insertRow();row.className=x.selected?'sel':'';[x.name,x.host+':'+x.port,x.online+'/'+x.total].forEach(v=>{let c=row.insertCell();c.textContent=v});let c=row.insertCell(),b=document.createElement('button');b.type='button';b.className='pick';b.textContent=x.selected?'\u5f53\u524d':'\u9009\u62e9';b.disabled=!!x.selected;b.onclick=()=>selectNrl(i);c.appendChild(b)});if(!nrlData.length)body.innerHTML='<tr><td colspan=4 class=hint>\u6682\u65e0\u670d\u52a1\u5668</td></tr>'}catch(e){body.innerHTML='<tr><td colspan=4>'+esc(e.message)+'</td></tr>'}}"
    "async function selectNrl(i){let x=nrlData[i];if(!x)return;try{let r=await fetch('/api/servers/select',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({kind:'nrl',key:x.key})});if(!r.ok)throw Error(await r.text());document.querySelector('[name=nrl_host]').value=x.host;document.querySelector('[name=nrl_port]').value=x.port;toast('\u2713 \u5df2\u5207\u6362\u5230 '+x.name,true);await loadNrlServers()}catch(e){toast('\u2717 '+e.message,false)}}"
    "function renderSaved(a){saved_wifi.innerHTML='<b>\u5df2\u4fdd\u5b58\u7684\u7f51\u7edc</b>';if(!a.length)saved_wifi.innerHTML+='<p class=hint>\u6682\u65e0</p>';"
    "a.forEach(x=>saved_wifi.innerHTML+='<div class=saved><span>'+(x.index+1)+'. '+esc(x.ssid)+'</span><button type=button onclick=delWifi('+x.index+')>\u5220\u9664</button></div>')}"
    "function loadConfig(){fetch('/api/config').then(r=>r.json()).then(c=>{renderSaved(c.wifi_profiles||[]);"
    "for(const[k,v]of Object.entries(c)){let e=document.querySelector('[name='+k+']');"
    "if(!e||k==='wifi_password')continue;if(e.type==='checkbox')e.checked=!!v;else e.value=v;"
    "let b=document.getElementById(k+'_btn');if(b)b.textContent=toneLbl(v)}"
    "let sg=document.getElementById('station_state');if(sg)sg.textContent='广播状态: '+(c.fmo_station_gate||'-')+'  目标: '+(c.fmo_station_host||'-')+':'+(c.fmo_station_port||'-');"
    "let sc=document.getElementById('station_counters');if(sc)sc.textContent='在线/峰值填 0 使用自动（当前 在线 '+c.fmo_station_online_auto+' / 峰值 '+c.fmo_station_peak_auto+'）';"
    "let bg=document.getElementById('beacon_state');if(bg)bg.textContent='信标状态: '+(c.fmo_beacon_gate||'-')+'  已发: '+(c.fmo_beacon_tx||0)})}"
    "function loadStatus(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(s=>{"
    "status.textContent='\u56fa\u4ef6: '+s.firmware+'  \u677f\u578b: '+s.board+'\\n'"
    "+'WiFi: '+(s.wifi_connected?'\u5df2\u8fde\u63a5 '+s.ip+' ('+s.rssi+'dBm)':'\u672a\u8fde\u63a5')+'\\n'"
    "+'NRL: '+s.nrl_host+':'+s.nrl_port+'  \u547c\u53f7: '+s.callsign+'-'+s.ssid+'\\n'"
    "+'RX: '+s.rx_mhz+' MHz  TX: '+s.tx_mhz+' MHz\\n'"
    "+'\u4e9a\u97f3: RX='+s.rx_ctcss+' TX='+s.tx_ctcss+'  \u9759\u566a: '+s.squelch+'  \u529f\u7387: '+s.tx_power+'\\n'"
    "+'\u7535\u53f0\u97f3\u91cf: RX='+s.rx_volume+' TX='+s.tx_volume+'  \u7f16\u7801: '+s.voice_codec+'\\n'"
    "+'APRS: '+(s.aprs_connected?'\u5df2\u8fde\u63a5':'\u672a\u8fde\u63a5')+' RX:'+s.aprs_rx+' TX:'+s.aprs_tx+'\\n'"
    "+'FMO MQTT: '+(s.fmo_connected?'\u5df2\u8fde\u63a5':'\u672a\u8fde\u63a5')+'  Client ID: '+(s.mqtt_client_id||'---')+'\\n'"
    "+'SQL: '+(s.sql_active?'\u6253\u5f00':'\u5173\u95ed')+'  PTT: '+(s.network_ptt?'\u53d1\u5c04':'\u5f85\u673a')}).catch(()=>{})}"
    "async function scanWifi(){wifi_list.innerHTML='<option>\u6b63\u5728\u626b\u63cf...</option>';"
    "try{let r=await fetch('/scan',{cache:'no-store'});if(!r.ok)throw Error(await r.text());let a=await r.json();"
    "wifi_list.innerHTML='<option value=\"\">\u8bf7\u9009\u62e9\u70ed\u70b9</option>';a.forEach(x=>{let o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' ('+x.rssi+' dBm)';wifi_list.appendChild(o)})}"
    "catch(e){wifi_list.innerHTML='<option>\u626b\u63cf\u5931\u8d25\uff0c\u8bf7\u91cd\u8bd5</option>'}}"
    "async function delWifi(i){if(!confirm('\u5220\u9664\u8fd9\u4e2a Wi-Fi\uff1f'))return;"
    "let r=await fetch('/wifi/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'index='+i});"
    "if(r.ok){toast('\u2713 \u5df2\u5220\u9664',true);loadConfig()}else toast('\u2717 '+await r.text(),false)}"
    "async function radioPost(b){let r=await fetch('/api/radio',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});"
    "if(r.ok){toast('\u2713 \u5b8c\u6210',true);loadRadio();return true}toast('\u2717 '+await r.text(),false);return false}"
    "function radioPlay(i){radioPost('action=play&index='+i)}"
    "function radioDel(i){if(confirm('\u5220\u9664\u8fd9\u4e2a\u7535\u53f0\uff1f'))radioPost('action=del&index='+i)}"
    "function radioStop(){radioPost('action=stop')}"
    "async function radioAdd(){if(!rurl.value){toast('\u8bf7\u586b\u5199\u6d41\u5730\u5740',false);return}"
    "if(await radioPost(new URLSearchParams({action:'add',name:rname.value,url:rurl.value}))){rname.value='';rurl.value=''}}"
    "async function loadRadio(){try{let d=await(await fetch('/api/radio',{cache:'no-store'})).json();"
    "radio_state.textContent='\u72b6\u6001: '+d.state+(d.current>=0?'  \u5f53\u524d: '+(d.current+1)+'/'+d.count:'');"
    "radio_list.innerHTML='';(d.stations||[]).forEach(x=>{radio_list.innerHTML+='<div class=saved><span>'+(x.index+1)+'. '+esc(x.name)+(d.playing&&x.index===d.current?' \u25b6':'')+'</span>'"
    "+'<button type=button onclick=radioPlay('+x.index+')>\u64ad\u653e</button><button type=button onclick=radioDel('+x.index+')>\u5220\u9664</button></div>'})}catch(e){}}"
    "loadConfig();if(PAGE==='identity')loadNrlServers();if(PAGE==='wifi')scanWifi();if(PAGE==='netradio'){loadRadio();setInterval(loadRadio,5000)}"
    "</script></body></html>";

static const char k_servers_html[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>FMO &#26381;&#21153;&#22120;&#19982;&#35777;&#20070;</title><style>body{font:15px sans-serif;max-width:980px;margin:auto;padding:16px;background:#111;color:#eee}"
    "a{color:#f80}section{border:1px solid #555;margin:12px 0;padding:12px}button,select,input{font:inherit;padding:6px;margin:2px}button{background:#f80;border:0;font-weight:bold}"
    "table{width:100%;border-collapse:collapse}th,td{padding:6px;border-bottom:1px solid #333;text-align:left}tr.sel{background:#352000}.muted{color:#999}"
    ".server-tools{display:grid;grid-template-columns:minmax(220px,1fr) auto;gap:8px;align-items:center;margin:10px 0}.server-tools input{width:100%;box-sizing:border-box}@media(max-width:620px){.server-tools{grid-template-columns:1fr}table{font-size:13px}th,td{padding:5px}}</style></head>"
    "<body><p><a href=/>&lt; &#37197;&#32622;</a></p><h1>FMO &#26381;&#21153;&#22120;&#19982;&#35777;&#20070;</h1>"
    "<section><h2>FMO &#36523;&#20221;</h2><p>FMO &#21628;&#21495;&#65306;<strong id=cert_call>&#20174;&#35777;&#20070;&#35835;&#21462;</strong></p>"
    "<p class=muted>FMO &#22522;&#30784;&#21628;&#21495;&#30001; userCert &#30830;&#23450;&#65292;&#19981;&#38656;&#35201;&#25163;&#24037;&#37197;&#32622;&#12290;</p>"
    "<label>FMO SSID <input id=fmo_ssid type=number min=0 max=15></label> "
    "<label><input id=no_local type=checkbox> MQTT 5 No Local</label> "
    "<button onclick=saveFmoSettings()>&#20445;&#23384; FMO &#37197;&#32622;</button></section>"
    "<section><h2>FMO &#26381;&#21153;&#22120; <span class=muted>(&#20027;&#23631;&#21482;&#20999;&#25442;&#25910;&#34255;&#39033;)</span></h2>"
    "<div class=server-tools><input id=fmo_filter type=search placeholder='&#25628;&#32034;&#21517;&#31216;&#12289;&#21628;&#21495;&#12289;&#22320;&#22336;&#12289;&#31471;&#21475;&#25110; UID' oninput=renderFmo()>"
    "<select id=fmo_sort onchange=renderFmo()><option value=default>&#40664;&#35748;&#39034;&#24207;</option><option value=favorite>&#25910;&#34255;&#20248;&#20808;</option><option value=name>&#21517;&#31216;&#25490;&#24207;</option><option value=online>&#22312;&#32447;&#20154;&#25968;</option><option value=total>&#24635;&#20154;&#25968;</option></select></div>"
    "<p id=fmo_count class=muted></p><table><thead><tr><th>&#25910;&#34255;</th><th>&#21517;&#31216;</th><th>&#22320;&#22336;</th><th>&#22312;&#32447;</th><th></th></tr></thead><tbody id=fmo></tbody></table></section>"
    "<section><h2>FMO &#35777;&#20070;</h2><p class=muted>&#31169;&#38053;&#21482;&#20889;&#20837; Flash&#65292;&#19981;&#25552;&#20379;&#35835;&#22238;&#25509;&#21475;&#12290;</p><pre id=certstatus></pre>"
    "<p><strong>&#33258;&#21160;&#30003;&#35831;&#35777;&#20070;</strong></p><p class=muted>&#26412;&#26426; MAC: <span id=act_mac>--</span>&#12290;&#35831;&#20808;&#22312; hamptt.com &#30331;&#35760;&#24182;&#32465;&#23450;&#27492; MAC&#12290;</p>"
    "<label>&#35777;&#20070;&#26381;&#21153;&#22120; <input id=act_host maxlength=128 placeholder=www.hamptt.com></label><button onclick=saveActivateHost()>&#20445;&#23384;&#22320;&#22336;</button><button onclick=runActivate()>&#33258;&#21160;&#30003;&#35831;</button><p id=act_status class=muted></p>"
    "<p>userCert <input id=cu type=file accept=.json,application/json><button onclick=\"uploadCert('user',cu)\">&#19978;&#20256;</button></p>"
    "<p>intermediateCert <input id=ci type=file accept=.json,application/json><button onclick=\"uploadCert('intermediate',ci)\">&#19978;&#20256;</button></p>"
    "<p>deviceKey <input id=ck type=file accept=.json,application/json><button onclick=\"uploadCert('devicekey',ck)\">&#19978;&#20256;</button></p></section>"
    "<section><h2>&#36890;&#32852; / QSO</h2>"
    "<p class=muted>&#29366;&#24577;: <span id=qso_state>-</span></p>"
    "<p><label>&#23545;&#26041;&#21628;&#21495; <input id=qso_peer maxlength=10 placeholder=BG8LLD></label> "
    "<label>&#23545;&#26041; UID <input id=qso_uid type=number min=0 style=width:8em placeholder=2533></label> "
    "<button onclick=qsoCall()>&#21457;&#36215;&#21628;&#21483;</button>"
    "<button onclick=qsoCancel()>&#21462;&#28040; / &#32467;&#26463;</button></p>"
    "<table><thead><tr><th>&#26102;&#38388;</th><th></th><th>&#23545;&#26041;</th><th>&#32467;&#26524;</th><th>&#31069;&#31119;</th></tr></thead>"
    "<tbody id=qso_log></tbody></table></section>"
    "<script>let data;function esc(s){let d=document.createElement('div');d.textContent=s||'';return d.innerHTML}"
    "async function post(url,obj){let r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(obj)});if(!r.ok)alert(await r.text());else load()}"
    "function selectServer(kind,key){post('/api/servers/select',{kind,key})}function fav(key,v){post('/api/servers/favorite',{key,favorite:v?1:0})}"
    "function saveFmoSettings(){post('/save',{section:'fmo_identity',fmo_callsign_ssid:fmo_ssid.value,fmo_mqtt_no_local:no_local.checked?1:0})}"
    "async function loadCert(){let s=await(await fetch('/api/fmo/cert',{cache:'no-store'})).json();cert_call.textContent=s.callsign||(s.ready?'--':'\u7b49\u5f85\u5b8c\u6574\u8bc1\u4e66');certstatus.textContent=(s.ready?'READY ':'INCOMPLETE ')+(s.callsign||'')+(s.uid?' / '+s.uid:'')+'\\nuser='+s.user+' intermediate='+s.intermediate+' deviceKey='+s.device_key+(s.error?'\\n'+s.error:'')}"
    "async function uploadCert(kind,input){if(!input.files.length)return;let r=await fetch('/api/fmo/cert/'+kind,{method:'POST',headers:{'Content-Type':'application/json'},body:input.files[0]});if(!r.ok)alert(await r.text());await loadCert()}"
    "async function loadActivation(){try{let a=await(await fetch('/api/fmo/activate',{cache:'no-store'})).json();act_mac.textContent=a.mac||'--';if(document.activeElement!==act_host)act_host.value=a.host||'';act_status.textContent=a.last||''}catch(e){act_status.textContent='activation status unavailable'}}"
    "async function activate(saveOnly){act_status.textContent=saveOnly?'saving...':'requesting certificate...';let r=await fetch('/api/fmo/activate',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({cert_host:act_host.value,save_only:saveOnly?1:0})});let t=await r.text();act_status.textContent=t;if(!r.ok)return;await loadActivation();if(!saveOnly)await loadCert()}"
    "function saveActivateHost(){activate(true)}function runActivate(){activate(false)}"
    "function renderFmo(){if(!data)return;let q=fmo_filter.value.trim().toLowerCase(),list=(data.fmo||[]).filter(x=>!q||[x.name,x.callsign,x.host,x.port,x.uid].join(' ').toLowerCase().includes(q));let mode=fmo_sort.value;if(mode==='favorite')list.sort((a,b)=>(b.favorite-a.favorite)||(b.selected-a.selected));else if(mode==='name')list.sort((a,b)=>a.name.localeCompare(b.name));else if(mode==='online')list.sort((a,b)=>(b.online-a.online)||(b.total-a.total));else if(mode==='total')list.sort((a,b)=>(b.total-a.total)||(b.online-a.online));fmo_count.textContent='\u663e\u793a '+list.length+' / '+(data.fmo||[]).length+' \u4e2a\u670d\u52a1\u5668';fmo.innerHTML='';"
    "function cell(r,t){let c=r.insertCell();c.textContent=t;return c}function button(c,t,fn){let b=document.createElement('button');b.textContent=t;b.onclick=fn;c.appendChild(b)}"
    "list.forEach(x=>{let r=fmo.insertRow();r.className=x.selected?'sel':'';let c=r.insertCell(),favbox=document.createElement('input');favbox.type='checkbox';favbox.checked=x.favorite;favbox.onchange=()=>fav(x.key,favbox.checked);c.appendChild(favbox);let cs=x.callsign+(x.has_ssid?'-'+x.ssid:'');cell(r,x.name+' / '+cs+(x.uid?' / '+x.uid:''));cell(r,x.host+':'+x.port);cell(r,x.online+'/'+x.total);let bc=r.insertCell();button(bc,x.selected?'\u5f53\u524d':'\u9009\u62e9',()=>selectServer('fmo',x.key));if(x.selected)bc.firstChild.disabled=true});if(!list.length)fmo.innerHTML='<tr><td colspan=5 class=muted>\u6ca1\u6709\u5339\u914d\u7684\u670d\u52a1\u5668</td></tr>'}"
    "async function load(){let a=await Promise.all([fetch('/api/servers',{cache:'no-store'}),fetch('/api/config',{cache:'no-store'})]);data=await a[0].json();let cfg=await a[1].json();fmo_ssid.value=cfg.fmo_callsign_ssid;no_local.checked=!!data.no_local;"
    "renderFmo()}"
    "async function qsoPost(url,obj){let r=await fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(obj)});if(!r.ok)alert(await r.text());await loadQso()}"
    "function qsoCall(){qsoPost('/api/qso/call',{peer:qso_peer.value,uid:qso_uid.value||0})}"
    "function qsoCancel(){qsoPost('/api/qso/cancel',{})}"
    "async function loadQso(){try{let d=await(await fetch('/api/qso',{cache:'no-store'})).json();"
    "qso_state.textContent=d.phase+(d.peer?' / '+d.peer:'')+(d.detail?' -- '+d.detail:'');"
    "qso_log.innerHTML='';(d.log||[]).forEach(x=>{qso_log.innerHTML+='<tr><td>'+new Date(x.ts*1000).toLocaleString()+'</td><td>'+(x.dir==='in'?'&larr;':'&rarr;')+'</td><td>'+esc(x.peer)+(x.uid?' / '+x.uid:'')+'</td><td>'+esc(x.result)+'</td><td>'+esc(x.comment||'')+'</td></tr>'})}catch(e){}}"
    "load();loadCert();loadActivation();loadQso();setInterval(loadQso,3000)</script></body></html>";

static const char k_update_html[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Open FMO OTA</title><style>body{font:16px sans-serif;max-width:760px;margin:auto;padding:16px;background:#111;color:#eee}"
    "section{border:1px solid #f80;padding:14px;margin:14px 0}input,button,select{font:inherit;padding:8px;margin:4px 0}input,select{width:96%}"
    "button{background:#f80;border:0;font-weight:bold;cursor:pointer}progress{width:100%;height:24px}pre{white-space:pre-wrap;color:#ccc}a{color:#f80}</style></head>"
    "<body><p><a href=/>&lt; &#37197;&#32622; / Configuration</a></p>"
    "<h1>&#22266;&#20214;&#21319;&#32423; / Firmware Update</h1>"
    "<p>&#24403;&#21069;&#29256;&#26412; / Current: " FMO_FIRMWARE_VERSION
    " &middot; &#26495;&#21345; / Board: " FMO_BOARD_TYPE "</p>"
    "<section><h2>OTA &#26381;&#21153;&#22120; / NRL OTA Server</h2>"
    "<label>&#26381;&#21153;&#22120;&#22320;&#22336; / Server URL</label><input id=url>"
    "<label>&#35774;&#22791;&#20196;&#29260; (&#21487;&#36873;) / Device Token (optional)</label><input id=token type=password>"
    "<p><button onclick=saveConfig()>&#20445;&#23384;&#26381;&#21153;&#22120; / Save</button> "
    "<button onclick=checkNow()>&#26816;&#26597;&#29256;&#26412; / Check</button></p>"
    "<select id=releases></select><button onclick=installRelease()>&#23433;&#35013;&#36873;&#20013;&#29256;&#26412; / Install Selected</button></section>"
    "<section><h2>&#26412;&#22320;&#22266;&#20214;&#25991;&#20214; / Local Firmware File</h2>"
    "<p>&#19978;&#20256; .bin &#22266;&#20214;&#21040;&#22791;&#29992; OTA &#20998;&#21306;&#24182;&#37325;&#21551;&#12290;/ Upload a .bin firmware image to the inactive OTA partition and reboot.</p>"
    "<input id=file type=file accept=.bin,application/octet-stream>"
    "<button onclick=uploadFile()>&#19978;&#20256;&#24182;&#37325;&#21551; / Upload &amp; Reboot</button></section>"
    "<progress id=progress max=100 value=0></progress><pre id=status>&#21152;&#36733;&#20013;...</pre>"
    "<script>let last={};async function poll(){try{last=await(await fetch('/ota/status',{cache:'no-store'})).json();"
    "if(document.activeElement!==url)url.value=last.server_url||'';progress.value=last.update_percent||0;status.textContent=last.updating?'&#27491;&#22312;&#21319;&#32423; / Updating '+last.update_percent+'%':"
    "last.checking?'&#27491;&#22312;&#26816;&#26597;... / Checking...':last.last_error||('&#26368;&#26032; / Latest: '+(last.latest_version||'&#26080; / none'));"
    "let old=releases.value;releases.innerHTML='';(last.releases||[]).forEach(x=>{let o=document.createElement('option');o.value=x.version;o.textContent=x.version+(x.notes?' - '+x.notes:'');releases.appendChild(o)});if(old)releases.value=old}catch(e){status.textContent=e}setTimeout(poll,1000)}"
    "async function post(path,data){let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(data)});let t=await r.text();if(!r.ok)throw Error(t);return t}"
    "async function saveConfig(){try{await post('/ota/config',{server_url:url.value,device_token:token.value});status.textContent='&#24050;&#20445;&#23384; / Saved'}catch(e){status.textContent=e}}"
    "async function checkNow(){try{await post('/ota/check',{});status.textContent='&#24050;&#35831;&#27714;&#26816;&#26597; / Check requested'}catch(e){status.textContent=e}}"
    "async function installRelease(){if(!releases.value)return;try{await post('/ota/install',{version:releases.value});status.textContent='&#24050;&#35831;&#27714;&#23433;&#35013; / Install requested'}catch(e){status.textContent=e}}"
    "async function uploadFile(){let f=file.files[0];if(!f){status.textContent='&#35831;&#36873;&#25321; .bin &#25991;&#20214; / Select a .bin file';return}if(!confirm('&#23433;&#35013; / Install '+f.name+' &#24182;&#37325;&#21551; / and reboot?'))return;"
    "status.textContent='&#27491;&#22312;&#19978;&#20256; / Uploading '+f.size+' bytes...';let r=await fetch('/update',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});status.textContent=await r.text()}poll()</script></body></html>";

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static void url_decode(char *value)
{
    char *write = value;
    for (char *read = value; *read != '\0'; ++read) {
        if (*read == '+') *write++ = ' ';
        else if (*read == '%' && read[1] && read[2]) {
            int high = hex_digit(read[1]);
            int low = hex_digit(read[2]);
            if (high >= 0 && low >= 0) {
                *write++ = (char)((high << 4) | low);
                read += 2;
            }
        } else *write++ = *read;
    }
    *write = '\0';
}

static bool form_value(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) return false;
    url_decode(out);
    return true;
}

/* Count Unicode characters in a UTF-8 string: every byte that is not a
 * continuation byte starts a new character. */
static size_t utf8_char_count(const char *text)
{
    size_t count = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xc0) != 0x80) ++count;
    }
    return count;
}

static void json_escape(char *out, size_t out_size, const char *input);
static esp_err_t read_form_body(httpd_req_t *request, char **out);

static esp_err_t index_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, k_nav_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_page_get(httpd_req_t *request)
{
    if (strcmp(request->uri, "/config/fmo-identity") == 0) {
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", "/servers");
        return httpd_resp_sendstr(request, "FMO settings moved");
    }
    static const char *const pages[] = {
        "/config/identity",
        "/config/wifi",
        "/config/radio",
        "/config/audio",
        "/config/aprs",
        "/config/netradio",
    };
    bool valid = false;
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        if (strcmp(request->uri, pages[i]) == 0) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "configuration page not found");
    }
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if (strcmp(request->uri, "/config/wifi") == 0) {
        ESP_LOGI(TAG, "serve provisioning page");
        return httpd_resp_send(request, k_wifi_html, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send(request, k_index_html_v2,
                           HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_get(httpd_req_t *request)
{
    fmo_config_t config;
    ESP_RETURN_ON_ERROR(config_store_load(&config), TAG, "load Wi-Fi config");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "{\"profiles\":[", 13),
                        TAG, "send Wi-Fi JSON");
    const size_t count = config_store_wifi_count(&config);
    for (size_t i = 0; i < count; ++i) {
        char escaped[96];
        char item[144];
        json_escape(escaped, sizeof(escaped), config.wifi_profiles[i].ssid);
        int length = snprintf(item, sizeof(item),
                              "%s{\"index\":%u,\"ssid\":\"%s\"}",
                              i == 0 ? "" : ",", (unsigned)i, escaped);
        if (length < 0 || length >= (int)sizeof(item)) {
            return ESP_FAIL;
        }
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, item, length), TAG,
                            "send Wi-Fi profile");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "]}", 2), TAG,
                        "finish Wi-Fi JSON");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t update_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, k_update_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get(httpd_req_t *request)
{
    fmo_config_t config;
    config_store_load(&config);
    char callsign[48], fmo_callsign[48], host[128], primary_ssid[96];
    char aprs_host[130], aprs_comment[170];
    char station_name[100], station_gate[130], station_host[130];
    char station_country[8];
    char beacon_rig[100], beacon_ant[100], beacon_msg[390];
    char beacon_notice[772], beacon_qso[772], beacon_gate[130];
    json_escape(callsign, sizeof(callsign), config.callsign);
    json_escape(fmo_callsign, sizeof(fmo_callsign), config.fmo_callsign);
    json_escape(host, sizeof(host), config.nrl_host);
    json_escape(primary_ssid, sizeof(primary_ssid), config.wifi_ssid);
    json_escape(aprs_host, sizeof(aprs_host), config.aprs_server_host);
    json_escape(aprs_comment, sizeof(aprs_comment), config.aprs_comment);
    fmo_station_beacon_status_t station = {0};
    fmo_station_beacon_get_status(&station);
    json_escape(station_name, sizeof(station_name), config.fmo_station_name);
    json_escape(station_gate, sizeof(station_gate), station.gate);
    json_escape(station_host, sizeof(station_host), station.host);
    json_escape(station_country, sizeof(station_country), config.fmo_country);
    json_escape(beacon_rig, sizeof(beacon_rig), config.fmo_rig);
    json_escape(beacon_ant, sizeof(beacon_ant), config.fmo_ant);
    json_escape(beacon_msg, sizeof(beacon_msg), config.fmo_aprs_msg);
    json_escape(beacon_notice, sizeof(beacon_notice), config.fmo_notice);
    json_escape(beacon_qso, sizeof(beacon_qso), config.fmo_qso_msg);
    json_escape(beacon_gate, sizeof(beacon_gate), station.beacon_gate);
    const size_t json_capacity = 12288;
    char *json = malloc(json_capacity);
    if (json == NULL) return httpd_resp_send_500(request);
    size_t used = snprintf(json, json_capacity,
             "{\"callsign\":\"%s\",\"callsign_ssid\":%u,"
             "\"fmo_callsign\":\"%s\",\"fmo_callsign_ssid\":%u,"
             "\"nrl_host\":\"%s\",\"nrl_port\":%u,"
                          "\"wifi_ssid\":\"%s\",\"rx_mhz\":%.4f,"
             "\"tx_mhz\":%.4f,\"rx_ctcss\":%.1f,\"tx_ctcss\":%.1f,"
             "\"squelch\":%u,\"tx_power\":%u,\"rf_enabled\":%s,"
             "\"voice_codec\":%u,\"rx_volume\":%u,\"tx_volume\":%u,"
                          "\"es8311_dac_vol\":%u,\"es8311_adc_vol\":%u,"
                          "\"es8311_hp_drive\":%s,\"mic_gain\":%u,"
                          "\"freq_tune\":%d,\"tx_network\":%u,"
                          "\"audio_policy\":%u,\"fmo_mqtt_no_local\":%s,"
             "\"aprs_enabled\":%s,\"aprs_position_set\":%s,\"aprs_ssid\":%u,"
             "\"aprs_latitude\":%.6f,\"aprs_longitude\":%.6f,\"aprs_interval\":%u,"
             "\"aprs_host\":\"%s\",\"aprs_port\":%u,\"aprs_comment\":\"%s\","
             "\"aprs_rf_rx\":%s,\"aprs_rf_tx\":%s,\"aprs_nrl_rx\":%s,\"aprs_nrl_tx\":%s,"
             "\"fwd_rf_is\":%s,\"fwd_is_rf\":%s,\"fwd_nrl_is\":%s,\"fwd_is_nrl\":%s,"
             "\"fwd_rf_nrl\":%s,\"fwd_nrl_rf\":%s,"
             "\"fmo_station_enabled\":%s,\"fmo_station_interval\":%u,"
             "\"fmo_country\":\"%s\",\"fmo_station_name\":\"%s\","
             "\"fmo_coverage_km\":%u,\"fmo_station_online\":%u,"
             "\"fmo_station_peak\":%u,\"fmo_station_online_auto\":%u,"
             "\"fmo_station_peak_auto\":%u,\"fmo_station_online_effective\":%u,"
             "\"fmo_station_peak_effective\":%u,\"fmo_station_gate\":\"%s\","
             "\"fmo_station_host\":\"%s\",\"fmo_station_port\":%u,"
             "\"fmo_beacon_enabled\":%s,\"fmo_freq\":%.4f,\"fmo_height\":%u,"
             "\"fmo_rig\":\"%s\",\"fmo_ant\":\"%s\",\"fmo_aprs_msg\":\"%s\","
             "\"fmo_notice\":\"%s\",\"fmo_qso_msg\":\"%s\","
             "\"fmo_beacon_gate\":\"%s\",\"fmo_beacon_tx\":%u,"
             "\"wifi_profiles\":[",
             callsign, config.callsign_ssid, fmo_callsign,
             config.fmo_callsign_ssid, host, config.nrl_port,
             primary_ssid, config.radio_rx_mhz,
             config.radio_tx_mhz, config.rx_ctcss_hz, config.tx_ctcss_hz,
             config.squelch, config.tx_power,
             config.rf_enabled ? "true" : "false",
             (unsigned)config.voice_codec,
             (unsigned)config.rx_volume, (unsigned)config.tx_volume,
             (unsigned)config.es8311_dac_vol, (unsigned)config.es8311_adc_vol,
             config.es8311_hp_drive ? "true" : "false",
                          (unsigned)config.mic_gain,
             (int)config.freq_tune_hz,
             (unsigned)config.tx_network, (unsigned)config.audio_policy,
             config.fmo_mqtt_no_local ? "true" : "false",
             config.aprs_enabled ? "true" : "false",
             config.aprs_position_set ? "true" : "false",
             (unsigned)config.aprs_ssid,
             (double)config.aprs_latitude_e6 / 1000000.0,
             (double)config.aprs_longitude_e6 / 1000000.0,
             (unsigned)config.aprs_beacon_interval_s, aprs_host,
             (unsigned)config.aprs_server_port, aprs_comment,
             config.aprs_rf_rx ? "true" : "false",
             config.aprs_rf_tx ? "true" : "false",
             config.aprs_nrl_rx ? "true" : "false",
             config.aprs_nrl_tx ? "true" : "false",
             (config.aprs_fwd & FMO_APRS_FWD_RF_TO_IS) ? "true" : "false",
             (config.aprs_fwd & FMO_APRS_FWD_IS_TO_RF) ? "true" : "false",
             (config.aprs_fwd & FMO_APRS_FWD_NRL_TO_IS) ? "true" : "false",
             (config.aprs_fwd & FMO_APRS_FWD_IS_TO_NRL) ? "true" : "false",
             (config.aprs_fwd & FMO_APRS_FWD_RF_TO_NRL) ? "true" : "false",
             (config.aprs_fwd & FMO_APRS_FWD_NRL_TO_RF) ? "true" : "false",
             config.fmo_station_beacon_enabled ? "true" : "false",
             (unsigned)config.fmo_station_beacon_interval_min,
             station_country, station_name,
             (unsigned)config.fmo_coverage_km,
             (unsigned)config.fmo_station_online,
             (unsigned)config.fmo_station_peak,
             (unsigned)station.auto_online, (unsigned)station.auto_peak,
             config.fmo_station_online > 0
                 ? (unsigned)config.fmo_station_online
                 : (unsigned)station.auto_online,
             config.fmo_station_peak > 0
                 ? (unsigned)config.fmo_station_peak
                 : (unsigned)station.auto_peak,
             station_gate,
             station_host, (unsigned)station.port,
             config.fmo_beacon_enabled ? "true" : "false",
             (double)config.fmo_freq_x10000 / 10000.0,
             (unsigned)config.fmo_height_m,
             beacon_rig, beacon_ant, beacon_msg,
             beacon_notice, beacon_qso,
             beacon_gate, (unsigned)station.beacon_tx_count);
    size_t wifi_count = config_store_wifi_count(&config);
    for (size_t i = 0; i < wifi_count && used < json_capacity - 128; ++i) {
        char ssid[96];
        json_escape(ssid, sizeof(ssid), config.wifi_profiles[i].ssid);
        used += snprintf(json + used, json_capacity - used,
                         "%s{\"index\":%u,\"ssid\":\"%s\"}",
                         i == 0 ? "" : ",", (unsigned)i, ssid);
    }
    snprintf(json + used, json_capacity - used, "]}");
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_sendstr(request, json);
    free(json);
    return result;
}

static bool valid_frequency(float mhz)
{
    return (mhz >= 136.0f && mhz <= 174.0f) ||
           (mhz >= 400.0f && mhz <= 480.0f);
}

/* Shared validation for the personal-beacon text fields: UTF-8 character
 * count cap, no ',' (APRS comment field separator) or control chars.
 * Wire text is UTF-8, so no GBK mapping check is needed. */
static bool valid_beacon_text(const char *text, size_t max_chars)
{
    if (utf8_char_count(text) > max_chars) return false;
    for (const char *p = text; *p; ++p) {
        if (*p == ',' || (unsigned char)*p < 0x20) return false;
    }
    return true;
}

/* Percent-encoded form values expand to 9 bytes per UTF-8 character
 * (%XX per byte, 3 bytes per CJK char).  These buffers are static so the
 * httpd task stack stays small; the httpd task runs handlers serialized. */
static char s_rig_value[16 * 9 + 1];
static char s_ant_value[16 * 9 + 1];
static char s_msg_value[64 * 9 + 1];
static char s_notice_value[128 * 9 + 1];
static char s_qso_value[128 * 9 + 1];

static esp_err_t save_post(httpd_req_t *request)
{
    /* The personal-beacon texts (notice/qso up to 128 CJK chars each) push
     * the percent-encoded APRS form well past the old 2 KB cap. */
    if (request->content_len <= 0 || request->content_len > 6144) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char *body = calloc(1, request->content_len + 1);
    if (body == NULL) return httpd_resp_send_500(request);
    int received = httpd_req_recv(request, body, request->content_len);
    if (received <= 0) {
        free(body);
        return ESP_FAIL;
    }

    fmo_config_t config;
    config_store_load(&config);
    char section[16] = {0};
    char value[96];
    /* The station name arrives percent-encoded: 32 CJK chars are 96 UTF-8
     * bytes, i.e. 288 bytes in x-www-form-urlencoded form. */
    char name_value[32 * 9 + 1];
    const char *error = NULL;
    const char *location = "/";
    bool save_identity = false;
    bool save_fmo_identity = false;
    bool save_wifi = false;
    bool save_radio = false;
    bool save_audio = false;
    bool save_aprs = false;

    if (!form_value(body, "section", section, sizeof(section))) {
        error = "missing section";
    } else if (strcmp(section, "identity") == 0) {
        save_identity = true;
        location = "/config/identity";
    } else if (strcmp(section, "fmo_identity") == 0) {
        save_fmo_identity = true;
        location = "/servers";
    } else if (strcmp(section, "wifi") == 0) {
        save_wifi = true;
        location = "/config/wifi";
    } else if (strcmp(section, "radio") == 0) {
        save_radio = true;
        location = "/config/radio";
    } else if (strcmp(section, "audio") == 0) {
        save_audio = true;
        location = "/config/audio";
    } else if (strcmp(section, "aprs") == 0) {
        save_aprs = true;
        location = "/config/aprs";
    } else {
        error = "unknown section";
    }

    if (save_identity) {
        unsigned long number;
        if (!form_value(body, "callsign", value, sizeof(value)) || !value[0]) {
            error = "callsign required";
        } else {
            strlcpy(config.callsign, value, sizeof(config.callsign));
        }
        if (!error && form_value(body, "callsign_ssid", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 255) error = "SSID range is 0-255";
            else config.callsign_ssid = (uint8_t)number;
        } else if (!error) {
            error = "SSID required";
        }
        if (!error && (!form_value(body, "nrl_host", value, sizeof(value)) || !value[0])) {
            error = "NRL host required";
        } else if (!error) {
            strlcpy(config.nrl_host, value, sizeof(config.nrl_host));
        }
        if (!error && form_value(body, "nrl_port", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number == 0 || number > 65535) error = "NRL port range error";
            else config.nrl_port = (uint16_t)number;
        } else if (!error) {
            error = "NRL port required";
        }
    } else if (save_fmo_identity) {
        unsigned long number;
        if (form_value(body, "fmo_callsign_ssid", value,
                                 sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 15) error = "FMO SSID range is 0-15";
            else config.fmo_callsign_ssid = (uint8_t)number;
        } else {
            error = "FMO SSID required";
        }
        config.fmo_mqtt_no_local =
            strstr(body, "fmo_mqtt_no_local=1") != NULL;
    } else if (save_wifi) {
        char ssid[33] = {0};
        char password[65] = {0};
        if (!form_value(body, "wifi_ssid", ssid, sizeof(ssid)) || !ssid[0]) {
            error = "Wi-Fi SSID required";
        } else {
            (void)form_value(body, "wifi_password", password, sizeof(password));
            if (!config_store_wifi_add(&config, ssid, password, true)) {
                error = "Wi-Fi list full (maximum 5)";
            }
        }
    } else if (save_radio) {
        float frequency;
        unsigned long number;
        if (form_value(body, "rx_mhz", value, sizeof(value)) &&
            valid_frequency(frequency = strtof(value, NULL))) {
            config.radio_rx_mhz = frequency;
        } else {
            error = "RX frequency range error";
        }
        if (!error && form_value(body, "tx_mhz", value, sizeof(value)) &&
            valid_frequency(frequency = strtof(value, NULL))) {
            config.radio_tx_mhz = frequency;
        } else if (!error) {
            error = "TX frequency range error";
        }
        if (!error && form_value(body, "rx_ctcss", value, sizeof(value)))
            config.rx_ctcss_hz = strtof(value, NULL);
        if (!error && form_value(body, "tx_ctcss", value, sizeof(value)))
            config.tx_ctcss_hz = strtof(value, NULL);
        if (!error && form_value(body, "squelch", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 10) error = "squelch range is 0-10";
            else config.squelch = (uint8_t)number;
        }
        if (!error && form_value(body, "tx_power", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 2) error = "TX power range is 0-2";
            else config.tx_power = (uint8_t)number;
        }
        config.rf_enabled = strstr(body, "rf_enabled=1") != NULL;
        if (!error && form_value(body, "rx_volume", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 10) error = "rx_volume range is 0-10";
            else config.rx_volume = (uint8_t)number;
        }
        if (!error && form_value(body, "tx_volume", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 10) error = "tx_volume range is 0-10";
            else config.tx_volume = (uint8_t)number;
        }
        if (!error && form_value(body, "freq_tune", value, sizeof(value))) {
            long ft = strtol(value, NULL, 10);
            if (ft < -5000 || ft > 5000) error = "freq_tune range is -5000~5000";
            else config.freq_tune_hz = (int16_t)ft;
        }
    } else if (save_audio) {
        unsigned long number;
        if (form_value(body, "voice_codec", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 1) error = "voice_codec must be 0 or 1";
            else config.voice_codec = (uint8_t)number;
        } else {
            error = "voice_codec required";
        }
        if (!error && form_value(body, "es8311_dac_vol", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > FMO_ES8311_DAC_VOL_MAX) error = "es8311_dac_vol range is 0-255";
            else config.es8311_dac_vol = (uint8_t)number;
        }
        if (!error && form_value(body, "es8311_adc_vol", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > FMO_ES8311_ADC_VOL_MAX) error = "es8311_adc_vol range is 0-170";
            else config.es8311_adc_vol = (uint8_t)number;
        }
        config.es8311_hp_drive =
            strstr(body, "es8311_hp_drive=1") != NULL;
        if (!error && form_value(body, "mic_gain", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number < FMO_MIC_GAIN_MIN || number > FMO_MIC_GAIN_MAX) error = "mic_gain range is 1-5";
            else config.mic_gain = (uint8_t)number;
        }
        if (!error && form_value(body, "tx_network", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 1) error = "tx_network must be 0 or 1";
            else config.tx_network = (uint8_t)number;
        }
        if (!error && form_value(body, "audio_policy", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 1) error = "audio_policy must be 0 or 1";
            else config.audio_policy = (uint8_t)number;
        }
    } else if (save_aprs) {
        unsigned long number;
        config.aprs_enabled = strstr(body, "aprs_enabled=1") != NULL;
        config.aprs_position_set = strstr(body, "aprs_position_set=1") != NULL;
        config.aprs_rf_rx = strstr(body, "aprs_rf_rx=1") != NULL;
        config.aprs_rf_tx = strstr(body, "aprs_rf_tx=1") != NULL;
        config.aprs_nrl_rx = strstr(body, "aprs_nrl_rx=1") != NULL;
        config.aprs_nrl_tx = strstr(body, "aprs_nrl_tx=1") != NULL;
        uint8_t fwd = 0;
        if (strstr(body, "fwd_rf_is=1") != NULL) fwd |= FMO_APRS_FWD_RF_TO_IS;
        if (strstr(body, "fwd_is_rf=1") != NULL) fwd |= FMO_APRS_FWD_IS_TO_RF;
        if (strstr(body, "fwd_nrl_is=1") != NULL) fwd |= FMO_APRS_FWD_NRL_TO_IS;
        if (strstr(body, "fwd_is_nrl=1") != NULL) fwd |= FMO_APRS_FWD_IS_TO_NRL;
        if (strstr(body, "fwd_rf_nrl=1") != NULL) fwd |= FMO_APRS_FWD_RF_TO_NRL;
        if (strstr(body, "fwd_nrl_rf=1") != NULL) fwd |= FMO_APRS_FWD_NRL_TO_RF;
        config.aprs_fwd = fwd;
        if (form_value(body, "aprs_ssid", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 15) error = "APRS SSID range is 0-15";
            else config.aprs_ssid = (uint8_t)number;
        }
        if (!error && form_value(body, "aprs_latitude", value, sizeof(value)) &&
            !aprs_service_parse_coordinate(value, true, &config.aprs_latitude_e6)) {
            error = "invalid WGS-84 latitude";
        }
        if (!error && form_value(body, "aprs_longitude", value, sizeof(value)) &&
            !aprs_service_parse_coordinate(value, false, &config.aprs_longitude_e6)) {
            error = "invalid WGS-84 longitude";
        }
        if (!error && form_value(body, "aprs_interval", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number < 30 || number > 3600) error = "APRS interval range is 30-3600";
            else config.aprs_beacon_interval_s = (uint16_t)number;
        }
        if (!error && (!form_value(body, "aprs_host", value, sizeof(value)) || !value[0])) {
            error = "APRS host required";
        } else if (!error) {
            strlcpy(config.aprs_server_host, value, sizeof(config.aprs_server_host));
        }
        if (!error && form_value(body, "aprs_port", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number == 0 || number > 65535) error = "APRS port range error";
            else config.aprs_server_port = (uint16_t)number;
        }
        if (!error && form_value(body, "aprs_comment", value, sizeof(value)))
            strlcpy(config.aprs_comment, value, sizeof(config.aprs_comment));
        /* FMO-V4 STATION broadcast settings.  Runtime gates still apply on
         * every send; saving here only stores the operator's choices. */
        config.fmo_station_beacon_enabled =
            strstr(body, "fmo_station_enabled=1") != NULL;
        if (!error && form_value(body, "fmo_station_interval", value,
                                 sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number != 5 && number != 10 && number != 60) {
                error = "FMO 广播周期只能是 5/10/60 分钟";
            } else {
                config.fmo_station_beacon_interval_min = (uint8_t)number;
            }
        }
        if (!error && form_value(body, "fmo_country", value, sizeof(value))) {
            if (value[0] == '\0') {
                config.fmo_country[0] = '\0';
            } else if (strlen(value) != 2 ||
                       !isalpha((unsigned char)value[0]) ||
                       !isalpha((unsigned char)value[1])) {
                error = "国家码需为 2 位字母";
            } else {
                config.fmo_country[0] = (char)toupper((unsigned char)value[0]);
                config.fmo_country[1] = (char)toupper((unsigned char)value[1]);
                config.fmo_country[2] = '\0';
            }
        }
        if (!error && form_value(body, "fmo_station_name", name_value,
                                 sizeof(name_value))) {
            /* Original FMO allows 32 characters (counted as Unicode
             * characters, not bytes). */
            bool name_ok = utf8_char_count(name_value) <= 32;
            for (const char *p = name_value; name_ok && *p; ++p) {
                /* ',' splits APRS comment fields; control chars break the
                 * TNC2 line.  Wire text is UTF-8, anything else goes. */
                if (*p == ',' || (unsigned char)*p < 0x20) {
                    name_ok = false;
                    break;
                }
            }
            if (!name_ok) {
                error = "台站名最大 32 字符，不能含逗号/控制字符";
            } else {
                strlcpy(config.fmo_station_name, name_value,
                        sizeof(config.fmo_station_name));
            }
        }
        if (!error && form_value(body, "fmo_coverage_km", value,
                                 sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 5000) error = "覆盖半径范围 0-5000 km";
            else config.fmo_coverage_km = (uint16_t)number;
        }
        if (!error && form_value(body, "fmo_station_online", value,
                                 sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 65535) error = "在线人数范围错误";
            else config.fmo_station_online = (uint16_t)number;
        }
        if (!error && form_value(body, "fmo_station_peak", value,
                                 sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 65535) error = "峰值人数范围错误";
            else config.fmo_station_peak = (uint16_t)number;
        }
        /* FMO personal BEACON settings.  Runtime gates still apply on every
         * send; saving here only stores the operator's choices. */
        config.fmo_beacon_enabled =
            strstr(body, "fmo_beacon_enabled=1") != NULL;
        if (!error && form_value(body, "fmo_freq", value, sizeof(value))) {
            if (value[0] == '\0') {
                config.fmo_freq_x10000 = 0;
            } else {
                char *end = NULL;
                double mhz = strtod(value, &end);
                if (end == value || *end != '\0' ||
                    mhz < 20.0 || mhz > 500.0) {
                    error = "信标频率范围 20-500 MHz";
                } else {
                    config.fmo_freq_x10000 =
                        (uint32_t)(mhz * 10000.0 + 0.5);
                }
            }
        }
        if (!error && form_value(body, "fmo_height", value, sizeof(value))) {
            number = strtoul(value, NULL, 10);
            if (number > 65535) error = "天线高度范围错误";
            else config.fmo_height_m = (uint16_t)number;
        }
        if (!error && form_value(body, "fmo_rig", s_rig_value,
                                 sizeof(s_rig_value))) {
            if (!valid_beacon_text(s_rig_value, 16)) {
                error = "RIG 最大 16 字符，不能含逗号/控制字符";
            } else {
                strlcpy(config.fmo_rig, s_rig_value, sizeof(config.fmo_rig));
            }
        }
        if (!error && form_value(body, "fmo_ant", s_ant_value,
                                 sizeof(s_ant_value))) {
            if (!valid_beacon_text(s_ant_value, 16)) {
                error = "ANT 最大 16 字符，不能含逗号/控制字符";
            } else {
                strlcpy(config.fmo_ant, s_ant_value, sizeof(config.fmo_ant));
            }
        }
        if (!error && form_value(body, "fmo_aprs_msg", s_msg_value,
                                 sizeof(s_msg_value))) {
            if (!valid_beacon_text(s_msg_value, 64)) {
                error = "APRS 消息最大 64 字符，不能含逗号/控制字符";
            } else {
                strlcpy(config.fmo_aprs_msg, s_msg_value,
                        sizeof(config.fmo_aprs_msg));
            }
        }
        if (!error && form_value(body, "fmo_notice", s_notice_value,
                                 sizeof(s_notice_value))) {
            if (!valid_beacon_text(s_notice_value, 128)) {
                error = "公告最大 128 字符，不能含逗号/控制字符";
            } else {
                strlcpy(config.fmo_notice, s_notice_value,
                        sizeof(config.fmo_notice));
            }
        }
        if (!error && form_value(body, "fmo_qso_msg", s_qso_value,
                                 sizeof(s_qso_value))) {
            if (!valid_beacon_text(s_qso_value, 128)) {
                error = "QSO 消息最大 128 字符，不能含逗号/控制字符";
            } else {
                strlcpy(config.fmo_qso_msg, s_qso_value,
                        sizeof(config.fmo_qso_msg));
            }
        }
    }
    free(body);

    if (error != NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, error);
    }
    if (config_store_save(&config) != ESP_OK) return httpd_resp_send_500(request);

    if (save_identity) {
        nrl_link_update_config(&config);
        aprs_service_update_config(&config);
    } else if (save_fmo_identity) {
        fmo_discovery_update_config(&config);
        fmo_link_update_config(&config);
        fmo_qso_update_config(&config);
        aprs_service_update_config(&config);
        fmo_station_beacon_update_config(&config);
    } else if (save_wifi) {
        (void)network_manager_update_profiles(&config, true);
    } else if (save_radio) {
        (void)radio_at_set_frequency(false, config.radio_rx_mhz);
        (void)radio_at_set_frequency(true, config.radio_tx_mhz);
        nrl_audio_codec_configure_ctcss(config.rx_ctcss_hz, config.tx_ctcss_hz);
        (void)radio_at_set_squelch(config.squelch);
        (void)radio_at_set_tx_power(config.tx_power);
        (void)radio_at_set_rf_enabled(config.rf_enabled);
        (void)radio_at_set_volume(false, config.rx_volume);
        (void)radio_at_set_volume(true, config.tx_volume);
        (void)radio_at_set_freq_tune(config.freq_tune_hz);
    } else if (save_audio) {
        audio_passthrough_set_voice_codec(config.voice_codec);
        (void)es8311_codec_set_dac_volume(config.es8311_dac_vol);
        (void)es8311_codec_set_adc_volume(config.es8311_adc_vol);
        (void)es8311_codec_set_headphone_drive(config.es8311_hp_drive);
        audio_passthrough_set_mic_gain(config.mic_gain);
        audio_passthrough_set_audio_policy(config.audio_policy);
        audio_passthrough_set_tx_network(config.tx_network);
    } else if (save_aprs) {
        aprs_service_update_config(&config);
        fmo_station_beacon_update_config(&config);
        fmo_qso_update_config(&config);
    }

    httpd_resp_set_status(request, "303 See Other");
    httpd_resp_set_hdr(request, "Location", location);
    return httpd_resp_sendstr(request, "Saved");
}

static esp_err_t status_get(httpd_req_t *request)
{
    fmo_config_t config;
    config_store_load(&config);
    network_status_t network = {0};
    network_manager_get_status(&network);
    aprs_status_t aprs = {0};
    aprs_service_get_status(&aprs);
    fmo_link_status_t fmo = {0};
    fmo_link_get_status(&fmo);
    uint8_t vu_mic = 0, vu_spk = 0;
    status_io_get_vu(&vu_mic, &vu_spk);
    char json[1024];
    snprintf(json, sizeof(json),
        "{\"firmware\":\"%s\",\"board\":\"%s\","
        "\"wifi_connected\":%s,\"ip\":\"%s\",\"rssi\":%d,"
        "\"nrl_host\":\"%s\",\"nrl_port\":%u,"
        "\"rx_mhz\":%.4f,\"tx_mhz\":%.4f,"
        "\"rx_ctcss\":%.1f,\"tx_ctcss\":%.1f,"
        "\"squelch\":%u,\"tx_power\":%u,\"rf_enabled\":%s,"
        "\"rx_volume\":%u,\"tx_volume\":%u,"
        "\"voice_codec\":\"%s\","
        "\"aprs_connected\":%s,\"aprs_rx\":%lu,\"aprs_tx\":%lu,"
        "\"fmo_connected\":%s,\"mqtt_client_id\":\"%s\","
        "\"callsign\":\"%s\",\"ssid\":%u,"
        "\"sql_active\":%s,\"network_ptt\":%s,"
        "\"vu_mic\":%u,\"vu_spk\":%u}",
        FMO_FIRMWARE_VERSION, FMO_BOARD_TYPE,
        network.station_connected ? "true" : "false",
        network.ip_address, (int)network.wifi_rssi_dbm,
        config.nrl_host, (unsigned)config.nrl_port,
        (double)config.radio_rx_mhz, (double)config.radio_tx_mhz,
        (double)config.rx_ctcss_hz, (double)config.tx_ctcss_hz,
        (unsigned)config.squelch, (unsigned)config.tx_power,
        config.rf_enabled ? "true" : "false",
        (unsigned)config.rx_volume, (unsigned)config.tx_volume,
        config.voice_codec == 1 ? "OPUS" : "G711",
        aprs.connected ? "true" : "false",
        (unsigned long)aprs.rx_count, (unsigned long)aprs.tx_count,
        fmo.connected ? "true" : "false", fmo.client_id,
        config.callsign, (unsigned)config.callsign_ssid,
        status_io_is_sql_active() ? "true" : "false",
        status_io_is_network_ptt() ? "true" : "false",
        (unsigned)vu_mic, (unsigned)vu_spk);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, json);
}

static void json_escape(char *out, size_t out_size, const char *input)
{
    size_t used = 0;
    if (out_size == 0) return;
    for (const char *p = input != NULL ? input : ""; *p && used + 2 < out_size; ++p) {
        if (*p == '"' || *p == '\\') out[used++] = '\\';
        if ((unsigned char)*p >= 0x20) out[used++] = *p;
    }
    out[used] = '\0';
}

static esp_err_t servers_page_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, k_servers_html, sizeof(k_servers_html) - 1);
}

static esp_err_t servers_get(httpd_req_t *request)
{
    fmo_config_t config;
    config_store_load(&config);
    httpd_resp_set_type(request, "application/json");
    char chunk[768];
    snprintf(chunk, sizeof(chunk),
             "{\"tx_network\":%u,\"audio_policy\":%u,\"no_local\":%s,\"nrl\":[",
             (unsigned)config.tx_network, (unsigned)config.audio_policy,
             config.fmo_mqtt_no_local ? "true" : "false");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, chunk,
                                               HTTPD_RESP_USE_STRLEN), TAG,
                        "server JSON header");
    size_t count = server_directory_count();
    for (size_t i = 0; i < count; ++i) {
        const nrl_server_t *server = server_directory_get(i);
        if (server == NULL) continue;
        char name[200], host[140], key[180];
        char raw_key[96];
        snprintf(raw_key, sizeof(raw_key), "%s:%u", server->host,
                 (unsigned)server->port);
        json_escape(name, sizeof(name), server->name);
        json_escape(host, sizeof(host), server->host);
        json_escape(key, sizeof(key), raw_key);
        bool selected = strcmp(config.nrl_host, server->host) == 0 &&
                        config.nrl_port == server->port;
        snprintf(chunk, sizeof(chunk),
                 "%s{\"key\":\"%s\",\"name\":\"%s\",\"host\":\"%s\","
                 "\"port\":%u,\"online\":%lu,\"total\":%lu,\"selected\":%s}",
                 i == 0 ? "" : ",", key, name, host, (unsigned)server->port,
                 (unsigned long)server->online, (unsigned long)server->total,
                 selected ? "true" : "false");
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, chunk,
                                                   HTTPD_RESP_USE_STRLEN), TAG,
                            "NRL server JSON");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "],\"fmo\":[",
                                               HTTPD_RESP_USE_STRLEN), TAG,
                        "FMO JSON header");
    count = fmo_server_directory_count();
    for (size_t i = 0; i < count; ++i) {
        const fmo_server_t *server = fmo_server_directory_get(i);
        if (server == NULL) continue;
        char key[180], name[200], host[140], callsign[48];
        json_escape(key, sizeof(key), server->key);
        json_escape(name, sizeof(name), server->name);
        json_escape(host, sizeof(host), server->host);
        json_escape(callsign, sizeof(callsign), server->callsign);
        snprintf(chunk, sizeof(chunk),
                 "%s{\"key\":\"%s\",\"name\":\"%s\",\"host\":\"%s\","
                 "\"callsign\":\"%s\",\"ssid\":%u,\"has_ssid\":%s,"
                 "\"port\":%u,\"uid\":%lu,"
                 "\"online\":%lu,\"total\":%lu,\"favorite\":%s,"
                 "\"selected\":%s}",
                 i == 0 ? "" : ",", key, name, host, callsign,
                 (unsigned)server->ssid,
                 server->has_ssid ? "true" : "false",
                 (unsigned)server->port, (unsigned long)server->uid,
                 (unsigned long)server->online, (unsigned long)server->total,
                 server->favorite ? "true" : "false",
                 strcmp(config.fmo_server_key, server->key) == 0 ? "true" : "false");
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, chunk,
                                                   HTTPD_RESP_USE_STRLEN), TAG,
                            "FMO server JSON");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "]}", 2), TAG,
                        "server JSON tail");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static char *receive_small_form(httpd_req_t *request)
{
    if (request->content_len <= 0 || request->content_len > 1024) return NULL;
    char *body = calloc(1, request->content_len + 1);
    if (body == NULL) return NULL;
    int received = httpd_req_recv(request, body, request->content_len);
    if (received != request->content_len) {
        free(body);
        return NULL;
    }
    return body;
}

static esp_err_t server_select_post(httpd_req_t *request)
{
    char *body = receive_small_form(request);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                                 "invalid body");
    char kind[8], key[FMO_SERVER_KEY_MAX];
    bool valid = form_value(body, "kind", kind, sizeof(kind)) &&
                 form_value(body, "key", key, sizeof(key));
    free(body);
    if (!valid) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                           "kind/key required");
    fmo_config_t config;
    config_store_load(&config);
    if (strcmp(kind, "nrl") == 0) {
        size_t count = server_directory_count();
        size_t found = SIZE_MAX;
        for (size_t i = 0; i < count; ++i) {
            const nrl_server_t *server = server_directory_get(i);
            char candidate[96];
            if (server == NULL) continue;
            snprintf(candidate, sizeof(candidate), "%s:%u", server->host,
                     (unsigned)server->port);
            if (strcmp(candidate, key) == 0) { found = i; break; }
        }
        const nrl_server_t *server = found != SIZE_MAX
            ? server_directory_get(found) : NULL;
        if (server == NULL) return httpd_resp_send_err(
            request, HTTPD_404_NOT_FOUND, "NRL server not found");
        config.selected_server = (uint16_t)found;
        strlcpy(config.nrl_server_key, key, sizeof(config.nrl_server_key));
        strlcpy(config.nrl_host, server->host, sizeof(config.nrl_host));
        config.nrl_port = server->port;
        if (config_store_save(&config) != ESP_OK) return httpd_resp_send_500(request);
        nrl_link_update_config(&config);
    } else if (strcmp(kind, "fmo") == 0) {
        size_t found = fmo_server_directory_find(key);
        const fmo_server_t *server = found != SIZE_MAX
            ? fmo_server_directory_get(found) : NULL;
        if (server == NULL) return httpd_resp_send_err(
            request, HTTPD_404_NOT_FOUND, "FMO server not found");
        strlcpy(config.fmo_server_key, key, sizeof(config.fmo_server_key));
        strlcpy(config.fmo_host, server->host, sizeof(config.fmo_host));
        config.fmo_port = server->port;
        if (config_store_save(&config) != ESP_OK) return httpd_resp_send_500(request);
        fmo_link_update_config(&config);
    } else {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "kind must be nrl or fmo");
    }
    return httpd_resp_sendstr(request, "OK");
}

static esp_err_t server_favorite_post(httpd_req_t *request)
{
    char *body = receive_small_form(request);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                                 "invalid body");
    char key[FMO_SERVER_KEY_MAX], value[8];
    bool valid = form_value(body, "key", key, sizeof(key)) &&
                 form_value(body, "favorite", value, sizeof(value));
    free(body);
    if (!valid) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                           "key/favorite required");
    esp_err_t error = fmo_server_directory_set_favorite(key,
                                                         strcmp(value, "1") == 0);
    if (error == ESP_ERR_NOT_FOUND) return httpd_resp_send_err(
        request, HTTPD_404_NOT_FOUND, "FMO server not found");
    if (error != ESP_OK) return httpd_resp_send_500(request);
    return httpd_resp_sendstr(request, "OK");
}

static esp_err_t server_config_post(httpd_req_t *request)
{
    char *body = receive_small_form(request);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                                 "invalid body");
    char tx[8], policy[8], no_local[8];
    bool valid = form_value(body, "tx_network", tx, sizeof(tx)) &&
                 form_value(body, "audio_policy", policy, sizeof(policy)) &&
                 form_value(body, "no_local", no_local, sizeof(no_local));
    free(body);
    if (!valid || (strcmp(tx, "0") != 0 && strcmp(tx, "1") != 0) ||
        (strcmp(policy, "0") != 0 && strcmp(policy, "1") != 0) ||
        (strcmp(no_local, "0") != 0 && strcmp(no_local, "1") != 0)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid policy");
    }
    fmo_config_t config;
    config_store_load(&config);
    config.tx_network = (uint8_t)(tx[0] - '0');
    config.audio_policy = (uint8_t)(policy[0] - '0');
    config.fmo_mqtt_no_local = no_local[0] == '1';
    if (config_store_save(&config) != ESP_OK) return httpd_resp_send_500(request);
    audio_passthrough_set_audio_policy(config.audio_policy);
    audio_passthrough_set_tx_network(config.tx_network);
    fmo_link_update_config(&config);
    return httpd_resp_sendstr(request, "OK");
}

static esp_err_t qso_get(httpd_req_t *request)
{
    fmo_qso_status_t status = {0};
    fmo_qso_get_status(&status);
    static const char *const phase_names[] = {
        "idle", "querying", "jumping", "calling",
        "ringing", "incoming", "established", "failed",
    };
    const char *phase =
        (size_t)status.phase < sizeof(phase_names) / sizeof(phase_names[0])
            ? phase_names[status.phase] : "idle";
    char peer[32], detail[192];
    json_escape(peer, sizeof(peer), status.peer);
    json_escape(detail, sizeof(detail), status.detail);
    httpd_resp_set_type(request, "application/json");
    char chunk[512];
    snprintf(chunk, sizeof(chunk),
             "{\"phase\":\"%s\",\"peer\":\"%s\",\"peerUid\":%lu,"
             "\"outgoing\":%s,\"incoming\":%s,\"detail\":\"%s\",\"log\":[",
             phase, peer, (unsigned long)status.peer_uid,
             status.outgoing ? "true" : "false",
             status.incoming ? "true" : "false", detail);
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, chunk,
                                              HTTPD_RESP_USE_STRLEN), TAG,
                        "qso JSON header");
    fmo_qso_log_entry_t *entries = malloc(16 * sizeof(*entries));
    if (entries == NULL) return httpd_resp_send_500(request);
    const size_t count = fmo_qso_get_log(entries, 16);
    for (size_t i = 0; i < count; ++i) {
        char e_peer[32], e_result[80], e_comment[FMO_QSO_LOG_COMMENT_MAX * 2];
        json_escape(e_peer, sizeof(e_peer), entries[i].peer);
        json_escape(e_result, sizeof(e_result), entries[i].result);
        json_escape(e_comment, sizeof(e_comment), entries[i].comment);
        snprintf(chunk, sizeof(chunk),
                 "%s{\"ts\":%lld,\"dir\":\"%s\",\"peer\":\"%s\",\"uid\":%lu,"
                 "\"result\":\"%s\",\"comment\":\"%s\"}",
                 i == 0 ? "" : ",", (long long)entries[i].ts, entries[i].dir,
                 e_peer, (unsigned long)entries[i].peer_uid, e_result,
                 e_comment);
        if (httpd_resp_send_chunk(request, chunk, HTTPD_RESP_USE_STRLEN) !=
            ESP_OK) {
            free(entries);
            return ESP_FAIL;
        }
    }
    free(entries);
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "]}", 2), TAG,
                        "qso JSON tail");
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t qso_call_post(httpd_req_t *request)
{
    char *body = receive_small_form(request);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                                 "invalid body");
    char peer[16] = {0}, uid_text[16] = {0};
    const bool have_peer = form_value(body, "peer", peer, sizeof(peer));
    (void)form_value(body, "uid", uid_text, sizeof(uid_text));
    free(body);
    if (!have_peer) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                               "peer required");
    char error[96];
    if (!fmo_qso_call(peer, (uint32_t)strtoul(uid_text, NULL, 10), error,
                      sizeof(error))) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, error);
    }
    return httpd_resp_sendstr(request, "OK");
}

static esp_err_t qso_cancel_post(httpd_req_t *request)
{
    (void)request;
    fmo_qso_cancel();
    return httpd_resp_sendstr(request, "OK");
}

static char *receive_json(httpd_req_t *request, size_t max_size,
                          size_t *received_size)
{
    if (request->content_len <= 0 || (size_t)request->content_len > max_size) {
        return NULL;
    }
    size_t expected = (size_t)request->content_len;
    char *body = malloc(expected + 1);
    if (body == NULL) return NULL;
    size_t received = 0;
    while (received < expected) {
        int count = httpd_req_recv(request, body + received,
                                   expected - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    *received_size = received;
    return body;
}

static void sync_fmo_callsign_from_certificate(void)
{
    fmo_identity_status_t identity = {0};
    if (fmo_cert_store_status(&identity) != ESP_OK || !identity.ready ||
        identity.callsign[0] == '\0') return;

    fmo_config_t config;
    if (config_store_load(&config) != ESP_OK ||
        strcmp(config.fmo_callsign, identity.callsign) == 0) return;
    strlcpy(config.fmo_callsign, identity.callsign,
            sizeof(config.fmo_callsign));
    if (config_store_save(&config) != ESP_OK) return;
    ESP_LOGI(TAG, "FMO callsign synchronized from certificate: %s",
             config.fmo_callsign);
    fmo_discovery_update_config(&config);
    fmo_link_update_config(&config);
    fmo_qso_update_config(&config);
    aprs_service_update_config(&config);
    fmo_station_beacon_update_config(&config);
}

static esp_err_t fmo_cert_get(httpd_req_t *request)
{
    fmo_identity_status_t status;
    esp_err_t error = fmo_cert_store_status(&status);
    char response[384];
    char fingerprint[65] = "";
    if (status.ready) {
        static const char hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 32; ++i) {
            fingerprint[i * 2] = hex[status.fingerprint[i] >> 4];
            fingerprint[i * 2 + 1] = hex[status.fingerprint[i] & 0x0f];
        }
    }
    snprintf(response, sizeof(response),
             "{\"ready\":%s,\"user\":%s,\"intermediate\":%s,"
             "\"device_key\":%s,\"callsign\":\"%s\",\"uid\":%lu,"
             "\"iat\":%llu,\"exp\":%llu,\"fingerprint\":\"%s\","
             "\"error\":\"%s\"}",
             status.ready ? "true" : "false",
             status.user_present ? "true" : "false",
             status.intermediate_present ? "true" : "false",
             status.device_key_present ? "true" : "false",
             status.callsign, (unsigned long)status.uid,
             (unsigned long long)status.issued_at,
             (unsigned long long)status.expires_at, fingerprint,
             error == ESP_OK ? "" : esp_err_to_name(error));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t fmo_cert_post(httpd_req_t *request)
{
    size_t size = 0;
    char *body = receive_json(request, 24U * 1024U, &size);
    if (body == NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "empty or oversized certificate JSON");
    }
    fmo_cert_kind_t kind = (fmo_cert_kind_t)(intptr_t)request->user_ctx;
    char message[96] = "";
    esp_err_t error = fmo_cert_store_put(kind, body, size, message,
                                         sizeof(message));
    free(body);
    if (error != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   message[0] != '\0' ? message :
                                   esp_err_to_name(error));
    }
    sync_fmo_callsign_from_certificate();
    fmo_link_request_certificate_refresh();
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static esp_err_t fmo_activate_get(httpd_req_t *request)
{
    char host[FMO_ACTIVATE_HOST_MAX + 1] = "";
    char last[128] = "";
    char escaped_host[(FMO_ACTIVATE_HOST_MAX + 1) * 2] = "";
    char escaped_last[sizeof(last) * 2] = "";
    uint64_t epoch = 0;
    uint8_t mac[6] = {};
    char mac_text[13];
    fmo_activate_get_host(host, sizeof(host));
    fmo_activate_get_status(last, sizeof(last), &epoch);
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_text, sizeof(mac_text), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    json_escape(escaped_host, sizeof(escaped_host), host);
    json_escape(escaped_last, sizeof(escaped_last), last);
    char response[512];
    snprintf(response, sizeof(response),
             "{\"host\":\"%s\",\"mac\":\"%s\",\"last\":\"%s\","
             "\"last_epoch\":%llu}", escaped_host, mac_text, escaped_last,
             (unsigned long long)epoch);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t fmo_activate_post(httpd_req_t *request)
{
    char *body = receive_small_form(request);
    if (body == NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid activation request");
    }
    char host[FMO_ACTIVATE_HOST_MAX + 1] = "";
    char save_value[8] = "";
    const bool has_host = form_value(body, "cert_host", host, sizeof(host));
    const bool save_only = form_value(body, "save_only", save_value,
                                      sizeof(save_value)) &&
                           strcmp(save_value, "1") == 0;
    if (has_host && host[0] != '\0' && !fmo_activate_set_host(host)) {
        free(body);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "invalid certificate server host");
    }
    free(body);
    if (save_only) return httpd_resp_sendstr(request, "certificate server saved");
    char message[160] = "";
    esp_err_t error = fmo_activate_run(message, sizeof(message));
    if (error != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   message[0] != '\0' ? message :
                                   "certificate activation failed");
    }
    sync_fmo_callsign_from_certificate();
    fmo_link_request_certificate_refresh();
    return httpd_resp_sendstr(request, message);
}

static int compare_ap_rssi(const void *left, const void *right)
{
    const wifi_ap_record_t *a = left;
    const wifi_ap_record_t *b = right;
    return (int)b->rssi - (int)a->rssi;
}

static esp_err_t wifi_scan_get(httpd_req_t *request)
{
    uint16_t count = 24;
    wifi_ap_record_t *records = calloc(count, sizeof(*records));
    if (records == NULL) return httpd_resp_send_500(request);
    esp_err_t error = request->user_ctx != NULL
        ? network_manager_cached_scan_records(records, &count)
        : network_manager_scan_records(records, &count);
    if (error != ESP_OK) {
        free(records);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Wi-Fi scan results unavailable");
    }
    qsort(records, count, sizeof(*records), compare_ap_rssi);
    char *json = calloc(1, 4096);
    if (json == NULL) {
        free(records);
        return httpd_resp_send_500(request);
    }
    size_t used = 0;
    json[used++] = '[';
    for (uint16_t i = 0; i < count && used < 3900; ++i) {
        if (records[i].ssid[0] == '\0') continue;
        bool duplicate = false;
        for (uint16_t previous = 0; previous < i; ++previous) {
            if (strcmp((const char *)records[i].ssid,
                       (const char *)records[previous].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        char ssid[96];
        json_escape(ssid, sizeof(ssid), (const char *)records[i].ssid);
        used += snprintf(json + used, 4096 - used,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                         used == 1 ? "" : ",", ssid, records[i].rssi,
                         records[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
    }
    if (used < 4095) json[used++] = ']';
    json[used] = '\0';
    free(records);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(request, json);
    free(json);
    return result;
}

static esp_err_t wifi_delete_post(httpd_req_t *request)
{
    char *body = NULL;
    if (read_form_body(request, &body) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    char value[16] = {0};
    bool valid = form_value(body, "index", value, sizeof(value));
    free(body);
    if (!valid) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "missing index");
    char *end = NULL;
    unsigned long index = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || index >= FMO_WIFI_PROFILE_MAX) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid index");
    }
    fmo_config_t config;
    if (config_store_load(&config) != ESP_OK ||
        !config_store_wifi_remove(&config, index)) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Wi-Fi not found");
    }
    if (config_store_save(&config) != ESP_OK) return httpd_resp_send_500(request);
    (void)network_manager_update_profiles(&config, true);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static esp_err_t radio_get(httpd_req_t *request)
{
    net_radio_status_t radio = {0};
    net_radio_get_status(&radio);
    const char *state = "idle";
    if (radio.state == NET_RADIO_STATE_CONNECTING) state = "connecting";
    else if (radio.state == NET_RADIO_STATE_PLAYING) state = "playing";
    else if (radio.state == NET_RADIO_STATE_ERROR) state = "error";
    const size_t json_capacity = 10240;
    char *json = calloc(1, json_capacity);
    if (json == NULL) return httpd_resp_send_500(request);
    size_t used = snprintf(json, json_capacity,
        "{\"count\":%u,\"current\":%d,\"playing\":%s,\"state\":\"%s\",\"stations\":[",
        (unsigned)net_radio_count(), radio.current,
        net_radio_is_playing() ? "true" : "false", state);
    const size_t count = net_radio_count();
    for (size_t i = 0; i < count && used < json_capacity - 768; ++i) {
        char name[NET_RADIO_NAME_MAX * 2];
        char url[NET_RADIO_URL_MAX * 2];
        char raw_name[NET_RADIO_NAME_MAX];
        char raw_url[NET_RADIO_URL_MAX];
        if (!net_radio_get(i, raw_name, sizeof(raw_name), raw_url, sizeof(raw_url))) {
            continue;
        }
        json_escape(name, sizeof(name), raw_name);
        json_escape(url, sizeof(url), raw_url);
        used += snprintf(json + used, json_capacity - used,
                         "%s{\"index\":%u,\"name\":\"%s\",\"url\":\"%s\"}",
                         i == 0 ? "" : ",", (unsigned)i, name, url);
    }
    snprintf(json + (used < json_capacity - 2 ? used : json_capacity - 2),
             used < json_capacity - 2 ? json_capacity - used : 2, "]}");
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    esp_err_t result = httpd_resp_sendstr(request, json);
    free(json);
    return result;
}

static esp_err_t radio_post(httpd_req_t *request)
{
    char *body = NULL;
    if (read_form_body(request, &body) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    char action[16] = {0};
    const char *error = NULL;
    if (!form_value(body, "action", action, sizeof(action))) {
        error = "missing action";
    } else if (strcmp(action, "add") == 0) {
        char name[NET_RADIO_NAME_MAX] = {0};
        char url[NET_RADIO_URL_MAX] = {0};
        (void)form_value(body, "name", name, sizeof(name));
        if (!form_value(body, "url", url, sizeof(url)) ||
            !net_radio_add(name, url)) {
            error = "add failed (invalid URL or list full)";
        }
    } else if (strcmp(action, "del") == 0 || strcmp(action, "play") == 0) {
        char value[16] = {0};
        char *end = NULL;
        unsigned long index = 0;
        if (!form_value(body, "index", value, sizeof(value)) ||
            (index = strtoul(value, &end, 10), end == value || *end != '\0')) {
            error = "invalid index";
        } else if (action[0] == 'd' && !net_radio_remove(index)) {
            error = "station not found";
        } else if (action[0] == 'p' && !net_radio_play(index)) {
            error = "play failed";
        }
    } else if (strcmp(action, "stop") == 0) {
        net_radio_stop();
    } else {
        error = "unknown action";
    }
    free(body);
    if (error != NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, error);
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ok\":true}");
}

static esp_err_t ota_status_get(httpd_req_t *request)
{
    fmo_ota_status_t status = {0};
    ota_service_get_status(&status);
    char server[FMO_OTA_URL_MAX * 2];
    char error[256];
    char latest[FMO_OTA_VERSION_MAX * 2];
    json_escape(server, sizeof(server), status.server_url);
    json_escape(error, sizeof(error), status.last_error);
    json_escape(latest, sizeof(latest), status.latest_version);
    const size_t json_capacity = 8192;
    char *json = calloc(1, json_capacity);
    if (json == NULL) return httpd_resp_send_500(request);
    size_t used = snprintf(json, json_capacity,
        "{\"firmware_version\":\"%s\",\"board_type\":\"%s\","
        "\"server_url\":\"%s\",\"latest_version\":\"%s\","
        "\"last_error\":\"%s\",\"checking\":%s,\"updating\":%s,"
        "\"update_percent\":%u,\"update_bytes\":%u,\"update_size\":%u,"
        "\"releases\":[", FMO_FIRMWARE_VERSION, FMO_BOARD_TYPE, server, latest,
        error, status.checking ? "true" : "false",
        status.updating ? "true" : "false", (unsigned)status.update_percent,
        (unsigned)status.update_bytes, (unsigned)status.update_size);
    for (size_t i = 0; i < status.release_count && used < json_capacity - 512; ++i) {
        char version[128];
        char notes[320];
        json_escape(version, sizeof(version), status.releases[i].version);
        json_escape(notes, sizeof(notes), status.releases[i].notes);
        used += snprintf(json + used, json_capacity - used,
                         "%s{\"version\":\"%s\",\"notes\":\"%s\"}",
                         i == 0 ? "" : ",", version, notes);
    }
    snprintf(json + (used < json_capacity - 2 ? used : json_capacity - 2),
             used < json_capacity - 2 ? json_capacity - used : 2, "]}");
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_sendstr(request, json);
    free(json);
    return result;
}

static esp_err_t read_form_body(httpd_req_t *request, char **out)
{
    if (request->content_len > 2048) return ESP_ERR_INVALID_SIZE;
    char *body = calloc(1, request->content_len + 1);
    if (body == NULL) return ESP_ERR_NO_MEM;
    int total = 0;
    while (total < request->content_len) {
        int count = httpd_req_recv(request, body + total, request->content_len - total);
        if (count <= 0) {
            free(body);
            return ESP_FAIL;
        }
        total += count;
    }
    *out = body;
    return ESP_OK;
}

static esp_err_t ota_config_post(httpd_req_t *request)
{
    char *body = NULL;
    if (read_form_body(request, &body) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    char server[FMO_OTA_URL_MAX] = {0};
    char token[96] = {0};
    bool have_server = form_value(body, "server_url", server, sizeof(server));
    (void)form_value(body, "device_token", token, sizeof(token));
    free(body);
    if (!have_server || !ota_service_set_config(server, token)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid OTA config");
    }
    return httpd_resp_sendstr(request, "saved");
}

static esp_err_t ota_check_post(httpd_req_t *request)
{
    if (!ota_service_check_now()) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "OTA service busy");
    }
    return httpd_resp_sendstr(request, "check requested");
}

static esp_err_t ota_install_post(httpd_req_t *request)
{
    char *body = NULL;
    if (read_form_body(request, &body) != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid form");
    }
    char version[FMO_OTA_VERSION_MAX] = {0};
    bool valid = form_value(body, "version", version, sizeof(version));
    free(body);
    if (!valid || !ota_service_update_version(version)) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "version unavailable or OTA service busy");
    }
    return httpd_resp_sendstr(request, "install requested");
}

static esp_err_t firmware_upload_post(httpd_req_t *request)
{
    const char *content_type = httpd_req_get_hdr_value_len(request, "Content-Type") > 0
                                   ? "present" : NULL;
    char type[80] = {0};
    if (content_type != NULL) {
        (void)httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type));
    }
    if (strstr(type, "multipart/form-data") != NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "upload raw application/octet-stream, not multipart");
    }
    if (request->content_len <= 0) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "empty firmware image");
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || request->content_len > target->size) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "image too large");
    }
    if (!ota_service_local_begin((uint32_t)request->content_len)) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "OTA service busy");
    }
    esp_ota_handle_t handle = 0;
    esp_err_t error = esp_ota_begin(target, request->content_len, &handle);
    uint32_t received = 0;
    uint8_t buffer[2048];
    int timeout_count = 0;
    while (error == ESP_OK && received < (uint32_t)request->content_len) {
        int count = httpd_req_recv(request, (char *)buffer,
                                   sizeof(buffer) < request->content_len - received
                                       ? sizeof(buffer) : request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeout_count <= 5) continue;
        if (count <= 0) {
            error = ESP_FAIL;
            break;
        }
        timeout_count = 0;
        error = esp_ota_write(handle, buffer, count);
        if (error == ESP_OK) {
            received += count;
            ota_service_local_progress(received, (uint32_t)request->content_len);
        }
    }
    if (error == ESP_OK) error = esp_ota_end(handle);
    else if (handle != 0) (void)esp_ota_abort(handle);
    if (error == ESP_OK) error = esp_ota_set_boot_partition(target);
    if (error != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), "upload failed: %s", esp_err_to_name(error));
        ota_service_local_end(false, message);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, message);
    }
    ota_service_local_end(true, NULL);
    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_sendstr(request, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return result;
}

/* Captive portal 404 handler: redirect any unknown path to "/".
 * Phones probe specific URLs (generate_204, hotspot-detect.html, etc.);
 * getting a redirect with body triggers the captive portal UI. */
static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/config/wifi");
    httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t captive_probe_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/config/wifi");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Open Wi-Fi setup", HTTPD_RESP_USE_STRLEN);
}

esp_err_t web_portal_start(void)
{
    sync_fmo_callsign_from_certificate();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* Keep headroom for optional FMO certificate activation routes. */
    config.max_uri_handlers = 36;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 30;
    /* Captive-portal probes can arrive while configuration handlers are
     * active; 8 KB overflowed in the HTTP server task on ESP-IDF 6.2. */
    config.stack_size = 12288;
    /* lwIP only allows 16 sockets system-wide; keep-alive stops the page's
     * status polling from churning sockets into TIME_WAIT, and lru_purge
     * recycles idle connections instead of rejecting new ones. */
    config.max_open_sockets = 6;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 15;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 2;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "HTTP start failed");

    /* Captive portal: redirect all unknown URLs to "/" so phones auto-open
     * the configuration page (iOS requires a response body). */
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, captive_404_handler);
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/config/*", .method = HTTP_GET, .handler = config_page_get},
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get},
        {.uri = "/api/config", .method = HTTP_GET, .handler = config_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/scan", .method = HTTP_GET, .handler = wifi_scan_get},
        {.uri = "/scan/cached", .method = HTTP_GET, .handler = wifi_scan_get,
         .user_ctx = (void *)1},
        {.uri = "/wifi/delete", .method = HTTP_POST, .handler = wifi_delete_post},
        {.uri = "/api/radio", .method = HTTP_GET, .handler = radio_get},
        {.uri = "/api/radio", .method = HTTP_POST, .handler = radio_post},
        {.uri = "/servers", .method = HTTP_GET, .handler = servers_page_get},
        {.uri = "/api/servers", .method = HTTP_GET, .handler = servers_get},
        {.uri = "/api/servers/select", .method = HTTP_POST, .handler = server_select_post},
        {.uri = "/api/servers/favorite", .method = HTTP_POST, .handler = server_favorite_post},
        {.uri = "/api/servers/config", .method = HTTP_POST, .handler = server_config_post},
        {.uri = "/api/qso", .method = HTTP_GET, .handler = qso_get},
        {.uri = "/api/qso/call", .method = HTTP_POST, .handler = qso_call_post},
        {.uri = "/api/qso/cancel", .method = HTTP_POST, .handler = qso_cancel_post},
        {.uri = "/api/fmo/cert", .method = HTTP_GET, .handler = fmo_cert_get},
        {.uri = "/api/fmo/cert/user", .method = HTTP_POST, .handler = fmo_cert_post,
         .user_ctx = (void *)(intptr_t)FMO_CERT_USER},
        {.uri = "/api/fmo/cert/intermediate", .method = HTTP_POST,
         .handler = fmo_cert_post,
         .user_ctx = (void *)(intptr_t)FMO_CERT_INTERMEDIATE},
        {.uri = "/api/fmo/cert/devicekey", .method = HTTP_POST,
         .handler = fmo_cert_post,
         .user_ctx = (void *)(intptr_t)FMO_CERT_DEVICE_KEY},
        {.uri = "/api/fmo/activate", .method = HTTP_GET,
         .handler = fmo_activate_get},
        {.uri = "/api/fmo/activate", .method = HTTP_POST,
         .handler = fmo_activate_post},
        {.uri = "/update", .method = HTTP_GET, .handler = update_get},
        {.uri = "/update", .method = HTTP_POST, .handler = firmware_upload_post},
        {.uri = "/ota/status", .method = HTTP_GET, .handler = ota_status_get},
        {.uri = "/ota/config", .method = HTTP_POST, .handler = ota_config_post},
        {.uri = "/ota/check", .method = HTTP_POST, .handler = ota_check_post},
        {.uri = "/ota/install", .method = HTTP_POST, .handler = ota_install_post},
        /* Keep this last: it catches Android/iOS/Windows captive-portal probes
         * (for example /generate_204 and /hotspot-detect.html). */
        {.uri = "/*", .method = HTTP_GET, .handler = captive_probe_get},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG,
                            "route register failed");
    }
    ESP_LOGI(TAG, "portal listening on port 80");
    return ESP_OK;
}
