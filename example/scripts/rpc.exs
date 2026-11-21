# mix run --no-start scripts/rpc.exs NODE 'ELIXIR' [TIMEOUT_MS]
#
# Evaluates ELIXIR on the app's node (after `mix mob.connect --no-iex`) and
# prints the result: the spike's measurements without an IEx session.
[node, src | rest] = System.argv()
timeout = case rest do
  [t] -> String.to_integer(t)
  [] -> 120_000
end

{:ok, _} = Node.start(:"spike_rpc_#{System.pid()}@127.0.0.1", :longnames)
Node.set_cookie(MobDev.DistCookie.for_project!())
node = String.to_atom(node)
true = Node.connect(node)

{result, _binding} = :rpc.call(node, Code, :eval_string, [src], timeout)
IO.inspect(result, pretty: true, limit: :infinity, printable_limit: 200)
