module depth_filter(a, clk, y1, y2, y3);
    input a, clk;
    output y1, y2, y3;
    wire q;

    buf g1(y1, a);
    not g2(y2, y1);
    buf g3(y3, y2);
    dff ff0(.D(y2), .CK(clk), .RN(1'b1), .SN(1'b1), .Q(q));
endmodule
