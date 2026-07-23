module depth_wrapper_circuit(a, b, c, d, clk, y, z, q);
input a, b, c, d, clk;
output y, z, q;

wire n_ab, n_mid, n_short;

and g0(n_ab, a, b);
or g1(n_mid, n_ab, c);
not g2(y, n_mid);

and g_short(n_short, a, c);
buf g_z(z, n_short);

dff ff1(.D(d), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q));

endmodule
