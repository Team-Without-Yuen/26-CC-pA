module function_expr_circuit(a, b, c, clk, rst_n, y, x, nand_out, xnor_out, limited, q, q_expr, const_zero, const_one, y_copy);
input a, b, c, clk, rst_n;
output y, x, nand_out, xnor_out, limited, q, q_expr, const_zero, const_one, y_copy;

wire n_and, n_not;
wire n_deep1, n_deep2;

and g_and(n_and, a, b);
not g_not(n_not, c);
or g_y(y, n_and, n_not);

xor g_x(x, b, c);
nand g_nand(nand_out, a, c);
xnor g_xnor(xnor_out, a, b);

and g_d1(n_deep1, a, b);
or g_d2(n_deep2, n_deep1, c);
not g_limited(limited, n_deep2);

dff ff1(.D(y), .CK(clk), .RN(rst_n), .SN(rst_n), .Q(q));
or g_q_expr(q_expr, q, a);
and g_const_zero(const_zero, a, 1'b0);
or g_const_one(const_one, b, 1'b1);
buf g_y_copy(y_copy, y);

endmodule
