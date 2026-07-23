module equiv_same(a, b, c, d, y, z);
input a, b, c, d;
output y, z;

wire n_ab_n, n_cd_n, n_ab, n_cd, n_y_n, n_z_tmp;

nand g_ab_n(n_ab_n, a, b);
not g_ab(n_ab, n_ab_n);
nand g_cd_n(n_cd_n, c, d);
not g_cd(n_cd, n_cd_n);
nor g_y_n(n_y_n, n_ab, n_cd);
not g_y(y, n_y_n);
xor g_z_tmp(n_z_tmp, c, a);
buf g_z(z, n_z_tmp);

endmodule
