module critical_path_circuit (
    input a0,
    input a1,
    input a2,
    input a3,
    input a4,
    input a5,
    input a6,
    input a7,
    input ck,
    output q,
    output z
);
    wire x1;
    wire x2;
    wire x3;
    wire x4;
    wire x5;
    wire x6;
    wire d;

    xor g1(x1, a0, a1);
    xor g2(x2, x1, a2);
    xor g3(x3, x2, a3);
    xor g4(x4, x3, a4);
    xor g5(x5, x4, a5);
    xor g6(x6, x5, a6);
    xor g7(d, x6, a7);
    dff ff0(.D(d), .CK(ck), .Q(q));
    and g8(z, a0, a1);
endmodule
