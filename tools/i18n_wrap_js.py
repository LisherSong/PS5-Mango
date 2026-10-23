#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Wrap user-visible CJK string literals in the inline <script> of assets/index.html
with t("jsNNNN", <original-literal>), so they become translatable.

Uses a proper string scanner that respects nested quotes and ${...} substitutions
inside template literals (the previous regex mis-parsed templates containing
double quotes).

SAFETY RULES (skip a literal if ANY of):
  - value contains a newline  (multi-line template that interleaves code)
  - value looks like a CSS selector (aria-label=, [data-, button[, querySelector,
    closest(, getElementById(, name=", input[)
  - value is a data URI (data:, svg+xml)
  - PRE (stripped) ends with === / !== / == / !=   -> comparison RHS  (add to skip-set)
  - PRE (stripped) ends with '[' AND POST(stripped) starts with ']' -> bracket access
  - POST(stripped) starts with ':' AND PRE(stripped) does NOT end with '?' -> object key
  - the literal value is in the global skip-set (e.g. also used in a comparison)

Output:
  - rewrites assets/index.html (inline script only)
  - writes tools/i18n_keys_js.json  -> { "js0001": "原始中文(含定界符)", ... }
"""
import re, json, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(ROOT, "assets", "index.html")

src = open(HTML, encoding="utf-8").read()

# locate the inline (no-attribute) <script> ... </script>
m = re.search(r"<script>", src)
s = m.end()
e = src.index("</script>", s)
js = src[s:e]


def scan_strings(js):
    out = []
    i = 0
    n = len(js)
    while i < n:
        c = js[i]
        if js.startswith("/*", i):
            j = js.find("*/", i + 2)
            i = j + 2 if j != -1 else n
            continue
        if js.startswith("//", i):
            j = js.find("\n", i)
            i = j if j != -1 else n
            continue
        if c == '"':
            j = i + 1
            while j < n:
                if js[j] == "\\":
                    j += 2
                    continue
                if js[j] == '"':
                    break
                j += 1
            out.append((i, j + 1, js[i:j + 1], '"'))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if js[j] == "\\":
                    j += 2
                    continue
                if js[j] == "'":
                    break
                j += 1
            out.append((i, j + 1, js[i:j + 1], "'"))
            i = j + 1
            continue
        if c == "`":
            j = i + 1
            while j < n:
                if js[j] == "\\":
                    j += 2
                    continue
                if js[j] == "`":
                    break
                if js[j] == "$" and js[j + 1:j + 2] == "{":
                    depth = 1
                    k = j + 2
                    while k < n and depth > 0:
                        cc = js[k]
                        if js.startswith("/*", k):
                            kk = js.find("*/", k + 2)
                            k = kk + 2 if kk != -1 else n
                            continue
                        if js.startswith("//", k):
                            kk = js.find("\n", k)
                            k = kk if kk != -1 else n
                            continue
                        if cc in ('"', "'", "`"):
                            q = cc
                            k += 1
                            while k < n:
                                if js[k] == "\\":
                                    k += 2
                                    continue
                                if js[k] == q:
                                    break
                                k += 1
                            k += 1
                            continue
                        if cc == "{":
                            depth += 1
                        elif cc == "}":
                            depth -= 1
                        k += 1
                    j = k
                    continue
                j += 1
            out.append((i, j + 1, js[i:j + 1], "`"))
            i = j + 1
            continue
        i += 1
    return out


matches = scan_strings(js)  # (start, end, literal, quote_type)

cjk = re.compile(r"[\u3000-\u303f\u3400-\u4dbf\u4e00-\u9fff\uff00-\uffef]")


def is_selector(v):
    for tok in ("aria-label=", "aria-label='", "[data-", "button[", "querySelector",
                "closest(", "getElementById(", "input[", 'name="', "name='"):
        if tok in v:
            return True
    if "[" in v and "=" in v and "]" in v:
        return True
    return False


skip_set = set()
candidates = []
for (a, b, lit, q) in matches:
    v = lit[1:-1] if len(lit) >= 2 else lit  # strip delimiters for CJK + rule checks
    if not cjk.search(v):
        continue
    pre = js[max(0, a - 25):a].rstrip()
    post = js[b:b + 20].lstrip()
    reason = None
    if "${" in lit:
        # template with interpolation: t() returns a plain JSON string, so ${}
        # would NOT be evaluated. Leave these as raw templates (Chinese, correct).
        reason = "template-expr"
    elif "\n" in lit:
        reason = "multiline"
    elif "data:" in v or "svg+xml" in v:
        reason = "datauri"
    elif is_selector(v):
        reason = "selector"
    elif pre.endswith("===") or pre.endswith("!==") or pre.endswith("==") or pre.endswith("!="):
        reason = "comparison"
        skip_set.add(lit)
    elif pre.endswith("[") and post.startswith("]"):
        reason = "bracket"
    elif post.startswith(":") and not pre.endswith("?"):
        reason = "objkey"
    if reason is None and lit in skip_set:
        reason = "paired-skip"
    candidates.append((a, b, lit, reason))

keys = {}
val_to_key = {}
counter = 0
to_wrap = []
for (a, b, lit, reason) in candidates:
    if reason is not None:
        continue
    if lit in val_to_key:
        key = val_to_key[lit]
    else:
        counter += 1
        key = "js%04d" % counter
        val_to_key[lit] = key
        keys[key] = lit
    to_wrap.append((a, b, key))

# Wrap from end to start using ORIGINAL js positions (literal preserved as fallback)
new_js = js
for (a, b, key) in sorted(to_wrap, key=lambda x: x[0], reverse=True):
    literal = js[a:b]
    repl = 't("%s", %s)' % (key, literal)
    new_js = new_js[:a] + repl + new_js[b:]

new_src = src[:s] + new_js + src[e:]

# keep only keys actually present in new_js
used = set(re.findall(r't\("js\d{4}",', new_js))
final_keys = {k: v for k, v in keys.items() if ('t("%s",' % k) in used}

open(HTML, "w", encoding="utf-8").write(new_src)
json.dump(final_keys, open(os.path.join(ROOT, "tools", "i18n_keys_js.json"), "w", encoding="utf-8"),
          ensure_ascii=False, indent=2)

from collections import Counter
print("total CJK string literals (scanner):", len(candidates))
print("skipped:", sum(1 for c in candidates if c[3] is not None))
print("wrapped keys:", len(final_keys))
print("skip reasons:", dict(Counter(c[3] for c in candidates if c[3] is not None)))
