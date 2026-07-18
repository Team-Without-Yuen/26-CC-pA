module functional_dependence_circuit(a, b, c, dependent, cancelled, absent, multi);
input a;
input b;
input c;
output dependent;
output cancelled;
output absent;
output multi;
wire internal_signal;

and g_dependent(dependent, a, b);
xor g_cancelled(cancelled, a, a);
buf g_absent(absent, b);
and g_multi(multi, a, b, c);
not g_internal(internal_signal, b);
endmodule
