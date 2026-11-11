"""Conformance cases for the httpd.ini (ISAPI_Rewrite 2) format.

Each case's rules only match URLs under its own prefix (/cNN/...), so all cases can share one
httpd.ini. Rewrites end at /um-echo.cfm, a page that prints the URL IIS finally served:
    ECHO|<SCRIPT_NAME>|<QUERY_STRING>|<REQUEST_METHOD>|<X-UM header>

A request is (url, headers, method). The recorded truth from ISAPI_Rewrite 2 lives in
oracle.json (tools/compat.py capture); urlmagic must reproduce it (tools/compat.py check).
"""

E = "/um-echo.cfm"   # the echo page


def req(url, headers=None, method="GET"):
    return {"url": url, "headers": headers or {}, "method": method}


CASES = [
    # --- matching
    ("c01", "basic capture", [
        r"RewriteRule /c01/(.*) /um-echo.cfm\?a=$1",
    ], [req("/c01/hello")]),
    ("c02", "patterns must match the whole URL", [
        r"RewriteRule /c02/x /um-echo.cfm\?hit=1",
    ], [req("/c02/x"), req("/c02/xy"), req("/zz/c02/x")]),
    ("c03", "case-sensitive without I", [
        r"RewriteRule /c03/ABC /um-echo.cfm\?hit=1",
    ], [req("/c03/ABC"), req("/c03/abc")]),
    ("c04", "I = case-insensitive", [
        r"RewriteRule /c04/ABC /um-echo.cfm\?hit=1 [I]",
    ], [req("/c04/abc")]),
    ("c05", "the query string is part of the matched URL", [
        r"RewriteRule /c05/page\?id=(\d+) /um-echo.cfm\?id=$1",
    ], [req("/c05/page?id=42"), req("/c05/page")]),
    ("c06", "a pattern without the query string, request with one", [
        r"RewriteRule /c06/([^?]*) /um-echo.cfm\?p=$1",
    ], [req("/c06/a?b=1"), req("/c06/a")]),
    ("c07", "rewritten URL without a query: is the original query kept?", [
        r"RewriteRule /c07/(.*) /um-echo.cfm",
    ], [req("/c07/x?q=1")]),
    ("c08", "regex: lookahead, non-greedy, backreference", [
        r"RewriteRule /c08/(?!no)(.*?)(\d*) /um-echo.cfm\?a=$1&b=$2",
        r"RewriteRule /c08b/(a+)-\1 /um-echo.cfm\?r=$1",
    ], [req("/c08/abc123"), req("/c08/nope"), req("/c08b/aa-aa"), req("/c08b/aa-a")]),

    # --- format strings
    ("c10", "$0 and $& = whole match", [
        r"RewriteRule /c10/.* /um-echo.cfm\?z=$0&a=$&",
    ], [req("/c10/x")]),
    ("c11", "$` and $' (before / after the match)", [
        r"RewriteRule /c11/(.*) /um-echo.cfm\?b=$`&a=$'",
    ], [req("/c11/x")]),
    ("c12", "?N conditional", [
        r"RewriteRule /c12/(a)?(.*) /um-echo.cfm\?x=?1yes:no&r=$2",
    ], [req("/c12/abc"), req("/c12/bc")]),
    ("c13", "backslash escapes in the format: \\( \\) \\$ \\:", [
        r"RewriteRule /c13/(.*) /um-echo.cfm\?p=\($1\)&d=\$&u=http\://x",
    ], [req("/c13/v")]),
    ("c14", "an unescaped ? in the format", [
        r"RewriteRule /c14/(.*) /um-echo.cfm?q=$1",
    ], [req("/c14/v")]),
    ("c15", "a forward slash before a special character", [
        r"RewriteRule /c15/(.*) /um-echo.cfm\?p=/($1/)",
    ], [req("/c15/v")]),
    ("c16", "$N past the last group, and $10", [
        r"RewriteRule /c16/(a)(b)(c)(d)(e)(f)(g)(h)(i)(j) /um-echo.cfm\?n=$9&t=$10&m=$11",
    ], [req("/c16/abcdefghij")]),
    ("c17", "CL lowercases the result", [
        r"RewriteRule /c17/(.*) /um-echo.cfm\?l=$1 [CL]",
    ], [req("/c17/MiXeD")]),
    ("c18", "CU uppercases the result", [
        r"RewriteRule /c18/(.*) /um-echo.cfm\?u=$1 [CU]",
    ], [req("/c18/MiXeD")]),

    # --- responses
    ("c20", "F refuses (404 or 403?)", [
        r"RewriteRule /c20/.* /um-echo.cfm [F]",
    ], [req("/c20/x")]),
    ("c21", "R redirects (status, relative Location)", [
        r"RewriteRule /c21/(.*) /new/$1 [R]",
    ], [req("/c21/x"), req("/c21/x?q=1")]),
    ("c22", "RP redirects permanently", [
        r"RewriteRule /c22/(.*) /new/$1 [RP]",
    ], [req("/c22/x")]),
    ("c23", "redirect to a full URL", [
        r"RewriteRule /c23/(.*) http\://example.com/$1 [RP]",
    ], [req("/c23/x")]),

    # --- rule flow
    ("c30", "L stops", [
        r"RewriteRule /c30/a /um-echo.cfm\?step=1 [L]",
        r"RewriteRule /um-echo.cfm\?step=1 /um-echo.cfm\?step=2",
    ], [req("/c30/a")]),
    ("c31", "without L the next rules see the rewritten URL", [
        r"RewriteRule /c31/a /c31/b",
        r"RewriteRule /c31/b /um-echo.cfm\?got=b",
    ], [req("/c31/a")]),
    ("c32", "N restarts from the first rule", [
        r"RewriteRule /c32/b /um-echo.cfm\?via=n",
        r"RewriteRule /c32/a /c32/b [N]",
    ], [req("/c32/a")]),
    ("c33", "NS repeats the same rule", [
        r"RewriteRule /c33/([^-]*)-(.*) /c33/$1_$2 [NS]",
        r"RewriteRule /c33/(.*) /um-echo.cfm\?r=$1",
    ], [req("/c33/x-y-z")]),
    ("c34", "a rule that loops forever with N (RepeatLimit)", [
        r"RewriteRule /c34/(.*) /c34/$1x [N]",
    ], [req("/c34/a")]),

    # --- conditions
    ("c40", "RewriteCond on a header", [
        r"RewriteCond Host: dev\.muncherelli\.com",
        r"RewriteRule /c40/(.*) /um-echo.cfm\?h=$1",
    ], [req("/c40/x")]),
    ("c41", "groups are numbered across conditions, then the rule", [
        r"RewriteCond Host: (dev)\..*",
        r"RewriteRule /c41/(.*) /um-echo.cfm\?c=$1&r=$2",
    ], [req("/c41/x")]),
    ("c42", "a failing condition", [
        r"RewriteCond Host: www\..*",
        r"RewriteRule /c42/(.*) /um-echo.cfm\?h=$1",
    ], [req("/c42/x")]),
    ("c43", "a missing header is an empty string", [
        r"RewriteCond X-Nope: .{0}",
        r"RewriteRule /c43/(.*) /um-echo.cfm\?m=1",
    ], [req("/c43/x")]),
    ("c44", "header names in conditions: case", [
        r"RewriteCond x-um: yes",
        r"RewriteRule /c44/(.*) /um-echo.cfm\?hit=1",
    ], [req("/c44/x", {"X-UM": "yes"}), req("/c44/x")]),
    ("c45", "two conditions = AND", [
        r"RewriteCond Host: dev.*",
        r"RewriteCond X-UM: go",
        r"RewriteRule /c45/(.*) /um-echo.cfm\?both=1",
    ], [req("/c45/x", {"X-UM": "go"}), req("/c45/x")]),
    ("c46", "%ServerVariable", [
        r"RewriteCond %SERVER_PORT 80",
        r"RewriteRule /c46/(.*) /um-echo.cfm\?port=80",
    ], [req("/c46/x")]),
    ("c47", "METHOD", [
        r"RewriteCond METHOD POST",
        r"RewriteRule /c47/(.*) /um-echo.cfm\?post=1",
    ], [req("/c47/x", method="POST"), req("/c47/x")]),
    ("c48", "I on the rule applies to its conditions", [
        r"RewriteCond X-UM: YES",
        r"RewriteRule /c48/x /um-echo.cfm\?hit=1 [I]",
    ], [req("/c48/x", {"X-UM": "yes"})]),
    ("c49", "VERSION", [
        r"RewriteCond VERSION HTTP/1\.1",
        r"RewriteRule /c49/(.*) /um-echo.cfm\?v11=1",
    ], [req("/c49/x")]),

    # --- normalisation
    ("c50", "O decodes %XX before matching", [
        r"RewriteRule /c50/a\sb /um-echo.cfm\?o=1 [O]",
    ], [req("/c50/a%20b")]),
    ("c51", "without O the URL is still encoded", [
        r"RewriteRule /c51/a%20b /um-echo.cfm\?raw=1",
        r"RewriteRule /c51/a\sb /um-echo.cfm\?dec=1",
    ], [req("/c51/a%20b")]),

    # --- headers
    ("c60", "RewriteHeader adds/sets a header", [
        r"RewriteHeader X-UM: .* injected",
        r"RewriteRule /c60/x /um-echo.cfm",
    ], [req("/c60/x"), req("/c60/x", {"X-UM": "orig"})]),
    ("c61", "RewriteHeader METHOD", [
        r"RewriteCond URL /c61/.*",
        r"RewriteHeader METHOD POST GET",
        r"RewriteRule /c61/x /um-echo.cfm",
    ], [req("/c61/x", method="POST")]),

    # --- prefixes
    ("c70", "UriMatchPrefix / UriFormatPrefix, then reset", [
        r"UriMatchPrefix /c70",
        r"UriFormatPrefix /c70",
        r"RewriteRule /a /b",
        r"UriMatchPrefix",
        r"UriFormatPrefix",
        r"RewriteRule /c70/b /um-echo.cfm\?p=1",
    ], [req("/c70/a")]),
]


def httpd_ini():
    lines = ["# urlmagic4 conformance cases (tests/compat/cases.py). Global httpd.ini for",
             "# ISAPI_Rewrite 2 Lite. Only URLs under /cNN/ are affected.",
             "[ISAPI_Rewrite]", ""]
    for cid, title, rules, _ in CASES:
        lines.append(f"# {cid}: {title}")
        lines.extend(rules)
        lines.append("")
    return "\r\n".join(lines) + "\r\n"
