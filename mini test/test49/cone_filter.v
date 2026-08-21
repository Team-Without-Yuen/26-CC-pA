module cone_filter (
    input a,
    input b,
    input c,
    input clk,
    input rst_n,
    output y,
    output q
);
    wire n_and;
    wire n_or;
    wire n_not;
    wire n_nand;
    wire n_nor;
    wire n_xor;
    wire n_xnor;

    and  g_and  (n_and, a, b);
    or   g_or   (n_or, n_and, c);
    not  g_not  (n_not, n_or);
    nand g_nand (n_nand, n_not, a);
    nor  g_nor  (n_nor, n_nand, b);
    xor  g_xor  (n_xor, n_nor, c);
    xnor g_xnor (n_xnor, n_xor, a);
    buf  g_buf  (y, n_xnor);
    dff  g_dff  (.RN(rst_n), .SN(1'b1), .CK(clk), .D(y), .Q(q));
endmodule
