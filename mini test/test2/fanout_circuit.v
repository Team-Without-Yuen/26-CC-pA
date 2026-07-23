module fanout_circuit(src, a, b, c, d, e, f, g, h, y0, y1, y2, y3, y4, y5, y6, y7);
input src, a, b, c, d, e, f, g, h;
output y0, y1, y2, y3, y4, y5, y6, y7;

and g0(y0, src, a);
or g1(y1, src, b);
nand g2(y2, src, c);
nor g3(y3, src, d);
xor g4(y4, src, e);
xnor g5(y5, src, f);
and g6(y6, src, g);
or g7(y7, src, h);

endmodule
