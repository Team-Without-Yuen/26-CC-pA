module register_timeout_circuit(clk, rst_n, a, b, q1, q2, q3);
input clk, rst_n, a, b;
output q1, q2, q3;

wire d2, d3, n12, n13, n23;

dff ff1(.D(a), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q1));

and g12a(n12, q1, a);
xor g12b(d2, n12, b);
dff ff2(.D(d2), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q2));

not g13(n13, q1);
buf g23(n23, q2);
or g3d(d3, n13, n23);
dff ff3(.D(d3), .CK(clk), .RN(rst_n), .SN(1'b1), .Q(q3));

endmodule
