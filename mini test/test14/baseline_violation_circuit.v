module baseline_violation_circuit(a, y, floating_y);
input a;
output y, floating_y;

// floating_y intentionally has no driver; edits must not be blamed for it.
and g_and0(y, a, 1'b0);
endmodule
