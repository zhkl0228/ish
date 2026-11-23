#!/usr/bin/env python3
# A Maven project of CLASSES generated classes (generics, lambdas, switches,
# collections) for javac and Maven to compile.
# usage: gen_project.py DIR CLASSES
import os
import sys

root, classes = sys.argv[1], int(sys.argv[2])
src = os.path.join(root, 'src', 'main', 'java', 'gen')
os.makedirs(src, exist_ok=True)
for i in range(classes):
    lines = ['package gen;', 'import java.util.*;', 'import java.util.function.*;', '',
             'public class C%d<T extends Comparable<T>> {' % i,
             '    private final List<T> items = new ArrayList<>();',
             '    private final Map<String, Integer> counts = new HashMap<>();']
    for m in range(16):
        lines += [
            '    public int m%d(int a, String s, List<T> xs) {' % m,
            '        int total = a * %d + s.length();' % (m + 1),
            '        for (T x : xs) { if (x.compareTo(xs.get(0)) > 0) total += x.hashCode() %% %d; else total -= %d; }' % (m + 7, m),
            '        Function<Integer, String> f = v -> "v" + v + s + (v > %d ? "big" : "small");' % m,
            '        counts.merge(f.apply(total), 1, Integer::sum);',
            '        switch (total %% 4) { case 0: total += %d; break; case 1: total ^= %d; break; default: total = Math.max(total, %d); }' % (m, m * 3, m * 5),
            '        return total + (items.isEmpty() ? 0 : items.size() * C%d.class.getSimpleName().length());' % ((i + 1) % classes),
            '    }',
        ]
    lines.append('}')
    with open(os.path.join(src, 'C%d.java' % i), 'w') as f:
        f.write('\n'.join(lines) + '\n')
with open(os.path.join(root, 'pom.xml'), 'w') as f:
    f.write('''<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion>
  <groupId>ish.aojit</groupId>
  <artifactId>gen</artifactId>
  <version>1</version>
  <packaging>jar</packaging>
  <properties>
    <maven.compiler.release>21</maven.compiler.release>
    <project.build.sourceEncoding>UTF-8</project.build.sourceEncoding>
  </properties>
</project>
''')
