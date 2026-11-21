defmodule MobAudioIo.MixProject do
  use Mix.Project

  @source_url "https://github.com/GenericJam/mob_audio_io"

  def project do
    [
      app: :mob_audio_io,
      version: "0.1.0-dev",
      elixir: "~> 1.18",
      start_permanent: false,
      deps: deps(),
      aliases: aliases(),
      description:
        "Low-latency audio streams for Mob apps (AAudio on Android, RemoteIO on iOS) with a C ABI for the app's own DSP",
      package: package(),
      docs: [
        main: "MobAudioIo",
        extras: ["README.md", "SPIKE.md"]
      ],
      source_url: @source_url
    ]
  end

  def application do
    [
      extra_applications: [:logger]
    ]
  end

  defp aliases do
    [setup: ["deps.get", "cmd git config core.hooksPath .githooks"]]
  end

  defp deps do
    [
      {:mob, "~> 0.9 and >= 0.9.31"},
      {:mob_dev, "~> 0.7.31", only: [:dev, :test], runtime: false},
      {:ex_doc, "~> 0.34", only: :dev, runtime: false},
      {:credo, "~> 1.7", only: [:dev, :test], runtime: false},
      {:ex_slop, "~> 0.4.2", only: [:dev, :test], runtime: false},
      {:jump_credo_checks, "~> 0.1.0", only: [:dev, :test], runtime: false}
    ]
  end

  defp package do
    [
      licenses: ["MIT"],
      links: %{"GitHub" => @source_url},
      # Native sources + manifest must ship: the host's native build compiles
      # them from deps/mob_audio_io/priv.
      files: ~w(lib src priv mix.exs README* CHANGELOG* LICENSE*)
    ]
  end
end
