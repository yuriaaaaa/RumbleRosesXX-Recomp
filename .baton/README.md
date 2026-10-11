# Baton Pass: how Claude and ChatGPT share this port

This folder is the hand-off point between two assistants that work for David on the Xbox 360 Android recomp ports. Neither assistant can see the other's conversations. The only thing both can read is this GitHub repository, so everything one needs from the other goes in a file here.

Format based on [blader/baton](https://github.com/blader/baton), with one change: batons here are **committed**, not git-ignored, because that is how the other assistant gets them.

## Who does what

| | Claude (claude.ai project "XBOX360 Android Recomp") | ChatGPT |
|---|---|---|
| Main job | Code changes, native/Gradle builds, log analysis, commits to this fork | Research, reading upstream repos and docs, finding and downloading files, writing up findings |
| Writes to the repo | Yes, pushes commits | Only if David's ChatGPT GitHub connection allows writes. Otherwise ChatGPT writes the baton text and David commits it (GitHub web: Add file → Create new file) or pastes it to Claude |
| Builds APKs | Yes, in a cloud session | No |

Research and downloads go to ChatGPT first, to save Claude usage. Claude only does research a task cannot proceed without.

## Files

- `.baton/<YYYY-MM-DD>-<from>-<slug>.md`, where `<from>` is `claude` or `chatgpt`. The newest file is the current hand-off: `ls .baton/ | sort | tail -1`.
- `.baton/README.md` is this protocol. Don't delete it.

## Grabbing a baton (starting work)

1. Read this README, then the newest baton.
2. Check its "State of play" against the repo as it is now (latest commit on `main`). If they disagree, trust the repo and say so.
3. Do the "Next task for ChatGPT" or "Next task for Claude" section that is addressed to you.

## Dropping a baton (handing back)

Write a new file in the format below. Fill every section; write "none" if a section is empty. Keep it short. Separate what was **verified** (ran it, read the file, saw the log) from what is **assumed**, and give a source for each verified claim (commit, file:line, URL, log name).

```markdown
# Baton: <title>
From: claude | chatgpt · Date: YYYY-MM-DD · Repo commit: <short sha>

**TL;DR:** <where it stands and the single next action>

## Goal
## State of play (verified)
## Assumed / unverified
## Findings and dead ends
## Next task for <ChatGPT|Claude>
## Open questions for David
```

## Rules both assistants follow

- Never commit or link game files: `default.xex`, ISOs, extracted game data, shader archives, generated/recompiled code, APKs, keystores or signing passwords.
- No prebuilt APKs in releases. Releases are source-only, and players build from their own legally obtained copy. David makes releases and tags himself.
- Don't rewrite history on `main`. Batons are added, not edited; retire old ones by deleting them in a later commit once the work has landed.
- Downloads ChatGPT finds (drivers, tools, upstream archives): give the URL, version and SHA-256 if published, and the licence. Don't commit third-party binaries.

## Related ports

- WET: [yuriaaaaa/WetRecomp](https://github.com/yuriaaaaa/WetRecomp)
- Rumble Roses XX: [yuriaaaaa/RumbleRosesXX-Recomp](https://github.com/yuriaaaaa/RumbleRosesXX-Recomp)
- Sonic '06 (Marathon Recompiled): [yuriaaaaa/MarathonRecomp](https://github.com/yuriaaaaa/MarathonRecomp)
- Dragon Ball Raging Blast 2: no fork yet (upstream [WistfulHopes/RB2](https://github.com/WistfulHopes/RB2)). Its baton is handed over as a file by David until yuriaaaaa/RB2 exists.
