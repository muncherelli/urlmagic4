<p align="center"><img src="docs/logo.png" alt="URL Magic 4" width="464"></p>

# urlmagic4

URL rewriting for **IIS 4.0 on Windows NT 4.0**, as an ISAPI filter.

Pretty URLs, 301 redirects and conditional rewrites for sites still served by IIS 4,
configured with Apache mod_rewrite rules in a `.htaccess` file:

```apache
RewriteEngine On
RewriteCond %{REQUEST_FILENAME} !-f
RewriteCond %{REQUEST_FILENAME} !-d
RewriteRule ^(.*)$ index.cfm?page=$1 [L,QSA]
```

> **Status:** 4.0.0 in development: Apache-style `.htaccess` rules work; `httpd.ini` support is next.

## Goals

* Runs on NT 4.0 SP6a / IIS 4.0 (x86); no runtime to install.
* Apache mod_rewrite syntax, so rules from tutorials and Apache docs work as is.
* Also reads ISAPI_Rewrite 2 `httpd.ini` files, so existing rules keep working.
* A `.htaccess` per site, plus one server-wide file. Changes apply without restarting IIS.
* Small and auditable: the filter runs inside IIS, so it must never crash it.

See [docs/spec.md](docs/spec.md) for what's supported, and
[docs/httpd-ini.md](docs/httpd-ini.md) for the `httpd.ini` format.

urlmagic4 is an independent project, not affiliated with Helicon Tech; ISAPI_Rewrite is
their trademark.

## Install

Requirements: Windows NT 4.0 (SP6a recommended) with IIS 4.0, any 486 or later CPU.

1. Download the installer `urlmagic4-v<version>.exe` from the
   [releases](https://github.com/muncherelli/urlmagic4/releases) and copy it to the server.
   It's a single file: both DLLs and the docs are inside it.
2. Close Internet Service Manager, then run it from a command prompt, e.g.:

   ```
   urlmagic4-v4.0.0
   ```

   It's a console program. It stops the WWW service, unpacks everything (and a copy of itself) to
   `%SystemDrive%\urlmagic4`, registers the URL Magic 4 tab, adds urlmagic4 to IIS's ISAPI
   filters and starts the WWW service again, printing each step.
   (`urlmagic4-v4.0.0 install D:\some\folder` installs elsewhere.)
3. Show the tab, once: open **Internet Service Manager**, **Console → Add/Remove Snap-in →
   Extensions**, choose *Internet Information Server*, tick **URL Magic 4**, OK, then
   **Console → Save**.
4. Put a `.htaccess` in a web site's home directory (see the example above). Changes are
   picked up within a couple of seconds; no restart needed.

Each web site's **Properties → URL Magic 4** tab turns rewriting on or off and chooses the
rules format and file. Settings live in `HKLM\SOFTWARE\urlmagic4\Sites\W3SVC\<site number>`.
Problems in rules files are written to `urlmagic4.log` in the install folder.

**Uninstall:** `C:\urlmagic4\urlmagic4-v4.0.0 uninstall` (keeps each site's settings), or add
`/purge` to remove those too.

**Sites whose home directory is a network share:** IIS runs filters as LocalSystem, which
can't read network shares. Keep that site's rules file on the server's own disk and choose it
with *In this file* on the tab; `-f`/`-d` tests can't see the site's files, so match static
folders by name instead (`RewriteRule ^assets/ - [L]`).

## Building

On Linux with the MinGW-w64 i686 cross-compiler (`gcc-mingw-w64-i686` on Debian,
`mingw-w64-gcc` on Arch):

* `make` builds `build/urlmagic4.dll`, `build/urlmagic4tab.dll` and the installer
  `build/urlmagic4-v<version>.exe`
* `make test` runs the rewrite engine's tests natively (needs `gcc`)
* `make dist` makes the release zip
* `python3 tools/make_art.py` redraws the icon and `docs/logo.png` from pixel art

Releases use [semver](https://semver.org) and release-please (from Conventional Commits): see
[docs/releasing.md](docs/releasing.md).
Regular expressions are [PCRE2](https://github.com/PCRE2Project/pcre2) (BSD licence), bundled
in `third_party/pcre2`.

## License

MIT. See [LICENSE](LICENSE).
