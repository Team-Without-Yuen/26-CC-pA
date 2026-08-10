module path_compact_circuit(a, b, y);
input a, b;
output y;

wire n0, n1, n2, n3, n4, n5, n6;

buf g0(n0, a);
buf g1(n1, a);
and g2(n2, n0, b);
or g3(n3, n0, b);
nand g4(n4, n1, b);
nor g5(n5, n1, b);
or g6(n6, n2, n3, n4, n5);
buf g7(y, n6);

endmodule
