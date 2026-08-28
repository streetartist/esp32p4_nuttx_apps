'use strict';
const size = ui.getSize();
const W = size.width;
const H = size.height;
ui.background(0x14141f);
const ctx = ui.canvas(0, 0, W, H);
const hint = ui.text('戳一戳，拖一拖', 28, 18, 20, 0x6a6a80);

function lerp(a, b, k) {
  return a + (b - a) * k;
}

function clamp(v, lo, hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

const f = {
  lx: 0, ly: 0, tlx: 0, tly: 0,
  ox: 0, oy: 0, tox: 0, toy: 0,
  blink: 0, tblink: 0,
  mouth: 0.62, tmouth: 0.62,
  stretch: 1, tstretch: 1,
  blush: 0, tblush: 0,
  touch: false,
  t: 0,
  nextBlink: 1600
};

function oval(x, y, rx, ry, color, a) {
  ctx.fillStyle = color;
  ctx.globalAlpha = a == null ? 1 : a;
  ctx.beginPath();
  ctx.ellipse(x, y, rx, ry, 0, 0, Math.PI * 2);
  ctx.fill();
  ctx.globalAlpha = 1;
}

function draw() {
  const cx = W * 0.5 + f.ox;
  const cy = H * 0.54 + f.oy;
  const s = Math.min(W, H) * 0.38 * f.stretch;
  const rx = s * 0.9;
  const ry = s;
  ctx.clearRect(0, 0, W, H);

  oval(cx, cy + ry * 0.08, rx * 1.02, ry * 0.95, '#c9a24a', 0.35);
  oval(cx, cy, rx, ry, '#ffd56a', 1);

  if (f.blush > 0.05) {
    oval(cx - rx * 0.52, cy + ry * 0.18, rx * 0.18, ry * 0.1, '#ff8aa0', f.blush * 0.55);
    oval(cx + rx * 0.52, cy + ry * 0.18, rx * 0.18, ry * 0.1, '#ff8aa0', f.blush * 0.55);
  }

  const eyeY = cy - ry * 0.12;
  const eyeDX = rx * 0.38;
  const ew = rx * 0.2;
  const eh = Math.max(3, ry * 0.15 * (1 - f.blink));
  const pl = ew * 0.38;
  const lookPx = f.lx * ew * 0.42;
  const lookPy = f.ly * eh * 0.4;

  function eye(x) {
    oval(x, eyeY, ew, eh, '#ffffff', 1);
    if (f.blink < 0.85) {
      oval(x + lookPx, eyeY + lookPy, pl, pl * 1.05, '#2b2b33', 1);
      oval(x + lookPx + pl * 0.28, eyeY + lookPy - pl * 0.3, pl * 0.28, pl * 0.28, '#ffffff', 0.9);
    }
  }
  eye(cx - eyeDX);
  eye(cx + eyeDX);

  const mw = rx * (0.34 + f.mouth * 0.16);
  const mh = ry * (0.08 + f.mouth * 0.18);
  const my = cy + ry * 0.38;
  ctx.strokeStyle = '#c45c48';
  ctx.lineWidth = Math.max(6, s * 0.045);
  ctx.beginPath();
  ctx.moveTo(cx - mw, my);
  ctx.quadraticCurveTo(cx, my + mh, cx + mw, my);
  ctx.stroke();
}

function tick() {
  f.t += 16;
  f.lx = lerp(f.lx, f.tlx, 0.22);
  f.ly = lerp(f.ly, f.tly, 0.22);
  f.ox = lerp(f.ox, f.tox, 0.18);
  f.oy = lerp(f.oy, f.toy, 0.18);
  f.mouth = lerp(f.mouth, f.tmouth, 0.16);
  f.stretch = lerp(f.stretch, f.tstretch, 0.12);
  f.blush = lerp(f.blush, f.tblush, 0.12);
  f.blink = lerp(f.blink, f.tblink, 0.4);
  if (f.blink > 0.92) f.tblink = 0;
  if (f.t >= f.nextBlink && !f.touch) {
    f.tblink = 1;
    f.nextBlink = f.t + 1800 + Math.floor(Math.random() * 2200);
  }
  if (!f.touch) {
    f.tlx *= 0.9;
    f.tly *= 0.9;
    f.tox *= 0.82;
    f.toy *= 0.82;
    f.tstretch = lerp(f.tstretch, 1, 0.08);
    f.tmouth = lerp(f.tmouth, 0.62, 0.06);
    f.tblush *= 0.94;
  }
  draw();
}

ui.onTouch(function (e) {
  const cx = W * 0.5;
  const cy = H * 0.54;
  if (e.type === 'end') {
    f.touch = false;
    return;
  }
  f.touch = true;
  ui.setHidden(hint, 1);
  const dx = e.x - cx;
  const dy = e.y - cy;
  f.tlx = clamp(dx / (W * 0.32), -1, 1);
  f.tly = clamp(dy / (H * 0.32), -1, 1);
  f.tox = f.tlx * 16;
  f.toy = f.tly * 10;
  f.tmouth = clamp(0.4 + dy / H, 0.2, 0.95);
  f.tstretch = clamp(1 + dx / W * 0.12, 0.94, 1.1);
  f.tblush = 0.85;
});

setInterval(tick, 16);
draw();
