module equiv_wrong(a, b, c, d, y, z);
input a, b, c, d;
output y, z;

wire n_ab, n_cd;

and g_ab(n_ab, a, b);
and g_cd(n_cd, c, d);
or g_y(y, n_ab, n_cd);
xnor g_z(z, a, c);

endmodule
