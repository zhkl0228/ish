# Installed by the iSH app, which rewrites this file when it updates the root.
# Node keeps the code it compiles from a program's JavaScript here, and the
# next run of the program uses it instead of compiling again: a large CLI
# (kimi's 25 MB) starts with a fifth less work, from its second run on. The
# cache is written when node exits normally. Set NODE_COMPILE_CACHE yourself
# to put it elsewhere, or NODE_DISABLE_COMPILE_CACHE=1 to go without.
export NODE_COMPILE_CACHE="${NODE_COMPILE_CACHE:-$HOME/.cache/node-compile-cache}"
