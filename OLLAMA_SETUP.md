# Ollama + DeepSeek Integration Setup

This project has been successfully converted from using OpenRouter API to using local Ollama with DeepSeek models.

## Prerequisites

1. **Install Ollama**: Download and install Ollama from https://ollama.ai/
2. **Install DeepSeek Model**: Run the following command to pull the DeepSeek Coder model:
   ```bash
   ollama pull deepseek-coder:latest
   ```

## What Changed

### Files Modified/Renamed:
- `ai/ai_open_router.cpp` → `ai/ai_ollama.cpp`
- `ai/ai_open_router.h` → `ai/ai_ollama.h`
- Updated class name from `OpenRouter` to `Ollama`
- Removed API key requirements (Ollama runs locally)
- Updated all API endpoints to use `http://localhost:11434/api/chat`

### Model Configuration:
- **Model**: `deepseek-coder:latest` (previously `deepseek-coder:6.7b`)
- **Temperature**: Optimized for coding tasks (0.1 for completion, 0.3 for chat)
- **Context Length**: Increased to 1000-200 tokens for better responses
- **Added Parameters**: `top_p`, `stop` tokens for better control

### Features:
- **Tab Completion**: Press Tab in the editor for AI code completion
- **AI Agent**: Full conversational AI agent with tool calling support
- **Dynamic Model Selection**: Dropdown shows your locally available Ollama models
- **Model Auto-Discovery**: Automatically fetches available models from Ollama API
- **Model Refresh**: Manual refresh button to update model list
- **Streaming**: Real-time response streaming for better user experience
- **No API Costs**: Everything runs locally through Ollama

## Usage

### Starting Ollama
Make sure Ollama is running before using the AI features:
```bash
ollama serve
```

### AI Tab Completion
1. Open any code file in the editor
2. Place cursor where you want code completion
3. Press Tab to trigger AI completion
4. Accept completion with Tab, dismiss with Escape

### AI Agent
1. Use the AI Agent panel for conversational assistance
2. Select your preferred model from the dropdown (shows your local Ollama models)
3. Click the 🔄 refresh button to update the model list
4. Ask questions about code, request explanations, get help with debugging
5. The agent can execute tool calls for complex tasks

## Troubleshooting

### Common Issues:

1. **"Model not found" error**:
   ```bash
   ollama pull deepseek-coder:latest
   ```

2. **"Ollama server error"**:
   - Check if Ollama is running: `ollama serve`
   - Verify port 11434 is available: `netstat -ln | grep 11434`

3. **Slow responses**:
   - DeepSeek requires significant RAM (8GB+ recommended)
   - Consider using a smaller model like `deepseek-coder:1.3b` if needed

### Alternative Models

If you want to use a different model, update the model name in:
- `ai/ai_ollama.cpp` (search for `deepseek-coder:latest`)

Available DeepSeek models:
- `deepseek-coder:1.3b` (lighter, faster)
- `deepseek-coder:6.7b` (balanced)
- `deepseek-coder:latest` (best quality)

## Performance Tips

1. **GPU Acceleration**: Ollama automatically uses GPU if available (CUDA/Metal)
2. **Memory**: Ensure sufficient RAM for the model size
3. **Concurrent Requests**: The system handles multiple concurrent requests efficiently

## Benefits of Local AI

- ✅ **Privacy**: Your code never leaves your machine
- ✅ **No API Costs**: No usage fees or rate limits
- ✅ **Offline**: Works without internet connection
- ✅ **Customizable**: Full control over models and parameters
- ✅ **Fast**: No network latency for requests