# musl (ld-musl, under every program): what the app's tools run, all in one
# run (Java and Maven, Kimi Code, Claude Code), and a shell's basics.
sh /tmp/aojit/java/java_train.sh
sh /tmp/aojit/kimi/kimi_train.sh
sh /tmp/aojit/claude/claude_train.sh
seq 1 20000 | sort -rn | head -1
find /usr/lib -name '*.so*' | wc -l
grep -c root /etc/passwd
ls -la /usr/bin | wc -l
