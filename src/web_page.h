#pragma once

// ===========================================================================
//  The mobile control page, served from flash by web_server.cpp.
//
//  Two globals are injected ahead of this markup by the HTTP handler:
//
//      window.TANK_CONTROL      1 in Stage 4, 0 in Stage 3 (video only)
//      window.TANK_STREAM_PORT  the MJPEG port (81)
//
//  No PROGMEM: on the ESP32 it expands to nothing. Flash is memory-mapped and
//  directly readable, so a plain const array already lives in .rodata.
//
//  Nothing is fetched from a CDN, deliberately - the phone has no internet while
//  it is joined to the vehicle's SoftAP.
// ===========================================================================

static const char kControlPageHtml[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<meta name="mobile-web-app-capable" content="yes">
<title>TankCam</title>
<style>
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html,body{margin:0;height:100%;overflow:hidden;overscroll-behavior:none;
  background:#0a0c10;color:#e8eef7;
  font:500 14px/1.3 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
  touch-action:none;-webkit-user-select:none;user-select:none}
#cam{position:fixed;inset:0;width:100%;height:100%;object-fit:contain;background:#000;
  display:block;z-index:0}

/* --- heads-up strip ---------------------------------------------------- */
#hud{position:fixed;top:0;left:0;right:0;z-index:3;display:flex;align-items:center;
  gap:14px;padding:8px 14px;padding-top:max(8px,env(safe-area-inset-top));
  background:linear-gradient(180deg,rgba(6,8,12,.82),rgba(6,8,12,0));
  font-variant-numeric:tabular-nums;pointer-events:none}
#hud .sp{flex:1}
#dot{width:9px;height:9px;border-radius:50%;background:#f2544b;
  box-shadow:0 0 8px currentColor;color:#f2544b;transition:background .2s,color .2s}
#dot.ok{background:#3ddc84;color:#3ddc84}
.tag{opacity:.62;font-size:11px;letter-spacing:.09em;text-transform:uppercase}
.num{font-size:15px;font-weight:700;min-width:3.4em;display:inline-block}
#fs{position:fixed;z-index:4;top:max(6px,env(safe-area-inset-top));
  right:max(10px,env(safe-area-inset-right));width:36px;height:36px;border-radius:11px;
  border:1px solid rgba(255,255,255,.18);background:rgba(14,17,23,.7);color:#e8eef7;
  font-size:15px;line-height:1;pointer-events:auto}

/* --- sliders ----------------------------------------------------------- */
#pads{position:fixed;inset:0;z-index:2;pointer-events:none}
.sl{position:absolute;top:58px;bottom:max(14px,env(safe-area-inset-bottom));width:88px;
  pointer-events:auto;touch-action:none}
.sl.l{left:max(12px,env(safe-area-inset-left))}
.sl.r{right:max(12px,env(safe-area-inset-right))}
.trk{position:relative;width:100%;height:100%;border-radius:46px;overflow:hidden;
  background:rgba(10,13,18,.5);border:1px solid rgba(255,255,255,.15);
  -webkit-backdrop-filter:blur(7px);backdrop-filter:blur(7px);
  transition:border-color .15s,background .15s}
.sl.on .trk{border-color:rgba(120,190,255,.65);background:rgba(10,13,18,.62)}
.mid{position:absolute;left:9px;right:9px;top:50%;height:2px;margin-top:-1px;
  background:rgba(255,255,255,.34);border-radius:2px}
