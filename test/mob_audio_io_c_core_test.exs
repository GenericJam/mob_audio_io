defmodule MobAudioIo.CCoreTest do
  # Compiles and runs test/c/mai_core_test.c against
  # priv/native/common/mai_core.c (registry, render clock and counters,
  # mai_post queue). test_helper.exs excludes :requires_cc without `cc`.
  use ExUnit.Case, async: true

  @moduletag :requires_cc

  @root Path.expand("..", __DIR__)

  setup do
    dir = Path.join(System.tmp_dir!(), "mai_c_test_#{System.unique_integer([:positive])}")
    File.mkdir_p!(dir)
    on_exit(fn -> File.rm_rf!(dir) end)
    %{dir: dir}
  end

  test "the C core passes its host test", %{dir: dir} do
    bin = Path.join(dir, "mai_core_test")

    args =
      ~w(-std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror -pedantic) ++
        [Path.join(@root, "test/c/mai_core_test.c"), "-o", bin, "-lm", "-lpthread"]

    {out, status} = System.cmd(System.find_executable("cc"), args, stderr_to_stdout: true)
    assert status == 0, "cc failed:\n" <> out

    {out, status} = System.cmd(bin, [], stderr_to_stdout: true)
    assert status == 0, "mai_core_test failed:\n" <> out
  end
end
