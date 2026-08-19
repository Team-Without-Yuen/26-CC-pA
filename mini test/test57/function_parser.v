module function_parser (
    input a,
    input b,
    input en,
    output y,
    output z
);
    and g_and (y, a, b);
    or  g_or  (z, a, b);
endmodule
