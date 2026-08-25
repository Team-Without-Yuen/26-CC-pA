module exact_xor4(a, b, c, d, y0, y1, keep_y);
input a, b, c, d;
output y0, y1, keep_y;

xor x0(y0, a, b);
xor x1(y1, c, d);
nand keep0(keep_y, a, d);

endmodule
