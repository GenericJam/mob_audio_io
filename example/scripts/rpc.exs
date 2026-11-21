# mix run --no-start scripts/rpc.exs NODE 'ELIXIR' [TIMEOUT_MS]
# mix run --no-start scripts/rpc.exs --iphone UDID 'ELIXIR' [TIMEOUT_MS]
#
# Evaluates ELIXIR on the app's node and prints the result: the
# verification's measurements without an IEx session.
#
# Android and the simulator: after `mix mob.connect --no-iex`, NODE is the
# app's node (e.g. audio_io_spike_android@127.0.0.1).
#
# A physical iPhone: the Mac can't reach the phone's Wi-Fi-named node
# (EHOSTUNREACH; macOS local-network privacy), so launch the app with its node
# named for 127.0.0.1 and go through usbmuxd (FMutate's tools/rpc.exs recipe):
#
#   xcrun devicectl device process launch --device UDID --terminate-existing \
#     --environment-variables '{"MOB_NODE_HOST":"127.0.0.1"}' com.genericjam.audioiospike
#
# This then forwards a free host port to the phone's 9101 with `iproxy` and
# starts a hidden host node that only connects out (dist_listen false) and
# finds the app's node at that port.
{target, src, timeout, undo} =
  case System.argv() do
    ["--iphone", udid, src | rest] ->
      {:ok, s} = :gen_tcp.listen(0, ip: {127, 0, 0, 1})
      {:ok, port} = :inet.port(s)
      :gen_tcp.close(s)

      iproxy =
        Port.open({:spawn_executable, System.find_executable("iproxy")}, [
          :binary,
          args: ["#{port}:9101", "-u", udid]
        ])

      {:os_pid, pid} = Port.info(iproxy, :os_pid)
      Process.sleep(500)
      :ok = :application.set_env(:kernel, :erl_epmd_node_listen_port, port)

      {:"audio_io_spike_ios@127.0.0.1", src, rest,
       fn -> System.cmd("kill", [Integer.to_string(pid)]) end}

    [node, src | rest] ->
      {String.to_atom(node), src, rest, fn -> :ok end}
  end

timeout =
  case timeout do
    [t] -> String.to_integer(t)
    [] -> 120_000
  end

try do
  {:ok, _} =
    :net_kernel.start(:"spike_rpc_#{System.pid()}@127.0.0.1", %{
      name_domain: :longnames,
      dist_listen: false
    })

  Node.set_cookie(target, MobDev.DistCookie.for_project!())
  true = Node.connect(target)
  {result, _binding} = :rpc.call(target, Code, :eval_string, [src], timeout)
  IO.inspect(result, pretty: true, limit: :infinity, printable_limit: 400)
after
  undo.()
end
