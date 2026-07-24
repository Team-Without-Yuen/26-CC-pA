module rewrite_scope_circuit (
    input a,
    input b,
    input ck,
    output q,
    output y
);
    wire n_and;
    wire n_xor;
    wire d;

    and g_and(n_and, a, b);
    xor g_xor(n_xor, a, b);
    or g_data(d, n_and, n_xor);
    dff ff0(.D(d), .CK(ck), .Q(q));
    buf g_out(y, a);
endmodule
