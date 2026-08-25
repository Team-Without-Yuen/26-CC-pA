module ranked_paths_large(a, y);
    input a;
    output y;

    wire a0, b0, n0;
    wire a1, b1, n1;
    wire a2, b2, n2;
    wire a3, b3, n3;
    wire a4, b4, n4;
    wire a5, b5, n5;
    wire a6, b6, n6;
    wire a7, b7;

    buf g_a0(a0, a);
    buf g_b0(b0, a);
    or  g_j0(n0, a0, b0);
    buf g_a1(a1, n0);
    buf g_b1(b1, n0);
    or  g_j1(n1, a1, b1);
    buf g_a2(a2, n1);
    buf g_b2(b2, n1);
    or  g_j2(n2, a2, b2);
    buf g_a3(a3, n2);
    buf g_b3(b3, n2);
    or  g_j3(n3, a3, b3);
    buf g_a4(a4, n3);
    buf g_b4(b4, n3);
    or  g_j4(n4, a4, b4);
    buf g_a5(a5, n4);
    buf g_b5(b5, n4);
    or  g_j5(n5, a5, b5);
    buf g_a6(a6, n5);
    buf g_b6(b6, n5);
    or  g_j6(n6, a6, b6);
    buf g_a7(a7, n6);
    buf g_b7(b7, n6);
    or  g_j7(y, a7, b7);
endmodule
