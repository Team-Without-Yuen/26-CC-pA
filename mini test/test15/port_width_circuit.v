module port_width_circuit(clk, data_in, flag, result, done);
input clk;
input [3:0] data_in;
input flag;
output [7:0] result;
output done;

buf g_done(done, flag);
buf g_result0(result[0], data_in[0]);
endmodule
