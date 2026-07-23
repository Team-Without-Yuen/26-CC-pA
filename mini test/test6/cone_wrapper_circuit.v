module cone_wrapper_circuit(a, b, c, d, y, z, direct);
input a, b, c, d;
output y, z, direct;

wire n_ab, n_or, n_short;

and g0(n_ab, a, b);
or g1(n_or, n_ab, c);
xor g2(y, n_or, d);

and g_short(n_short, a, c);
buf g_z(z, n_short);

buf g_direct(direct, a);

endmodule
