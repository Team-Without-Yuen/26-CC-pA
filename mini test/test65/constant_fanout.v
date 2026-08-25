module constant_fanout(a, y0, y1, y2, y3, y4);
input a;
output y0, y1, y2, y3, y4;

and g0(y0, a, 1'b1);
and g1(y1, a, 1'b1);
and g2(y2, a, 1'b1);
and g3(y3, a, 1'b1);
and g4(y4, a, 1'b1);

endmodule
