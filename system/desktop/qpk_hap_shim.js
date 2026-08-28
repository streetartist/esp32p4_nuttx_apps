/* HAP / UX page runtime for original RPK webpack bundles.
 * Loaded as a global script. Do not define `window`.
 */
(function () {
  if (typeof Object.assign !== 'function') {
    Object.assign = function (t) {
      for (var i = 1; i < arguments.length; i++) {
        var s = arguments[i];
        if (!s) continue;
        for (var k in s) if (Object.prototype.hasOwnProperty.call(s, k)) t[k] = s[k];
      }
      return t;
    };
  }

  function translateStyle(value) {
    if (typeof value !== 'string') return value || {};
    var out = {};
    var parts = value.split(';');
    for (var i = 0; i < parts.length; i++) {
      var item = parts[i] ? String(parts[i]).replace(/^\s+|\s+$/g, '') : '';
      if (!item) continue;
      var c = item.indexOf(':');
      if (c < 0) continue;
      var key = item.slice(0, c).replace(/^\s+|\s+$/g, '').replace(/-([a-z])/g, function (_, m) {
        return m.toUpperCase();
      });
      out[key] = item.slice(c + 1).replace(/^\s+|\s+$/g, '');
    }
    return out;
  }

  globalThis.$translateStyle$ = translateStyle;

  (function () {
    if (!globalThis.localStorage || typeof localStorage.setItem !== 'function') return;
    var orig = localStorage.setItem.bind(localStorage);
    var pending = {};
    var timer = 0;
    localStorage.setItem = function (key, value) {
      pending[String(key)] = String(value == null ? '' : value);
      if (timer) return;
      timer = setTimeout(function () {
        var k;
        timer = 0;
        for (k in pending) {
          try { orig(k, pending[k]); } catch (e) {}
        }
        pending = {};
      }, 250);
    };
  })();

  var modules = {};
  var currentVm = null;
  var inst = [];
  var pendingName = null;
  var designW = 1280;
  var designH = 720;
  var kx = 1;
  var ky = 1;
  var viewW = 1280;
  var viewH = 720;
  var viewScale = 1;
  var viewOx = 0;
  var viewOy = 0;

  var $app = { $def: {} };
  globalThis.$app = $app;

  function shortName(name) {
    var i = String(name).lastIndexOf('/');
    return i >= 0 ? name.slice(i + 1) : name;
  }

  function unwrap(exp) {
    if (exp && exp.__esModule && exp.default) return exp.default;
    return exp;
  }

  function storageKey(group, item) {
    var pkg = '';
    try {
      pkg = ($app.$def && $app.$def.manifest && $app.$def.manifest.package) ||
            ($app.$def && $app.$def.package) || '';
    } catch (e) {}
    pkg = String(pkg || 'hap').replace(/[^A-Za-z0-9._-]+/g, '_');
    return pkg + '_' + group + '_' + item;
  }

  function patchUtil(u) {
    if (!u || typeof u !== 'object' || u.__hapPatched) return u;
    u.__hapPatched = true;
    if (typeof u.writeINI !== 'function') {
      u.writeINI = function (group, item, val) {
        try { localStorage.setItem(storageKey(group, item), String(val)); } catch (e) {}
        if (group === 'options' && typeof u.setConfig === 'function') {
          u.setConfig(item, val);
        }
      };
    }
    if (!u.GV && typeof Proxy === 'function') {
      u.GV = new Proxy({}, {
        get: function (_, key) {
          if (typeof key !== 'string') return undefined;
          try {
            var raw = localStorage.getItem(storageKey('options', key));
            if (raw === null || raw === undefined || raw === '') return 0;
            var n = parseInt(raw, 10);
            return isNaN(n) ? raw : n;
          } catch (e) {
            return 0;
          }
        },
        set: function (_, key, v) {
          if (typeof key === 'string') u.writeINI('options', key, v);
          return true;
        }
      });
    }
    return u;
  }

  function instantiate(name) {
    var rec = modules[name];
    if (!rec) return {};
    if (rec.ready) return rec.exports;
    var module = { exports: {} };
    var factory = rec.factory;
    if (typeof factory === 'function') {
      factory($app_require$, module.exports, module);
    } else if (factory && typeof factory === 'object') {
      module.exports = factory;
    }
    rec.exports = module.exports;
    rec.ready = true;
    return rec.exports;
  }

  function $app_require$(name) {
    if (name === '@app-module/system.router' || name === '@system.router') {
      return { default: system.router, __esModule: true };
    }
    if (name === '@app-module/system.battery' || name === '@system.battery') {
      return { default: system.battery, __esModule: true };
    }
    if (name === '@app-module/system.storage' || name === '@system.storage') {
      return { default: system.storage, __esModule: true };
    }
    if (modules[name]) return instantiate(name);
    return {};
  }

  globalThis.$app_require$ = $app_require$;
  globalThis.$app_define_wrap$ = function () {};

  globalThis.$app_define$ = function (name, deps, factory) {
    modules[name] = { factory: factory, ready: false, exports: {} };
    var exp = instantiate(name);
    var short = shortName(name);
    if (String(name).indexOf('@app-component/') === 0) {
      var value = unwrap(exp);
      if (short === 'util') value = patchUtil(value);
      if (value && typeof value === 'object' && !value.template) {
        $app.$def[short] = value;
      }
    }
    if (String(name).indexOf('@app-application/') === 0) {
      var appExp = unwrap(exp) || {};
      if (appExp.manifest) $app.$def.manifest = appExp.manifest;
      if (appExp.util) $app.$def.util = patchUtil(appExp.util);
    }
  };

  function px(v, axis, parentLen) {
    if (v == null || v === '') return null;
    if (typeof v === 'number') return Math.round(v * (axis === 'x' ? kx : ky));
    var s = String(v).replace(/^\s+|\s+$/g, '');
    if (!s || s === 'undefined' || s === 'null' || s.indexOf('undefined') >= 0) {
      return null;
    }
    if (s.charAt(s.length - 1) === '%') {
      var p = parseFloat(s);
      if (isNaN(p)) return null;
      var base = parentLen != null ? parentLen : (axis === 'x' ? viewW : viewH);
      return Math.round(base * p / 100);
    }
    var n = parseFloat(s);
    if (isNaN(n)) return null;
    return Math.round(n * (axis === 'x' ? kx : ky));
  }

  function parseColor(v) {
    if (v == null || v === '') return 0;
    if (typeof v === 'number') return v >>> 0;
    var s = String(v).replace(/^\s+|\s+$/g, '');
    if (s.charAt(0) === '#') {
      var h = s.slice(1);
      if (h.length === 3) h = h.charAt(0) + h.charAt(0) + h.charAt(1) + h.charAt(1) + h.charAt(2) + h.charAt(2);
      return parseInt(h, 16) || 0;
    }
    var m = s.match(/rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)/);
    if (m) return ((parseInt(m[1], 10) << 16) | (parseInt(m[2], 10) << 8) | parseInt(m[3], 10)) >>> 0;
    return 0;
  }

  function parseOpa(st) {
    if (st.opacity == null) return 255;
    var n = parseFloat(st.opacity);
    if (isNaN(n)) return 255;
    if (n <= 1) return Math.round(n * 255);
    return Math.max(0, Math.min(255, Math.round(n)));
  }

  function classNames(node) {
    var list = node && node.classList;
    if (!list) return [];
    if (typeof list === 'string') return list.replace(/^\s+|\s+$/g, '').split(/\s+/);
    return list;
  }

  function mergeStyle(node) {
    var st = {};
    var sheet = (currentVm && currentVm.$def && currentVm.$def.style) || {};
    var list = classNames(node);
    for (var i = 0; i < list.length; i++) {
      var key = list[i];
      if (!key) continue;
      var part = sheet[key] || sheet['.' + key];
      if (part) Object.assign(st, part);
    }
    var inline = node.style;
    if (typeof inline === 'function') {
      try { inline = inline.call(currentVm); } catch (e) { inline = null; }
    }
    if (typeof inline === 'string') inline = translateStyle(inline);
    if (inline && typeof inline === 'object') {
      var sk;
      for (sk in inline) {
        if (!Object.prototype.hasOwnProperty.call(inline, sk)) continue;
        var sv = inline[sk];
        if (typeof sv === 'function') {
          try { sv = sv.call(currentVm); } catch (e) { sv = undefined; }
        }
        if (sv === undefined || sv === null || sv === '') continue;
        if (typeof sv === 'string' && sv.indexOf('undefined') >= 0) continue;
        st[sk] = sv;
      }
    }
    return st;
  }

  function attrOf(node, key) {
    var a = node.attr || {};
    var v = a[key];
    if (typeof v === 'function') {
      try { return v.call(currentVm); } catch (e) { return undefined; }
    }
    return v;
  }

  function shownOf(node) {
    if (!node.shown) return true;
    try { return !!node.shown.call(currentVm); } catch (e) { return true; }
  }

  function clampScale(n) {
    if (n !== n || n === Infinity || n === -Infinity) return 1;
    if (n < 0.05) return 0.05;
    if (n > 4) return 4;
    return n;
  }

  function applyTransform(handle, st, w, h) {
    if (!handle || !ui.setScale) return;
    var t = st.transform || '';
    var sx = 1;
    var sy = 1;
    var rot = 0;
    var m;
    if (t) {
      m = t.match(/scale\(([^)]+)\)/);
      if (m) {
        var p = m[1].split(',');
        sx = parseFloat(p[0]);
        sy = p.length > 1 ? parseFloat(p[1]) : sx;
        if (isNaN(sx)) sx = 1;
        if (isNaN(sy)) sy = 1;
      }
      m = t.match(/scaleX\(([^)]+)\)/);
      if (m) {
        sx = parseFloat(m[1]);
        if (isNaN(sx)) sx = 1;
      }
      m = t.match(/scaleY\(([^)]+)\)/);
      if (m) {
        sy = parseFloat(m[1]);
        if (isNaN(sy)) sy = 1;
      }
      m = t.match(/rotate\(([-0-9.]+)deg\)/);
      if (m) rot = parseFloat(m[1]) || 0;
    }
    sx = clampScale(sx);
    sy = clampScale(sy);
    var origin = String(st.transformOrigin || 'center center').split(/\s+/);
    var ox = Math.round(w / 2);
    var oy = Math.round(h / 2);
    if (origin[0] === 'left') ox = 0;
    else if (origin[0] === 'right') ox = w;
    if (origin[1] === 'top') oy = 0;
    else if (origin[1] === 'bottom') oy = h;
    ui.setPivot(handle, ox, oy);
    ui.setScale(handle, sx, sy);
    ui.setAngle(handle, rot);
  }

  function wrapEvt(fn) {
    return function (evt) {
      var r;
      try { r = fn.call(currentVm, evt); } catch (e) { console.log(e); }
      paint();
      return r;
    };
  }

  function childCount(node) {
    return (node.children && node.children.length) || 0;
  }

  function makeChildBox(geom, st, node) {
    var padL = px(st.paddingLeft || st.padding, 'x') || 0;
    var padT = px(st.paddingTop || st.padding, 'y') || 0;
    var padR = px(st.paddingRight || st.padding, 'x') || 0;
    var padB = px(st.paddingBottom || st.padding, 'y') || 0;
    return {
      x: geom.x,
      y: geom.y,
      w: geom.w,
      h: geom.h,
      contentW: Math.max(0, geom.w - padL - padR),
      contentH: Math.max(0, geom.h - padT - padB),
      cx: geom.x + padL,
      cy: geom.y + padT,
      dir: st.flexDirection === 'row' ? 'row' : 'column',
      justify: st.justifyContent || 'flex-start',
      childIndex: 0,
      childTotal: childCount(node),
      padL: padL,
      padT: padT
    };
  }

  function layout(node, parent) {
    var st = mergeStyle(node);
    var w = px(st.width, 'x', parent.contentW);
    var h = px(st.height, 'y', parent.contentH);
    var positioned = st.position === 'absolute' || st.position === 'fixed' ||
                     st.left != null || st.right != null ||
                     st.top != null || st.bottom != null;
    if (w == null) {
      if (positioned || node.type === 'image') w = Math.round(80 * kx);
      else w = parent.contentW;
    }
    if (h == null) {
      var fs = px(st.fontSize, 'y');
      if (fs) h = Math.round(fs * 1.5);
      else if (positioned || node.type === 'image') h = Math.round(80 * ky);
      else h = Math.round(40 * ky);
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (viewW > 0 && w > viewW) w = viewW;
    if (viewH > 0 && h > viewH) h = viewH;
    var x;
    var y;
    if (positioned) {
      if (st.left != null) x = parent.x + (px(st.left, 'x') || 0);
      else if (st.right != null) x = parent.x + parent.w - (px(st.right, 'x') || 0) - w;
      else x = parent.x;
      if (st.top != null) y = parent.y + (px(st.top, 'y') || 0);
      else if (st.bottom != null) y = parent.y + parent.h - (px(st.bottom, 'y') || 0) - h;
      else y = parent.y;
    } else {
      x = parent.cx;
      y = parent.cy;
      if (parent.dir === 'row') {
        if (parent.justify === 'space-between' && parent.childTotal > 1 &&
            parent.childIndex === parent.childTotal - 1) {
          x = parent.x + parent.padL + parent.contentW - w;
        }
        parent.cx = x + w;
      } else {
        parent.cy = y + h + (px(st.marginBottom, 'y') || 0);
      }
      parent.childIndex++;
    }
    return { x: x, y: y, w: w, h: h, st: st };
  }

  function createNode(slot, node, geom) {
    var type = node.type;
    var st = geom.st;
    var handle = 0;
    var click = node.events && node.events.click;
    if (type === 'image') {
      var src = attrOf(node, 'src') || '/common/images/0.png';
      try { handle = ui.image(src, geom.x, geom.y, geom.w, geom.h); }
      catch (e) { handle = ui.panel(geom.x, geom.y, geom.w, geom.h, 0x445566, 8, 255); }
      slot.src = src;
    } else if (type === 'text') {
      var val = attrOf(node, 'value');
      if (val == null) val = '';
      handle = ui.text(String(val), geom.x, geom.y, px(st.fontSize, 'y') || 20, parseColor(st.color || '#ffffff'));
      slot.text = String(val);
    } else if (type === 'input' && attrOf(node, 'type') === 'toggle') {
      var checked = !!attrOf(node, 'checked');
      slot.checked = checked;
      handle = ui.button(checked ? 'ON' : 'OFF', geom.x, geom.y, Math.max(geom.w, 80), Math.max(geom.h, 40), function () {
        slot.checked = !slot.checked;
        if (node.events && node.events.change) {
          wrapEvt(node.events.change)({ checked: slot.checked });
        } else paint();
      }, checked ? 0x3d9a5b : 0x4a4a5a);
    } else if (type === 'slider') {
      var value = parseFloat(attrOf(node, 'value'));
      if (isNaN(value)) value = parseFloat(attrOf(node, 'min')) || 0;
      slot.value = value;
      handle = ui.button(String(value), geom.x, geom.y, Math.max(geom.w, 120), Math.max(geom.h, 44), function () {
        var min = parseFloat(attrOf(node, 'min'));
        var max = parseFloat(attrOf(node, 'max'));
        if (isNaN(min)) min = 0;
        if (isNaN(max)) max = 100;
        var step = Math.max(1, Math.round((max - min) / 10));
        slot.value += step;
        if (slot.value > max) slot.value = min;
        if (node.events && node.events.change) {
          wrapEvt(node.events.change)({ value: slot.value });
        } else paint();
      }, 0x3949ab);
    } else if (type === 'div' && click) {
      var label = '';
      if (node.children && node.children[0] && node.children[0].type === 'text') {
        label = attrOf(node.children[0], 'value') || '';
      }
      if (st.backgroundColor || label) {
        handle = ui.button(String(label), geom.x, geom.y, geom.w, geom.h, wrapEvt(click), parseColor(st.backgroundColor || '#3a3a50'));
      } else {
        handle = ui.panel(geom.x, geom.y, geom.w, geom.h, 0x000000, 0, 0);
        try { ui.onClick(handle, wrapEvt(click)); } catch (e) {}
      }
      slot.skipChildren = true;
    } else if (type === 'div' && st.backgroundColor) {
      handle = ui.panel(geom.x, geom.y, geom.w, geom.h, parseColor(st.backgroundColor), px(st.borderRadius, 'x') || 0, parseOpa(st));
    }
    if (handle && click && type !== 'div') {
      try { ui.onClick(handle, wrapEvt(click)); } catch (e) {}
    }
    if (node.events && (node.events.touchstart || node.events.touchmove || node.events.touchend)) {
      bindTouch(node);
    }
    slot.handle = handle;
    slot.type = type;
    slot.node = node;
  }

  function toDesignX(px) {
    var scale = viewScale || 1;
    var x = (px - viewOx) / scale;
    if (x < 0) x = 0;
    if (x > designW) x = designW;
    return x;
  }

  function toDesignY(px) {
    var scale = viewScale || 1;
    var y = (px - viewOy) / scale;
    if (y < 0) y = 0;
    if (y > designH) y = designH;
    return y;
  }

  function bindTouch(node) {
    ui.onTouch(function (e) {
      var x = toDesignX(e.x);
      var y = toDesignY(e.y);
      var t = {
        clientX: x, pageX: x, globalX: x,
        clientY: y, pageY: y, globalY: y
      };
      var evt = { touches: [t], changedTouches: [t], type: e.type };
      try {
        if (e.type === 'start' && node.events.touchstart) node.events.touchstart.call(currentVm, evt);
        else if (e.type === 'move' && node.events.touchmove) node.events.touchmove.call(currentVm, evt);
        else if (e.type === 'end' && node.events.touchend) node.events.touchend.call(currentVm, evt);
      } catch (err) { console.log(err); }
      dirtyPaint();
    });
  }

  function updateNode(slot, node, geom, visible) {
    var handle = slot.handle;
    if (!handle) return;
    if (ui.setHidden) ui.setHidden(handle, visible ? 0 : 1);
    if (!visible) return;
    if (slot.x !== geom.x || slot.y !== geom.y) {
      ui.setPos(handle, geom.x, geom.y);
      slot.x = geom.x;
      slot.y = geom.y;
    }
    if (ui.setSize && slot.type !== 'text' &&
        (slot.w !== geom.w || slot.h !== geom.h)) {
      ui.setSize(handle, Math.max(1, geom.w), Math.max(1, geom.h));
      slot.w = geom.w;
      slot.h = geom.h;
    }
    applyTransform(handle, geom.st, geom.w, geom.h);
    if (slot.type === 'image') {
      var src = attrOf(node, 'src');
      if (src && src !== slot.src) {
        try { ui.setImage(handle, src); slot.src = src; } catch (e) {}
      }
      if (ui.setOpa) ui.setOpa(handle, parseOpa(geom.st));
    } else if (slot.type === 'text') {
      var val = attrOf(node, 'value');
      if (val == null) val = '';
      val = String(val);
      if (val !== slot.text) {
        ui.setText(handle, val);
        slot.text = val;
      }
    } else if (slot.type === 'input') {
      var checked = !!attrOf(node, 'checked');
      if (checked !== slot.checked) {
        slot.checked = checked;
        ui.setText(handle, checked ? 'ON' : 'OFF');
        if (ui.setColor) ui.setColor(handle, checked ? 0x3d9a5b : 0x4a4a5a);
      }
    } else if (slot.type === 'slider') {
      var value = parseFloat(attrOf(node, 'value'));
      if (!isNaN(value) && value !== slot.value) {
        slot.value = value;
        ui.setText(handle, String(value));
      }
    }
  }

  var walkIndex = 0;

  function walk(node, parent) {
    if (!node) return;
    var geom = layout(node, parent);
    var visible = shownOf(node);
    var slot = inst[walkIndex];
    if (!slot) {
      slot = inst[walkIndex] = {};
      if (visible || node.type !== 'image') createNode(slot, node, geom);
      else slot.deferred = true;
    } else if (visible && slot.deferred) {
      createNode(slot, node, geom);
      slot.deferred = false;
    }
    updateNode(slot, node, geom, visible);
    walkIndex++;
    if (slot.skipChildren) return;
    var childBox = makeChildBox(geom, geom.st, node);
    var kids = node.children || [];
    for (var i = 0; i < kids.length; i++) walk(kids[i], childBox);
  }

  function paint() {
    if (!currentVm || !currentVm.$def || !currentVm.$def.template) return;
    var size = ui.getSize();
    viewW = size.width || 0;
    viewH = size.height || 0;
    if (viewW < 64) viewW = designW;
    if (viewH < 64) viewH = designH;
    var sx = viewW / designW;
    var sy = viewH / designH;
    var scale = sx < sy ? sx : sy;
    if (scale <= 0) scale = 1;
    kx = scale;
    ky = scale;
    var usedW = Math.round(designW * scale);
    var usedH = Math.round(designH * scale);
    var ox = Math.floor((viewW - usedW) / 2);
    var oy = Math.floor((viewH - usedH) / 2);
    viewScale = scale;
    viewOx = ox;
    viewOy = oy;
    walkIndex = 0;
    walk(currentVm.$def.template, {
      x: ox, y: oy, w: usedW, h: usedH,
      contentW: usedW, contentH: usedH,
      cx: ox, cy: oy, dir: 'column', justify: 'flex-start',
      childIndex: 0, childTotal: 1, padL: 0, padT: 0
    });
  }

  var paintPending = false;
  var suspendPaint = false;

  function dirtyPaint() {
    if (suspendPaint || paintPending || !currentVm) return;
    paintPending = true;
    if (typeof setTimeout === 'function') {
      setTimeout(function () {
        paintPending = false;
        if (currentVm) paint();
      }, 16);
    } else {
      paintPending = false;
      paint();
    }
  }

  function collectData(def) {
    var data = {};
    function add(obj) {
      var k;
      if (!obj || typeof obj !== 'object') return;
      for (k in obj) {
        if (!Object.prototype.hasOwnProperty.call(obj, k)) continue;
        if (typeof obj[k] === 'function') continue;
        if (data[k] === undefined) data[k] = obj[k];
      }
    }
    add(def.data);
    add(def.private);
    add(def.public);
    add(def.protected);
    return data;
  }

  function wrapMethod(fn) {
    return function () {
      var r = fn.apply(this, arguments);
      dirtyPaint();
      return r;
    };
  }

  function createVm(def) {
    var vm = {};
    var data = collectData(def);
    var k;
    for (k in data) vm[k] = data[k];
    for (k in def) {
      if (k === 'private' || k === 'public' || k === 'protected' ||
          k === 'data' || k === 'template' || k === 'style' || k === '_descriptor') {
        continue;
      }
      if (typeof def[k] === 'function') vm[k] = wrapMethod(def[k]);
      else if (vm[k] === undefined) vm[k] = def[k];
    }
    vm.$app = $app;
    vm.$def = def;
    return vm;
  }

  function adoptDesign(w, h) {
    if (w && String(w).indexOf('%') < 0) {
      var nw = parseFloat(w);
      if (nw >= 64) designW = nw;
    }
    if (h && String(h).indexOf('%') < 0) {
      var nh = parseFloat(h);
      if (nh >= 64) designH = nh;
    }
  }

  function mount(def) {
    designW = 1280;
    designH = 720;
    paintPending = false;
    try {
      var man = $app.$def && $app.$def.manifest;
      if (man) adoptDesign(man.designWidth, man.designHeight);
    } catch (e) {}
    if (def.style && def.style['.stage']) {
      adoptDesign(def.style['.stage'].width, def.style['.stage'].height);
    }
    ui.background(parseColor((def.style && def.style['.stage'] &&
      def.style['.stage'].backgroundColor) || '#000000'));
    currentVm = createVm(def);
    suspendPaint = true;
    if (typeof currentVm.onInit === 'function') {
      try { currentVm.onInit(); } catch (e) { console.log(e); }
    }
    currentVm.viewportWidth = designW;
    suspendPaint = false;
    paint();
    try {
      if (system.battery && $app.$def.util && typeof $app.$def.util.onBatteryStatus === 'function') {
        var st = system.battery.getStatus();
        $app.$def.util.onBatteryStatus(!!(st && st.charging));
      }
    } catch (e) {}
  }

  globalThis.$app_bootstrap$ = function (name) {
    if (String(name).indexOf('@app-application/') === 0) {
      instantiate(name);
      return;
    }
    pendingName = name;
  };

  globalThis.$hap_mount$ = function () {
    var name = pendingName;
    pendingName = null;
    if (!name) {
      for (var k in modules) {
        if (String(k).indexOf('@app-component/') !== 0) continue;
        if (shortName(k) === 'util') continue;
        var probe = unwrap(instantiate(k));
        if (probe && probe.template) {
          name = k;
          break;
        }
      }
    }
    if (!name) {
      console.log('HAP bootstrap: no page');
      return;
    }
    var exp = instantiate(name);
    var def = unwrap(exp);
    if (!def || !def.template) {
      console.log('HAP bootstrap: no template for ' + name);
      return;
    }
    mount(def);
  };

  globalThis.$hap_teardown$ = function () {
    if (currentVm && typeof currentVm.onDestroy === 'function') {
      try { currentVm.onDestroy(); } catch (e) {}
    }
    currentVm = null;
    inst = [];
    modules = {};
    pendingName = null;
    paintPending = false;
    designW = 1280;
    designH = 720;
    $app.$def = {};
    if (ui.clear) ui.clear();
  };
})();
