#include <iostream>
using namespace std;

int main() {
    // ---- 1. Variable declaration ----
    int a;
    int b;
    float h;
    bool ok;

    // ---- 2. Assignment ----
    a = 10;
    b = 3;
    h = 5.7;
    ok = true;

    // ---- 3. Arithmetic ----
    cout << a + b << endl;
    cout << a - b << endl;
    cout << a * b << endl;
    cout << a / b << endl;
    cout << a % b << endl;
    cout << h * 2 << endl;

    // ---- 4. Relational & logical ----
    cout << (a > b) << endl;
    cout << (a == b) << endl;
    cout << (a != b) << endl;
    cout << ((a > 0) && (b > 0)) << endl;
    cout << ((a > 0) || (b < 0)) << endl;
    cout << !ok << endl;

    // ---- 5. if ----
    if (a > 0) {
        cout << 1 << endl;
    }

    // ---- 6. if-else (even / odd) ----
    if (a % 2 == 0) {
        cout << 100 << endl;
    } else {
        cout << 200 << endl;
    }

    // ---- 7. else-if ladder (marks -> grade) ----
    int marks;
    marks = 75;
    if (marks >= 80) {
        cout << 4 << endl;
    } else if (marks >= 70) {
        cout << 3 << endl;
    } else if (marks >= 60) {
        cout << 2 << endl;
    } else {
        cout << 0 << endl;
    }

    // ---- 8. Nested if ----
    if (a > 0) {
        if (b > 0) {
            cout << 11 << endl;
        } else {
            cout << 10 << endl;
        }
    } else {
        cout << 0 << endl;
    }

    // ---- 9. Largest of three ----
    int c;
    int largest;
    c = 7;
    largest = a;
    if (b > largest) { largest = b; }
    if (c > largest) { largest = c; }
    cout << "Largest = " << largest << endl;
    std::cout << 'X' << '\n';
    return 0;
}
