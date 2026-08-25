module area_target_circuit(
    input a,
    input b,
    output y
);
    wire n0;
    wire n1;

    and g0(n0, a, b);
    not g1(n1, n0);
    not g2(y, n1);
endmodule
