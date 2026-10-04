
const SUB = "本地盘与 NAS 是同一个空间的两个根，同屏并列，拉取方向一眼可见；进度只有一处口径，不会自相矛盾；安装只依赖 kstuff 这一个叶子载荷。";
/* 视图切换 —— 这里是整份 demo 最关键的一段。
   原版只改 aria-current 高亮、内容一个字都不动：用户点「文件 / 任务 / 游戏 /
   存档」什么都不会发生，导航看起来是坏的。这正是「布局看不懂」的根因：
   没有视图边界，五块内容平铺成一条长页，用户建立不起「我在哪、点这去哪」。
   现在导航真的驱动 .view.on，并且切完把滚动位置归零（否则从长页底部切过去
   会停在莫名其妙的半截位置）。 */
const VIEWS = ["home", "files", "tasks", "library", "saves"];
function goto(key) {
  if (!VIEWS.includes(key)) key = "home";
  document.querySelectorAll(".view").forEach(v => v.classList.toggle("on", v.id === "view-" + key));
  document.querySelectorAll("#nav button").forEach(b => b.setAttribute("aria-current", String(b.dataset.view === key)));
  window.scrollTo(0, 0);
}
document.getElementById("nav").addEventListener("click", e => {
  const b = e.target.closest("button[data-view]");
  if (b) goto(b.dataset.view);
});
document.querySelectorAll("[data-goto]").forEach(b => b.addEventListener("click", () => goto(b.dataset.goto)));
/* 亮色主题已按用户要求移除（2026-10-01）。本文件当前无人引用，见 main.css 顶部说明。 */
document.getElementById("btnNotes").addEventListener("click", () => document.getElementById("notes").classList.toggle("on"));
const scrim = document.getElementById("scrim");
const open = () => scrim.classList.add("on"), close = () => scrim.classList.remove("on");
document.getElementById("btnExtract").addEventListener("click", open);
document.getElementById("dc").addEventListener("click", close);
document.getElementById("dc2").addEventListener("click", close);
document.getElementById("dgo").addEventListener("click", close);
scrim.addEventListener("click", e => { if (e.target === scrim) close(); });
document.addEventListener("keydown", e => {
  if (e.key === "Escape") { close(); document.getElementById("notes").classList.remove("on"); }
});
document.querySelectorAll(".cmd-h button").forEach(b => b.addEventListener("click", () => {
  const t = b.innerHTML; b.innerHTML = "已复制";
  setTimeout(() => { b.innerHTML = t; }, 1200);
}));
/* 进度按「总字节 ÷ 总耗时」口径推进；均速不显示瞬时值 */
let done = 8.20, total = 11.6;
setInterval(() => {
  done = Math.min(total, done + 0.06);
  const pct = done / total * 100;
  document.querySelectorAll("[data-p] i").forEach(el => el.style.width = pct.toFixed(1) + "%");
  document.querySelectorAll("[data-gb]").forEach(el => el.textContent = done.toFixed(2));
}, 900);
document.getElementById("heroSub").textContent = SUB;

/* ---------- 存档工作台：详情 ⇄ 空状态 ----------
   默认停在「已选中」的详情态（信息量最大），关掉预览才回到空状态。空状态不是
   装饰 —— 它明确写出「点左边任意一项会发生什么」，而不是丢一块空白让用户猜。
   （这个模式借自 Garlic SaveMgr 的 browse 空态。） */
const svDetail = document.getElementById("svDetail"), svEmpty = document.getElementById("svEmpty");
const showDetail = on => { svDetail.hidden = !on; svEmpty.hidden = on; };
document.getElementById("svClose").addEventListener("click", () => showDetail(false));
document.querySelectorAll("#view-saves .svi").forEach(row => row.addEventListener("click", e => {
  if (e.target.tagName === "INPUT") return;          // 勾选框只负责多选，不改当前预览
  document.querySelectorAll("#view-saves .svi").forEach(x => x.setAttribute("aria-selected", String(x === row)));
  showDetail(true);
}));

/* ---------- 筛选控件必须真的筛 ----------
   注意：只换按下态 = 上一版被用户抓到的「假导航」。宁可少摆一个按钮，也不摆一个
   按下去什么都不发生的按钮 —— 那是在训练用户不信任界面。
   （原型没给每张卡加 data-* 属性，索性从已渲染的文案里读：平台角标 / 状态胶囊。
   真实实现里这些值当然来自扫描结果，不是 DOM 文本。） */
