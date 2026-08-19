module connectivity_strict (
    input a,
    input b,
    input clk,
    input rst_n,
    output y,
    output q
);
    wire n1;
    wire n2;
    wire n3;

    and g_and0 (n1, a, b);
    buf g_buf0 (n2, n1);
    or  g_or0  (n3, n1, a);
    xor g_xor0 (y, n2, n3);
    dff g_dff0 (.RN(rst_n), .SN(1'b1), .CK(clk), .D(n1), .Q(q));
endmodule
