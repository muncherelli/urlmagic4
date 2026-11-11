# urlmagic4 specification

What urlmagic4 does, written so it can be implemented and tested without looking at any
other product's code. Rule syntax follows the `httpd.ini` format of ISAPI_Rewrite 2.x so
existing rule files keep working; sources are that product's public documentation and
black-box testing against it.

Items marked **(verify)** are not certain from the documentation and must be settled by
testing against ISAPI_Rewrite 2 Lite on IIS 4.0 before they are implemented.

## 1. Target

* Windows NT 4.0 SP6a, IIS 4.0, x86. Must also load on later IIS 5/6 but that is not tested.
* One DLL, `urlmagic4.dll`, installed as a **global** ISAPI filter (server properties).
* No C runtime dependency; only `kernel32.dll` APIs present on NT 4.0.
* The filter runs inside `inetinfo.exe`: a crash takes every site down. Bounded memory per
  request, no recursion on user input, regex with match limits.

## 2. Configuration files

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

**Open question (site files on network shares):** IIS runs filters as LocalSystem, which has
no network credentials, so a site whose home directory is a UNC share (e.g. a dev site served
from Samba) cannot have its `httpd.ini` read there. Proposed urlmagic4 extension, global file
only: `SiteConfig <SiteID|"Site name"> <local path to httpd.ini>`.

## 3. Directives

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

## 4. Matching

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

## 5. Format strings

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

## 6. Flags

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

## 7. IIS 4 behaviour

* Hook: `SF_NOTIFY_PREPROC_HEADERS` (rewrite `url`, headers, method), `SF_NOTIFY_LOG` (for `U`).
* Rewrite: `SetHeader("url", new)`; IIS then maps the new URL to a file as usual.
* Redirect: send status line and `Location` with `SF_REQ_SEND_RESPONSE_HEADER`, then
  `SF_STATUS_REQ_FINISHED`.
* Regex failure (match limit hit, etc.): 500, logged to the Windows event log.

## 8. v1 scope summary

In: `RewriteRule`, `RewriteCond`, `RewriteHeader`, `RepeatLimit`, `Enable/DisableConfig`,
`Enable/DisableRewrite`, `UriMatchPrefix`/`UriFormatPrefix`, flags `I L N NS R RP F CL CU O U`,
global + per-site files with auto-reload, the `SiteConfig` extension.

Out: proxying (`RewriteProxy`, `P`, `RFStyle`).

## 9. Build and release

* Portable C core (parser, matcher, formatter) with tests that run on Linux.
* Thin ISAPI layer, cross-compiled with MinGW-w64 (i686), no CRT, NT 4.0 subsystem version.
* Regex: PCRE2 (BSD licence), compiled into the DLL.
* GitHub Actions builds the DLL on each push and attaches it to releases.
