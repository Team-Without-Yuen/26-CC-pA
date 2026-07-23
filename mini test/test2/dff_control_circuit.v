module dff_control_circuit(d0, d1, d2, d3, d4, clk, rst, q0, q1, q2, q3, q4);
input d0, d1, d2, d3, d4, clk, rst;
output q0, q1, q2, q3, q4;

dff ff0(.D(d0), .CK(clk), .RN(rst), .SN(rst), .Q(q0));
dff ff1(.D(d1), .CK(clk), .RN(rst), .SN(rst), .Q(q1));
dff ff2(.D(d2), .CK(clk), .RN(rst), .SN(rst), .Q(q2));
dff ff3(.D(d3), .CK(clk), .RN(rst), .SN(rst), .Q(q3));
dff ff4(.D(d4), .CK(clk), .RN(rst), .SN(rst), .Q(q4));

endmodule
