module equiv_extra_output(a, b, c, d, y, z, extra);
input a, b, c, d;
output y, z, extra;

wire n_ab, n_cd;

and g_ab(n_ab, a, b);
and g_cd(n_cd, c, d);
or g_y(y, n_ab, n_cd);
xor g_z(z, a, c);
buf g_extra(extra, d);

endmodule
