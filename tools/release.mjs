#!/usr/bin/env node
// Publish the current version as a GitHub release (called by the Release workflow).
// Expects `make dist` to have run. Does nothing if the tag already exists.
import { execFileSync } from "node:child_process";
import { readFileSync, existsSync } from "node:fs";

const { version } = JSON.parse(readFileSync("package.json", "utf8"));
const tag = `v${version}`;
const run = (cmd, args, opts = {}) => execFileSync(cmd, args, { encoding: "utf8", ...opts });

const exists = run("git", ["ls-remote", "--tags", "origin", `refs/tags/${tag}`]).trim() !== "";
if (exists) {
  console.log(`${tag} already released; nothing to do`);
  process.exit(0);
}

const zip = `build/urlmagic4-${version}.zip`;
if (!existsSync(zip)) throw new Error(`${zip} missing; run make dist first`);

// notes = this version's section of CHANGELOG.md
const changelog = readFileSync("CHANGELOG.md", "utf8");
const m = changelog.match(new RegExp(`^## ${version.replace(/\./g, "\\.")}\\s*\\n([\\s\\S]*?)(?=^## |$(?![\\s\\S]))`, "m"));
const notes = (m ? m[1].trim() : "") || `urlmagic4 ${version}`;

run("gh", ["release", "create", tag, zip, "--target", process.env.GITHUB_SHA || "trunk",
           "--title", tag, "--notes", notes], { stdio: "inherit" });
// changesets/action looks for this line to know something was published
console.log(`New tag: ${tag}`);
