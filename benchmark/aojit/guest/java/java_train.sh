# Java and Maven (libjvm.so): javac over a generated project, rounds in one JVM
# (HotSpot's C1 and C2 compiling, G1 collecting), then Maven building it: once
# fetching its plugins (the network), once offline. The JVM's interpreter and
# the code C2 makes are made at run time; the image is the VM's own code.
set -e
cd /tmp/aojit/java
rm -rf /tmp/aojit-java
python3 gen_project.py /tmp/aojit-java 120
java -version 2>&1 | head -1
mkdir -p /tmp/aojit-java/classes
java Rounds.java /tmp/aojit-java/src/main/java/gen /tmp/aojit-java/classes 2
javap -cp /tmp/aojit-java/classes gen.C0 | head -3
cd /tmp/aojit-java
mvn -B -q clean package
mvn -B -q -o clean package
ls -la target/gen-1.jar | awk '{print "jar", $5}'
