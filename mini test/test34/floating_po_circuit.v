module floating_po_circuit(a, b, y, z, bus_out);
input a, b;
output y, z;
output [1:0] bus_out;

wire live_mid;
wire dead_mid;
wire dead_out;
wire unused_net;

and g_live(live_mid, a, b);
buf g_y(y, live_mid);

and g_dead(dead_mid, a, b);
buf g_dead_out(dead_out, dead_mid);

// z and bus_out are intentionally undriven primary outputs.
// unused_net is intentionally unused and may be removed by cleanup.

endmodule
