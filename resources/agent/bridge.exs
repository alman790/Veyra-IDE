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
      config = System.get_env("VEYRA_AGENT_CONFIG") || Path.join(directory, "config.dets")
      File.mkdir_p!(Path.dirname(config))
      File.chmod!(Path.dirname(config), 0o700)

      original = Path.expand(System.get_env("VEYRA_BEAMCORE_CONFIG") || "~/.beamcore/config.dets")

      if not File.exists?(config) and File.regular?(original) do
        File.cp!(original, config)
        File.chmod!(config, 0o600)
      end

      Application.put_env(:beamcore, :config_dets_path, config)
      {:ok, _} = Beamcore.Config.start_link()
      File.chmod!(config, 0o600)
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
          {:ok, %{"method" => "save_provider", "provider" => provider}} -> save_provider(provider)
          {:ok, %{"method" => "remove_provider", "name" => name}} -> remove_provider(name)
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
        %{
          name: provider.name,
          model: provider.default_model,
          local: provider.capabilities.local,
          endpoint: provider.base_url,
          auth: provider.auth,
          has_key: not is_nil((provider.config || %{})["api_key"])
        }
      end)

    emit(%{type: "providers", providers: providers})
  end

  defp save_provider(%{"name" => name, "endpoint" => endpoint, "model" => model} = values) do
    uri = URI.parse(endpoint)
    auth = values["auth"] || "bearer"

    if String.trim(name) == "" or String.trim(model) == "" or
         uri.scheme not in ["http", "https"] or is_nil(uri.host) or
         not is_nil(uri.userinfo) or not is_binary(auth) or byte_size(auth) > 64 do
      emit(%{type: "error", message: "Enter a provider name, HTTP endpoint and model."})
    else
      original = Beamcore.Config.get_provider(name) || %{}

      config =
        Map.merge(original, %{"base_url" => endpoint, "default_model" => model, "auth" => auth})

      config =
        if values["api_key"] in [nil, ""],
          do: config,
          else: Map.put(config, "api_key", values["api_key"])

      if auth != "none" and is_nil(config["api_key"]) and map_size(original) == 0 do
        emit(%{type: "error", message: "Enter an API key or choose no authentication."})
      else
        case Beamcore.Config.put_provider(name, config) do
          :ok -> providers()
          _ -> emit(%{type: "error", message: "The provider could not be saved."})
        end
      end
    end
  rescue
    _ -> emit(%{type: "error", message: "The provider could not be saved."})
  end

  defp remove_provider(name) when is_binary(name) do
    remaining = Beamcore.Config.list_providers() |> Map.delete(name)

    case Beamcore.Config.put(:api_configs, Jason.encode!(remaining)) do
      :ok -> providers()
      _ -> emit(%{type: "error", message: "The provider could not be removed."})
    end
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
