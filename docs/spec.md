# urlmagic4 specification

What urlmagic4 does. It reads rules in two formats; use whichever you prefer:

* **Apache mod_rewrite** (`.htaccess` style), the default, described on this page. Rules from
  Apache documentation and tutorials work with little or no change. Behaviour follows the
  Apache httpd documentation for mod_rewrite; where this spec says "as Apache", that document
  is the reference. urlmagic4 implements a subset, listed below.
* **ISAPI_Rewrite 2** (`httpd.ini` style), for compatibility only: existing rule files from that
  product keep working, with exactly its behaviour and nothing added. Described in
  [httpd-ini.md](httpd-ini.md). New features go into the Apache format.

Both formats compile to the same internal rules and share one engine; only parsing and the
few matching differences (whole-string matching, query string in the URL, group numbering)
depend on the format.

## 1. Target

* Windows NT 4.0 SP6a, IIS 4.0, x86. Should also load on IIS 5/6; not tested there.
* One DLL, `urlmagic4.dll`, installed as a **global** ISAPI filter (server properties).
* No C runtime dependency; only `kernel32.dll` (and `advapi32.dll` for the event log) APIs
  present on NT 4.0.
* The filter runs inside `inetinfo.exe`, so a crash takes every site down. Bounded memory
  per request, no recursion on user input, regex with match limits.

## 2. The typical use: a front controller

Send every address that isn't a real file or folder to one script:

```apache
RewriteEngine On
RewriteCond %{REQUEST_FILENAME} !-f
RewriteCond %{REQUEST_FILENAME} !-d
RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]
```

`/about/team?x=1` → `/index.cfm?page=about/team&x=1`; `/assets/site.css` is served as is.

## 3. Configuration files

| File | Where | Applies to |
|---|---|---|
| Server (later) | `urlmagic4.conf` **or** `httpd.ini` in the folder holding `urlmagic4.dll` | every site, runs first |
| Site | `.htaccess` **or** `httpd.ini` in the site's home directory (physical root) | that site |

* The file name picks the format: `.htaccess` and `urlmagic4.conf` are Apache style,
  `httpd.ini` is ISAPI_Rewrite 2 style. So the server file and a site can use different
  formats.
* One file per place: if both exist in the same folder, the Apache-style file is used and a
  warning is logged.
