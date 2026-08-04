module unknown_primitive_probe (
    input select,
    input data0,
    input data1,
    output y
);
    mux g_mux (y, select, data0, data1);
endmodule
