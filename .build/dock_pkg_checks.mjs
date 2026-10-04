/* 三处修复的真实源码断言（vm + 最小 DOM 桩，Playwright 环境坏了走不了浏览器）：
   ① renderJob 的 delScrimExempt 豁免：存档删除任务不得弹 #scrimDel 进度框
   ② ppCleanupWhenDone：done + "already installed — skipped" ⇒ 保留暂存（不调 /api/delete）
   纪律：每条断言都要有反证（counterfactual）—— 改坏源码必须变红。 */
import fs from "node:fs";
import vm from "node:vm";

const src = fs.readFileSync("assets/index.html", "utf8");
let fails = 0;
const ok = (cond, name) => { console.log((cond ? "  ✓ " : "  ✗ ") + name); if (!cond) fails++; };

const cut = (re, name) => {
  const m = src.match(re);
  if (!m) { console.error("FAIL: 找不到 " + name); process.exit(1); }
  return m[0];
};

/* ---------- ① renderJob 豁免 ---------- */
console.log("【1】renderJob 尊重 delScrimExempt（存档删除不弹删除进度框）");
{
  const code = cut(/const delScrimExempt = new Set\(\);[\s\S]*?function renderJob\(tasks\)\s*\{[\s\S]*?\n\}/,
                   "delScrimExempt + renderJob");
  const mk = (mutate) => {
    const bar = { classes: new Set(),
      classList: { add: c => bar.classes.add(c), contains: c => bar.classes.has(c),
                   remove: c => bar.classes.delete(c) } };
    const el = () => ({ textContent: "", style: {}, dataset: {},
                        classList: { add() {}, remove() {}, contains: () => false } });
    const node = () => { const n = el(); n.parentElement = n; return n; };
    const ctx = {
      document: { getElementById: id => (id === "scrimDel" ? bar : node()) },
      isCountKind: k => k === 7,
      pctOf: () => 50,
      delArmed: false,
      fmtCount: n => String(n), decodeFsText: s => s, tailPath: s => s,
      fmtSpeed: () => "—", fmtDur: () => "0:00", taskElapsedMs: () => 0,
      fsClearSel() {}, fsView: null, toast() {},
      setInterval: () => 0, clearInterval() {},
    };
    vm.createContext(ctx);
    vm.runInContext(mutate ? mutate(code) : code, ctx);
    return { bar, ctx,
      add: id => vm.runInContext(`delScrimExempt.add(${id})`, ctx),
      run: tasks => vm.runInContext(`renderJob(${JSON.stringify(tasks)})`, ctx) };
  };
  const task = id => ({ id, kind: 7, state_name: "running", progress: 1, total: 2,
                        created_at: 1, label: "PPSA16608" });

  const a = mk();
  a.run([task(5)]);
  ok(a.bar.classes.has("on"), "非豁免的删除任务照旧弹进度框（控件本身没坏）");

  const b = mk();
  b.add(5);
  b.run([task(5)]);
  ok(!b.bar.classes.has("on"), "豁免后同样的任务不弹进度框");

  /* 反证：摘掉过滤（模拟回归）—— 豁免必须失效、进度框必须弹出来 */
  const c = mk(code => code.replace("!delScrimExempt.has(Number(t.id))", "true"));
  c.add(5);
  c.run([task(5)]);
  ok(c.bar.classes.has("on"), "反证：过滤被摘掉 ⇒ 豁免失效、进度框弹出（断言能红）");
}

