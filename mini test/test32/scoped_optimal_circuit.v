module scoped_optimal_circuit (
    input a,
    input b,
    input ck,
    output q,
    output and_y
);
    wire nand_y;

    nand g0(nand_y, a, b);
    not g1(and_y, nand_y);
    dff ff0(.D(a), .CK(ck), .Q(q));
endmodule
