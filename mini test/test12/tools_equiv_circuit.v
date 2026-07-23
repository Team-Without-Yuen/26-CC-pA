module tools_equiv(a, b, c, y, z);
input a, b, c;
output y, z;

wire n0, n1, n2;

and g0(n0, a, b);
buf g_buf0(n1, n0);
buf g_buf1(y, n1);
xor g1(n2, b, c);
not g2(z, n2);

endmodule
