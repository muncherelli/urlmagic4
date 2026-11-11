# PCRE2 (bundled)

The library part of [PCRE2](https://github.com/PCRE2Project/pcre2) **10.49**, used for
urlmagic4's regular expressions. BSD licence: see [LICENCE.md](LICENCE.md).

* Source: `pcre2-10.49.tar.gz` from the PCRE2 GitHub release,
  sha256 `929f0b20e62879252a15886b06c89f1edef61a363cbd5826fb041080a5e557ae`.
* Copied unchanged from its `src/`: the library `.c` files except JIT, DFA matching,
  pattern conversion, serialisation, substitution and substrings (not used); plus
  `pcre2_chartables.c.dist` → `pcre2_chartables.c`, `pcre2.h.generic` → `pcre2.h`,
  `config.h.generic` → `config.h`.
* Built without Unicode or JIT; limits set by the Makefile (`MATCH_LIMIT`, `HEAP_LIMIT`).
