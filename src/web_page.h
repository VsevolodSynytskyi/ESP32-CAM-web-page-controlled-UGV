#pragma once

// The viewer page: full-screen video with one joystick pad over it.
//
// A single square pad, crosshaired into quadrants, drives both tracks. Vertical
// deflection is throttle, horizontal is steering, and the mix between them is
// the whole design.
//
// Left is channel A (AO1/AO2), right is channel B (BO1/BO2). The readout names
// them the same way, so a motor turning the wrong way names the thing you
// rewire without anyone having to trace a cable.
//
//   MIXING
//
//   x runs -1 (left) to +1 (right), y runs -1 (bottom) to +1 (top).
//
//     inner = 1 - |x|/2        the inner track's share of throttle
//     spin  = x * (1 - |y|)    opposed-track authority, full at the centre row
//
//     x >= 0:  L = y         + spin,   R = y * inner - spin
//     x <  0:  L = y * inner + spin,   R = y         - spin
//
//   Two behaviours blended by |y|: along the centre row it is a pure pivot with
//   the tracks fully opposed, along the top and bottom edges it is a curve with
//   the outer track at full and the inner at half. Everything between is a
//   linear blend of the two, which is why this needs no trig and no square root.
//
//   Both outputs stay inside [-1, +1] over the whole square - for x,y >= 0,
//   L = x(1-y) + y is a convex combination of x and 1, and R is monotone in y
//   between -x and 1 - x/2. So there is no normalisation pass to get wrong.
//
//     top centre    ( 0, +1)   L +1.00  R +1.00   straight forward
//     bottom centre ( 0, -1)   L -1.00  R -1.00   straight back
//     centre left   (-1,  0)   L -1.00  R +1.00   pivot counter-clockwise
//     centre right  (+1,  0)   L +1.00  R -1.00   pivot clockwise
//     top left      (-1, +1)   L +0.50  R +1.00   curve left
//     top right     (+1, +1)   L +1.00  R +0.50   curve right
//     bottom left   (-1, -1)   L -0.50  R -1.00   reverse, tail to the left
//     bottom right  (+1, -1)   L -1.00  R -0.50   reverse, tail to the right
//
//   Steering is car-like in reverse: pushing left always slows the left track,
//   so backing up swings the tail toward the stick and the nose away from it.
//   The price is that yaw changes sense partway down the left and right edges
//   (at y = -0.8). That is forced by the two endpoints above - a pivot one way
//   at the centre and a curve the other way at the corner - not a defect in the
//   mix. Mirroring x when y < 0 removes it and costs the car-like feel.
//
// Nothing latches. Releasing the pad, losing the pointer, hiding the page or
// dropping the socket all mean stop, and the firmware stops by itself after
// CMD_TIMEOUT_MS regardless.
static const char kViewerHtml[] = R"HTML(<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover">
<title>UGV GCS</title><style>
html,body{margin:0;height:100%;background:#000;overflow:hidden;color:#fff;
  font:600 15px/1 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
  -webkit-user-select:none;user-select:none;touch-action:none;overscroll-behavior:none}
/* The camera is bolted on turned 90 degrees counter-clockwise, so the picture
   is turned back the other way here. It is done in CSS because the OV2640 has
   no rotate - it does h-mirror and v-flip only - and rotating JPEG on the ESP32
   would mean decode, rotate and re-encode every frame. The phone's compositor
   does it for nothing.
   The box is deliberately 100vh wide by 100vw tall: those are the viewport's
   dimensions swapped, so that once rotated the element covers the screen the
   right way round. The other way round is 90deg. */
#v{position:fixed;left:50%;top:50%;width:100vh;height:100vw;object-fit:contain;
  transform:translate(-50%,-50%) rotate(-90deg)}
