# bash for the app's tools: Claude Code runs every command with bash (and Kimi
# Code its own shell commands): its start, as a login shell too, and what the
# commands and scripts do: loops, arithmetic, strings, arrays, functions,
# subshells, pipes and globs.
for _ in $(seq 1 20); do bash -c 'echo started' > /dev/null; done
for _ in $(seq 1 5); do bash -l -c 'echo login' > /dev/null; done
bash -c '
count() { local n=0; for f in "$@"; do n=$((n + 1)); done; echo "$n"; }
words=()
for i in $(seq 1 2000); do
    s="item-$i"
    words+=("${s^^}")
    [[ $s == *5 ]] && words+=("${s/item/five}")
done
echo "${#words[@]}" "$(count "${words[@]}")"
declare -A seen
for w in "${words[@]}"; do seen[${w%%-*}]=$(( ${seen[${w%%-*}]:-0} + 1 )); done
echo "${!seen[@]}" | tr " " "\n" | sort | head -3
total=0
while read -r line; do total=$((total + ${#line})); done < <(printf "%s\n" "${words[@]}")
echo "$total"
(cd /tmp && ls -d /usr/* | wc -l)
for f in /usr/lib/*.so*; do case $f in *crypto*) echo "$f";; esac; done | head -2
printf "%s\n" "${words[@]:0:5}" | sed "s/ITEM/x/" | sort -r | head -2
'
