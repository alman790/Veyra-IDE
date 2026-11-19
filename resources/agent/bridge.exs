defmodule Veyra.BeamcoreBridge do
  @prefix "VEYRA_AGENT "

  def emit(message), do: IO.puts(@prefix <> Jason.encode!(message))

  def start do
    Logger.configure(level: :error)

    with {:ok, _} <- Application.ensure_all_started(:openai_ex),
         {:ok, _} <- Application.ensure_all_started(:req) do
      directory = System.fetch_env!("VEYRA_AGENT_STATE")
      File.mkdir_p!(directory)
      File.chmod!(directory, 0o700)
      config = Path.join(directory, "config.dets")
      original = Path.expand(System.get_env("VEYRA_BEAMCORE_CONFIG") || "~/.beamcore/config.dets")

      if File.regular?(original) do
        File.cp!(original, config)
        File.chmod!(config, 0o600)
      end

      Application.put_env(:beamcore, :config_dets_path, config)
      {:ok, _} = Beamcore.Config.start_link()
      {:ok, _} = Beamcore.Provider.Scheduler.start_link([])
      emit(%{type: "ready", version: 1})
      loop()
    else
      _ -> emit(%{type: "error", message: "Beamcore dependencies could not be started."})
    end
  rescue
    error -> emit(%{type: "error", message: Exception.message(error)})
  end

  defp loop do
    case IO.gets("") do
      :eof ->
        :ok

      {:error, _} ->
        :ok

      line ->
        case Jason.decode(line) do
          {:ok, %{"method" => "providers"}} -> providers()
          {:ok, %{"method" => "chat"} = request} -> chat(request)
          _ -> emit(%{type: "error", message: "Invalid IDE request."})
        end

        loop()
    end
  end

  defp providers do
    providers =
      Beamcore.Provider.Registry.list()
      |> Enum.filter(& &1.configured?)
      |> Enum.map(fn provider ->
        %{name: provider.name, model: provider.default_model, local: provider.capabilities.local}
      end)

    emit(%{type: "providers", providers: providers})
  end

  defp chat(%{"id" => id, "messages" => messages, "backend" => "local"} = request) do
    messages = Beamcore.Agent.Chat.ModelPayload.limit(messages, %{context_window: 8192})

    emit(%{
      type: "inference",
      id: id,
      messages: messages,
      max_tokens: request["max_tokens"] || 768
    })

    case IO.gets("") do
      line when is_binary(line) ->
        case Jason.decode(line) do
          {:ok, %{"method" => "inference_result", "id" => ^id, "content" => content}} ->
            emit(%{type: "response", id: id, content: content})

          _ ->
            emit(%{type: "error", id: id, message: "Invalid local inference response."})
        end

      _ ->
        :ok
    end
  end

  defp chat(
         %{"id" => id, "provider" => provider, "model" => model, "messages" => messages} = request
       ) do
    selection = %{provider: provider, model: model}

    case Beamcore.Agent.Chat.API.execute(nil, messages, [],
           selection: selection,
           model: model,
           temperature: 0,
           max_tokens: request["max_tokens"] || 1024
         ) do
      {:ok, %{message: message}} ->
        {content, _reasoning} = Beamcore.Agent.Chat.API.extract_reasoning(message)
        emit(%{type: "response", id: id, content: content})

      {:error, error} ->
        message = if is_binary(error), do: error, else: Exception.message(error)
        emit(%{type: "error", id: id, message: message})
    end
  rescue
    _ -> emit(%{type: "error", id: id, message: "The selected Beamcore provider failed."})
  end
end

Veyra.BeamcoreBridge.start()
