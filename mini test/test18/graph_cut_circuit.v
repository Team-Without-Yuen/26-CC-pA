module graph_cut_circuit(a, b, y, z);
input a;
input b;
output y;
output z;
wire branch_p;
wire branch_q;
wire join_r;
wire unique_cut;

buf g_p(branch_p, a);
not g_q(branch_q, a);
or g_join(join_r, branch_p, branch_q);
buf g_y(y, join_r);

buf g_cut(unique_cut, b);
buf g_z(z, unique_cut);
endmodule
