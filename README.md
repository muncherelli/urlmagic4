<p align="center"><img src="docs/logo.png" alt="The URL Magic tab" width="332"></p>

# urlmagic4

**The missing IIS tab.** URL rewriting for IIS 4.0 on Windows NT 4.0, with Apache mod_rewrite rules.

Turn it on in a site's **Properties → URL Magic** tab, then put an `httpd.htaccess` in the
site's home directory:

```apache
RewriteEngine On
RewriteCond %{REQUEST_FILENAME} !-f
RewriteCond %{REQUEST_FILENAME} !-d
RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]
```

Changes take effect within seconds. No restart.

## Install

Close Internet Service Manager and run `urlmagic<version>.exe` from
[releases](https://github.com/muncherelli/urlmagic4/releases). Run it again and press **U** to uninstall.

### Unattended

```
urlmagic<version>.exe install [folder] [/quiet] [/norestart | /stopiis] [/notab] [/order first|last]
urlmagic<version>.exe uninstall [/purge] [/quiet] [/norestart | /stopiis]
urlmagic<version>.exe version
urlmagic<version>.exe status [/quiet]
urlmagic<version>.exe validate <file>
urlmagic<version>.exe site <id> enable|disable [/quiet]
urlmagic<version>.exe site <id> set key=value [key=value ...] [/quiet]
```

- `/quiet`: no banner, colours or spinner, safe with redirected output. The last (only) line is
  `result=ok code=0`, `result=error code=N msg=<text>`, or `result=pending code=N msg=<text>` for
  exit codes 3 and 4 only: a reboot finishes the job.
- `/norestart`: stop and start no service. A file that is in use (the filter DLL IIS has loaded) is put
  in place at the next reboot instead, and the exit code is 4. The filter loads at the next W3SVC start.
- `/stopiis`: stop IISADMIN, which takes W3SVC, MSFTPSVC and SMTPSVC down with it, then start back
  what was running. Without it only W3SVC is stopped, and it is started again only if it was running.
- `/notab`: don't register the URL Magic tab (install only).
- `/order first|last`: where urlmagic4 goes in IIS's `FilterLoadOrder` (default: before `sspifilt`;
  left where it is if already listed). Other filters are never moved.
- Installing what is already installed (same version, same files, same filter settings), or removing
  what is gone, changes nothing and exits 0.
- `version` prints the version number only; with `/quiet` it prints `result=ok code=0 version=X`.
- Run from the full-screen or console menu, an install starts W3SVC again only if it was running, and
  a system check that fails exits 5.

| Exit code | Meaning |
| --- | --- |
| 0 | done (or nothing to do) |
| 1 | error |
| 2 | wrong command line |
| 3 | reboot needed: uninstall left files that go at the next reboot |
| 4 | files were staged: in use, so replaced at the next reboot (`/norestart`). A file that stays locked after a minute without `/norestart` is an error (1), nothing staged |
| 5 | not Windows NT, or IIS isn't installed |
| 6 | a site number, setting or value isn't valid (`site`), or a rules file has problems (`validate`) |

### Status and per-site settings

`status` prints the machine's state, one `key=value` per line (CRLF, no JSON, no value ever holds
a line break). It works on a machine without urlmagic4 too (`installed=0`):

```
version=4.2.0                  the installed version (empty if not installed)
setup_version=4.2.0            this program's version
installed=1
dir=C:\URLMagic4
filter_state=enabled           absent | unlisted | disabled | enabled | unknown
mmc_tab.registered=1           the tab's COM class is registered
mmc_tab.current=1              ... and points at the installed copy
site.3.enabled=1               one group for each site that has settings
site.3.format=auto
site.3.source=docroot
site.3.rulesfile=
site.3.debug=0
```

`filter_state` is IIS's metabase: `unlisted` means the filter's key exists but isn't in
`FilterLoadOrder`, `unknown` that the metabase couldn't be opened. Which MMC consoles show the tab
is not reported.

`site <id> enable|disable` turns rewriting on or off for W3SVC site `<id>`, and `site <id> set`
changes any of `enabled=0|1`, `format=auto|htaccess|httpd.ini`, `source=docroot|file`,
`rulesfile=<full path, drive or UNC; empty clears it>` and `debug=0|1`. They are the same values the
URL Magic tab writes (`HKLM\SOFTWARE\URLMagic4\Sites\W3SVC\<id>`), so the filter picks them up
within seconds and there is no restart. Everything is checked before anything is written (exit 6
for a bad number, key or value, and for `source=file` without a `rulesfile`), and the site's
settings are then printed as `site.<id>.*` lines. The number isn't checked against IIS, so a
site can be configured before or after it is created. Writing needs an administrator (exit 1
otherwise).

With `/quiet`, the lines are followed by the usual `result=` line, and an error (including a wrong
`status` or `site` command line) is only that line; without it an error is a single `error=<text>`
line.

`dir` and `rulesfile` are printed in the console's OEM code page: a character it can't show
comes out as `?`, and control characters (below a space, and 0x7F to 0x9F) as a space.

### Checking a rules file

`validate <file>` reads a rules file with the filter's own parser, so a typo is found before
it reaches inetinfo. Each problem is one line, `file(line): message`, then a result line:

```
rules.txt(3): bad regular expression ...
result=error code=6 errors=1
```

`result=ok code=0 rules=N` means the file is fine; exit 6 means problems, 1 that the file can't be
read, 2 a wrong command line. A file over 1 MB is a problem too (the filter ignores it). It prints
those lines with or without `/quiet`.

The same check runs on Linux, for validating centrally before upload: `make validate` builds
`build/urlmagic4-validate` (static, from the same engine sources), and each release carries it as
`urlmagic4-validate-<version>-linux-x86_64`. `urlmagic4-validate <file>` prints the same lines
and uses the same exit codes. Releases also carry a bin-only zip (`urlmagic<version>-bin.zip`:
the installer and the two DLLs) and a `SHA256SUMS` file covering every asset.

The filter is registered enabled (`FilterEnabled`) with a `FilterDescription`.

Sites are off until you turn them on, so URL Magic costs nothing where it isn't used.
No tab? **Console → Add/Remove Snap-in → Extensions**, tick **URL Magic**, then **Console → Save**.

Supported rules: [docs/spec.md](docs/spec.md). Errors go to `urlmagic4.log` in the install folder.
To build: `make`, with the MinGW-w64 i686 cross-compiler.

## License

MIT. Bundles [PCRE2](https://github.com/PCRE2Project/pcre2) (BSD).
