module constant_simplification_circuit(
    a, b, clk, rst,
    y_and0, y_and1, y_or1, y_nand1, y_nand3,
    y_nor0, y_po_or1, y_xor3, q
);
input a, b, clk, rst;
output y_and0, y_and1, y_or1, y_nand1, y_nand3;
output y_nor0, y_po_or1, y_xor3, q;

wire and0_n, and1_n, or1_n, nand1_n, nand3_n, nor0_n, xor3_n;

and  g_and0(and0_n, a, 1'b0);
and  g_and1(and1_n, a, 1'b1);
or   g_or1(or1_n, a, 1'b1);
nand g_nand1(nand1_n, a, 1'b1);
nand g_nand3(nand3_n, a, b, 1'b1);
nor  g_nor0(nor0_n, a, 1'b0);
xor  g_xor3(xor3_n, a, b, 1'b1);

buf g_and0_out(y_and0, and0_n);
buf g_and1_out(y_and1, and1_n);
buf g_or1_out(y_or1, or1_n);
buf g_nand1_out(y_nand1, nand1_n);
buf g_nand3_out(y_nand3, nand3_n);
buf g_nor0_out(y_nor0, nor0_n);
buf g_xor3_out(y_xor3, xor3_n);

// Direct PO driver verifies that simplification preserves the PO net.
or g_po_or1(y_po_or1, b, 1'b1);

// Constant DFF control pins are queryable but are not combinational candidates.
dff ff0(.D(a), .CK(clk), .RN(rst), .SN(1'b1), .Q(q));
endmodule
