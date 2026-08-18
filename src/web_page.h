#pragma once

// The viewer page: full-screen video with a motor test pad over it.
//
// Four hold-to-run buttons, forward and back per motor, labelled A and B to
// match the TB6612FNG silkscreen - when a motor spins the wrong way, the label
// has to name the thing you rewire.
//
// Nothing latches. Releasing a button, losing the pointer, hiding the page or
// dropping the socket all mean stop, and the firmware stops by itself after
// CMD_TIMEOUT_MS regardless. This is the same contract the spring-to-centre
// sliders will run on, so only this file changes when they land.
static const char kViewerHtml[] = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<title>UGV GCS</title><style>
html,body{margin:0;height:100%;background:#000;overflow:hidden;color:#fff;
  font:600 15px/1 -apple-system,system-ui,sans-serif;
  -webkit-user-select:none;user-select:none;touch-action:none;overscroll-behavior:none}
#v{position:fixed;inset:0;width:100%;height:100%;object-fit:contain}
#s{position:fixed;top:calc(env(safe-area-inset-top) + 10px);left:10px;z-index:2;
  padding:5px 11px;border-radius:99px;background:#000a;font-size:12px;letter-spacing:.04em}
#s.on{color:#4ade80}
#s.off{color:#f87171}
#c{position:fixed;left:0;right:0;bottom:0;z-index:2;display:flex;gap:10px;justify-content:center;
  padding:10px 10px calc(env(safe-area-inset-bottom) + 10px)}
.g{flex:1;max-width:210px;padding:8px;border-radius:14px;background:#000a}
.g b{display:block;padding-bottom:2px;font-size:11px;letter-spacing:.12em;opacity:.55;text-align:center}
button{display:block;width:100%;margin-top:6px;padding:15px 0;border-radius:10px;
  font:inherit;font-size:16px;color:#fff;background:#2a2f3a;border:1px solid #4b5563;
  touch-action:none;-webkit-tap-highlight-color:transparent}
button.hot{background:#2563eb;border-color:#93c5fd}
</style></head><body>
<img id="v" alt="">
<div id="s" class="off">connecting</div>
<div id="c">
  <div class="g"><b>MOTOR A</b>
    <button data-m="l" data-d="1">&#9650; forward</button>
    <button data-m="l" data-d="-1">&#9660; back</button>
  </div>
  <div class="g"><b>MOTOR B</b>
    <button data-m="r" data-d="1">&#9650; forward</button>
    <button data-m="r" data-d="-1">&#9660; back</button>
  </div>
</div>
<script>
var FULL = 100;  // wire units: +-100 is full scale, see web_server.cpp

// --- video -----------------------------------------------------------------
// One port up from this page - see the two-instance note in stream_server.h.
var v = document.getElementById('v');
var vurl = 'http://' + location.hostname + ':' + (Number(location.port || 80) + 1) + '/stream';
function startVideo() { v.src = vurl + '?n=' + Date.now(); }
v.onerror = function () { setTimeout(startVideo, 1000); };
startVideo();

// --- control link ----------------------------------------------------------
var st = document.getElementById('s');
var btns = [].slice.call(document.querySelectorAll('button'));
var held = {};
var ws = null;

function connect() {
  ws = new WebSocket('ws://' + location.host + '/control');
  ws.onopen = function () { st.textContent = 'linked'; st.className = 'on'; };
  ws.onclose = function () {
    st.textContent = 'reconnecting'; st.className = 'off';
    release();
    setTimeout(connect, 1000);
  };
  ws.onerror = function () { ws.close(); };
}
connect();

function send() {
  if (!ws || ws.readyState !== 1) return;
  var l = (held.l1 ? FULL : 0) + (held['l-1'] ? -FULL : 0);
  var r = (held.r1 ? FULL : 0) + (held['r-1'] ? -FULL : 0);
  ws.send(new Int8Array([l, r]));
}

function paint() {
  btns.forEach(function (b) { b.classList.toggle('hot', !!held[b.dataset.m + b.dataset.d]); });
}

function hold(b, on) {
  var k = b.dataset.m + b.dataset.d;
  if (!!held[k] === on) return;
  if (on) held[k] = 1; else delete held[k];
  paint();
  send();  // out of band, so press and release both act now, not on the next tick
}

function release() { held = {}; paint(); send(); }

btns.forEach(function (b) {
  b.addEventListener('pointerdown', function (e) {
    e.preventDefault();
    b.setPointerCapture(e.pointerId);
    hold(b, true);
  });
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(function (ev) {
    b.addEventListener(ev, function () { hold(b, false); });
  });
  b.addEventListener('contextmenu', function (e) { e.preventDefault(); });
});

// A held button has to keep saying so or the firmware failsafe stops it. This
// also re-sends the zeroes, which costs nothing and covers a release whose
// out-of-band send was dropped.
setInterval(send, 100);

// Do not trust the page to stay in front of you.
addEventListener('pagehide', release);
addEventListener('blur', release);
document.addEventListener('visibilitychange', function () { if (document.hidden) release(); });
</script></body></html>)HTML";