.fill{position:absolute;left:7px;right:7px;top:50%;height:0;border-radius:40px;
  background:linear-gradient(180deg,#4da3ff,#2b6fd4);opacity:.5}
.sl.rev .fill{background:linear-gradient(180deg,#ff9d4d,#d4652b)}
.thumb{position:absolute;left:5px;right:5px;top:50%;height:60px;margin-top:-30px;
  border-radius:34px;background:#eef3fa;
  box-shadow:0 3px 14px rgba(0,0,0,.55),inset 0 -2px 0 rgba(0,0,0,.09)}
.thumb::after{content:"";position:absolute;left:50%;top:50%;width:26px;height:3px;
  margin:-1.5px 0 0 -13px;border-radius:2px;background:rgba(20,26,36,.32)}
.cap{position:absolute;left:0;right:0;bottom:10px;text-align:center;font-size:11px;
  letter-spacing:.14em;opacity:.5;pointer-events:none}

/* --- notices ----------------------------------------------------------- */
.note{position:fixed;z-index:5;left:50%;top:50%;transform:translate(-50%,-50%);
  max-width:78%;padding:16px 20px;border-radius:14px;text-align:center;
  background:rgba(12,15,21,.92);border:1px solid rgba(255,255,255,.16);
  box-shadow:0 10px 40px rgba(0,0,0,.6)}
.note b{display:block;margin-bottom:5px;font-size:15px}
.note span{opacity:.7;font-size:13px}
#rot{display:none}
@media (orientation:portrait) and (max-width:820px){#rot{display:block}}
</style>
</head>
<body>

<img id="cam" alt="camera feed">

<div id="hud">
  <span id="dot"></span>
  <span><span class="tag">L</span> <span class="num" id="nl">0</span></span>
  <span><span class="tag">R</span> <span class="num" id="nr">0</span></span>
  <span class="sp"></span>
  <span><span class="num" id="nf">--</span><span class="tag">fps</span></span>
</div>
<button id="fs" title="fullscreen">&#9974;</button>

<div id="pads">
  <div class="sl l" id="sL">
    <div class="trk"><div class="fill"></div><div class="mid"></div><div class="thumb"></div>
      <div class="cap">LEFT</div></div>
  </div>
  <div class="sl r" id="sR">
    <div class="trk"><div class="fill"></div><div class="mid"></div><div class="thumb"></div>
      <div class="cap">RIGHT</div></div>
  </div>
</div>

<div class="note" id="rot"><b>Rotate to landscape</b><span>One thumb per track.</span></div>
<div class="note" id="vonly" style="display:none">
  <b>Video only</b><span>Stage 3 has no control channel yet.</span></div>

<script>
(function(){
"use strict";
var CONTROL = !!window.TANK_CONTROL;
var SEND_HZ = 20;

// ---- video -------------------------------------------------------------
var cam = document.getElementById('cam');
var streamBase = location.protocol + '//' + location.hostname + ':' +
                 (window.TANK_STREAM_PORT || 81) + '/stream';
function loadStream(){ cam.src = streamBase + '?t=' + Date.now(); }
// An MJPEG response dies when the board reboots and the browser will not retry
// on its own, so nudge it. Without this, one brownout means a black screen until
// the pilot thinks to reload.
cam.addEventListener('error', function(){ setTimeout(loadStream, 1200); });
loadStream();

document.getElementById('fs').addEventListener('click', function(){
  var el = document.documentElement;
  if (document.fullscreenElement || document.webkitFullscreenElement) {
    (document.exitFullscreen || document.webkitExitFullscreen).call(document);
  } else {
    (el.requestFullscreen || el.webkitRequestFullscreen || function(){}).call(el);
  }
});

// ---- slider widget -----------------------------------------------------
// Custom pointer-event widget rather than <input type=range>: a native range
// input latches where you leave it, and vertical orientation is inconsistent
// across mobile browsers. This also lets us tap anywhere on the track instead
// of demanding a hit on a thin thumb.
function makeSlider(root, onChange){
  var trk   = root.querySelector('.trk'),
      fill  = root.querySelector('.fill'),
      thumb = root.querySelector('.thumb');
  // One captured pointer per slider, keyed by pointerId. A single shared
  // "dragging" flag would let a second thumb steal the first slider, which
  // breaks two-handed control entirely.
  var pid = null, value = 0;

  function paint(v){
    var pct = Math.abs(v) * 50;
    fill.style.top    = (v > 0 ? 50 - pct : 50) + '%';
    fill.style.height = pct + '%';
    thumb.style.top   = (50 - v * 50) + '%';
    root.classList.toggle('rev', v < 0);
  }
  function fromEvent(e){
    var r = trk.getBoundingClientRect();
    if (!r.height) return 0;
    var v = 1 - 2 * (e.clientY - r.top) / r.height;   // top=+1, bottom=-1
    return v < -1 ? -1 : (v > 1 ? 1 : v);
  }
  function down(e){
    if (pid !== null) return;
    pid = e.pointerId;
    try { trk.setPointerCapture(pid); } catch(_){}
    root.classList.add('on');
    // Touch-down transmits at once so the vehicle responds the instant you
    // commit, rather than waiting up to a tick.
    value = fromEvent(e); paint(value); onChange(true);
    e.preventDefault();
  }
  function move(e){
    if (e.pointerId !== pid) return;
    // Local state and pixels only - the timer owns the wire.
    value = fromEvent(e); paint(value); onChange(false);
    e.preventDefault();
  }
  function release(e){
    if (pid === null) return;
    if (e && e.pointerId !== pid) return;
    try { trk.releasePointerCapture(pid); } catch(_){}
    pid = null;
    // Spring back to centre. Releasing the screen IS the stop, which is why
    // there is no separate stop button to fumble for.
    value = 0; paint(0);
    root.classList.remove('on');
    onChange(true);          // out-of-band send: never wait for the next tick
  }

  trk.addEventListener('pointerdown', down);
  trk.addEventListener('pointermove', move);
  trk.addEventListener('pointerup', release);
  trk.addEventListener('pointercancel', release);
  trk.addEventListener('lostpointercapture', release);

  paint(0);
  return {
    get value(){ return value; },
    zero: function(){ if (pid !== null) release(null); else { value = 0; paint(0); } }
  };
}

var nl = document.getElementById('nl'), nr = document.getElementById('nr'),
    nf = document.getElementById('nf'), dot = document.getElementById('dot');

// ---- control channel ---------------------------------------------------
var ws = null, open = false, retry = null;
var L, R;

function q(v){ v = Math.round(v * 100); return v < -100 ? -100 : (v > 100 ? 100 : v); }

function hud(){ nl.textContent = q(L.value); nr.textContent = q(R.value); }

function tx(){
  if (!CONTROL || !open) return;
  // Deliberately sent every tick even when unchanged. Deduplicating identical
  // frames looks like a free optimisation but would let the firmware's
  // CMD_TIMEOUT_MS deadman trip while holding a steady throttle.
  try { ws.send(new Int8Array([q(L.value), q(R.value)])); } catch(_){}
}

// A pointermove fires up to 120 times a second on a modern phone. Transmission
// is owned by the 20 Hz timer instead, so the wire rate stays bounded no matter
// how fast the browser reports movement. Only touch-down and release transmit
// out of band, and each of those happens once per touch.
function onSlider(immediate){
  hud();
  if (immediate) tx();
}

L = makeSlider(document.getElementById('sL'), onSlider);
R = makeSlider(document.getElementById('sR'), onSlider);

function zero(){ L.zero(); R.zero(); hud(); tx(); }

function connect(){
  if (!CONTROL) return;
  try { ws = new WebSocket('ws://' + location.host + '/ws'); }
  catch(_) { retry = setTimeout(connect, 1500); return; }
  ws.binaryType = 'arraybuffer';
  ws.onopen  = function(){ open = true; dot.className = 'ok'; };
  ws.onmessage = function(ev){
    if (typeof ev.data !== 'string') return;
    var m = /fps=([\d.]+)/.exec(ev.data);
    if (m) nf.textContent = Math.round(parseFloat(m[1]));
  };
  ws.onclose = function(){
    open = false; dot.className = ''; nf.textContent = '--';
    zero();
    if (retry) clearTimeout(retry);
    retry = setTimeout(connect, 1200);
  };
  ws.onerror = function(){ try { ws.close(); } catch(_){} };
}

// Every path that means "the pilot is no longer looking at this" stops the
// vehicle. The firmware deadman is the backstop that trusts none of these.
document.addEventListener('visibilitychange', function(){
  if (document.hidden) zero();
});
window.addEventListener('pagehide', function(){ zero(); });
window.addEventListener('blur',     function(){ zero(); });

if (CONTROL) {
  setInterval(tx, Math.round(1000 / SEND_HZ));
  connect();
} else {
  document.getElementById('pads').style.display = 'none';
  document.getElementById('vonly').style.display = 'block';
  dot.className = 'ok';
}
})();
</script>
</body>
</html>
)HTML";
