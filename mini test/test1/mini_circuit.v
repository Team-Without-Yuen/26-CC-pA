module mini_circuit(a, b, c, clk, rst_n, bus, y, z, q, direct_out, bus_out);
input a, b, c, clk, rst_n;
input [1:0] bus;
output y, z, q, direct_out, bus_out;

wire n_and, n_or, n_nand, n_nor;
wire n_b1, n_b2, n_b3, n_buf;
wire n_not, n_xnor, n_short;

and g_and(n_and, a, b);
or g_or(n_or, n_and, c);
nand g_nand(n_nand, n_or, 1'b1);
nor g_nor(n_nor, n_nand, 1'b0);

buf g_b1(n_b1, n_nor);
buf g_b2(n_b2, n_b1);
buf g_b3(n_b3, n_b2);
buf g_buf(n_buf, n_b3);
buf g_y(y, n_buf);

not g_not(n_not, n_or);
xor g_z(z, n_or, c);
xnor g_xnor(n_xnor, a, c);
and g_short(n_short, n_xnor, b);
xor g_bus(bus_out, bus[0], bus[1]);
buf g_direct(direct_out, a);

dff ff1(.D(n_buf), .CK(clk), .RN(rst_n), .SN(rst_n), .Q(q));

endmodule
