module register_path_circuit(clk, rst_n, a, b, q1, q2, q3, y);
input clk, rst_n, a, b;
output q1, q2, q3, y;

wire d2, d3;
wire n12a, n13, n23;

dff ff1(.D(a), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q1));

and g12a(n12a, q1, a);
xor g12b(d2, n12a, b);
dff ff2(.D(d2), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q2));

not g13(n13, q1);
buf g23(n23, q2);
or g3d(d3, n13, n23);
dff ff3(.D(d3), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q3));

buf gy(y, q3);

endmodule