.pill{position:fixed;top:calc(env(safe-area-inset-top) + 10px);z-index:2;
  padding:5px 11px;border-radius:99px;background:#000a;font-size:12px;letter-spacing:.04em}
#s{left:10px}
#s.on{color:#4ade80}
#s.off{color:#f87171}
#n{right:10px}
/* Under the status pill, and gone entirely when the firmware has nothing to
   report - which is most of the time the stream is not running. */
#d{left:10px;top:calc(env(safe-area-inset-top) + 40px);font-size:11px;opacity:.72}
#d:empty{display:none}
#n i{font-style:normal;opacity:.45;padding-right:4px}
#n span{padding-right:10px}
#n span:last-child{padding-right:0}
#j{position:fixed;left:50%;bottom:calc(env(safe-area-inset-bottom) + 14px);z-index:2;
  width:min(48vmin,260px);aspect-ratio:1;transform:translateX(-50%);
  border-radius:16px;border:1px solid #ffffff33;touch-action:none;
  -webkit-tap-highlight-color:transparent;
  background:
    linear-gradient(#ffffff2b,#ffffff2b) center/100% 1px no-repeat,
    linear-gradient(#ffffff2b,#ffffff2b) center/1px 100% no-repeat,
    #000000a6}
#k{position:absolute;left:50%;top:50%;width:24%;aspect-ratio:1;
  border-radius:50%;background:#fff;transform:translate(-50%,-50%);
  box-shadow:0 0 0 1px #00000059,0 2px 7px #00000073;transition:width .08s ease-out}
#j.hot #k{width:29%}
</style></head><body>
<img id="v" alt="">
<div id="s" class="pill off">connecting</div>
<div id="d" class="pill"></div>
<div id="n" class="pill"><span><i>L</i><b id="nl">+000</b></span><span><i>R</i><b id="nr">+000</b></span></div>
<div id="j"><div id="k"></div></div>
<script>
var FULL = 100;  // wire units: +-100 is full scale, see web_server.cpp

// Everything the overlay shows also goes to the console, stamped with seconds
// since the page loaded. The overlay is one line that keeps overwriting itself;
// this is the same run as a transcript you can select and paste. One string per
// call, so a copied block stays one line per event.
function log(msg) {
  console.log('[' + (performance.now() / 1000).toFixed(1) + 's] ' + msg);
}

// --- video -----------------------------------------------------------------
// One port up from this page - see the two-instance note in stream_server.h.
var v = document.getElementById('v');
var vurl = 'http://' + location.hostname + ':' + (Number(location.port || 80) + 1) + '/stream';
function startVideo() { v.src = vurl + '?n=' + Date.now(); }
v.onerror = function () { log('video error, retrying'); setTimeout(startVideo, 1000); };
startVideo();

// --- control link ----------------------------------------------------------
var st = document.getElementById('s');
var dg = document.getElementById('d');
var ws = null;
var pingAt = 0;  // performance.now() of the probe still in flight, 0 if none
var rttPeak = 0; // worst round trip since the link came up

function connect() {
  ws = new WebSocket('ws://' + location.host + '/control');
  ws.onopen = function () {
    pingAt = 0; rttPeak = 0; dg.textContent = '';
    st.textContent = 'linked'; st.className = 'pill on';
    log('link up');
  };
  // The only thing the firmware ever sends is the answer to a probe, carrying
  // the stream's stats as its payload. Shown verbatim - the firmware owns the
  // format, so there is nothing here to keep in step with it.
  ws.onmessage = function (e) {
    if (!pingAt) return;
    var rtt = Math.round(performance.now() - pingAt);
    pingAt = 0;
    if (rtt > rttPeak) rttPeak = rtt;
    // The peak is the point: an outage is one bad second in twenty, so the live
    // figure will almost always look fine when you glance at it.
    st.textContent = 'linked ' + rtt + 'ms  peak' + rttPeak + 'ms';
    dg.textContent = e.data || '';
    log('rtt ' + rtt + 'ms  ' + (e.data || 'no stream'));
  };
  ws.onclose = function () {
    st.textContent = 'reconnecting'; st.className = 'pill off';
    dg.textContent = '';
    log('link down');
    release();
    setTimeout(connect, 1000);
  };
  ws.onerror = function () { ws.close(); };
}
connect();

// --- joystick --------------------------------------------------------------
var pad = document.getElementById('j'), knob = document.getElementById('k');
var nl = document.getElementById('nl'), nr = document.getElementById('nr');
var jx = 0, jy = 0;      // stick position, -1..+1 per axis
var cl = 0, cr = 0;      // mixed wire values, what send() will put on the socket
var pid = null;          // the one pointer that owns the pad

function clamp1(n) { return n < -1 ? -1 : n > 1 ? 1 : n; }

function fmt(n) { return (n < 0 ? '-' : '+') + ('00' + Math.abs(n)).slice(-3); }

// The mix. See the table at the top of this file.
function apply() {
  var inner = 1 - Math.abs(jx) / 2;
  var spin  = jx * (1 - Math.abs(jy));
  var l, r;
  if (jx >= 0) { l = jy + spin;         r = jy * inner - spin; }
  else         { l = jy * inner + spin; r = jy - spin; }
  // Both are already inside +-1; the clamp is here to make that a fact of the
  // wire format rather than a property of the algebra above.
  cl = Math.max(-FULL, Math.min(FULL, Math.round(l * FULL)));
  cr = Math.max(-FULL, Math.min(FULL, Math.round(r * FULL)));
  knob.style.left = (50 + jx * 50) + '%';
  knob.style.top  = (50 - jy * 50) + '%';
  nl.textContent = fmt(cl);
  nr.textContent = fmt(cr);
}

function send() {
  if (!ws || ws.readyState !== 1) return;
  ws.send(new Int8Array([cl, cr]));
}

function grab(e) {
  var b = pad.getBoundingClientRect();
  // Screen y grows downward, stick y grows up - hence the reversed subtraction.
  jx = clamp1((e.clientX - (b.left + b.width  / 2)) / (b.width  / 2));
  jy = clamp1(((b.top + b.height / 2) - e.clientY) / (b.height / 2));
  apply();
}

function release() {
  pid = null; jx = 0; jy = 0;
  pad.classList.remove('hot');
  apply();
  send();  // out of band, so letting go stops now rather than on the next tick
}

pad.addEventListener('pointerdown', function (e) {
  e.preventDefault();
  if (pid !== null) return;  // a second thumb does not get to steal the stick
  pid = e.pointerId;
  pad.setPointerCapture(pid);
  pad.classList.add('hot');
  grab(e);
  send();
});

// Capture keeps these coming once the thumb slides off the pad, where grab()
// clamps to the edge. Sliding out is a full-deflection hold, not a release.
pad.addEventListener('pointermove', function (e) {
  if (e.pointerId !== pid) return;
  e.preventDefault();
  grab(e);
});

['pointerup', 'pointercancel', 'lostpointercapture'].forEach(function (ev) {
  pad.addEventListener(ev, function (e) { if (e.pointerId === pid) release(); });
});
pad.addEventListener('contextmenu', function (e) { e.preventDefault(); });

apply();

// Movement rides this tick rather than going out per pointermove: a phone can
// fire those at 120Hz, and the socket does not need to hear about all of it.
// 20Hz also keeps three frames of margin under the CMD_TIMEOUT_MS failsafe.
setInterval(send, 50);

// Round trip on the control socket, which shares the air with the video. If
// this stays in the tens of ms while the picture lags, the radio is not what is
// holding the picture up.
setInterval(function () {
  if (!ws || ws.readyState !== 1) return;
  if (pingAt) {
    if (performance.now() - pingAt < 3000) return;  // one probe in flight at a time
    st.textContent = 'linked >3s';
    log('rtt >3000ms, no reply');
  }
  pingAt = performance.now();
  ws.send(new Int8Array([0]));
}, 1000);

// Do not trust the page to stay in front of you.
addEventListener('pagehide', release);
addEventListener('blur', release);
document.addEventListener('visibilitychange', function () { if (document.hidden) release(); });
</script></body></html>)HTML";
