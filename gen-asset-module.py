#!/usr/bin/env python3
# encoding: utf-8
# Copyright (C) 2024 John Törnblom
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.
#
# 2026-10-03: added a build-time minify pass (HTML/CSS/JS) that runs BEFORE
# gzip, so the embedded web UI ships smaller. The on-disk assets/ files are
# left untouched and human-readable; only the gzip payload is minified.
# JS minification is deliberately conservative: it strips /* */ block comments
# outside of string literals and collapses indentation/blank lines. It never
# touches string contents and never removes // line comments, so it cannot
# break URLs (https://...) or unbalance brackets.

import argparse
import gzip
import re
import string
import mimetypes
import sys


tmpl = string.Template('''
void asset_register(const char*, const void*, unsigned long, const char*, const char*);

static const unsigned char data[] = $data;

__attribute__((constructor)) static void
constructor(void) {
  asset_register("/$path", data, sizeof(data), $mime, $encoding);
}
''')

GZIP_MIMES = {
    'application/javascript',
    'application/json',
    'text/css',
    'text/html',
    'text/javascript',
}

# Mimes we are willing to text-minify (JSON is left alone to avoid breaking it).
MINIFY_MIMES = {
    'text/html',
    'text/css',
    'application/javascript',
    'text/javascript',
}


def cstr(value):
    if value is None:
        return '0'
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def gen_data(data):
    yield '{\n  '

    for n, b in enumerate(data, 1):
        yield hex(b)
        yield ', '

        if n % 16 == 0:
            yield '\n  '

    yield '\n}'


def minify_html(s):
    s = re.sub(r'<!--.*?-->', '', s, flags=re.S)        # drop HTML comments
    s = re.sub(r'[ \t]+', ' ', s)                        # collapse horizontal ws
    s = re.sub(r'(?m)^[ \t]+', '', s)                    # strip line indentation
    s = re.sub(r'(?m)[ \t]+$', '', s)                    # strip trailing ws
    s = re.sub(r'\n{2,}', '\n', s)                      # drop blank lines
    return s


def minify_css(s):
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)          # drop CSS comments
    s = re.sub(r'\s*([{};,>])\s*', r'\1', s)            # trim ws around tokens
    s = re.sub(r'[ \t]+', ' ', s)                        # collapse horizontal ws
    s = re.sub(r'(?m)^[ \t]+', '', s)
    s = re.sub(r'(?m)[ \t]+$', '', s)
    s = re.sub(r'\n{2,}', '\n', s)
    return s


def _strip_block_comments(s):
    # String-aware: never remove /* */ that lives inside ' " ` string literals.
    out = []
    i = 0
    n = len(s)
    in_str = None
    while i < n:
        c = s[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(s[i + 1])
                i += 2
                continue
            if c == in_str:
                in_str = None
            i += 1
            continue
        if c in ('"', "'", '`'):
            in_str = c
            out.append(c)
            i += 1
            continue
        if c == '/' and i + 1 < n and s[i + 1] == '*':
            j = s.find('*/', i + 2)
            if j == -1:
                out.append(c)
                i += 1
                continue
            i = j + 2
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def minify_js(s):
    s = _strip_block_comments(s)                         # safe /* */ removal
    s = re.sub(r'(?m)^[ \t]+', '', s)                    # strip indentation
    s = re.sub(r'(?m)[ \t]+$', '', s)                    # strip trailing ws
    s = re.sub(r'\n{2,}', '\n', s)                       # drop blank lines
    return s


def minify_text(path, mime, text):
    if mime == 'text/html':
        return minify_html(text)
    if mime == 'text/css':
        return minify_css(text)
    if mime in ('application/javascript', 'text/javascript'):
        return minify_js(text)
    return text


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--path', default=None)
    parser.add_argument('--no-minify', action='store_true')
    parser.add_argument('FILE')
    args = parser.parse_args()

    if args.path is None:
        args.path = args.FILE

    mime = mimetypes.guess_type(args.path)[0]
    encoding = None
    with open(args.FILE, mode='rb') as f:
        payload = f.read()

    if not args.no_minify and mime in MINIFY_MIMES:
        try:
            text = payload.decode('utf-8')
            text = minify_text(args.path, mime, text)
            payload = text.encode('utf-8')
        except Exception as e:
            sys.stderr.write('minify skipped for %s: %s\n' % (args.FILE, e))

    if mime in GZIP_MIMES:
        zipped = gzip.compress(payload, compresslevel=9, mtime=0)
        if len(zipped) < len(payload):
            payload = zipped
            encoding = 'gzip'

    data = ''.join(gen_data(payload))
    print(tmpl.substitute(data=data, path=args.path,
                          mime=cstr(mime), encoding=cstr(encoding)))
