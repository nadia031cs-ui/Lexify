#include <stdio.h>
#define N 5
int main(void) {
    int a = 10, b = 3, c;
    float f = 2.5;
    double d = -1.5;
    bool ok = a > b && !(b == 0);
    c = a % b + -a * 2;
    a += 5; b *= 2; c--; ++c;
    printf("Hello, World!\n");
    printf("a=%d b=%d f=%f\n", a, b, f);
    if (a > b) {
        printf("a bigger\n");
    } else if (a == b) {
        printf("equal\n");
    } else {
        printf("b bigger\n");
    }
    for (int i = 0; i < 3; i++) {
        printf("%d ", i);
    }
    for (int i = 0; i < 3; i = i + 1) a += i;
    if (ok) printf("ok %d%%\n", c);
    return 0;
}
