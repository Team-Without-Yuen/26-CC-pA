module fanout_filter (
    input zero,
    input one,
    input two,
    input three,
    output y_one,
    output y_two,
    output y_three
);
    wire unused_internal;

    buf g_one (y_one, one);
    and g_two (y_two, two, two);
    and g_three_pair (unused_internal, three, three);
    buf g_three_single (y_three, three);
endmodule
