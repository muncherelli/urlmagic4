# httpd.ini compatibility (ISAPI_Rewrite 2 style)

urlmagic4 also reads rule files in the `httpd.ini` format of ISAPI_Rewrite 2.x, so existing
rule files from that product keep working. This page describes that format's behaviour,
written from that product's public documentation and black-box testing. urlmagic4 is not
affiliated with Helicon Tech; ISAPI_Rewrite is their trademark.

Items marked **(verify)** are not certain from the documentation and must be settled by
testing against ISAPI_Rewrite 2 Lite on IIS 4.0 before they are implemented.

For the default Apache-style format and everything shared by both (target, build, IIS
hooks, per-site settings), see [spec.md](spec.md).

## 1. Configuration files

| File | Where | Applies to |
|---|---|---|
| Global | `httpd.ini` in the folder holding `urlmagic4.dll` | every site, evaluated first |
| Site | `httpd.ini` in the site's home directory (physical root) | that site, after the global file |

* INI style. Only lines inside the `[ISAPI_Rewrite]` section count; everything else is
  ignored. One directive per line; `#` starts a comment line. Blank lines ignored.
* A file is re-read when it changes (checked at most once per second per file); no restart.
* Syntax errors are written to `httpd.parse.errors` beside the file; the bad line is skipped,
  the rest of the file still loads.
* The filter itself always refuses requests for `httpd.ini` and `httpd.parse.errors`
  (any directory, any case) with 404, so site rule files can never be downloaded.

Sites on network shares: as in [spec.md](spec.md), the site's URL Magic tab can point it at
a rules file on the server's own disk.

## 2. Directives

### v1

| Directive | Syntax | Scope |
|---|---|---|
| `RewriteRule` | `Pattern FormatString [Flags]` | global, site |
| `RewriteCond` | `TestVerb Pattern [Flags]` | global, site; binds to the next `RewriteRule`/`RewriteHeader` only |
| `RewriteHeader` | `HeaderName Pattern FormatString [Flags]` | global, site; empty result removes the header. `METHOD` rewrites the verb. Header name with or without colon (verify) |
| `RepeatLimit` | `N` (default 32) | global, site (capped by the global value). 0 or 1 = no looping |
| `EnableConfig` / `DisableConfig` | `[SiteID\|"Site name"]` | global: whether site files are read |
| `EnableRewrite` / `DisableRewrite` | `[SiteID\|"Site name"]` | global: whether a site is rewritten at all |
| `UriMatchPrefix` / `UriFormatPrefix` | `Prefix` | prepended to later patterns / format strings |

### Later or never

* `RewriteProxy`, flag `P`, `RFStyle`, the helper extension: proxying (Full-only in the
  original). Not planned; Caddy or IIS itself can proxy.
* `CacheClockRate`: accepted and ignored (we reload on file change instead).

## 3. Matching

* **TestVerb** for `RewriteCond`: `URL` (request URI), `METHOD`, `VERSION`, `Header-Name:`
  (any request header, trailing colon), `%ServerVariable` (IIS server variable, case-sensitive).
  A missing header is an empty string.
* `RewriteRule` tests the request URI **including the query string** (verify).
* Patterns are Perl-style regular expressions and must match the **whole** string (implicitly
  anchored at both ends).
* A rule and its conditions are matched as one combined expression:
  `(?:cond1)\n(?:cond2)\n...\n(?:rule)` against `value1\nvalue2\n...\nuri`. Therefore capture
  groups are numbered across the conditions first, then the rule: with one condition holding
  one group, the rule's first group is `$2`.
* Flag `I` on the rule makes the rule and all its conditions case-insensitive.
* Rules run top to bottom, global file then site file.

## 4. Format strings

| Syntax | Meaning |
|---|---|
| `$N` | capture group N (0-99) |
| `$0`, `$&` | whole match |
| `` $` ``, `$'` | text before / after the match |
| `?N<text>:<text>` | first text if group N matched, else the second (verify exact form) |
| `\` + char | literal: `\(` `\)` `\$` `\?` `\:` (verify: backslash vs other) |
| `\n \r \t \xHH` | control / hex characters |

The result replaces the URL. If the result contains `?`, the part after it becomes the new
query string.

## 5. Flags

### `RewriteRule` / `RewriteHeader`, v1

| Flag | Meaning |
|---|---|
| `I` | case-insensitive (rule and its conditions) |
| `L` | stop after this rule if it matched |
| `N` | after rewriting, restart from the first rule (bounded by `RepeatLimit`) |
| `NS` | after rewriting, re-run from this same rule (bounded by `RepeatLimit`) |
| `R` | redirect, 302 |
| `RP` | redirect, 301 permanent |
| `F` | refuse the request: 404 (verify: 403 or 404) |
| `CL` / `CU` | lowercase / uppercase the result |
| `O` | normalise the input first (URL-decode, clean up) — drops the query string |
| `U` | log the original URL in the IIS log instead of the rewritten one |

### `RewriteCond`

| Flag | Meaning |
|---|---|
| `O` | normalise the input first |

Unknown flags are a parse error (logged; the line is skipped).
