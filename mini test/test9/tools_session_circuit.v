module tools_session(a, b, y, z);
input a, b;
output y, z;
wire n1, n2;

and g0(n1, a, b);
not g1(n2, n1);
buf g2(y, n2);
or g3(z, a, b);
endmodule
