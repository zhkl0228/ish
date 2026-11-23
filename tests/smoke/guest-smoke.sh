#!/bin/sh
# Smoke test for the arm64 guest: real programs doing real work, inside iSH.
# Run by tests/smoke/run.sh, which copies it into the rootfs after installing
# what it needs with ish-install (python3, node and npm, Claude Code, go; git
# is preinstalled). Needs the network. Prints one "PASS name" or "FAIL name" line per check, then a
# summary line.
pass=0; fail=0
check() {
    name=$1; shift
    out=$(timeout 300 "$@" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        pass=$((pass+1)); echo "PASS $name: $(echo "$out" | tail -1 | cut -c1-100)"
    else
        fail=$((fail+1)); echo "FAIL $name (rc=$rc):"; echo "$out" | tail -15 | sed 's/^/    /'
    fi
}
W=/tmp/smoke; rm -rf $W; mkdir -p $W; cd $W

check python-compute python3 -c 'import hashlib,json,sqlite3,ssl; print(sum(i*i for i in range(10**6)), hashlib.sha256(b"x").hexdigest()[:8], ssl.OPENSSL_VERSION)'
check python-threads python3 -c 'import concurrent.futures as f; print(sum(f.ThreadPoolExecutor(4).map(lambda x: x*x, range(1000))))'
check pip-venv sh -c 'python3 -m venv venv && ./venv/bin/pip install -q requests && ./venv/bin/python -c "import requests; print(requests.get(\"https://pypi.org/simple/requests/\", timeout=30).status_code)"'

# Processes sharing a lock (POSIX semaphores in /dev/shm) under each start
# method; forkserver passes file descriptors over a unix socket.
cat > mp.py <<'EOF'
import multiprocessing as mp

def inc(lock, counter, n):
    for _ in range(n):
        with lock:
            counter.value += 1

if __name__ == "__main__":
    for method in ("fork", "forkserver", "spawn"):
        ctx = mp.get_context(method)
        lock, counter = ctx.Lock(), ctx.Value("i", 0, lock=False)
        ps = [ctx.Process(target=inc, args=(lock, counter, 500)) for _ in range(4)]
        for p in ps: p.start()
        for p in ps: p.join()
        assert counter.value == 2000, (method, counter.value)
    print("multiprocessing ok")
EOF
check python-multiprocessing python3 mp.py
check python-asyncio-unix python3 -c '
import asyncio
async def main():
    async def handle(r, w):
        w.write((await r.readline()).upper()); await w.drain(); w.close()
    srv = await asyncio.start_unix_server(handle, "/tmp/smoke/aio.sock")
    r, w = await asyncio.open_unix_connection("/tmp/smoke/aio.sock")
    w.write(b"hello\n"); await w.drain()
    print((await r.readline()).decode().strip())
    w.close(); srv.close(); await srv.wait_closed()
asyncio.run(main())'
check python-passfd python3 -c '
import os, socket
a, b = socket.socketpair()
r, w = os.pipe()
socket.send_fds(a, [b"fd"], [w])
msg, fds, flags, addr = socket.recv_fds(b, 16, 4)
os.write(fds[0], b"x"); print(msg, os.read(r, 1))'

check node-eval node -e 'const c=require("crypto"); console.log([1,2,3].map(x=>x*x).join(","), c.createHash("sha256").update("x").digest("hex").slice(0,8))'
check node-fetch node -e 'fetch("https://registry.npmjs.org/lodash").then(r=>console.log(r.status)).catch(e=>{console.error(e);process.exit(1)})'
check node-unix-net node -e '
const net = require("net"), p = "/tmp/smoke/nn.sock";
const srv = net.createServer(s => s.on("data", d => s.end(d.toString().toUpperCase()))).listen(p, () => {
  const c = net.connect(p, () => c.write("ping")); c.on("data", d => { console.log(d.toString()); srv.close(); });
});'
# A socket handle sent to a child process (SCM_RIGHTS over node's IPC channel)
cat > sendhandle.js <<'EOF'
const { fork } = require('child_process');
const net = require('net');
if (process.argv[2] === 'child') {
  process.on('message', (m, sock) => { sock.end('from child\n'); process.disconnect(); });
} else {
  const srv = net.createServer({ pauseOnConnect: true }).listen(0, '127.0.0.1', () => {
    const child = fork(__filename, ['child']);
    srv.on('connection', (sock) => child.send('sock', sock));
    const c = net.connect(srv.address().port, '127.0.0.1');
    c.on('data', (d) => { console.log(d.toString().trim()); c.end(); srv.close(); });
  });
}
EOF
check node-sendhandle node sendhandle.js
check npm-install sh -c 'mkdir -p np && cd np && npm init -y >/dev/null && npm install --no-audit --no-fund lodash chalk@4 >/dev/null && node -e "console.log(require(\"lodash\").chunk([1,2,3,4],2).length, typeof require(\"chalk\").red)"'

cat > hello.go <<'EOF'
package main

import (
	"fmt"
	"sync"
)

func main() {
	var wg sync.WaitGroup
	sum := make(chan int, 8)
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(n int) { defer wg.Done(); s := 0; for j := 0; j < 100000; j++ { s += j % (n + 1) }; sum <- s }(i)
	}
	wg.Wait(); close(sum)
	t := 0
	for s := range sum { t += s }
	fmt.Println("go ok", t)
}
EOF
check go-build sh -c 'go build -o hello hello.go && ./hello'
# GC stopping a goroutine that loops without calls: needs asynchronous
# preemption, and mmap that a spinning thread does not starve.
cat > preempt.go <<'EOF'
package main

import (
	"fmt"
	"os"
	"runtime"
	"time"
)

var sink int

func spin() {
	x := 0
	for i := 0; ; i++ {
		x += i
		if i == -1 {
			break
		}
	}
	sink = x
}

func main() {
	go spin()
	time.Sleep(50 * time.Millisecond)
	start := time.Now()
	runtime.GC()
	if d := time.Since(start); d > 5*time.Second {
		fmt.Println("GC took", d)
		os.Exit(1)
	}
	fmt.Println("GC beside a spinning goroutine ok")
}
EOF
check go-preempt sh -c 'go build -o preempt preempt.go && timeout -s KILL 60 ./preempt'
# go's own output kept: a failed download says why (once only an empty FAIL)
check go-mod sh -c 'mkdir -p gm && cd gm && go mod init example.com/gm && printf "package main\nimport (\"fmt\"; \"github.com/google/uuid\")\nfunc main(){ fmt.Println(len(uuid.NewString())) }\n" > main.go && go mod tidy && go run .'

check git-clone sh -c 'git clone -q --depth 1 https://github.com/octocat/Hello-World.git hw && git -C hw log --oneline -1'

check claude-install sh -c 'npm install -g --no-audit --no-fund @anthropic-ai/claude-code >/dev/null 2>&1; command -v claude'
check claude-version claude --version
check claude-help sh -c 'claude --help > /tmp/smoke/claude-help.txt && head -1 /tmp/smoke/claude-help.txt'
check claude-print-noauth sh -c 'out=$(ANTHROPIC_API_KEY=invalid timeout 60 claude -p hi 2>&1); echo "$out" | tail -2; echo "$out" | grep -qiE "api key|authentication|401|login"'

echo "SUMMARY pass=$pass fail=$fail"
