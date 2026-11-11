# Changesets

Each change worth a line in the changelog gets a small file here, e.g.
`.changeset/brave-owls-sing.md` (any name):

```md
---
"urlmagic4": minor
---

Add the `QSD` rewrite flag.
```

`patch` = bug fix, `minor` = new feature, `major` = breaking change. Create one with
`npx changeset`, or just write the file by hand. See [docs/releasing.md](../docs/releasing.md).
