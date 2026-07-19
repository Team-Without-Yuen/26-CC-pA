module techmap_circuit(a, b, c, d, y, z, w);
input a, b, c, d;
output y, z, w;

wire n_xor, n_or, n_nand, n_nor, n_xnor;

xor g_xor(n_xor, a, b);
or g_or(n_or, c, d);
nand g_nand(n_nand, n_xor, n_or);
nor g_nor(n_nor, n_or, a);
xnor g_xnor(n_xnor, b, c);

and g_y(y, n_xor, n_nand);
or g_z(z, n_nor, n_xnor);
xor g_w(w, n_xor, n_xnor);

endmodule