/* ---------- ② ppCleanupWhenDone：任何终态都不再自动删暂存 ---------- */
console.log("【2】ppCleanupWhenDone：所有终态都保留暂存（不调 /api/delete）");
{
  const code = cut(/function ppCleanupWhenDone\(taskId, path\)\s*\{[\s\S]*?\n\}/,
                   "ppCleanupWhenDone");
  const mk = (pollResp, mutate) => {
    const calls = [];
    const ctx = {
      PKG_STAGING: "/data/wfm/incoming",
      setInterval: (fn) => { fn(); return 1; },   // 立即跑第一轮
      clearInterval() {},
      apiPost: async (url, body) => {
        calls.push({ url, body });
        return url === "/api/install/poll" ? pollResp : { ok: true };
      },
      toast: m => calls.push({ url: "@toast", body: m }),
    };
    vm.createContext(ctx);
    vm.runInContext(mutate ? mutate(code) : code, ctx);
    return { calls,
      run: (id, path) => vm.runInContext(`ppCleanupWhenDone(${id}, ${JSON.stringify(path)})`, ctx) };
  };

  (async () => {
    const a = mk({ ok: true, state_name: "done",
                   note: "already installed (CUSA45599) — skipped" });
    a.run(9, "/data/wfm/incoming/x.pkg");
    await new Promise(r => setImmediate(r));
    ok(!a.calls.some(c => c.url === "/api/delete"),
       "skipped 的 done：不删暂存");
    ok(a.calls.some(c => c.url === "@toast" && /跳过/.test(c.body)),
       "skipped 的 done：给用户明说（toast 提到跳过）");

    /* 2026-10-03 契约反转：真装完的 done 也不删 —— 两轮真机反馈「传的包自己
       消失了」，自动清理省的空间换不回信任；文件页可见、用户自己删。 */
    const b = mk({ ok: true, state_name: "done", note: "" });
    b.run(9, "/data/wfm/incoming/x.pkg");
    await new Promise(r => setImmediate(r));
    ok(!b.calls.some(c => c.url === "/api/delete"),
       "真装完的 done：也不删暂存（保留在 /data/wfm/incoming）");

    /* 反证：把自动删除塞回 done 分支 —— 断言必须打红（证明它有牙）。
       ⚠️ index.html 是 CRLF 行尾（core.autocrlf），替换必须 \r?\n 兼容。 */
    const c = mk({ ok: true, state_name: "done", note: "" },
                 code => code.replace(/clearInterval\(t\);\r?\n      if \(\/already/,
                    'clearInterval(t);\n      await apiPost("/api/delete", { paths: [path] });\n      if (/already'));
    c.run(9, "/data/wfm/incoming/x.pkg");
    await new Promise(r => setImmediate(r));
    ok(c.calls.some(c2 => c2.url === "/api/delete"),
       "反证：自动删除塞回 ⇒ /api/delete 被调用（断言能红）");
  })();
}

/* ---------- ③ ZIP 导入两步走 + 扫光挂填充条 i::after（静态源码断言） ---------- */
console.log("【3】导入走流式通道 + 扫光挂在填充条 i::after");
{
  /* 导入两步：流式上传到暂存 → /api/save_mgr/import_zip 从盘上读。
     一步式 POST /api/save_mgr/upload 的直传必须从导入按钮里消失 ——
     那条路被 4MB body 上限封死（真机 "request body is too large"）。 */
  ok(/function smZipStreamUpload/.test(src) &&
     /save_mgr\/import_zip/.test(src) &&
     !/saveMgrRawPost\("\/api\/save_mgr\/upload\?"/.test(src),
     "ZIP 导入：先流式上传、再 import_zip；一步式直传已移除");

  /* 扫光挂在填充条 i::after（百分比 40% 宽，随 i 缩放，永不比进度条长）；
     轨道版（.track.pulse::after）必须不存在。 */
  ok(/\.track\.pulse i,\s*\.row \.bar\[data-p\] i\{/.test(src) &&
     /\.track\.pulse i::after[^}]*width:40%/.test(src) &&
     !/\.track\.pulse::after,\s*\.row \.bar\[data-p\]::after/.test(src),
     "扫光挂在填充条 i::after（width:40%，轨道 ::after 旧实现已移除）");

  /* 反证：把填充条选择器改回轨道 ::after —— 正向标记消失、断言打红 */
  ok((() => {
    const bad = src.replace(/\.track\.pulse i,\s*\.row \.bar\[data-p\] i\{/,
                            ".track.pulse::after,.row .bar[data-p]::after{");
    return bad !== src &&
           /\.track\.pulse::after,\s*\.row \.bar\[data-p\]::after/.test(bad) &&
           !/\.track\.pulse i,\s*\.row \.bar\[data-p\] i\{/.test(bad);
  })(), "反证：填充条选择器改成轨道 ::after ⇒ 正向标记消失（断言能红）");
}

/* ---------- ④ 库扫描：大小写双拼写 + 暂存目录；小写去重 + 不可读根提示 ---------- */
console.log("【4】库扫描根与去重（/data/pkg 大小写、暂存目录、unreadable 提示）");
let section4Done = false;
{
  const roots = cut(/const LIB_SCAN_ROOTS = \[[\s\S]*?\];/, "LIB_SCAN_ROOTS");
  ok(/"\/data\/pkg",\s*"\/data\/PKG"/.test(roots) &&
     /"\/data\/wfm\/incoming"/.test(roots),
     "扫描根含 /data/pkg 与 /data/PKG 双拼写 + 暂存目录 /data/wfm/incoming");

  const code = roots + "\n" +
               cut(/async function renderLibrary\(\)\s*\{[\s\S]*?\n\}/, "renderLibrary");
  const mk = (responses, mutate) => {
    const box = { innerHTML: "" };
    const count = { textContent: "" };
    const ctx = {
      LIVE: true,
      document: { getElementById: id => id === "libCovers" ? box
                                        : id === "libCount" ? count : null },
      apiPost: async (url, body) =>
        responses[body.root] || { ok: true, packages: [], unreadable: [] },
      pcCatalog: [],
      libCard: p => `<card path="${p.path}">`,
      esc: s => s,
    };
    vm.createContext(ctx);
    vm.runInContext(mutate ? mutate(code) : code, ctx);
    return { box, count,
             run: () => vm.runInContext("renderLibrary()", ctx) };
  };
  const cards = b => (b.innerHTML.match(/<card /g) || []).length;

  (async () => {
    /* 大小写不敏感 FS：两个拼写扫出同一批文件（路径拼写不同）⇒ 只出一张卡 */
    const a = mk({
      "/data/pkg": { ok: true, packages: [{ path: "/data/pkg/a.pkg" }], unreadable: [] },
      "/data/PKG": { ok: true, packages: [{ path: "/data/PKG/a.pkg" }], unreadable: [] },
    });
    a.run();
    await new Promise(r => setImmediate(r));
    ok(cards(a.box) === 1,
       "大小写不敏感 FS：/data/pkg 与 /data/PKG 的同一文件只出一张卡");

    /* 大小写敏感 FS：用户目录是大写 PKG ⇒ 大写根必须扫得到 */
    const b = mk({
      "/data/pkg": { ok: true, packages: [], unreadable: ["/data/pkg"] },
      "/data/PKG": { ok: true, packages: [{ path: "/data/PKG/b.pkg" }], unreadable: [] },
    });
    b.run();
    await new Promise(r => setImmediate(r));
    ok(/\/data\/PKG\/b\.pkg/.test(b.box.innerHTML),
       "大小写敏感 FS：大写 /data/PKG 根能扫到包");

    /* 全空：不可读根要写进空态提示，且 USB 缺省不算（噪音过滤） */
    const c = mk({
      "/data/pkg": { ok: true, packages: [], unreadable: ["/data/pkg", "/mnt/usb0"] },
    });
    c.run();
    await new Promise(r => setImmediate(r));
    ok(/打不开的目录/.test(c.box.innerHTML) &&
       /\/data\/pkg/.test(c.box.innerHTML) &&
       !/\/mnt\/usb0/.test(c.box.innerHTML),
       "空态提示列出打不开的目录（/mnt/usb0 缺省被过滤）");

    /* 反证：去重键退回不用小写 ⇒ 双拼写双卡，断言能红 */
    const d = mk({
      "/data/pkg": { ok: true, packages: [{ path: "/data/pkg/a.pkg" }], unreadable: [] },
      "/data/PKG": { ok: true, packages: [{ path: "/data/PKG/a.pkg" }], unreadable: [] },
    }, code => code.replace(
      /const key = String\(p && p\.path \|\| ""\)\.toLowerCase\(\);/,
      'const key = String(p && p.path || "");'));
    d.run();
    await new Promise(r => setImmediate(r));
    ok(cards(d.box) === 2,
       "反证：去重键不用小写 ⇒ 双卡出现（断言能红）");

    section4Done = true;
  })();
}

/* ---------- ⑤ 安装失败提示：toast 时长参数 + pollInstall 失败 6 秒 ---------- */
console.log("【5】安装失败提示的可读性（toast 时长、6 秒失败提示）");
{
  ok(/function toast\(msg, ms\)/.test(src) &&
     /Number\(ms\) > 0 \? Number\(ms\) : 2400/.test(src),
     "toast(msg, ms)：失败类提示可指定更长的阅读时间（默认仍 2400ms）");
  ok(/toast\(verb \+ "失败：" \+ \(t\.note \|\| nm\), 6000\)/.test(src),
     "pollInstall 失败分支：带服务端原话 + 6000ms");
  /* 反证：时长参数被摘掉 ⇒ 正向标记消失 */
  ok((() => {
    const bad = src.replace(/function toast\(msg, ms\)/, "function toast(msg)");
    return bad !== src && !/function toast\(msg, ms\)/.test(bad);
  })(), "反证：toast 的 ms 参数被摘掉 ⇒ 正向标记消失（断言能红）");
}

/* ---------- 收尾：等所有异步段落跑完再统一报告退出 ----------
   原 exit 在【2】的 IIFE 末尾；新增的异步段会被它提前 process.exit 杀死。 */
(async () => {
  for (let i = 0; i < 50 && !section4Done; i++)
    await new Promise(r => setImmediate(r));
  console.log(fails ? `\n${fails} 项失败` : "\n全部通过");
  process.exit(fails ? 1 : 0);
})();
