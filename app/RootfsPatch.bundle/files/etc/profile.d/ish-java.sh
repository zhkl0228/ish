# Installed by the iSH app, which rewrites this file when it updates the root.
# JAVA_HOME, once a JDK is installed (ish-install java): Alpine's default-jvm,
# the link to the JDK java runs, which stays the same when the JDK is upgraded.
# A JAVA_HOME already set is kept; set your own in ~/.profile.
if [ -z "$JAVA_HOME" ] && [ -x /usr/lib/jvm/default-jvm/bin/javac ]; then
    export JAVA_HOME=/usr/lib/jvm/default-jvm
fi
