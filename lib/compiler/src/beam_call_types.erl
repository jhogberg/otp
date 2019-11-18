%%
%% %CopyrightBegin%
%%
%% Copyright Ericsson AB 2019. All Rights Reserved.
%%
%% Licensed under the Apache License, Version 2.0 (the "License");
%% you may not use this file except in compliance with the License.
%% You may obtain a copy of the License at
%%
%%     http://www.apache.org/licenses/LICENSE-2.0
%%
%% Unless required by applicable law or agreed to in writing, software
%% distributed under the License is distributed on an "AS IS" BASIS,
%% WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
%% See the License for the specific language governing permissions and
%% limitations under the License.
%%
%% %CopyrightEnd%
%%

-module(beam_call_types).

-include("beam_types.hrl").

-import(lists, [duplicate/2,foldl/3]).

-export([will_succeed/3, types/3]).

%%
%% Returns whether a call will succeed or not.
%%
%% Note that it only answers 'yes' for functions in the 'erlang' module as
%% calls to other modules may fail due to not being loaded, even if we consider
%% the module to be known.
%%

-spec will_succeed(Mod, Func, ArgTypes) -> Result when
      Mod :: atom(),
      Func :: atom(),
      ArgTypes :: [normal_type()],
      Result :: yes | no | maybe.

will_succeed(erlang, '++', [LHS, _RHS]) ->
    succeeds_if_type(LHS, proper_list());
will_succeed(erlang, '--', [LHS, RHS]) ->
    case {succeeds_if_type(LHS, proper_list()),
          succeeds_if_type(RHS, proper_list())} of
        {yes, yes} -> yes;
        {no, _} -> no;
        {_, no} -> no;
        {_, _} -> maybe
    end;
will_succeed(erlang, BoolOp, [LHS, RHS]) when BoolOp =:= 'and';
                                              BoolOp =:= 'or' ->
    case {succeeds_if_type(LHS, beam_types:make_boolean()),
          succeeds_if_type(RHS, beam_types:make_boolean())} of
        {yes, yes} -> yes;
        {no, _} -> no;
        {_, no} -> no;
        {_, _} -> maybe
    end;
