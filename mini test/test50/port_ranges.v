module port_ranges(descending, ascending, scalar_out);
input [3:0] descending;
input [0:2] ascending;
output scalar_out;

buf g_scalar(scalar_out, descending[0]);
endmodule
