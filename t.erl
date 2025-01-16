-module(t).
-export([t/1]).

t(Data) ->
    <<Rest/bits>> = Data,
    {_, Bits} = ext:ernal(Rest),
    case Rest of
        <<0, _:Bits>> -> not_empty;
        <<>> -> empty
    end.
