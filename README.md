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
```

- `/quiet`: no banner, colours or spinner, safe with redirected output. The last (only) line is
  `result=ok code=0` or `result=error code=N msg=<text>`.
- `/norestart`: stop and start no service. A file that is in use (the filter DLL IIS has loaded) is put
  in place at the next reboot instead, and the exit code is 4. The filter loads at the next W3SVC start.
- `/stopiis`: stop IISADMIN, which takes W3SVC, MSFTPSVC and SMTPSVC down with it, then start back
  what was running. Without it only W3SVC is stopped, and it is started again only if it was running.
- `/notab`: don't register the URL Magic tab (install only).
- `/order first|last`: where urlmagic4 goes in IIS's `FilterLoadOrder` (default last). `sspifilt` always
  stays last.
- Installing what is already installed (same version, same files, same filter settings), or removing
  what is gone, changes nothing and exits 0.
- `version` prints the version number only.

| Exit code | Meaning |
| --- | --- |
| 0 | done (or nothing to do) |
| 1 | error |
| 2 | wrong command line |
| 3 | reboot needed: uninstall left files that go at the next reboot |
| 4 | a file was in use; it is replaced at the next reboot (`/norestart`), or it stayed locked |
| 5 | not Windows NT, or IIS isn't installed |
| 6 | validation failed (reserved) |

The filter is registered enabled (`FilterEnabled`) with a `FilterDescription`.

Sites are off until you turn them on, so URL Magic costs nothing where it isn't used.
No tab? **Console → Add/Remove Snap-in → Extensions**, tick **URL Magic**, then **Console → Save**.

Supported rules: [docs/spec.md](docs/spec.md). Errors go to `urlmagic4.log` in the install folder.
To build: `make`, with the MinGW-w64 i686 cross-compiler.

## License

MIT. Bundles [PCRE2](https://github.com/PCRE2Project/pcre2) (BSD).
