#!/usr/bin/env python3
# encoding: utf-8
"""
PS5 Mango i18n extractor.

Scans assets/index.html and pulls every *user-visible* Chinese string into the
i18n key table:
  - visible text nodes (text between tags, outside <script>/<style>/<svg>/<title>/comments)
  - selected attributes (placeholder/title/aria-label/alt/value/label) containing CJK

Each hit becomes:
  - text node  -> <span data-i18n="kNNNN">原始中文</span>
  - attribute  -> data-i18n-attr="attr:kNNNN" added to the element
The Chinese text is recorded (entities decoded) as the zh value; English is left
for the translator. The original Chinese always stays in the HTML as the
fallback, so an untranslated key degrades to Chinese, never blank.

Usage:
  python3 tools/i18n_extract.py            # writes index.html + i18n_keys.json
"""
import re
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "assets", "index.html")
BAK = SRC + ".bak.i18n"
KEYS_JSON = os.path.join(HERE, "i18n_keys.json")

TARGET_ATTRS = {"placeholder", "title", "aria-label", "alt", "value", "label"}

# CJK unified ideographs + CJK punctuation + fullwidth forms.
CJK = re.compile(r"[\u3000-\u303f\u3400-\u4dbf\u4e00-\u9fff\uff00-\uffef]")
ENTITY = re.compile(r"&(?:#(\d+)|#x([0-9a-fA-F]+)|(amp|lt|gt|quot|apos|nbsp));")
ATTR_RE = re.compile(r"(\s)([\w-]+)(\s*=\s*)(\"([^\"]*)\"|'([^']*)'|([^\s>]+))")

NAMED = {"amp": "&", "lt": "<", "gt": ">", "quot": '"', "apos": "'", "nbsp": "\u00a0"}


def dec(s):
    def rep(m):
        if m.group(1):
            return chr(int(m.group(1)))
        if m.group(2):
            return chr(int(m.group(2), 16))
        return NAMED.get(m.group(3), m.group(0))
    return ENTITY.sub(rep, s)


keys = {}      # key -> zh (decoded)
order = []     # preserve insertion order
counter = [0]


def nkey():
    counter[0] += 1
    return "k%04d" % counter[0]


def find_tag_end(s, i):
    j = i + 1
    n = len(s)
    while j < n:
        c = s[j]
        if c == '"' or c == "'":
            q = c
            j += 1
            while j < n and s[j] != q:
                j += 1
            if j < n:
                j += 1
            continue
        if c == ">":
            return j
        j += 1
    return n


def process_tag(tag):
    additions = []

    def repl(m):
        name = m.group(2)
        val = m.group(5) or m.group(6) or m.group(7) or ""
        if name in TARGET_ATTRS and CJK.search(val):
            k = nkey()
            keys[k] = dec(val)
            order.append(k)
            additions.append("%s:%s" % (name, k))
        return m.group(0)

    tag = ATTR_RE.sub(repl, tag)
    if additions:
        spec = ";".join(additions)
        if tag.rstrip().endswith("/>"):
            tag = tag.rstrip()[:-2].rstrip() + ' data-i18n-attr="%s" />' % spec
        else:
            tag = tag[:-1] + ' data-i18n-attr="%s">' % spec
    return tag


WS = " \t\r\n"


def process_text(text):
    if not CJK.search(text):
        return text
    stripped = text.strip(WS)
    if not CJK.search(stripped):
        return text
    lead = text[: len(text) - len(text.lstrip(WS))]
    trail = text[len(text.rstrip(WS)):]
    k = nkey()
    keys[k] = dec(stripped)
    order.append(k)
    return lead + '<span data-i18n="%s">%s</span>' % (k, stripped) + trail


def extract(data):
    out = []
    i = 0
    n = len(data)
    while i < n:
        c = data[i]
        if c != "<":
            j = data.find("<", i)
            if j == -1:
                j = n
            out.append(process_text(data[i:j]))
            i = j
            continue
        # comment
        if data.startswith("<!--", i):
            j = data.find("-->", i + 4)
            end = j + 3 if j != -1 else n
            out.append(data[i:end])
            i = end
            continue
        low = data[i:i + 40].lower()
        if low.startswith("<script"):
            j = data.lower().find("</script>", i)
            end = j + 9 if j != -1 else n
            out.append(data[i:end])
            i = end
            continue
        if low.startswith("<style"):
            j = data.lower().find("</style>", i)
            end = j + 8 if j != -1 else n
            out.append(data[i:end])
            i = end
            continue
        if low.startswith("<svg"):
            j = data.lower().find("</svg>", i)
            end = j + 6 if j != -1 else n
            out.append(data[i:end])
            i = end
            continue
        if low.startswith("<title"):
            j = data.lower().find("</title>", i)
            end = j + 8 if j != -1 else n
            out.append(data[i:end])
            i = end
            continue
        # normal tag
        j = find_tag_end(data, i)
        tag = data[i:j + 1]
        out.append(process_tag(tag))
        i = j + 1
    return "".join(out)


def main():
    data = open(SRC, encoding="utf-8").read()
    result = extract(data)
    # write modified html
    open(SRC, "w", encoding="utf-8").write(result)
    # write keys json
    table = {k: {"zh": keys[k]} for k in order}
    json.dump(table, open(KEYS_JSON, "w", encoding="utf-8"),
              ensure_ascii=False, indent=2)
    print("extracted keys:", len(order))
    print("wrote", SRC)
    print("wrote", KEYS_JSON)


if __name__ == "__main__":
    main()
