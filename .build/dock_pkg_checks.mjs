/* 页面逻辑的真实源码断言（vm + 最小 DOM 桩，Playwright 环境坏了走不了浏览器）：
   ① renderJob 的 delScrimExempt 豁免：存档删除任务不得弹 #scrimDel 进度框
   ② ppCleanupWhenDone：真装完的 done ⇒ 自动删暂存（/api/delete）；already
      installed / 失败 / 取消 ⇒ 保留暂存
   ③ ZIP 导入走流式通道；扫光挂在填充条 i::after
   ④ 库扫描根 + 去重（大小写双拼写、unreadable 提示、**跨种类同 title 只出一张卡**）
   ⑤ 安装失败提示的可读性；⑥ 版本牌

   由 .build/frontend-verify-windows.sh 驱动（2026-10-22 补入：此前全仓无人调用）。
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

/* ---------- ② ppCleanupWhenDone：真装完自动删暂存；跳过/失败保留 ---------- */
console.log("【2】ppCleanupWhenDone：装完自动删暂存（跳过/失败保留）");
{
  const code = cut(/function ppCleanupWhenDone\(taskId, path\)\s*\{[\s\S]*?\n\}/,
                   "ppCleanupWhenDone");
  const mk = (pollResp, mutate) => {
    const calls = [];
    const ctx = {
      PKG_STAGING: "/data/Mango/incoming",
      setInterval: (fn) => { fn(); return 1; },   // 立即跑第一轮
      clearInterval() {},
      apiPost: async (url, body) => {
        calls.push({ url, body });
        return url === "/api/install/poll" ? pollResp : { ok: true };
      },
      apiJson: async (url, body) => {
        calls.push({ url, body });
        return { ok: true };
      },
      toast: m => calls.push({ url: "@toast", body: m }),
      /* 后台清理用：真机源码在删成功时调它把任务从删除坞里撤掉。
         这里只需要存在（不关心副作用），缺了会 ReferenceError 整段崩掉。 */
      delTrackQuiet: () => {},
    };
    vm.createContext(ctx);
    vm.runInContext(mutate ? mutate(code) : code, ctx);
    return { calls,
      run: (id, path) => vm.runInContext(`ppCleanupWhenDone(${id}, ${JSON.stringify(path)})`, ctx) };
  };

  (async () => {
    const a = mk({ ok: true, state_name: "done",
                   note: "already installed (CUSA45599) — skipped" });
    a.run(9, "/data/Mango/incoming/x.pkg");
    await new Promise(r => setImmediate(r));
    ok(!a.calls.some(c => c.url === "/api/delete"),
       "skipped 的 done：不删暂存（已装过，别把用户留下的文件删了）");
    ok(a.calls.some(c => c.url === "@toast" && /跳过/.test(c.body)),
       "skipped 的 done：给用户明说（toast 提到跳过）");

    /* 2026-10-07 契约：真装完的 done → 自动删掉这一个暂存 PKG（用户拍板；
       失败/中断/跳过保留）。只删本次上传的文件，不碰目录里其他东西。 */
    const b = mk({ ok: true, state_name: "done", note: "" });
    b.run(9, "/data/Mango/incoming/x.pkg");
    await new Promise(r => setImmediate(r));
    await new Promise(r => setImmediate(r));
    ok(b.calls.some(c => c.url === "/api/delete" &&
                         c.body && c.body.paths === "/data/Mango/incoming/x.pkg"),
       "真装完的 done：自动删这一个暂存 PKG（/api/delete 带它的完整路径）");
    ok(b.calls.some(c => c.url === "@toast" && /已清理/.test(c.body)),
       "真装完的 done：toast 告知暂存已清理");

    /* 反证：把自动删除从 done 分支摘掉 —— 断言必须打红（证明它有牙）。
       ⚠️ 锚点必须与源码逐字一致：源码是 { paths: path }（字符串，后端
       parse_paths 按行切，hosttest 也用字符串），写成数组 [path] 就替换不到，
       反证会以「/api/delete 仍被调用」的形式失败 —— 那正是本段第一次跑起来
       时撞到的事。 */
    const c = mk({ ok: true, state_name: "done", note: "" },
                 code => code.replace('apiJson("/api/delete", { paths: path })',
                                      "Promise.resolve({ ok: false })"));
    c.run(9, "/data/Mango/incoming/x.pkg");
    await new Promise(r => setImmediate(r));
    await new Promise(r => setImmediate(r));
    ok(!c.calls.some(c2 => c2.url === "/api/delete"),
       "反证：自动删除被摘掉 ⇒ /api/delete 不再被调用（断言能红）");
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
     /"\/data\/Mango\/incoming"/.test(roots),
     "扫描根含 /data/pkg 与 /data/PKG 双拼写 + 暂存目录 /data/Mango/incoming");

  /* ⚠️ 取的是 renderLibraryInner：2026-10-07 起 renderLibrary 变成一层薄壳
     （重入锁 + 脏标记 + 上次扫描时间），真正的扫描/去重/渲染在 Inner 里。
     断言盯的是**渲染逻辑**，所以抽 Inner —— 否则壳里没有 libCard 调用，cut 直接失手。 */
  const code = roots + "\n" +
               cut(/async function renderLibraryInner\(\)\s*\{[\s\S]*?\n\}/, "renderLibraryInner");
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
             run: () => vm.runInContext("renderLibraryInner()", ctx) };
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
      "const key = String(p.path).toLowerCase();",
      "const key = String(p.path);"));
    d.run();
    await new Promise(r => setImmediate(r));
    ok(cards(d.box) === 2,
       "反证：去重键不用小写 ⇒ 双卡出现（断言能红）");

    /* ---- 跨种类去重（真机 2026-10-22 v1.0.28 报障）----
       用户原话：「看下 pkg 页面是不是有重复的游戏了」。实测：/data/pkg 里留着
       .pkg 的游戏（CUSA45599 DAVE THE DIVER、PPSA01650 YouTube）在 PKG 页各出
       两张卡 —— 一张是 .pkg 文件卡（v1.0.28 之前就有），一张是 app 根的
       「安装目录」卡（v1.0.28 新增）。口径：有 .pkg 文件卡时留文件卡。 */
    const e = mk({
      "/data/pkg": { ok: true, packages: [
        { title_id: "CUSA45599", path: "/data/pkg/dave.pkg", installed: true }],
        unreadable: [] },
      "/user/app": { ok: true, packages: [
        { title_id: "CUSA45599", path: "/user/app/CUSA45599", app_dir: true }],
        unreadable: [] },
    });
    e.run();
    await new Promise(r => setImmediate(r));
    ok(cards(e.box) === 1 && /\/data\/pkg\/dave\.pkg/.test(e.box.innerHTML),
       "同一游戏：.pkg 文件卡 + app 安装目录卡 ⇒ 只出一张（留信息更全的 .pkg 卡）");

    /* 本机没有 .pkg 副本的游戏（SMB 流式安装）—— app 目录卡必须留着，
       这正是 v1.0.28 加 app 根要解决的那个报障，不能被误杀成空列表。 */
    const f = mk({
      "/user/app": { ok: true, packages: [
        { title_id: "PPSA23226", path: "/user/app/PPSA23226", app_dir: true }],
        unreadable: [] },
    });
    f.run();
    await new Promise(r => setImmediate(r));
    ok(cards(f.box) === 1 && /\/user\/app\/PPSA23226/.test(f.box.innerHTML),
       "只有安装目录卡（SMB 装好的游戏）⇒ 照常出现");

    /* 反证：跨种类判定被摘掉 ⇒ 双卡立刻回来（断言能红） */
    const g = mk({
      "/data/pkg": { ok: true, packages: [
        { title_id: "CUSA45599", path: "/data/pkg/dave.pkg", installed: true }],
        unreadable: [] },
      "/user/app": { ok: true, packages: [
        { title_id: "CUSA45599", path: "/user/app/CUSA45599", app_dir: true }],
        unreadable: [] },
    }, code => code.replace("pkgTid.has(t) || seenTid.has(t)", "seenTid.has(t)"));
    g.run();
    await new Promise(r => setImmediate(r));
    ok(cards(g.box) === 2,
       "反证：跨种类去重被摘掉 ⇒ 同游戏双卡（断言能红）");

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

