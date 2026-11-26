# node's image, of both the modes the app runs node in (kernel/exec.c): V8's interpreter, with
# node's image and the JIT not translating; V8's tiers (Sparkplug, Maglev, TurboFan), with the JIT
# translating and room in its code region, where V8's compilers run too: their code then native
# from the image, not translated into the region. kimi_train.sh in each.
ISH_NODE_MODE=interpreter sh /tmp/aojit/kimi/kimi_train.sh
ISH_NODE_MODE=tiers sh /tmp/aojit/kimi/kimi_train.sh
