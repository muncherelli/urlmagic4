# urlmagic4

## 4.0.0

First release (in development).

- ISAPI filter for IIS 4.0 on Windows NT 4.0 (`urlmagic4.dll`), built for 486 and later CPUs.
- Apache mod_rewrite rules in a site's `.htaccess`: `RewriteEngine`, `RewriteBase`,
  `RewriteCond` (regex, `-f`, `-d`, `=`, `<`, `>`, flags `NC`, `OR`), `RewriteRule` (flags `L`,
  `END`, `NC`, `R`, `QSA`, `QSD`, `F`, `G`, `NE`, `C`, `S`), `Redirect`, `RedirectMatch`.
  Regular expressions are PCRE2.
- Rules files reload automatically when they change; problems are logged to `urlmagic4.log`.
- Requests for rule files (`.htaccess`, `httpd.ini`, ...) are refused with 403.
- URL Magic 4 tab in Internet Service Manager: per site, turn rewriting on or off and choose
  the rules format and file.
- Console `setup.exe` to install and uninstall.