/* ---------- ⑥ 版本牌：/api/status.version → #chipVer（vm 级） ---------- */
console.log("【6】版本牌：applyStatus 消费 /api/status 的 version 字段");
{
  /* 静态底座：牌子在顶栏品牌旁、以 display:none 起步（.chip 是显式 inline-flex，
     hidden 属性压不住 —— .stats div 的前车之鉴）；窄屏随 1240px 断点折叠。 */
  ok(/<span class="chip" id="chipVer" style="display:none"><\/span>/.test(src),
     "顶栏品牌旁有 #chipVer（内联 display:none 起步，不造假版本号）");
  ok(/#chipVer\{font-family:var\(--mono\)[^}]*\}/.test(src) &&
     /#chipVer\{display:none!important\}/.test(src),
     "版本牌样式：等宽小字 + 窄屏 ≤1240px 随状态胶囊一起折叠");

  const code = cut(/function applyStatus\(s\)\s*\{[\s\S]*?\n\}/, "applyStatus");
  const mk = (mutate) => {
    const el = () => ({ textContent: "", title: "", style: {}, dataset: {},
                        querySelector: () => null });
    const els = { chipHttp: el(), ipChip: el(), chipVer: el() };
    const ctx = {
      document: {
        getElementById: id => els[id] || null,
        querySelector: () => null,
      },
    };
    vm.createContext(ctx);
    vm.runInContext(mutate ? mutate(code) : code, ctx);
    return { els,
             run: s => vm.runInContext(`applyStatus(${JSON.stringify(s)})`, ctx) };
  };

  const a = mk();
  a.run({ ok: true, http: true, port: 2026, addr: "192.168.1.42", version: "v1.0.0" });
  ok(a.els.chipVer.textContent === "v1.0.0" &&
     a.els.chipVer.style.display === "",
     "带 version 的 status：牌子上版本号、display 置空（显示）");

  const b = mk();
  b.run({ ok: true, http: true, port: 2026, addr: "192.168.1.42" });
  ok(b.els.chipVer.textContent === "",
     "旧后端无 version 字段：牌子保持隐藏，不渲染 undefined");

  const c = mk();
  c.run({ ok: false, version: "v9.9.9" });
  ok(c.els.chipVer.textContent === "",
     "!s.ok 早退：不碰牌子（状态不可信时版本也不可信）");

  /* 反证：摘掉 textContent 赋值 ⇒ 版本号不再上牌（断言能红）。
     ⚠️ index.html 是 CRLF 行尾，替换串本身不含换行，无 \r?\n 问题。 */
  const d = mk(code2 => code2.replace("ver.textContent = s.version;", ""));
  d.run({ ok: true, http: true, port: 2026, addr: "192.168.1.42", version: "v1.0.0" });
  ok(d.els.chipVer.textContent === "",
     "反证：textContent 赋值被摘掉 ⇒ 牌子空着（断言能红）");
}

