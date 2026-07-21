# STAGED — history scrub for exposed Android signing key

**STATUS: NOT EXECUTED. Do not run until Shin gives the coordinated go.**
Running the `git filter-repo` step rewrites every commit hash on `main` (and every
other branch/tag) from the beginning of history forward. Every existing clone,
fork, and open PR becomes stale and must be re-cloned or hard-reset to the new
history. This is why it is staged, not applied.

## What's already done (safe, non-destructive)
- New keystore generated (`keytool`, 4096-bit RSA, fresh random password),
  installed at `android/hone-release.keystore` (gitignored, not tracked).
  Password stored only in gitignored `android/keystore.properties` on this
  machine — **Shin must copy the password out to a durable, separate store
  (the local 2FA vault or an `age`-encrypted `pc-secrets` entry) and not rely
  on this working copy as the sole holder.**
- `build.gradle` reads signing config from `keystore.properties` instead of
  literals. Template at `android/keystore.properties.example`.
- Old tracked keystore removed from HEAD via `git rm --cached`, committed on
  branch `security/keystore-rotation`, pushed. **Not merged to main.**
- Full pre-scrub backup bundle (all refs, full history) at:
  `X:\HONE_SECURITY_BACKUPS_LOCAL_ONLY\keystore-scrub-20260721\hone-full-backup-pre-scrub.bundle`
  (174MB, `git bundle verify`'d OK — "records a complete history"). This path
  is local-only, outside any git remote, not synced.

## Old key is already worthless
Rotation alone (above) defangs the leak — the exposed key can no longer sign
anything that matters going forward, whether or not the history scrub ever
runs. The scrub below is belt-and-suspenders: it removes the compromised blob
and plaintext passwords from history so they aren't sitting in every clone.

## What history actually contains (confirmed by `git log --all`)
Two keystore filenames were committed across history:
- `android/hone-release.keystore`
- `android/btcpc-release.keystore`

Two plaintext passwords appear in `build.gradle` history (both now rotated away):
- `hone_release_2026`
- `btcpc_release_2026`

## Staged commands (run only after the coordinated go-ahead)

### 0. Prerequisite — install git-filter-repo (not present on this box)
```bash
pip install git-filter-repo
# or: choco install git-filter-repo   (Windows)
# verify:
git filter-repo --version
```

### 1. Work from a FRESH throwaway clone, never the working copy in daily use
```bash
git clone --mirror https://github.com/shindevlin/hone.git hone-scrub-mirror.git
cd hone-scrub-mirror.git
```

### 2. Purge the keystore blobs (by path, across all history/branches/tags)
```bash
git filter-repo --force \
  --path android/hone-release.keystore \
  --path android/btcpc-release.keystore \
  --invert-paths
```

### 3. Purge the plaintext passwords from any historical blob content
```bash
cat > /tmp/replacements.txt <<'EOF'
hone_release_2026==>REDACTED
btcpc_release_2026==>REDACTED
EOF
git filter-repo --force --replace-text /tmp/replacements.txt
```

(Steps 2 and 3 can be combined into one `git filter-repo` invocation with both
`--path`/`--invert-paths` and `--replace-text` flags if preferred — kept
separate here for clarity and so each step's effect can be checked before the
next.)

### 4. Verify the purge worked before touching the remote
```bash
git log --all --oneline -- android/hone-release.keystore android/btcpc-release.keystore
# must be EMPTY
git log --all -p | grep -E "hone_release_2026|btcpc_release_2026"
# must be EMPTY
```

### 5. Force-push — THE COORDINATED STEP, requires Shin in person
```bash
git remote add origin https://github.com/shindevlin/hone.git   # filter-repo strips origin by default
git push --force --all origin
git push --force --tags origin
```
After this: every existing local clone (all bridge nodes: beastly, grouchly,
and any others) has diverged history and **must re-clone** or
`git fetch && git reset --hard origin/<branch>` on every branch they track —
a stale clone that pushes again will resurrect the purged blobs/passwords.

### 6. Post-scrub cleanup on GitHub (recommended, ask Shin)
- GitHub caches old commits reachable from PRs/forks — consider whether any
  open PRs/forks need a heads-up or re-creation.
- Contact GitHub support to purge cached views of the removed blob if maximum
  assurance is required (rare; rotation + history rewrite is normally sufficient).

## PARK
Posting this as staged only. **Not running step 5 (force-push) or anything
after it without Shin confirming in person.**
