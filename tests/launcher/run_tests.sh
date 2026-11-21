#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
ANDROID_JAR=${ANDROID_JAR:-$HOME/android-sdk/platforms/android-36/android.jar}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

if [ ! -f "$ANDROID_JAR" ]; then
    echo "missing Android API jar: $ANDROID_JAR" >&2
    exit 1
fi

# A shell on Windows still runs a JVM that wants ';' between classpath entries and a native
# path inside them, so hand it the Windows spelling of everything it is given.
case $(uname -s) in
    MINGW*|MSYS*|CYGWIN*)
        CP_SEP=';'
        ROOT=$(cygpath -w "$ROOT")
        ANDROID_JAR=$(cygpath -w "$ANDROID_JAR")
        OUT_CP=$(cygpath -w "$OUT")
        ;;
    *)
        CP_SEP=':'
        OUT_CP=$OUT
        ;;
esac

javac -encoding UTF-8 -source 17 -target 17 -cp "$ANDROID_JAR" -d "$OUT" \
    "$ROOT/android/launcher/app/src/main/java/com/zettabridge/launcher/PluginFiles.java" \
    "$ROOT/android/launcher/app/src/main/java/com/zettabridge/launcher/PluginClassLoader.java" \
    "$ROOT/android/launcher/app/src/main/java/com/zettabridge/launcher/PluginRecord.java" \
    "$ROOT/tests/launcher/LauncherContractsTest.java"
java -ea -cp "$OUT_CP$CP_SEP$ANDROID_JAR" com.zettabridge.launcher.LauncherContractsTest
