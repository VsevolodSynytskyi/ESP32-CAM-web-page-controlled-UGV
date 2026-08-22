# Working rules for this repo

## Git

**Never run `git commit`. Ever, unless explicitly asked in that message.**

Finish the work, verify it builds, report what changed and in which files, then
stop and leave everything in the working tree. Do not stage it either — leave
`git diff` showing the whole change. The commit is the review, and it belongs to
the user.

The same applies to `git push`, `git reset`, `git checkout` of modified files,
and anything else that rewrites history or discards work.

When a commit *is* asked for: straight onto `main`, no feature branch, no asking
whether to branch. One developer, linear process, nothing to isolate from.

## Build

```bash
export PATH="$HOME/.platformio/penv/bin:$PATH"   # pio is not on PATH
pio run                                          # build
pio run -t upload -t monitor                     # flash and watch
```

One environment, one `main.cpp`. No per-stage build targets — staging is a way
of talking about the work, not a thing the repo carries.

## Hardware claims

Measure before asserting. The documented boot-state pull-ups on this board are
wrong in at least one place, and the camera's effect on WiFi was found by
measuring, not by reading. When a hardware fact matters, print it or probe it.
