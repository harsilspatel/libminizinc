% RUNS ON mzn20_fd
% RUNS ON mzn-fzn_fd
% RUNS ON mzn20_fd_linear
% RUNS ON mzn20_mip

include "nvalue.mzn";

array[1..4] of var 1..5: a;
var 1..10: n;

constraint nvalue(n, [1, 1, 2, 2, 3, 3]);
constraint nvalue(4, [3, 1, 7, 16]);
constraint nvalue(3, a);

solve satisfy;

output [
   "a = array1d(1..4, ", show(a), ");\n",
   "n = ", show(n), ";\n"
];
