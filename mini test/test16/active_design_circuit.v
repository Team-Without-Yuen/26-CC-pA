module active_design_circuit(a, y);
input a;
output y;
wire dead_logic;

and g_dead(dead_logic, a, 1'b0);
buf g_live(y, a);
endmodule
