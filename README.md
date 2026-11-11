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

Sites are off until you turn them on, so URL Magic costs nothing where it isn't used.
No tab? **Console → Add/Remove Snap-in → Extensions**, tick **URL Magic**, then **Console → Save**.

Supported rules: [docs/spec.md](docs/spec.md). Errors go to `urlmagic4.log` in the install folder.
To build: `make`, with the MinGW-w64 i686 cross-compiler.

## License

MIT. Bundles [PCRE2](https://github.com/PCRE2Project/pcre2) (BSD).
