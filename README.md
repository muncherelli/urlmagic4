# urlmagic4

URL rewriting for **IIS 4.0 on Windows NT 4.0**, as an ISAPI filter.

Pretty URLs, 301 redirects and conditional rewrites for sites still served by IIS 4,
configured with mod_rewrite-style rules in a plain text file.

> **Status:** early design. Nothing to download yet.

## Goals

* Runs on NT 4.0 SP6a / IIS 4.0 (x86); no runtime to install.
* Rule syntax compatible with the `httpd.ini` format used by ISAPI_Rewrite 2, so existing
  rule files keep working.
* Per-site rule files.
* Small and auditable: the filter runs inside IIS, so it must never crash it.

urlmagic4 is an independent project, written from public documentation and black-box
testing. It is not affiliated with Helicon Tech; ISAPI_Rewrite is their trademark.

## License

MIT. See [LICENSE](LICENSE).
