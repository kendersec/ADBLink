// Array allocation tests

void testLocalInt()
{
    int a[3];
    a[0] = 1;
    a[1] = 2;
    a[2] = a[0] + a[1];
    printf("localInt: %d\n", a[2]);
}

char a[3];
double d[3];

void testGlobalChar()
{
    a[0] = 1;
    a[1] = 2;
    a[2] = a[0] + a[1];
    printf("globalChar: %d\n", a[2]);
}

void testGlobalDouble()
{
    d[0] = 1;
    d[1] = 2;
    d[2] = d[0] + d[1];
    printf("globalDouble: %g\n", d[2]);
}

void testLocalDouble()
{
    double d[3];
    float  m[12];
    m[0] = 1.0f;
    m[1] = 2.0f;
    d[0] = 1.0;
    d[1] = 2.0;
    d[2] = d[0] + d[1];
    m[2] = m[0] + m[1];
    printf("localDouble: %g %g\n", d[2], m[2]);
}

void vectorAdd(int* a, int* b, float* c, int len) {
    int i;
    for(i = 0; i < len; i++) {
        c[i] = a[i] + b[i];
    }
}

void testArgs() {
    int a[3], b[3];
    float c[3];
    int i;
    int len = 3;
    for(i = 0; i < len; i++) {
        a[i] = i;
        b[i] = i;
        c[i] = 0;
    }
    vectorAdd(a,b,c, len);
    printf("testArgs:");
    for(i = 0; i < len; i++) {
        printf(" %g", c[i]);
    }
    printf("\n");
}

void testDecay() {
    char c[4];
    c[0] = 'H';
    c[1] = 'i';
    c[2] = '!';
    c[3] = 0;
    printf("testDecay: %s\n", c);
}

void test2D() {
    char c[10][20];
    int x;
    int y;
    printf("test2D:\n");
    for(y = 0; y < 10; y++) {
        for(x = 0; x < 20; x++) {
            c[y][x] = 'a' + (15 & (y * 19 + x));
        }
    }
    for(y = 0; y < 10; y++) {
        for(x = 0; x < 20; x++) {
            printf("%c", c[y][x]);
        }
        printf("\n");
    }

}

int main()
{
    testLocalInt();
    testLocalDouble();
    testGlobalChar();
    testGlobalDouble();
    testArgs();
    testDecay();
    test2D();
    return 0;
}
