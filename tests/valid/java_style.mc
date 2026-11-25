public class Main {
    public static void main(String[] args) {
        // ---- 1. Variable declaration ----
        int a;
        int b;
        double h;
        boolean ok;

        // ---- 2. Assignment ----
        a = 10;
        b = 3;
        h = 5.7;
        ok = true;

        // ---- 3. Arithmetic ----
        System.out.println(a + b);
        System.out.println(a - b);
        System.out.println(a * b);
        System.out.println(a / b);
        System.out.println(a % b);
        System.out.println(h * 2);

        // ---- 4. Relational & logical ----
        System.out.println(a > b);
        System.out.println(a == b);
        System.out.println(a != b);
        System.out.println((a > 0) && (b > 0));
        System.out.println((a > 0) || (b < 0));
        System.out.println(!ok);

        // ---- 5. if ----
        if (a > 0) {
            System.out.println(1);
        }

        // ---- 6. if-else (even / odd) ----
        if (a % 2 == 0) {
            System.out.println(100);
        } else {
            System.out.println(200);
        }

        // ---- 7. else-if ladder (marks -> grade) ----
        int marks;
        marks = 75;
        if (marks >= 80) {
            System.out.println(4);
        } else if (marks >= 70) {
            System.out.println(3);
        } else if (marks >= 60) {
            System.out.println(2);
        } else {
            System.out.println(0);
        }

        // ---- 8. Nested if ----
        if (a > 0) {
            if (b > 0) {
                System.out.println(11);
            } else {
                System.out.println(10);
            }
        } else {
            System.out.println(0);
        }

        // ---- 9. Largest of three ----
        int c;
        int largest;
        c = 7;
        largest = a;
        if (b > largest) { largest = b; }
        if (c > largest) { largest = c; }
        System.out.println("Largest = " + largest);
        System.out.println("sum " + a + b + " vs " + (a + b));
        System.out.print("no newline ");
        System.out.println(20.0);
    }
}
