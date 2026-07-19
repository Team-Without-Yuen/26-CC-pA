module equivalent_gate_pairs_circuit (
    input a,
    input b,
    input c,
    output y,
    output z
);
    wire n_and1;
    wire n_and2;
    wire n_nand;
    wire n_not_nand;
    wire n_buf;
    wire n_ac;
    wire n_or1;
    wire n_or2;

    and  g_and1     (n_and1, a, b);
    and  g_and2     (n_and2, b, a);
    nand g_nand     (n_nand, a, b);
    not  g_not_nand (n_not_nand, n_nand);
    buf  g_buf      (n_buf, n_and1);
    and  g_ac       (n_ac, a, c);
    or   g_or1      (n_or1, a, c);
    or   g_or2      (n_or2, c, a);

    xor  g_mix      (y, n_and1, n_not_nand);
    or   g_out      (z, n_and2, n_ac);
endmodule
