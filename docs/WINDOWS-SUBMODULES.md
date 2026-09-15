# Windows submodule checkout — issues and fixes

The checkout pulls nested submodules (paraLLEl-GS -> Granite -> ~20 more), so a
plain `git submodule update --init --recursive` on Windows can fail in two
independent ways. Both were hit on a clean clone (2026-09-15) and are fixed as
follows.

## 1. `error: unable to create file ...: Filename too long`

SPIRV-Cross and SPIRV-Tools commit reference `.comp` shader files whose full
paths exceed the legacy 260-character Windows limit, so `git checkout` fails on
those trees even though the submodule is marked "checked out".

Enable long paths in Git (repo-local, no admin rights needed):

```sh
git config core.longpaths true
```

Re-run the recursive update; the long-path files then check out cleanly.

## 2. `fatal: Unable to find current revision in submodule path '.../fossilize/rapidjson'`

Interrupting/aborting the recursive clone leaves the fossilize `rapidjson`
submodule with a registered `.git` pointer but no working tree and a broken
gitdir (`fatal: your current branch appears to be broken`). The recursive
update then fails at that point on every retry.

Repair it from inside the fossilize checkout:

```sh
cd ps2xRuntime/third_party/parallel-gs/Granite/third_party/fossilize
git submodule deinit -f rapidjson
git submodule update --init rapidjson
```

`deinit` clears the broken worktree/local config placeholders; `update --init`
re-clones rapidjson and its `thirdparty/gtest`.

## Verification

After the fixes, from the repository root:

```sh
git submodule update --init --recursive
git submodule status          # no '-' or '+' prefixes left
git submodule status --recursive --quiet   # clean exit if everything resolves
```

A note on transient errors: `fatal: 'submodule' appears to be a git command,
but we were not able to execute it. Maybe git-submodule is broken?` was seen
twice in the middle of the recursive run; it resolved on retry and did not
recur. If it persists, check `git --exec-path`/`git-submodule` is intact and try
again — it is not a repo state problem.