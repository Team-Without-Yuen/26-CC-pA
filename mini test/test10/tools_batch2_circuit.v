module tools_batch2(a, b, c, clk, rst_n, y, z, q1, q2);
input a, b, c, clk, rst_n;
output y, z, q1, q2;
wire n1, n2, reg_mid, d2;

and g0(n1, a, b);
or g1(n2, n1, c);
not g2(y, n2);

buf g_short(z, a);

dff ff1(.D(a), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q1));
and g_reg1(reg_mid, q1, b);
or g_reg2(d2, reg_mid, c);
dff ff2(.D(d2), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q2));
endmodule