* Apache style: `#` starts a comment; blank lines ignored; a trailing `\` continues a line.
* A file is re-read when it changes (checked at most once per second per file); no restart.
* Errors are written to `urlmagic4.log` next to the DLL, with file and line (rotated at 1 MB
  to `urlmagic4.log.old`). A bad line is skipped; the rest of the file still loads.
* The filter always refuses requests for `.htaccess`, `urlmagic4.conf`, `httpd.ini` and
  `httpd.parse.errors` (any folder, any case) with 403, so rule files can't be downloaded.
* 4.0 reads `.htaccess` only in the site root. **Planned:** a `.htaccess` in any folder, applying
  to that folder and below exactly as in Apache: its rules see the path relative to that
  folder, `RewriteBase` defaults to it, and the deepest `.htaccess` with `RewriteEngine On`
  is the one that runs (no inheritance unless `RewriteOptions Inherit`).

### Sites on network shares

IIS runs filters as LocalSystem, which has no network credentials. For a site whose home
directory is a UNC share (e.g. a dev site served from Samba):

* its `.htaccess` can't be read there: keep the rules on the server's disk and pick that file
  in the site's **URL Magic** tab (*In this file*).
* `-f` / `-d` tests can't see its files and always report "missing". Exclude static folders
  by pattern instead (`RewriteRule ^assets/ - [L]`).

### Per-site settings (URL Magic tab)

Each web site's Properties dialog in Internet Service Manager has a **URL Magic** tab:
rewriting on/off, rules format (automatic by file name, `.htaccess`, `httpd.ini`), and rules
file (the site's home directory, or any file). Stored in
`HKLM\SOFTWARE\urlmagic4\Sites\W3SVC\<site number>` (see `src/common/settings.h`); the filter
re-reads it every few seconds.

## 4. Directives (v1)

| Directive | Syntax |
|---|---|
| `RewriteEngine` | `On` \| `Off` (default Off; each file needs `On`) |
| `RewriteBase` | `URL-path` prefix added to relative substitutions (default `/`) |
| `RewriteCond` | `TestString CondPattern [flags]`; applies to the next `RewriteRule` only |
| `RewriteRule` | `Pattern Substitution [flags]` |
| `Redirect` | `[status] URL-path URL` (prefix match, as Apache mod_alias) |
| `RedirectMatch` | `[status] regex URL` |

Later: `RewriteMap`, `RewriteOptions`, per-folder `.htaccess` files. `<IfModule>` wrapper lines
are ignored and what's inside is read, so copied files still load.

## 5. Matching

* **What `RewriteRule` sees:** the URL path, %-decoded, **without the leading `/`** and
  without the query string (as Apache per-directory context). `/about/team?x=1` → `about/team`.
  The server file sees the same (with a leading `/`, as Apache server context).
* Patterns are PCRE (Perl-compatible) regexes, not anchored unless you write `^`/`$`.
  A leading `!` negates the pattern (then no `$N` groups).
* Rules run once, top to bottom: server file first (later), then the site's `.htaccess`.
  Unlike Apache's per-directory mode, a rewritten URL is **not** run through the rules again,
  so `[L]` really is the last rule and front-controller rules can't loop, even where `-f`
  can't see the files (network shares). `[END]` behaves like `[L]`.

### Variables in `TestString` and `Substitution`

`%{HTTP_HOST}`, `%{HTTP_USER_AGENT}`, `%{HTTP_REFERER}`, `%{HTTP_COOKIE}`, `%{HTTP:Any-Header}`,
`%{REQUEST_URI}`, `%{QUERY_STRING}`, `%{REQUEST_METHOD}`, `%{REQUEST_FILENAME}`,
`%{THE_REQUEST}`, `%{SERVER_NAME}`, `%{SERVER_PORT}`, `%{HTTPS}`, `%{REMOTE_ADDR}`.

### Condition patterns

| Pattern | True when |
|---|---|
| `regex` / `!regex` | matches / doesn't |
| `-f` / `!-f` | `TestString` is an existing file / isn't |
| `-d` / `!-d` | existing folder / isn't |
| `=text`, `<text`, `>text` | string equal / less / greater |

### Back-references

`$1`..`$9` from the rule pattern; `%1`..`%9` from the last matched `RewriteCond`.

## 6. Flags

### `RewriteRule`

| Flag | Meaning |
|---|---|
| `L` | last rule this round |
| `END` | stop all rewriting for this request |
| `NC` | case-insensitive |
| `R`, `R=301` | external redirect (302 default; 301/302/303/307) |
| `QSA` | append the original query string to the new one |
| `QSD` | drop the original query string |
| `F` | 403 Forbidden |
| `G` | 410 Gone |
| `NE` | don't %-escape the substitution |
| `C` | chain: skip the following rules if this one doesn't match |
| `S=N` | skip the next N rules |

Substitution `-` means "no change" (use with `F`, `G`, `L`).
If the substitution has a `?`, its query replaces the original unless `QSA`.

### `RewriteCond`

| Flag | Meaning |
|---|---|
| `NC` | case-insensitive |
| `OR` | OR with the next condition (default is AND) |

Unknown flags: logged, line skipped.

## 7. IIS 4 behaviour

* Hook `SF_NOTIFY_PREPROC_HEADERS`: read `url` and headers, run rules, `SetHeader("url", ...)`.
* `%{REQUEST_FILENAME}` and `-f`/`-d` need the URL mapped to a disk path before IIS does
  it (to be decided: read the site's home directory from the metabase at startup, or defer
  the decision to `SF_NOTIFY_URL_MAP`), then `GetFileAttributes`.
* Redirects, 403, 410: send the status and headers with `SF_REQ_SEND_RESPONSE_HEADER`, return
  `SF_STATUS_REQ_FINISHED`.
* Regex failure (match limit hit, etc.): 500, logged to the event log.
* IIS logs the original URL (urlmagic4 logs rewrites at debug level only).

## 8. Build and release

* Portable C core (parser, matcher, substitution) with tests that run on Linux.
* Thin ISAPI layer, cross-compiled with MinGW-w64 (i686), no CRT, NT 4.0 subsystem version.
* Compile with `-march=i486`: NT 4.0 boxes (and emulators of them) often have pre-P6 CPUs
  without `CMOV`; MinGW's default (`pentiumpro`) crashes there with 0xC000001E.
* PCRE2 (BSD licence) compiled into the DLL.
* GitHub Actions builds the DLL on every push and attaches it to releases.