/* ---------- ⑦ 安装包格式名牌分类 libFmt ---------- */
console.log("【7】libFmt：安装包格式分类（卡片格式名牌徽章）");
{
  const code = cut(/function libFmt\(p\)\s*\{[\s\S]*?\n\}/, "libFmt");
  const ctx = {};
  vm.createContext(ctx);
  vm.runInContext(code, ctx);
  const f = p => vm.runInContext(`libFmt(${JSON.stringify(p)})`, ctx);

  ok(f({ fmt: "app" }) === "app",   "后端权威 fmt=app 直接采用");
  ok(f({ app_dir: true }) === "app","兜底：app_dir ⇒ 文件夹");
  ok(f({ path: "/data/pkg/GAME.pkg" }) === "pkg", "兜底：.pkg ⇒ PKG");
  ok(f({ is_fpkg: true, path: "/x/GAME.ffpkg" }) === "ffpkg", "兜底：.ffpkg ⇒ ffpkg");
  ok(f({ is_fpkg: true, path: "/x/GAME.exfat" }) === "exfat", "兜底：.exfat ⇒ exFAT");
  ok(f({ path: "/x/GAME.lz4" }) === "lz4", "兜底：.lz4 ⇒ LZ4");

  /* 反证：摘掉 exfat 后缀判据 ⇒ .exfat 退回 pkg（断言能红）。
     ⚠️ index.html 是 CRLF 行尾，替换串不含换行，无 \r?\n 问题。 */
  const broken = code.replace('  if (/\\.exfat$/.test(path)) return "exfat";', '');
  const ctx2 = {}; vm.createContext(ctx2);
  vm.runInContext(broken, ctx2);
  const fb = p => vm.runInContext(`libFmt(${JSON.stringify(p)})`, ctx2);
  ok(fb({ path: "/x/GAME.exfat" }) !== "exfat",
     "反证：exfat 判据被摘掉 ⇒ 退回 pkg（断言能红）");
}

/* ---------- 收尾：等所有异步段落跑完再统一报告退出 ----------
   原 exit 在【2】的 IIFE 末尾；新增的异步段会被它提前 process.exit 杀死。 */
(async () => {
  for (let i = 0; i < 50 && !section4Done; i++)
    await new Promise(r => setImmediate(r));
  /* 【4】是唯一带 await 的段落：它没跑完（抛了 / 卡住）就当失败 —— 否则这只
     IIFE 会拿「没跑过」当「通过」，正是这类门禁最常见的假绿形态。 */
  if (!section4Done) {
    console.error("  ✗ 【4】异步断言没有跑完（抛异常或卡住）");
    fails++;
  }
  console.log(fails ? `\n${fails} 项失败` : "\n全部通过");
  process.exit(fails ? 1 : 0);
})();
