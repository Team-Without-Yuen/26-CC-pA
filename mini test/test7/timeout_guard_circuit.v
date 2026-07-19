module timeout_guard_circuit(a, b, c, y, z);
input a, b, c;
output y, z;

wire n1, n2, n3, n4, n5, n6, n7, n8, n9, n10;

and g1(n1, a, b);
or g2(n2, a, b);
xor g3(n3, n1, c);
xnor g4(n4, n2, c);
nand g5(n5, a, c);
nor g6(n6, a, c);
and g7(n7, n5, b);
or g8(n8, n6, b);
or g9(n9, n3, n4);
or g10(n10, n7, n8);
or g11(y, n9, n10);
buf g12(z, y);

endmodule
