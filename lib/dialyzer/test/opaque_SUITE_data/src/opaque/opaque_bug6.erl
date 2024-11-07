-module(opaque_bug6).
-export([record_update/1]).

record_update(R) ->
    Anno = element(2, R),
    [ln(Anno), Anno].

ln(Anno) ->
    opaque_adt:line(Anno).
