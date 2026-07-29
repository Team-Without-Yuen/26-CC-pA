module path_pruning_circuit(a, b, c, d, y, z);
input a, b, c, d;
output y, z;

wire n1, n2, n3, n4, n5, n6;
wire side1, side2;

and g0(n1, a, b);
or  g1(n2, a, c);
xor g2(n3, n1, d);
nand g3(n4, n2, d);
or  g4(n5, n3, n4);
not g5(n6, n5);
buf g6(y, n6);

and g7(side1, a, c);
or  g8(side2, side1, d);
buf g9(z, side2);

endmodule
