module unsupported_scope_probe (
    input a,
    input b,
    output good_y,
    output bad_y,
    output nand_y
);
    wire good1;
    wire good2;
    wire target;
    wire same1;
    wire same2;
    wire floating_input;
    wire bad1;
    wire bad2;

    and g_good1 (good1, a, b);
    and g_good2 (good2, b, a);
    buf g_good_out (good_y, good1);

    not g_target (target, a);
    buf g_same1 (same1, a);
    buf g_same2 (same2, a);
    buf g_nand_out (nand_y, target);

    and g_bad1 (bad1, a, floating_input);
    and g_bad2 (bad2, b, floating_input);
    or  g_bad_out (bad_y, bad1, bad2);
endmodule
