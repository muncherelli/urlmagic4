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

> **Status:** early design. Nothing to download yet.

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

## Install (IIS manager tab)

The **URL Magic 4** tab in Internet Service Manager sets, per web site, whether rewriting is on,
which rule format to use, and where the rules file is.

1. Copy `urlmagic4tab.dll` to a folder on the server's local disk, e.g. `C:\urlmagic4\`.
2. Register it: `regsvr32 C:\urlmagic4\urlmagic4tab.dll`
3. Open **Internet Service Manager** and enable it once: **Console → Add/Remove Snap-in →
   Extensions**, choose *Internet Information Server*, tick **URL Magic 4**, OK. Then
   **Console → Save** so it stays enabled.
4. Right-click a web site → **Properties** → **URL Magic 4**.

Settings are stored in `HKLM\SOFTWARE\urlmagic4\Sites\W3SVC\<site number>`.
To remove the tab: `regsvr32 /u C:\urlmagic4\urlmagic4tab.dll` (site settings are kept).

## Building

On Linux with the MinGW-w64 i686 cross-compiler (`gcc-mingw-w64-i686` on Debian,
`mingw-w64-gcc` on Arch): `make`. The DLLs land in `build/`.

## License

MIT. See [LICENSE](LICENSE).
