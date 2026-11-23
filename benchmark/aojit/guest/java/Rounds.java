import javax.tools.JavaCompiler;
import javax.tools.ToolProvider;
import java.io.File;
import java.util.ArrayList;
import java.util.List;

// javac over a directory's sources, again and again in one JVM: C1 and C2
// compile javac's own hot methods, and G1 collects as it goes.
// usage: java Rounds.java SRC_DIR OUT_DIR ROUNDS
public class Rounds {
    public static void main(String[] args) {
        List<String> options = new ArrayList<>(List.of("-d", args[1]));
        for (File f : new File(args[0]).listFiles())
            options.add(f.getPath());
        JavaCompiler javac = ToolProvider.getSystemJavaCompiler();
        for (int r = 0; r < Integer.parseInt(args[2]); r++) {
            long t0 = System.nanoTime();
            int rc = javac.run(null, null, null, options.toArray(new String[0]));
            System.out.println("javac round " + r + ": rc " + rc + ", " + (System.nanoTime() - t0) / 1000000 + " ms");
            if (rc != 0)
                System.exit(rc);
        }
    }
}
