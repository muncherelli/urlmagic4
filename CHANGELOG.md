# Changelog

## [4.1.1](https://github.com/muncherelli/urlmagic4/compare/v4.1.0...v4.1.1) (2026-11-12)


### Bug Fixes

* security hardening of the filter, engine and setup ([#13](https://github.com/muncherelli/urlmagic4/issues/13)) ([c8e6a72](https://github.com/muncherelli/urlmagic4/commit/c8e6a72ad0340c7be3c70ac51fd588865c9d80cd))
* **tab:** fill the property sheet's width; repo link beside the switch ([#11](https://github.com/muncherelli/urlmagic4/issues/11)) ([7db96db](https://github.com/muncherelli/urlmagic4/commit/7db96db4ff016da55f5236058b96f25c7caeaa77))

## [4.1.0](https://github.com/muncherelli/urlmagic4/compare/v4.0.0...v4.1.0) (2026-11-11)


### Features

* sites are off until turned on; faster rule matching ([#9](https://github.com/muncherelli/urlmagic4/issues/9)) ([1b6a506](https://github.com/muncherelli/urlmagic4/commit/1b6a5061cc686982cdb2ffda1cc50e181f7ebc4e))


### Bug Fixes

* **engine:** clean up the fast path's test switch and %00 handling ([#10](https://github.com/muncherelli/urlmagic4/issues/10)) ([a9761be](https://github.com/muncherelli/urlmagic4/commit/a9761be5fcf68db9917bf5fe86cfc2edc1fd3c0d))
* **setup:** leave the README's HTML logo line out of README.txt ([#7](https://github.com/muncherelli/urlmagic4/issues/7)) ([2339a34](https://github.com/muncherelli/urlmagic4/commit/2339a34ce5269659528506d72e45815c077359ed))

## 4.0.0 (2026-11-11)


### Features

* **engine:** Apache mod_rewrite rule engine on bundled PCRE2, with tests ([a5a5487](https://github.com/muncherelli/urlmagic4/commit/a5a5487006bd00dce6ccb65dc8e65125423baa64))
* **filter:** ISAPI filter and console setup.exe; Apache rules work end to end ([795f2af](https://github.com/muncherelli/urlmagic4/commit/795f2af555dbdc8f2d62cc03a21f874b77362541))
* per-site debug logging with timings; install the docs as Notepad-friendly .txt ([bf51655](https://github.com/muncherelli/urlmagic4/commit/bf51655c5b07d5db4c42e39b55a37c2955dcd487))
* read Apache rules from httpd.htaccess (NT's Explorer can't create .htaccess), .htaccess still works ([53888f0](https://github.com/muncherelli/urlmagic4/commit/53888f07264664f1022896f1e7e8cc6ca4f1e89c))
* registry key HKLM\SOFTWARE\URLMagic4; log every configured site when the filter loads ([614bc30](https://github.com/muncherelli/urlmagic4/commit/614bc30edea14a136cfaa5e651dadc8febee0d7d))
* **setup:** ask whether to delete site settings when uninstalling ([f175ee3](https://github.com/muncherelli/urlmagic4/commit/f175ee3553f3abe69bfa5dda549fb6999efc9b82))
* **setup:** colour console with a block-letter logo, spinner and step results; confirm before stopping IIS ([7a5cb9b](https://github.com/muncherelli/urlmagic4/commit/7a5cb9bc0c6b02a8c4a75cc182d29e227b8784a9))
* **setup:** credit line under the logo on the splash screen ([40cd981](https://github.com/muncherelli/urlmagic4/commit/40cd981ae65d93234b9586ba00bf947e5bd039bc))
* **setup:** default install folder C:\URLMagic4 ([be99449](https://github.com/muncherelli/urlmagic4/commit/be994497962f97525c31409c7e5f3f7992eb386f))
* **setup:** draw the wand icon in the console banner and on the console window ([067c772](https://github.com/muncherelli/urlmagic4/commit/067c77210315f8ce38533f8231ed9abcf3d3fa7d))
* **setup:** full-screen splash with system check and one-screen progress; Add/Remove Programs entry ([1de0585](https://github.com/muncherelli/urlmagic4/commit/1de0585f0312374bd105f5e54a30ba7e0882c99c))
* **setup:** make the installer one self-contained exe with the DLLs inside ([71830e2](https://github.com/muncherelli/urlmagic4/commit/71830e2cec191432ced142b1691720ca63dd2524))
* **setup:** name the installer urlmagic4-v&lt;version&gt;.exe; add a 4 to the icon and a README logo ([843b39b](https://github.com/muncherelli/urlmagic4/commit/843b39b2c852cb81b22c51916d25bcba0969462c))
* **setup:** Reinstall / Upgrade / Downgrade depending on the installed version ([66d79c6](https://github.com/muncherelli/urlmagic4/commit/66d79c6f74f1aab9b17a1992d2116b9cca607fc8))
* **setup:** tighter splash: no in-screen title, two-row system check, CTRL+C to quit, space above the key bar ([15c0903](https://github.com/muncherelli/urlmagic4/commit/15c09036ab3475012cc612ab3e06fbd1bd1162f1))
* **setup:** type the install folder on the splash; roomier system check without the slogan ([628aab5](https://github.com/muncherelli/urlmagic4/commit/628aab53350dcd1f698308688ae74cc4f07a026d))
* **setup:** urlmagic4.0.0.exe asks before acting, checks for NT 4.0 and IIS 4.0, waits before closing; tab registers as a required IIS extension ([28a394e](https://github.com/muncherelli/urlmagic4/commit/28a394e41198b874eb83d82c95dd59b4e15a499c))
* **spike:** log loads and MMC calls to umtab.log ([2698bb9](https://github.com/muncherelli/urlmagic4/commit/2698bb928050b9c35aa95036993e4be2fd1284a8))
* **spike:** MMC property-sheet extension to test an IIS 4 manager tab ([3ac7029](https://github.com/muncherelli/urlmagic4/commit/3ac702921aa8aec2d4e8ed089917f870f5da9a91))
* **spike:** probe every registered clipboard format and log the sheet title ([4d00a1a](https://github.com/muncherelli/urlmagic4/commit/4d00a1aee43181084b8abeacde393ee4172d27e7))
* **tab:** label the tab "URL Magic" (the 4 is in the icon); README picture shows the tab ([efcbbe1](https://github.com/muncherelli/urlmagic4/commit/efcbbe1dadf424b2689748d6de01420cd10d7eba))
* **tab:** rename to urlmagic4-mmc-snap-in.dll; MMC lists it as "URL Magic 4" ([58f11a1](https://github.com/muncherelli/urlmagic4/commit/58f11a1d1454b21e1b44b66a541a2010786cd144))
* **tab:** URL Magic 4 tab in IIS manager to set each site's rules format and file ([d47cc5d](https://github.com/muncherelli/urlmagic4/commit/d47cc5d4b5e0552a7a0fd5598c52d7403eeab115))


### Bug Fixes

* **build:** probe the stack for big frames ([#3](https://github.com/muncherelli/urlmagic4/issues/3)) ([ebe2edc](https://github.com/muncherelli/urlmagic4/commit/ebe2edc79a56ed2f089058a0de4fa3b4eb49f03a))
* **filter:** write microseconds as µs in the debug log ([da00e74](https://github.com/muncherelli/urlmagic4/commit/da00e7461db59d57a970fefbc11e5a542afa61fe))
* **setup:** line up the right edge of the logo box ([0ae23c3](https://github.com/muncherelli/urlmagic4/commit/0ae23c3f9d3190031052854bcc5914826dd86485))
* **setup:** wait for IIS to let go of the old DLL ([#4](https://github.com/muncherelli/urlmagic4/issues/4)) ([cfc5e7f](https://github.com/muncherelli/urlmagic4/commit/cfc5e7fa7f92ca630872ac4e775e801b1f9e02da))
* **spike:** build for 486/Pentium CPUs; CMOV crashed regsvr32 on NT4 ([7522da7](https://github.com/muncherelli/urlmagic4/commit/7522da7025cbcd4a9d5872c0906977860fbeed48))


### Performance Improvements

* make the request path fast ([#2](https://github.com/muncherelli/urlmagic4/issues/2)) ([2a89113](https://github.com/muncherelli/urlmagic4/commit/2a89113d4fc792d47bb0fecf3d6289837fc63549))

## Changelog