will_succeed(erlang, bit_size, [Arg]) ->
    succeeds_if_type(Arg, #t_bitstring{});
will_succeed(erlang, byte_size, [Arg]) ->
    succeeds_if_type(Arg, #t_bitstring{});
will_succeed(erlang, hd, [Arg]) ->
    succeeds_if_type(Arg, #t_cons{});
will_succeed(erlang, length, [Arg]) ->
    succeeds_if_type(Arg, proper_list());
will_succeed(erlang, map_size, [Arg]) ->
    succeeds_if_type(Arg, #t_map{});
will_succeed(erlang, 'not', [Arg]) ->
    succeeds_if_type(Arg, beam_types:make_boolean());
will_succeed(erlang, setelement, [#t_integer{elements={Min,Max}},
                                  #t_tuple{exact=Exact,size=Size}, _]) ->
    case Min >= 1 andalso Max =< Size of
        true -> yes;
        false when Exact -> no;
        false -> maybe
    end;
will_succeed(erlang, size, [Arg]) ->
    succeeds_if_type(Arg, #t_bitstring{});
will_succeed(erlang, tuple_size, [Arg]) ->
    succeeds_if_type(Arg, #t_tuple{});
will_succeed(erlang, tl, [Arg]) ->
    succeeds_if_type(Arg, #t_cons{});
will_succeed(Mod, Func, Args) ->
    Arity = length(Args),
    case erl_bifs:is_safe(Mod, Func, Arity) of
        true ->
            yes;
        false ->
            case erl_bifs:is_exit_bif(Mod, Func, Arity) of
                true ->
                    no;
                false ->
                    %% While we can't infer success for functions outside the
                    %% 'erlang' module (see above comment), it's safe to infer
                    %% failure when we know the arguments must have certain
                    %% types.
                    {_, ArgTypes, _} = types(Mod, Func, Args),
                    fails_on_conflict(Args, ArgTypes)
            end
    end.

fails_on_conflict([ArgType | Args], [Required | Types]) ->
    case beam_types:meet(ArgType, Required) of
        none -> no;
        _ -> fails_on_conflict(Args, Types)
    end;
fails_on_conflict([], []) ->
    maybe.

succeeds_if_type(ArgType, Required) ->
    case beam_types:meet(ArgType, Required) of
        ArgType -> yes;
        none -> no;
        _ -> maybe
    end.

%%
%% Returns the inferred return and argument types for known functions, and
%% whether it's safe to subtract argument types on failure.
%%
%% Note that the return type will be 'none' if we can statically determine that
%% the function will fail at runtime.
%%

-spec types(Mod, Func, ArgTypes) -> {RetType, ArgTypes, CanSubtract} when
      Mod :: atom(),
      Func :: atom(),
      ArgTypes :: [normal_type()],
      RetType :: type(),
      CanSubtract :: boolean().

%% Functions that only fail due to bad argument *types*, meaning it's safe to
%% subtract argument types on failure.
%%
%% Note that these are all from the erlang module; suitable functions in other
%% modules could fail due to the module not being loaded.
types(erlang, 'map_size', [_]) ->
    sub_safe(#t_integer{}, [#t_map{}]);
types(erlang, 'tuple_size', [_]) ->
    sub_safe(#t_integer{}, [#t_tuple{}]);
types(erlang, 'bit_size', [_]) ->
    sub_safe(#t_integer{}, [#t_bitstring{}]);
types(erlang, 'byte_size', [_]) ->
    sub_safe(#t_integer{}, [#t_bitstring{}]);
types(erlang, 'hd', [Cons]) ->
    RetType = case Cons of
                  #t_cons{head=Type} -> Type;
                  #t_list{type=Type} -> Type;
                  _ -> any
              end,
    sub_safe(RetType, [#t_cons{}]);
types(erlang, 'tl', [Cons]) ->
    RetType = case Cons of
                  #t_cons{head=T,proper=true} -> #t_list{type=T,proper=true};
                  #t_list{proper=true} -> Cons;
                  _ -> any
              end,
    sub_safe(RetType, [#t_cons{}]);
types(erlang, 'not', [_]) ->
    Bool = beam_types:make_boolean(),
    sub_safe(Bool, [Bool]);
types(erlang, 'length', [_]) ->
    sub_safe(#t_integer{}, [proper_list()]);

%% Boolean ops
types(erlang, 'and', [_,_]) ->
    Bool = beam_types:make_boolean(),
    sub_unsafe(Bool, [Bool, Bool]);
types(erlang, 'or', [_,_]) ->
    Bool = beam_types:make_boolean(),
    sub_unsafe(Bool, [Bool, Bool]);
types(erlang, 'xor', [_,_]) ->
    Bool = beam_types:make_boolean(),
    sub_unsafe(Bool, [Bool, Bool]);

%% Bitwise ops
types(erlang, 'band', [_,_]=Args) ->
    sub_unsafe(band_return_type(Args), [#t_integer{}, #t_integer{}]);
types(erlang, 'bor', [_,_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}, #t_integer{}]);
types(erlang, 'bxor', [_,_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}, #t_integer{}]);
types(erlang, 'bsl', [_,_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}, #t_integer{}]);
types(erlang, 'bsr', [_,_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}, #t_integer{}]);
types(erlang, 'bnot', [_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}]);

%% Fixed-type arithmetic
types(erlang, 'float', [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(erlang, 'round', [_]) ->
    sub_unsafe(#t_integer{}, [number]);
types(erlang, 'floor', [_]) ->
    sub_unsafe(#t_integer{}, [number]);
types(erlang, 'ceil', [_]) ->
    sub_unsafe(#t_integer{}, [number]);
types(erlang, 'trunc', [_]) ->
    sub_unsafe(#t_integer{}, [number]);
types(erlang, '/', [_,_]) ->
    sub_unsafe(#t_float{}, [number, number]);
types(erlang, 'div', [_,_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}, #t_integer{}]);
types(erlang, 'rem', [_,_]) ->
    sub_unsafe(#t_integer{}, [#t_integer{}, #t_integer{}]);

%% Mixed-type arithmetic; '+'/2 and friends are handled in the catch-all
%% clause for the 'erlang' module.
types(erlang, 'abs', [_]=Args) ->
    mixed_arith_types(Args);

%% List operations
types(erlang, '++', [LHS,RHS]) ->
    %% `[] ++ RHS` yields RHS, even if RHS is not a list.
    RetType = beam_types:join(make_list(LHS, same_type, same_length), RHS),
    sub_unsafe(RetType, [proper_list(), any]);
types(erlang, '--', [LHS,_]) ->
    sub_unsafe(make_list(LHS, same_type, new_length),
               [proper_list(), proper_list()]);

types(erlang, 'iolist_to_binary', [_]) ->
    %% Arg is an iodata(), despite its name.
    ArgType = beam_types:join(#t_list{}, #t_bitstring{size_unit=8}),
    sub_unsafe(#t_bitstring{size_unit=8}, [ArgType]);
types(erlang, 'list_to_binary', [_]) ->
    %% Arg is an iolist(), despite its name.
    sub_unsafe(#t_bitstring{size_unit=8}, [#t_list{}]);
types(erlang, 'list_to_bitstring', [_]) ->
    %% As list_to_binary but with bitstrings rather than binaries.
    sub_unsafe(#t_bitstring{}, [proper_list()]);

%% Misc ops.
types(erlang, 'binary_part', [_, _]) ->
    PosLen = make_two_tuple(#t_integer{}, #t_integer{}),
    Binary = #t_bitstring{size_unit=8},
    sub_unsafe(Binary, [Binary, PosLen]);
types(erlang, 'binary_part', [_, _, _]) ->
    Binary = #t_bitstring{size_unit=8},
    sub_unsafe(Binary, [Binary, #t_integer{}, #t_integer{}]);
types(erlang, 'is_map_key', [_,_]) ->
    sub_unsafe(beam_types:make_boolean(), [any,#t_map{}]);
types(erlang, 'map_get', [_,_]) ->
    sub_unsafe(any, [any,#t_map{}]);
types(erlang, 'node', [_]) ->
    sub_unsafe(#t_atom{}, [any]);
types(erlang, 'node', []) ->
    sub_unsafe(#t_atom{}, []);
types(erlang, 'size', [_]) ->
    sub_unsafe(#t_integer{}, [any]);

%% Tuple element ops
types(erlang, element, [PosType, TupleType]) ->
    Index = case PosType of
                #t_integer{elements={Same,Same}} when is_integer(Same) ->
                    Same;
                _ ->
                    0
            end,

    RetType = case TupleType of
                  #t_tuple{size=Sz,elements=Es} when Index =< Sz,
                                                     Index >= 1 ->
                      beam_types:get_tuple_element(Index, Es);
                  _ ->
                      any
              end,

    sub_unsafe(RetType, [#t_integer{}, #t_tuple{size=Index}]);
types(erlang, setelement, [PosType, TupleType, ArgType]) ->
    RetType = case {PosType,TupleType} of
                  {#t_integer{elements={Index,Index}},
                   #t_tuple{elements=Es0,size=Size}=T} when Index >= 1 ->
                      %% This is an exact index, update the type of said
                      %% element or return 'none' if it's known to be out of
                      %% bounds.
                      Es = beam_types:set_tuple_element(Index, ArgType, Es0),
                      case T#t_tuple.exact of
                          false ->
                              T#t_tuple{size=max(Index, Size),elements=Es};
                          true when Index =< Size ->
                              T#t_tuple{elements=Es};
                          true ->
                              none
                      end;
                  {#t_integer{elements={Min,Max}},
                   #t_tuple{elements=Es0,size=Size}=T} when Min >= 1 ->
                      %% We know this will land between Min and Max, so kill
                      %% the types for those indexes.
                      Es = discard_tuple_element_info(Min, Max, Es0),
                      case T#t_tuple.exact of
                          false ->
                              T#t_tuple{elements=Es,size=max(Min, Size)};
                          true when Min =< Size ->
                              T#t_tuple{elements=Es,size=Size};
                          true ->
                              none
                      end;
                  {_,#t_tuple{}=T} ->
                      %% Position unknown, so we have to discard all element
                      %% information.
                      T#t_tuple{elements=#{}};
                  {#t_integer{elements={Min,_Max}},_} ->
                      #t_tuple{size=Min};
                  {_,_} ->
                      #t_tuple{}
              end,
    sub_unsafe(RetType, [#t_integer{}, #t_tuple{}, any]);

types(erlang, make_fun, [_,_,Arity0]) ->
    Type = case Arity0 of
               #t_integer{elements={Arity,Arity}} when Arity >= 0 ->
                   #t_fun{arity=Arity};
               _ ->
                   #t_fun{}
           end,
    sub_unsafe(Type, [#t_atom{}, #t_atom{}, #t_integer{}]);

types(erlang, Name, Args) ->
    Arity = length(Args),

    case erl_bifs:is_exit_bif(erlang, Name, Arity) of
        true ->
            {none, Args, false};
        false ->
            case erl_internal:arith_op(Name, Arity) of
                true ->
                    mixed_arith_types(Args);
                false ->
                    IsTest =
                        erl_internal:new_type_test(Name, Arity) orelse
                        erl_internal:comp_op(Name, Arity),

                    RetType = case IsTest of
                                  true -> beam_types:make_boolean();
                                  false -> any
                              end,

                    sub_unsafe(RetType, duplicate(Arity, any))
            end
    end;

%%
%% Math BIFs
%%

types(math, cos, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, cosh, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, sin, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, sinh, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, tan, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, tanh, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, acos, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, acosh, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, asin, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, asinh, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, atan, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, atanh, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, erf, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, erfc, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, exp, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, log, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, log2, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, log10, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, sqrt, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, atan2, [_,_]) ->
    sub_unsafe(#t_float{}, [number, number]);
types(math, pow, [_,_]) ->
    sub_unsafe(#t_float{}, [number, number]);
types(math, ceil, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, floor, [_]) ->
    sub_unsafe(#t_float{}, [number]);
types(math, fmod, [_,_]) ->
    sub_unsafe(#t_float{}, [number, number]);
types(math, pi, []) ->
    sub_unsafe(#t_float{}, []);

%%
%% List functions
%%
%% These tend to have tricky edge cases around nil and proper lists, be very
%% careful and try not to narrow the types needlessly. Keep in mind that they
%% need to be safe regardless of how the function is implemented, so it's best
%% not to say that a list is proper unless every element must be visited to
%% succeed.
%%

%% Operator aliases.
types(lists, append, [_,_]=Args) ->
    types(erlang, '++', Args);
types(lists, append, [_]) ->
    %% This is implemented through folding the list over erlang:'++'/2, so it
    %% can hypothetically return anything, but we can infer that its argument
    %% is a proper list on success.
    sub_unsafe(any, [proper_list()]);
types(lists, subtract, [_,_]=Args) ->
    types(erlang, '--', Args);

%% Functions returning booleans.
types(lists, all, [_,_]) ->
    %% This can succeed on improper lists if the fun returns 'false' for an
    %% element before reaching the end.
    sub_unsafe(beam_types:make_boolean(), [#t_fun{arity=1}, #t_list{}]);
types(lists, any, [_,_]) ->
    %% Doesn't imply that the argument is a proper list; see lists:all/2
    sub_unsafe(beam_types:make_boolean(), [#t_fun{arity=1}, #t_list{}]);
types(lists, keymember, [_,_,_]) ->
    %% Doesn't imply that the argument is a proper list; see lists:all/2
    sub_unsafe(beam_types:make_boolean(), [any, #t_integer{}, #t_list{}]);
types(lists, member, [_,_]) ->
    %% Doesn't imply that the argument is a proper list; see lists:all/2
    sub_unsafe(beam_types:make_boolean(), [any, #t_list{}]);
types(lists, prefix, [_,_]) ->
    %% This function doesn't need to reach the end of either list to return
    %% false, so we can succeed even when both are improper lists.
    sub_unsafe(beam_types:make_boolean(), [#t_list{}, #t_list{}]);
types(lists, suffix, [_,_]) ->
    %% A different implementation could return true when the first list is nil,
    %% so we can't tell if either is proper.
    sub_unsafe(beam_types:make_boolean(), [#t_list{}, #t_list{}]);

%% Functions returning plain lists.
types(lists, droplast, [List]) ->
    RetType = make_list(List, same_type, new_length),
    sub_unsafe(RetType, [proper_list()]);
types(lists, dropwhile, [_Fun, List]) ->
    %% Doesn't imply that the argument is a proper list; see lists:all/2
    RetType = make_list(List, same_type, new_length),
    sub_unsafe(RetType, [#t_fun{arity=1}, #t_list{}]);
types(lists, duplicate, [_Count, Element]) ->
    sub_unsafe(#t_list{type=Element,proper=true}, [#t_integer{}, any]);
types(lists, filter, [_Fun, List]) ->
    RetType = make_list(List, same_type, new_length),
    sub_unsafe(RetType, [#t_fun{arity=1}, proper_list()]);
types(lists, flatten, [_]) ->
    sub_unsafe(proper_list(), [proper_list()]);
types(lists, map, [_Fun, List]) ->
    RetType = make_list(List, new_type, same_length),
    sub_unsafe(RetType, [#t_fun{arity=1}, proper_list()]);
types(lists, reverse, [List]) ->
    RetType = make_list(List, same_type, same_length),
    sub_unsafe(RetType, [proper_list()]);
types(lists, sort, [List]) ->
    RetType = make_list(List, same_type, same_length),
    sub_unsafe(RetType, [proper_list()]);
types(lists, takewhile, [_Fun, List]) ->
    %% Doesn't imply that the argument is a proper list; see lists:all/2
    RetType = make_list(List, same_type, new_length),
    sub_unsafe(RetType, [#t_fun{arity=1}, #t_list{}]);
types(lists, usort, [List]) ->
    RetType = make_list(List, same_type, same_length),
    sub_unsafe(RetType, [proper_list()]);
types(lists, zip, [_,_]=Lists) ->
    {RetType, ArgType} = lists_zip_types(Lists),
    sub_unsafe(RetType, [ArgType, ArgType]);
types(lists, zipwith, [Fun | [_,_]=Lists]) ->
    {RetType, ArgType} = lists_zipwith_types(Fun, Lists),
    sub_unsafe(RetType, [#t_fun{arity=2}, ArgType, ArgType]);

%% Functions with complex return values.
types(lists, keyfind, [KeyType,PosType,_]) ->
    %% Doesn't imply a proper list; see lists:all/2
    TupleType = case PosType of
                    #t_integer{elements={Index,Index}} when is_integer(Index),
                                                            Index >= 1 ->
                        Es = beam_types:set_tuple_element(Index, KeyType, #{}),
                        #t_tuple{size=Index,elements=Es};
                    _ ->
                        #t_tuple{}
                end,
    RetType = beam_types:join(TupleType, beam_types:make_atom(false)),
    sub_unsafe(RetType, [any, #t_integer{}, #t_list{}]);
types(lists, MapFold, [_Fun, _Init, List])
  when MapFold =:= mapfoldl; MapFold =:= mapfoldr ->
    RetType = make_two_tuple(make_list(List, new_type, same_length), any),
    sub_unsafe(RetType, [#t_fun{arity=2}, any, proper_list()]);
types(lists, partition, [_,List]) ->
    Type = make_list(List, same_type, new_length),
    sub_unsafe(make_two_tuple(Type, Type),
               [#t_fun{arity=1}, Type]);
types(lists, splitwith, [_,_]) ->
    %% Doesn't imply that the argument is a proper list; see lists:all/2
    RetType = make_two_tuple(proper_list(), #t_list{}),
    sub_unsafe(RetType, [#t_fun{arity=1}, #t_list{}]);
types(lists, unzip, [List]) ->
    RetType = lists_unzip_type(2, List),
    sub_unsafe(RetType, [proper_list()]);

%% Catch-all clause for unknown functions.

types(_, _, Args) ->
    sub_unsafe(any, [any || _ <- Args]).

%%
%% Helpers
%%

sub_unsafe(RetType, ArgTypes) ->
    {RetType, ArgTypes, false}.

sub_safe(RetType, ArgTypes) ->
    {RetType, ArgTypes, true}.

mixed_arith_types([FirstType | _]=Args0) ->
    RetType = foldl(fun(#t_integer{}, #t_integer{}) -> #t_integer{};
                       (#t_integer{}, number) -> number;
                       (#t_integer{}, #t_float{}) -> #t_float{};
                       (#t_float{}, #t_integer{}) -> #t_float{};
                       (#t_float{}, number) -> #t_float{};
                       (#t_float{}, #t_float{}) -> #t_float{};
                       (number, #t_integer{}) -> number;
                       (number, #t_float{}) -> #t_float{};
                       (number, number) -> number;
                       (any, _) -> number;
                       (_, _) -> none
                    end, FirstType, Args0),
    sub_unsafe(RetType, [number || _ <- Args0]).

band_return_type([#t_integer{elements={Int,Int}}, RHS]) when is_integer(Int) ->
    band_return_type_1(RHS, Int);
band_return_type([LHS, #t_integer{elements={Int,Int}}]) when is_integer(Int) ->
    band_return_type_1(LHS, Int);
band_return_type(_) ->
    #t_integer{}.

band_return_type_1(LHS, Int) ->
    case LHS of
        #t_integer{elements={Min0,Max0}} when Max0 - Min0 < 1 bsl 256 ->
            {Intersection, Union} = range_masks(Min0, Max0),

            Min = Intersection band Int,
            Max = min(Max0, Union band Int),

            #t_integer{elements={Min,Max}};
        _ when Int >= 0 ->
            %% The range is either unknown or too wide, conservatively assume
            %% that the new range is 0 .. Int.
            #t_integer{elements={0,Int}};
        _ when Int < 0 ->
            %% We can't infer boundaries when the range is unknown and the
            %% other operand is a negative number, as the latter sign-extends
            %% to infinity and we can't express an inverted range at the
            %% moment (cf. X band -8; either less than -7 or greater than 7).
            #t_integer{}
    end.

%% Returns two bitmasks describing all possible values between From and To.
%%
%% The first contains the bits that are common to all values, and the second
%% contains the bits that are set by any value in the range.
range_masks(From, To) when From =< To ->
    range_masks_1(From, To, 0, -1, 0).

range_masks_1(From, To, BitPos, Intersection, Union) when From < To ->
    range_masks_1(From + (1 bsl BitPos), To, BitPos + 1,
                  Intersection band From, Union bor From);
range_masks_1(_From, To, _BitPos, Intersection0, Union0) ->
    Intersection = To band Intersection0,
    Union = To bor Union0,
    {Intersection, Union}.

discard_tuple_element_info(Min, Max, Es) ->
    foldl(fun(El, Acc) when Min =< El, El =< Max ->
                  maps:remove(El, Acc);
             (_El, Acc) -> Acc
          end, Es, maps:keys(Es)).

proper_list() ->
    #t_list{proper=true}.

%% Constructs a new list type based on another, optionally keeping the original
%% type and/or length.
make_list(#t_cons{proper=true}=T, same_type, same_length) ->
    T;
make_list(#t_cons{head=Head,proper=true}, same_type, _) ->
    #t_list{type=Head,proper=true};
make_list(#t_list{proper=true}=T, same_type, _) ->
    T;
make_list(nil, _, same_length) ->
    nil;
make_list(_, _, _) ->
    #t_list{proper=true}.

make_two_tuple(Type1, Type2) ->
    Es0 = beam_types:set_tuple_element(1, Type1, #{}),
    Es = beam_types:set_tuple_element(2, Type2, Es0),
    #t_tuple{size=2,exact=true,elements=Es}.

%%
%% Function-specific helpers.
%%

lists_unzip_type(Size, List) ->
    Es = lut_make_elements(lut_list_types(Size, List), 1, #{}),
    #t_tuple{size=Size,exact=true,elements=Es}.

lut_make_elements([Type | Types], Index, Es0) ->
    Es = beam_types:set_tuple_element(Index, Type, Es0),
    lut_make_elements(Types, Index + 1, Es);
lut_make_elements([], _Index, Es) ->
    Es.

lut_list_types(Size, #t_cons{head=#t_tuple{size=Size,elements=Es}}) ->
    Types = lut_element_types(1, Size, Es),
    [#t_cons{head=T,proper=true} || T <- Types];
lut_list_types(Size, #t_list{type=#t_tuple{size=Size,elements=Es}}) ->
    Types = lut_element_types(1, Size, Es),
    [#t_list{type=T,proper=true} || T <- Types];
lut_list_types(Size, nil) ->
    lists:duplicate(Size, nil);
lut_list_types(Size, _) ->
    lists:duplicate(Size, proper_list()).

lut_element_types(Index, Max, #{}) when Index > Max ->
    [];
lut_element_types(Index, Max, Es) ->
    ElementType = beam_types:get_tuple_element(Index, Es),
    [ElementType | lut_element_types(Index + 1, Max, Es)].

%% lists:zip/2 and friends only succeed when all arguments have the same
%% length, so if one of them is #t_cons{}, we can infer that all of them are
%% #t_cons{} on success.

lists_zip_types(Types) ->
    lists_zip_types_1(Types, false, #{}, 1).

lists_zip_types_1([nil | _], _AnyCons, _Es, _N) ->
    %% Early exit; we know the result is [] on success.
    {nil, nil};
lists_zip_types_1([#t_cons{head=Type,proper=true} | Lists], _AnyCons, Es0, N) ->
    Es = beam_types:set_tuple_element(N, Type, Es0),
    lists_zip_types_1(Lists, true, Es, N + 1);
lists_zip_types_1([#t_list{type=Type,proper=true} | Lists], AnyCons, Es0, N) ->
    Es = beam_types:set_tuple_element(N, Type, Es0),
    lists_zip_types_1(Lists, AnyCons, Es, N + 1);
lists_zip_types_1([_ | Lists], AnyCons, Es, N) ->
    lists_zip_types_1(Lists, AnyCons, Es, N + 1);
lists_zip_types_1([], true, Es, N) ->
    %% At least one element was cons, so we know it's non-empty on success.
    ElementType = #t_tuple{exact=true,size=(N - 1),elements=Es},
    RetType = #t_cons{head=ElementType,proper=true},
    ArgType = #t_cons{proper=true},
    {RetType, ArgType};
lists_zip_types_1([], false, Es, N) ->
    ElementType = #t_tuple{exact=true,size=(N - 1),elements=Es},
    RetType = #t_list{type=ElementType,proper=true},
    ArgType = #t_list{proper=true},
    {RetType, ArgType}.

lists_zipwith_types(_Fun, Types) ->
    ListType = lists_zipwith_type_1(Types),
    {ListType, ListType}.

lists_zipwith_type_1([nil | _]) ->
    %% Early exit; we know the result is [] on success.
    nil;
lists_zipwith_type_1([#t_cons{} | _Lists]) ->
    %% Early exit; we know the result is cons on success.
    #t_cons{proper=true};
lists_zipwith_type_1([_ | Lists]) ->
    lists_zipwith_type_1(Lists);
lists_zipwith_type_1([]) ->
    #t_list{proper=true}.
