// PS5 Mango — i18n runtime (loaded in <head>, before <body> is parsed)
// Responsibilities:
//   1. Detect the host/browser default language and pick the UI language.
//   2. Expose window.MANGO_LANG + t(key, fallback) + applyI18n(root).
//   3. On DOMContentLoaded, swap every [data-i18n] / [data-i18n-attr] node.
//
// Language rule (per project requirement):
//   - Any Chinese variant (zh / zh-CN / zh-TW / zh-HK / zh-Hans / zh-Hant / …)
//     → Simplified Chinese ("zh-CN").
//   - Every other language → English ("en").
//   - A manual override stored in localStorage["mango-lang"] (set by a future
//     language switcher) wins over auto-detection.
//
// Safety: every lookup falls back to the original Chinese text (either the
// `fallback` argument passed to t(), or the node's existing content for
// [data-i18n]). An untranslated key NEVER blanks or breaks the UI.

(function () {
  "use strict";

  var STORE_KEY = "mango-lang";

  function resolveLang() {
    try {
      var ov = localStorage.getItem(STORE_KEY);
      if (ov === "zh-CN" || ov === "en") return ov;
    } catch (e) { /* localStorage may be unavailable (private mode, file://) */ }

    var langs = navigator.languages;
    if (!langs || !langs.length) {
      langs = [navigator.language || navigator.userLanguage || "en"];
    }
    for (var i = 0; i < langs.length; i++) {
      var l = (langs[i] || "").toLowerCase();
      // Any Chinese language tag starts with "zh".
      if (l.indexOf("zh") === 0) return "zh-CN";
    }
    return "en";
  }

  function dotGet(pack, key) {
    if (!pack) return undefined;
    if (Object.prototype.hasOwnProperty.call(pack, key)) return pack[key];
    // support nested keys like "nav.files"
    var parts = key.split(".");
    var cur = pack;
    for (var i = 0; i < parts.length; i++) {
      if (cur == null || typeof cur !== "object") return undefined;
      cur = cur[parts[i]];
    }
    return cur;
  }

  var LANG = resolveLang();
  window.MANGO_LANG = LANG;
  // Set as early as possible so the <body> is parsed with the right lang (no flash).
  document.documentElement.lang = (LANG === "zh-CN") ? "zh-CN" : "en";

  window.MANGO_I18N = window.MANGO_I18N || {};

  // Translate a key; fall back to `fallback` (usually the original Chinese
  // string) so the UI degrades to Chinese rather than breaking.
  window.t = function (key, fallback) {
    var pack = window.MANGO_I18N[LANG];
    var v = dotGet(pack, key);
    if (v !== undefined && v !== null) return v;
    if (fallback !== undefined && fallback !== null) return fallback;
    return key;
  };

  function applyI18n(root) {
    root = root || document;
    var nodes = root.querySelectorAll("[data-i18n]");
    for (var i = 0; i < nodes.length; i++) {
      var el = nodes[i];
      var key = el.getAttribute("data-i18n");
      var v = dotGet(window.MANGO_I18N[LANG], key);
      if (v !== undefined && v !== null) el.textContent = v; // else keep original
    }
    var attrNodes = root.querySelectorAll("[data-i18n-attr]");
    for (var j = 0; j < attrNodes.length; j++) {
      var ael = attrNodes[j];
      var spec = ael.getAttribute("data-i18n-attr");
      if (!spec) continue; // defensive: only nodes with the attribute are selected
      var pairs = spec.split(";");
      for (var k = 0; k < pairs.length; k++) {
        var kv = pairs[k].split(":");
        if (kv.length !== 2) continue;
        var attr = kv[0].trim();
        var akey = kv[1].trim();
        var av = dotGet(window.MANGO_I18N[LANG], akey);
        if (av !== undefined && av !== null) ael.setAttribute(attr, av);
      }
    }
  }
  window.applyI18n = applyI18n;

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", function () { applyI18n(document); });
  } else {
    applyI18n(document);
  }
})();
