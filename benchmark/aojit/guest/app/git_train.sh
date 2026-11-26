# git: what a coding session runs (Claude Code's, its user's) in a repository of its own, no
# network: a repository made and files added, then changes with status, diff and commit, the log
# in its forms, show, branches merged and a topic rebased, stash, tags, describe, blame, grep, a
# local clone pulled, a worktree, reset, reflog, format-patch, archive, gc and fsck. Its image is
# for the JIT's code region (git's code native from the image, not translated into the region,
# where Claude Code's sessions ran git more than any program with no image) and for the app
# without the JIT.
set -e
work=/tmp/aojit/git/work
export HOME=/tmp/aojit/git/home GIT_CONFIG_NOSYSTEM=1 GIT_PAGER=cat PAGER=cat
rm -rf "$work" "$HOME"
mkdir -p "$work" "$HOME"
git config --global user.name ish
git config --global user.email ish@example.com
git config --global init.defaultBranch main
cd "$work"
git init -q repo
cd repo

# the files, five directories of them
for i in $(seq 1 30); do
    mkdir -p "dir$((i % 5))"
    seq 1 200 | sed "s/^/file $i line /" > "dir$((i % 5))/f$i.txt"
done
git add -A
git commit -q -m "The files"

# changes, as a session makes them and looks at them
for round in 1 2 3 4 5; do
    for i in 1 7 13 19 25; do
        echo "change $round" >> "dir$((i % 5))/f$i.txt"
    done
    sed -i "s/line 10\$/line ten, round $round/" dir0/f5.txt
    git status --short > /dev/null
    git status > /dev/null
    git diff > /dev/null
    git diff --stat > /dev/null
    git add -A
    git diff --cached --name-only > /dev/null
    git commit -q -m "Change $round"
done

git log --oneline -n 5 > /dev/null
git log --format='%H %an %ad %s' --date=iso > /dev/null
git log -p -2 > /dev/null
git log --stat --oneline -3 > /dev/null
git log --graph --decorate --all --oneline > /dev/null
git show HEAD > /dev/null
git show --stat HEAD~2 > /dev/null
git rev-parse HEAD > /dev/null
git rev-parse --show-toplevel > /dev/null
git rev-parse --abbrev-ref HEAD > /dev/null
git ls-files > /dev/null

# a branch merged, a topic rebased onto main and brought in
git branch feature
git switch -q feature
echo feature >> dir1/f1.txt
git commit -qam "On the feature"
git switch -q main
echo main >> dir2/f2.txt
git commit -qam "On main"
git merge -q --no-edit feature
git switch -q -c topic HEAD~2
echo topic > topic.txt
git add topic.txt
git commit -q -m "The topic"
git rebase -q main
git switch -q main
git merge -q --ff-only topic
git branch -a > /dev/null
git branch -d feature topic > /dev/null

# stash, tags, describe
echo stashed >> dir4/f4.txt
git stash -q
git stash list > /dev/null
git stash pop -q
git checkout -- .
git tag v1
git tag -a v2 -m "Version 2"
git tag -l > /dev/null
git describe --tags > /dev/null

git blame dir1/f1.txt > /dev/null
git grep -n "line 1" > /dev/null
git grep -c change > /dev/null

# a local clone, pulled after a commit; a worktree
git clone -q "$work/repo" "$work/clone"
echo after >> dir3/f3.txt
git commit -qam "After the clone"
git -C "$work/clone" pull -q --no-rebase
git -C "$work/clone" log --oneline -3 > /dev/null
git worktree add -q "$work/wt" -b side
git -C "$work/wt" status --short > /dev/null
git worktree remove "$work/wt"

git reset -q --hard HEAD~1
git reflog > /dev/null
git format-patch -1 --stdout > /dev/null
git archive HEAD > /dev/null
git gc -q
git fsck > /dev/null 2>&1

cd /
rm -rf "$work" "$HOME"
