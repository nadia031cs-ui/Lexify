#include <stdio.h>

int square(int x) {
    return x * x;
}

int main() {
    int n;
    printf("Koyta number? ");
    scanf("%d", &n);

    int nums[100];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        scanf("%d", &nums[i]);
        sum += nums[i];
        printf("%d er square = %d\n", nums[i], square(nums[i]));
    }

    printf("Total sum = %d\n", sum);
    return 0;
}