module basic_filter (
    input a,
    input b,
    input c,
    input [1:0] d,
    input clk,
    input rst_n,
    output y0,
    output y1,
    output [1:0] q
);
    wire n1;
    wire n2;
    wire n3;
    wire n4;
    wire n5;
    wire n6;
    wire n7;
    wire n8;
    wire n9;
    wire n10;
    wire n11;
    wire n12;
    wire n_dead;

    // AND x3 (g_and2 has a constant 0 input)
    and  g_and0  (n1, a, b);
    and  g_and1  (n2, c, d[0]);
    and  g_and2  (y1, a, 1'b0);

    // OR x1
    or   g_or0   (n3, n1, n2);

    // NAND x3 (g_nand1 has a constant 1 input; g_nand2 uses one net on both pins)
    nand g_nand0 (n4, n3, a);
    nand g_nand1 (n5, n4, 1'b1);
    nand g_nand2 (n6, b, b);

    // NOR x2 (g_nor1 has a constant 0 input)
    nor  g_nor0  (n7, n5, n6);
    nor  g_nor1  (n8, n7, 1'b0);

    // XOR x2 / XNOR x1 (g_dead drives nothing: dangling-logic fixture)
    xor  g_xor0  (n9, n8, d[1]);
    xor  g_dead  (n_dead, a, c);
    xnor g_xnor0 (n10, n9, c);

    // NOT x2 / BUF x1
    not  g_not0  (n11, n10);
    not  g_not1  (n12, n11);
    buf  g_buf0  (y0, n12);

    // DFF x2 (both hold SN at constant 1)
    dff  g_dff0  (.RN(rst_n), .SN(1'b1), .CK(clk), .D(y0), .Q(q[0]));
    dff  g_dff1  (.RN(rst_n), .SN(1'b1), .CK(clk), .D(y1), .Q(q[1]));
endmodule
