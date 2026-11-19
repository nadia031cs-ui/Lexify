#include <stdio.h>

int main() {
int x;
int y;
x = 10;
y = 0;

while (x > 0) {
    y = y + x;
    x = x - 1;
}
print y;
}
