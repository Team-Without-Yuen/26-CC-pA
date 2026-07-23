module equiv_missing_output(a, b, c, d, y);
input a, b, c, d;
output y;

wire n_ab, n_cd;

and g_ab(n_ab, a, b);
and g_cd(n_cd, c, d);
or g_y(y, n_ab, n_cd);

endmodule
