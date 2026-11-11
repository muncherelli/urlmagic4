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

## License

MIT. See [LICENSE](LICENSE).
