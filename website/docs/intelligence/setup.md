---
title: Set up a provider
---

# Set up a provider

Open the **Lume Intelligence** panel (**Tools > Lume Intelligence**) and choose **Set up Lume Intelligence...** (also in **Settings > Lume Intelligence**).

1. **Choose a provider**: **Anthropic**, **OpenAI** or **Local model**.
2. **Give it what it needs**:
   - **Anthropic**: paste your API key in the masked field.
   - **OpenAI**: **Continue with ChatGPT** to use your ChatGPT plan (below), or **Use an API key instead** and paste the key. **Load models** lists the models your account can use; click one to pick it.
   - **Local model**: the address of an AI server running **on this computer** (`http://127.0.0.1:<port>/...`) that accepts the Messages API format. Lume refuses any other address and never sends a cloud key there.
3. **Test connection**: sends one fixed sentence, none of your data.
4. **Enable**: available only after a successful test.

## Where keys are kept

API keys and the ChatGPT session are stored in your operating system's secret store (Keychain on macOS, Credential Manager on Windows, the Secret Service on Linux) under "Lume Data". They are never written to a file, a log or the screen. **Settings > Lume Intelligence** shows where the key comes from, with **Replace key** and **Remove key**.

The environment variables `ANTHROPIC_API_KEY` (or `LUME_AI_KEY`) and `OPENAI_API_KEY`, when set, take precedence over a saved key.

A Claude subscription token cannot be used: Anthropic allows third-party apps to use API keys only.

## Your ChatGPT plan

**Continue with ChatGPT** opens OpenAI's sign-in page in your browser. After you sign in, Lume keeps the session in the secret store and renews it in the background. Requests count against your plan's usage; when you reach a limit, Lume says so with a **Manage usage** link. **Sign out** ends the session. If a renewal is refused, Lume signs you out and asks you to sign in again.

The sign-in returns to Lume on port 1455 of your computer. If another program is using that port, Lume says so and stops; close that program and try again.

## Settings

**Settings > Lume Intelligence**:

- **Provider and model**: **Change provider, model or key...**, **Turn off**.
- **Fast model**: used for quick requests (Explain, Fix the error, **Cmd+K**, Document). Empty uses the main model.
- **Answers**: answers stream as they are written (Anthropic and the ChatGPT plan); a proposal is applied only when complete and checked. Shows this session's requests and tokens.
- **Remembered per connection**: what each connection allows (structure, category values) and its scope, with **Forget**.
- **Glossary**: your business terms (see [What is sent](what-is-sent.md#glossary)), with **Edit glossary...**.

Every answer also shows its tokens in the chat, and the panel header the conversation total.
