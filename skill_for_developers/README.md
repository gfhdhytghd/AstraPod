# Skills for developers

Claude Code skills for working on this tree. `.claude/` is git-ignored, so
link each skill in after cloning:

```sh
mkdir -p .claude/skills
for s in skill_for_developers/*/; do
    ln -sfn "../../$s" ".claude/skills/$(basename "$s")"
done
```

| Skill | Use |
|---|---|
| `podlink-dev` | PodLink, the media protocols, EP0 commands, HerdRemote, the desktop app and input plugins |