const segPick = (scope, label) => {
  const g = [...scope.querySelectorAll(".seg")].find(s => s.getAttribute("aria-label") === label);
  const b = g && g.querySelector('button[aria-pressed="true"]');
  return b ? b.textContent.replace(/\s*\d+\s*$/, "").trim() : "";
};
const txt = (el, fb = "") => (el ? el.textContent.trim() : fb);
function applyFilter(scope) {
  if (scope.id === "view-library") {
    const plat = segPick(scope, "平台"), st = segPick(scope, "状态");
    let n = 0;
    scope.querySelectorAll(".gcard").forEach(c => {
      const p = txt(c.querySelector(".cover .plat"));
      const s = txt(c.querySelector(".gacts .pill"));
      const okP = plat === "全部" || p === plat;
      const okS = st === "在盘上" ? true : (st === "已安装" ? s === "已安装" : s === "需口令");
      c.hidden = !(okP && okS);
      if (!c.hidden) n++;
    });
    const out = scope.querySelector(".count");
    if (out) out.textContent = n + " / 6 个游戏 · 已按筛选显示";
  } else if (scope.id === "view-saves") {
    const st = segPick(scope, "状态");
    let n = 0;
    scope.querySelectorAll(".svi").forEach(r => {
      const pills = [...r.querySelectorAll(".tags .pill")].map(x => x.textContent.trim());
      const ok = st === "全部" || st === "有快照" ? true : pills.includes(st);
      r.hidden = !ok;
      if (!r.hidden) n++;
    });
    const out = document.getElementById("svCount");
    if (out) out.textContent = n + " 个容器 · 20 个存档 · 0.9 GB";
  }
}
["view-library", "view-saves"].forEach(id => {
  const s = document.getElementById(id);
  if (!s) return;
  s.querySelectorAll(".seg").forEach(g => g.addEventListener("click", e => {
    const b = e.target.closest("button"); if (!b) return;
    g.querySelectorAll("button").forEach(x => x.setAttribute("aria-pressed", String(x === b)));
    applyFilter(s);
  }));
});
/* 本机地址：点一下复制 http://<ip>:<port>/。
   两条真实约束决定这段不能写得太天真：
    ① navigator.clipboard 只在**安全上下文**存在。插件跑在 http://<局域网 IP>:2026 ——
       既不是 https 也不是 localhost，所以那里 navigator.clipboard 是 undefined；
       必须留 execCommand("copy") 兜底，否则「复制」恰好在它最该工作的地方失效。
    ② 权限被拒时 writeText 的 Promise 可能迟迟不 settle，按钮看着像卡死 ⇒ 加超时；
       失败也必须给出可见文案，静默无反应和假按钮是同一种毛病。 */
(() => {
  const c = document.getElementById("ipChip");
  if (!c) return;
  const url = "http://" + c.dataset.ip + "/";
  const label = c.querySelector(".ipv");
  const keep = label.textContent;
  let timer = 0;
  const flash = t => {
    label.textContent = t;
    clearTimeout(timer);
    timer = setTimeout(() => { label.textContent = keep; c.removeAttribute("data-copied"); }, 1500);
  };
  const legacyCopy = () => {
    const ta = document.createElement("textarea");
    ta.value = url; ta.setAttribute("readonly", "");
    ta.style.cssText = "position:fixed;top:-100px;left:0;opacity:0";
    document.body.appendChild(ta); ta.select();
    let ok = false;
    try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
    ta.remove();
    return ok;
  };
  const withTimeout = (p, ms) => Promise.race([
    p, new Promise((_, rej) => setTimeout(() => rej(new Error("clipboard timeout")), ms)),
  ]);
  c.addEventListener("click", async () => {
    let ok = false;
    try {
      if (navigator.clipboard) { await withTimeout(navigator.clipboard.writeText(url), 400); ok = true; }
    } catch (e) { ok = false; }
    if (!ok) ok = legacyCopy();
    if (ok) c.setAttribute("data-copied", "");
    flash(ok ? "已复制" : "复制失败，请手动输入");
  });
})();
