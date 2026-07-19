module functional_merge_rollback_circuit (
    input a,
    input b
);
    wire n_and1;
    wire n_and2;

    and g_and1 (n_and1, a, b);
    and g_and2 (n_and2, b, a);
endmodule
