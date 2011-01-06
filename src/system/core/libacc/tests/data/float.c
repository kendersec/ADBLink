int ftoi(float f) {
    return f;
}

int dtoi(double d) {
    return d;
}

float itof(int i) {
    return i;
}

double itod(int i) {
    return i;
}

float f0, f1;
double d0, d1;

void testParseConsts() {
    printf("Constants: %g %g %g %g %g %g %g %g %g\n", 0e1, 0E1, 0.f, .01f,
          .01e0f, 1.0e-1, 1.0e1, 1.0e+1,
          .1f);
}
void testVars(float arg0, float arg1, double arg2, double arg3) {
    float local0, local1;
    double local2, local3;
    f0 = arg0;
    f1 = arg1;
    d0 = arg2;
    d1 = arg3;
    local0 = arg0;
    local1 = arg1;
    local2 = arg2;
    local3 = arg3;
    printf("globals: %g %g %g %g\n", f0, f1, d0, d1);
    printf("args: %g %g %g %g\n", arg0, arg1, arg2, arg3);
    printf("locals: %g %g %g %g\n", local0, local1, local2, local3);


    printf("cast rval: %g %g\n", * (float*) & f1, * (double*) & d1);

    * (float*) & f0 = 1.1f;
    * (double*) & d0 = 3.3;
    printf("cast lval: %g %g %g %g\n", f0, f1, d0, d1);
}

int main() {
    testParseConsts();
    printf("int: %d float: %g double: %g\n", 1, 2.2f, 3.3);
    printf(" ftoi(1.4f)=%d\n", ftoi(1.4f));
    printf(" dtoi(2.4)=%d\n", dtoi(2.4));
    printf(" itof(3)=%g\n", itof(3));
    printf(" itod(4)=%g\n", itod(4));
    testVars(1.0f, 2.0f, 3.0, 4.0);
    return 0;
}
