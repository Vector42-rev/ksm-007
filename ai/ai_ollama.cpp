#include "ai_ollama.h"
#include "../lib/json.hpp"
#include "../util/settings.h"
#include <atomic>
#include <curl/curl.h>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

using json = nlohmann::json;

extern std::atomic<bool> g_should_cancel;
extern Settings gSettings;

// Global variable to hold the current token callback with mutex protection
static std::function<void(const std::string &)> g_currentTokenCallback = nullptr;
static std::mutex g_callbackMutex;

// Mutex to protect CURL initialization
static std::mutex g_curlInitMutex;

// Initialize the CURL initialization state
std::atomic<bool> Ollama::curl_initialized(false);
std::mutex Ollama::g_curlInitMutex;

// Global variables for tool call accumulation
static std::map<std::string, json> g_accumulatedToolCalls;
static std::mutex g_toolCallMutex;

// Global mapping from index to id for tool call accumulation
static std::map<int, std::string> g_indexToIdMapping;
static std::mutex g_indexMappingMutex;

// Global variables for full response accumulation
static json g_fullResponse;
static std::mutex g_responseMutex;
static std::function<void(const json &)> g_responseCallback = nullptr;

// Global variable to track malformed fragment counts for tool calls
static std::map<std::string, int> g_malformedFragmentCounts;
static std::mutex g_malformedFragmentMutex;

// Helper function to get CURL error string
std::string getCurlErrorString(CURLcode code) { return curl_easy_strerror(code); }

// Helper function to get HTTP status description
std::string getHttpStatusDescription(long http_code)
{
	switch (http_code)
	{
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 429:
		return "Too Many Requests";
	case 500:
		return "Internal Server Error";
	case 502:
		return "Bad Gateway";
	case 503:
		return "Service Unavailable";
	case 504:
		return "Gateway Timeout";
	default:
		return "Unknown";
	}
}

// Forward declaration for detailed error response function
std::string getDetailedErrorResponse(const std::string &payload,
									 const std::string &api_key);

bool Ollama::initializeCURL()
{
	std::lock_guard<std::mutex> lock(g_curlInitMutex);

	if (curl_initialized.load())
	{
		return true; // Already initialized
	}

	CURLcode res = curl_global_init(CURL_GLOBAL_ALL);
	if (res != CURLE_OK)
	{
		return false;
	}

	curl_initialized.store(true);
	return true;
}

void Ollama::cleanupCURL()
{
	std::lock_guard<std::mutex> lock(g_curlInitMutex);

	if (curl_initialized.load())
	{
		curl_global_cleanup();
		curl_initialized.store(false);
	}
}

size_t Ollama::WriteData(void *ptr, size_t size, size_t nmemb, std::string *data)
{
	if (g_should_cancel)
	{
		return 0; // Stop receiving data if cancelled
	}
	data->append((char *)ptr, size * nmemb);
	return size * nmemb;
}

// Simple write function to capture raw response body
size_t WriteDataRaw(void *ptr, size_t size, size_t nmemb, std::string *data)
{
	if (data)
	{
		data->append((char *)ptr, size * nmemb);
	}
	return size * nmemb;
}

std::string Ollama::request(const std::string &prompt, const std::string &api_key)
{
	if (g_should_cancel)
	{
		return "";
	}

	if (!initializeCURL())
	{
		return "";
	}

	CURL *curl = curl_easy_init();
	std::string response;
	long http_code = 0;

	if (curl)
	{
		// Ollama JSON payload format
		json payload = {{"model", "deepseek-coder:latest"},
						{"messages",
						 {{{"role", "system"},
						   {"content",
							"You are a code completion assistant. Provide only the code "
							"that should replace the cursor position. No markdown "
							"formatting, no explanations, just the raw code."}},
						  {{"role", "user"}, {"content", prompt}}}},
						{"stream", false},
						{"options",
						 {{"temperature", 0.1},
						  {"num_predict", 200},
						  {"top_p", 0.9},
						  {"stop", json::array({"\n\n", "```"})}}}};

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		std::string json_str = payload.dump();

		curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, gSettings.getAgentTimeout());
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		CURLM *multi_handle = curl_multi_init();
		curl_multi_add_handle(multi_handle, curl);

		int still_running = 1;
		while (still_running && !g_should_cancel)
		{
			CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
			if (mc != CURLM_OK)
			{
				break;
			}

			int numfds;
			mc = curl_multi_wait(multi_handle, nullptr, 0, 10, &numfds);
			if (mc != CURLM_OK)
			{
				break;
			}

			CURLMsg *msg;
			int msgs_left;
			while ((msg = curl_multi_info_read(multi_handle, &msgs_left)))
			{
				if (msg->msg == CURLMSG_DONE)
				{
					curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
					break;
				}
			}
		}

		curl_multi_remove_handle(multi_handle, curl);
		curl_multi_cleanup(multi_handle);
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		if (g_should_cancel)
		{
			return "";
		}

		if (http_code != 200)
		{
			return "HTTP error " + std::to_string(http_code) + ": " + response;
		}

		try
		{
			if (response.empty())
			{
				return "";
			}
			json result = json::parse(response);
			// Ollama response format: message.content
			if (!result.contains("message") || !result["message"].contains("content"))
			{
				return "";
			}
			std::string raw_content = result["message"]["content"].get<std::string>();
			if (raw_content.empty())
			{
				return "";
			}
			return sanitize_completion(raw_content);
		} catch (const json::exception &e)
		{
			return "";
		}
	}

	return "";
}

std::string Ollama::promptRequest(const std::string &prompt, const std::string &api_key)
{
	if (g_should_cancel)
	{
		return "";
	}

	if (!initializeCURL())
	{
		return "";
	}

	CURL *curl = curl_easy_init();
	std::string response;
	long http_code = 0;

	if (curl)
	{
		json payload = {{"model", "deepseek-coder:latest"},
						{"messages", {{{"role", "user"}, {"content", prompt}}}},
						{"stream", false},
						{"options",
						 {{"temperature", 0.3}, {"num_predict", 1000}, {"top_p", 0.95}}}};

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		std::string json_str = payload.dump();

		curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		CURLM *multi_handle = curl_multi_init();
		curl_multi_add_handle(multi_handle, curl);

		int still_running = 1;
		while (still_running && !g_should_cancel)
		{
			CURLMcode mc = curl_multi_perform(multi_handle, &still_running);
			if (mc != CURLM_OK)
			{
				break;
			}

			int numfds;
			mc = curl_multi_wait(multi_handle, nullptr, 0, 10, &numfds);
			if (mc != CURLM_OK)
			{
				break;
			}

			CURLMsg *msg;
			int msgs_left;
			while ((msg = curl_multi_info_read(multi_handle, &msgs_left)))
			{
				if (msg->msg == CURLMSG_DONE)
				{
					curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
					break;
				}
			}
		}

		curl_multi_remove_handle(multi_handle, curl);
		curl_multi_cleanup(multi_handle);
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		if (g_should_cancel)
		{
			return "";
		}

		if (http_code != 200)
		{
			return "HTTP error " + std::to_string(http_code) + ": " + response;
		}

		try
		{
			if (response.empty())
			{
				return "";
			}
			json result = json::parse(response);
			if (!result.contains("message") || !result["message"].contains("content"))
			{
				return "";
			}
			std::string raw_content = result["message"]["content"].get<std::string>();
			if (raw_content.empty())
			{
				return "";
			}
			return raw_content;
		} catch (const json::exception &e)
		{
			return "";
		}
	}

	return "";
}

size_t Ollama::WriteDataStream(void *ptr, size_t size, size_t nmemb, std::string *data)
{
	try
	{
		if (g_should_cancel)
		{
			return 0;
		}

		const char *charPtr = static_cast<const char *>(ptr);
		if (!charPtr)
		{
			return 0;
		}

		std::string chunk(charPtr, size * nmemb);

		// Ollama sends newline-delimited JSON (not SSE format)
		std::string buffer = chunk;
		size_t pos = 0;
		while ((pos = buffer.find('\n', pos)) != std::string::npos)
		{
			std::string line = buffer.substr(0, pos);
			buffer.erase(0, pos + 1);
			pos = 0;

			if (line.empty())
				continue;

			try
			{
				json result = json::parse(line);

				// Check if stream is done
				if (result.contains("done") && result["done"].get<bool>())
				{
					return size * nmemb;
				}

				// Extract content from message
				if (result.contains("message") && result["message"].contains("content"))
				{
					std::string content = result["message"]["content"].get<std::string>();
					if (!content.empty())
					{
						std::lock_guard<std::mutex> lock(g_callbackMutex);
						if (g_currentTokenCallback)
						{
							g_currentTokenCallback(content);
						}
					}
				}

				// Handle tool calls if present
				if (result.contains("message") &&
					result["message"].contains("tool_calls"))
				{
					std::lock_guard<std::mutex> lock(g_toolCallMutex);
					const auto &toolCalls = result["message"]["tool_calls"];

					for (size_t i = 0; i < toolCalls.size(); ++i)
					{
						const auto &toolCall = toolCalls[i];
						std::string toolCallId = "tool_" + std::to_string(i);

						if (g_accumulatedToolCalls.find(toolCallId) ==
							g_accumulatedToolCalls.end())
						{
							g_accumulatedToolCalls[toolCallId] = toolCall;
						}
					}

					// Send tool calls via callback
					if (g_currentTokenCallback)
					{
						for (const auto &[id, toolCall] : g_accumulatedToolCalls)
						{
							std::string toolCallJson = toolCall.dump();
							g_currentTokenCallback("TOOL_CALL:" + toolCallJson);
						}
						g_accumulatedToolCalls.clear();
					}
				}
			} catch (const json::exception &e)
			{
				// Ignore JSON parsing errors for individual chunks
			}
		}
		return size * nmemb;
	} catch (const std::exception &e)
	{
		return 0;
	} catch (...)
	{
		return 0;
	}
}

size_t Ollama::WriteDataStreamWithResponse(void *ptr,
										   size_t size,
										   size_t nmemb,
										   std::string *data)
{
	// Use thread-local to avoid race conditions in concurrent requests
	thread_local int chunkCount = 0;
	chunkCount++;

	try
	{
		if (g_should_cancel)
		{
			std::cout << "Stream cancelled in chunk " << chunkCount << std::endl;
			return 0; // Stop receiving data if cancelled
		}

		const char *charPtr = static_cast<const char *>(ptr);
		if (!charPtr)
		{
			std::cout << "ERROR: Null pointer in chunk " << chunkCount << std::endl;
			return 0;
		}

		std::string chunk(charPtr, size * nmemb);
		// Only log every 10th chunk to reduce noise
		if (chunkCount % 10 == 0)
		{
			std::cout << "Received chunk " << chunkCount << " (" << chunk.length()
					  << " bytes)" << std::endl;
		}

		// Process Ollama newline-delimited JSON format
		std::string buffer = chunk;
		size_t pos = 0;
		int lineCount = 0;
		while ((pos = buffer.find('\n', pos)) != std::string::npos)
		{
			std::string line = buffer.substr(0, pos);
			buffer.erase(0, pos + 1);
			pos = 0;
			lineCount++;

			if (line.empty())
				continue;

			// Parse the JSON line directly (no SSE "data:" prefix)
			try
			{
				json result = json::parse(line);

				// Check if stream is done
				if (result.contains("done") && result["done"].get<bool>())
				{
					std::cout << "Received [DONE] signal, ending stream" << std::endl;
					std::lock_guard<std::mutex> responseLock(g_responseMutex);
					if (g_responseCallback && !g_fullResponse.is_null())
					{
						std::cout << "Calling response callback with full response"
								  << std::endl;
						g_responseCallback(g_fullResponse);
					} else
					{
						std::cout << "DEBUG: No response callback or null response"
								  << std::endl;
					}
					return size * nmemb; // End of stream
				}

				// Handle Ollama streaming format
				{
					std::lock_guard<std::mutex> responseLock(g_responseMutex);
					if (g_fullResponse.is_null())
					{
						// Initialize the full response structure using Ollama format
						g_fullResponse = json::object();
						g_fullResponse["model"] = result.value("model", "");
						g_fullResponse["created_at"] = result.value("created_at", "");
						g_fullResponse["message"] = json::object();
						g_fullResponse["message"]["role"] = "assistant";
						g_fullResponse["message"]["content"] = "";
						g_fullResponse["done"] = false;
					}

					// Process Ollama message content
					if (result.contains("message") &&
						result["message"].contains("content"))
					{
						std::string content =
							result["message"]["content"].get<std::string>();
						if (!content.empty())
						{
							// Append content to the full response
							g_fullResponse["message"]["content"] =
								g_fullResponse["message"]["content"].get<std::string>() +
								content;

							// Call token callback for streaming display
							{
								std::lock_guard<std::mutex> lock(g_callbackMutex);
								if (g_currentTokenCallback)
								{
									g_currentTokenCallback(content);
								}
							}
						}
					}

					// Update done status
					if (result.contains("done"))
					{
						g_fullResponse["done"] = result["done"];
					}
				}
			} catch (const json::exception &e)
			{
				// Only log JSON parsing errors occasionally to reduce noise
				if (chunkCount % 20 == 0)
				{
					std::cout << "JSON parsing error in chunk " << chunkCount << ": "
							  << e.what() << std::endl;
				}
			}
		}
		return size * nmemb;
	} catch (const std::exception &e)
	{
		std::cout << "EXCEPTION in WriteDataStreamWithResponse chunk " << chunkCount
				  << ": " << e.what() << std::endl;
		return 0;
	} catch (...)
	{
		std::cout << "UNKNOWN EXCEPTION in WriteDataStreamWithResponse chunk "
				  << chunkCount << std::endl;
		return 0;
	}
}

bool Ollama::promptRequestStream(const std::string &prompt,
								 const std::string &api_key,
								 std::function<void(const std::string &)> tokenCallback,
								 std::atomic<bool> *cancelFlag)
{
	try
	{
		if (cancelFlag && cancelFlag->load())
		{
			return false;
		}

		{
			std::lock_guard<std::mutex> toolLock(g_toolCallMutex);
			std::lock_guard<std::mutex> indexLock(g_indexMappingMutex);
			g_accumulatedToolCalls.clear();
			g_indexToIdMapping.clear();
		}

		if (!initializeCURL())
		{
			return false;
		}

		CURL *curl = curl_easy_init();
		if (!curl)
		{
			return false;
		}

		long http_code = 0;
		std::string response_body;

		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_currentTokenCallback = tokenCallback;
		}

		json payload = {{"model", "deepseek-coder:latest"},
						{"messages", {{{"role", "user"}, {"content", prompt}}}},
						{"stream", true},
						{"options",
						 {{"temperature", 0.3}, {"num_predict", 1000}, {"top_p", 0.95}}}};

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		std::string json_str = payload.dump();

		curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataStream);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, gSettings.getAgentTimeout());
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl,
						 CURLOPT_XFERINFOFUNCTION,
						 [](void *clientp,
							curl_off_t dltotal,
							curl_off_t dlnow,
							curl_off_t ultotal,
							curl_off_t ulnow) -> int {
							 if (g_should_cancel)
							 {
								 return 1;
							 }
							 return 0;
						 });

		CURLcode res = curl_easy_perform(curl);
		if (res != CURLE_OK)
		{
			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);
			{
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				g_currentTokenCallback = nullptr;
			}
			return false;
		}

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_currentTokenCallback = nullptr;
		}

		return http_code == 200;
	} catch (const std::exception &e)
	{
		return false;
	} catch (...)
	{
		return false;
	}
}

bool Ollama::jsonPayloadStream(const std::string &jsonPayload,
							   const std::string &api_key,
							   std::function<void(const std::string &)> tokenCallback,
							   std::atomic<bool> *cancelFlag)
{
	try
	{
		if (cancelFlag && cancelFlag->load())
		{
			return false;
		}

		{
			std::lock_guard<std::mutex> toolLock(g_toolCallMutex);
			std::lock_guard<std::mutex> indexLock(g_indexMappingMutex);
			g_accumulatedToolCalls.clear();
			g_indexToIdMapping.clear();
		}

		if (!initializeCURL())
		{
			return false;
		}

		CURL *curl = curl_easy_init();
		if (!curl)
		{
			return false;
		}

		long http_code = 0;
		std::string response_body;

		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_currentTokenCallback = tokenCallback;
		}

		json payload;
		try
		{
			payload = json::parse(jsonPayload);
		} catch (const json::parse_error &e)
		{
			curl_easy_cleanup(curl);
			{
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				g_currentTokenCallback = nullptr;
			}
			return false;
		}

		// Clean payload for Ollama - remove OpenRouter-specific fields
		if (payload.contains("max_tokens"))
		{
			if (!payload.contains("options"))
			{
				payload["options"] = json::object();
			}
			payload["options"]["num_predict"] = payload["max_tokens"];
			payload.erase("max_tokens");
		}
		if (payload.contains("temperature"))
		{
			if (!payload.contains("options"))
			{
				payload["options"] = json::object();
			}
			payload["options"]["temperature"] = payload["temperature"];
			payload.erase("temperature");
		}

		// Remove OpenRouter-specific fields that Ollama doesn't support
		payload.erase("tools");
		payload.erase("tool_choice");

		// Ensure model and stream are set correctly
		payload["stream"] = true;

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		std::string json_str = payload.dump();

		curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataStream);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, gSettings.getAgentTimeout());
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl,
						 CURLOPT_XFERINFOFUNCTION,
						 [](void *clientp,
							curl_off_t dltotal,
							curl_off_t dlnow,
							curl_off_t ultotal,
							curl_off_t ulnow) -> int {
							 if (g_should_cancel)
							 {
								 return 1;
							 }
							 return 0;
						 });

		CURLcode res = curl_easy_perform(curl);
		if (res != CURLE_OK)
		{
			std::cerr << "CURL Error: " << getCurlErrorString(res) << std::endl;
			if (!response_body.empty())
			{
				std::cerr << "Response: " << response_body << std::endl;
			}

			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);
			{
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				g_currentTokenCallback = nullptr;
			}
			{
				std::lock_guard<std::mutex> responseLock(g_responseMutex);
				g_responseCallback = nullptr;
			}
			return false;
		}

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_currentTokenCallback = nullptr;
		}

		if (http_code != 200)
		{
			std::cerr << "HTTP Error " << http_code << ": "
					  << getHttpStatusDescription(http_code) << std::endl;
			if (!response_body.empty())
			{
				std::cerr << "Response: " << response_body << std::endl;
			}
		}

		return http_code == 200;
	} catch (const std::exception &e)
	{
		return false;
	} catch (...)
	{
		return false;
	}
}

std::pair<bool, std::string> Ollama::jsonPayloadStreamWithResponse(
	const std::string &jsonPayload,
	const std::string &api_key,
	std::function<void(const std::string &)> tokenCallback,
	std::function<void(const json &)> responseCallback,
	std::atomic<bool> *cancelFlag)
{
	std::cout << "=== OLLAMA: STARTING STREAMING REQUEST ===" << std::endl;
	std::cout << "API Key length: " << api_key.length()
			  << " (first 10 chars: " << api_key.substr(0, 10) << "...)" << std::endl;
	std::cout << "JSON Payload length: " << jsonPayload.length() << " bytes" << std::endl;

	try
	{
		if (cancelFlag && cancelFlag->load())
		{
			std::cout << "Request cancelled before starting" << std::endl;
			return {false, "Request cancelled"};
		}

		// Clear any previous tool call state
		{
			std::lock_guard<std::mutex> toolLock(g_toolCallMutex);
			std::lock_guard<std::mutex> indexLock(g_indexMappingMutex);
			g_accumulatedToolCalls.clear();
			g_indexToIdMapping.clear();
		}

		// Clear previous response state
		{
			std::lock_guard<std::mutex> responseLock(g_responseMutex);
			g_fullResponse = json::value_t::null;
			g_responseCallback = responseCallback;
		}

		// Ensure CURL is initialized
		std::cout << "Initializing CURL..." << std::endl;
		if (!initializeCURL())
		{
			std::cerr << "ERROR: Failed to initialize CURL" << std::endl;
			return {false, "Failed to initialize CURL"};
		}
		std::cout << "CURL initialized successfully" << std::endl;

		CURL *curl = curl_easy_init();
		if (!curl)
		{
			std::cerr << "ERROR: Failed to create CURL handle" << std::endl;
			return {false, "Failed to create CURL handle"};
		}
		std::cout << "CURL handle created successfully" << std::endl;

		long http_code = 0;
		std::string response_body; // Capture response body

		// Set the global callback with mutex protection
		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_currentTokenCallback = tokenCallback;
		}

		// Parse the JSON payload and add streaming
		json payload;
		try
		{
			payload = json::parse(jsonPayload);
			std::cout << "JSON payload parsed successfully" << std::endl;
		} catch (const json::parse_error &e)
		{
			std::cerr << "ERROR: Failed to parse JSON payload: " << e.what() << std::endl;
			std::cerr << "Raw payload: " << jsonPayload << std::endl;
			curl_easy_cleanup(curl);
			{
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				g_currentTokenCallback = nullptr;
			}
			{
				std::lock_guard<std::mutex> responseLock(g_responseMutex);
				g_responseCallback = nullptr;
			}
			return {false, "Failed to parse JSON payload"};
		}

		// Add streaming to the payload
		payload["stream"] = true;

		struct curl_slist *headers = nullptr;
		headers = curl_slist_append(headers, "Content-Type: application/json");

		std::string json_str = payload.dump();
		std::cout << "Request payload prepared, length: " << json_str.length() << " bytes"
				  << std::endl;

		curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_str.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataStreamWithResponse);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA,
						 &response_body); // Capture response
		curl_easy_setopt(curl,
						 CURLOPT_TIMEOUT,
						 gSettings.getAgentTimeout()); // Configurable timeout
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
						 10L); // Increased connect timeout
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,
						 1000L); // 1KB/s minimum speed
		curl_easy_setopt(curl,
						 CURLOPT_LOW_SPEED_TIME,
						 30L); // 30 seconds at low speed before timeout

		// Add progress callback to check cancellation more frequently
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
		curl_easy_setopt(curl,
						 CURLOPT_XFERINFOFUNCTION,
						 [](void *clientp,
							curl_off_t dltotal,
							curl_off_t dlnow,
							curl_off_t ultotal,
							curl_off_t ulnow) -> int {
							 if (g_should_cancel)
							 {
								 return 1; // Return non-zero to abort the transfer
							 }
							 return 0; // Continue the transfer
						 });

		std::cout << "Executing CURL request..." << std::endl;
		CURLcode res = curl_easy_perform(curl);
		if (res != CURLE_OK)
		{
			std::cerr << "ERROR: CURL request failed with code: " << res << std::endl;
			std::cerr << "CURL error description: " << curl_easy_strerror(res)
					  << std::endl;

			// Get the response body even if CURL failed
			if (!response_body.empty())
			{
				std::cerr << "=== API ERROR RESPONSE ===" << std::endl;
				std::cerr << "Response body: " << response_body << std::endl;
				std::cerr << "=== END API ERROR RESPONSE ===" << std::endl;
			}

			curl_easy_cleanup(curl);
			curl_slist_free_all(headers);
			// Clear the global callbacks with mutex protection
			{
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				g_currentTokenCallback = nullptr;
			}
			{
				std::lock_guard<std::mutex> responseLock(g_responseMutex);
				g_responseCallback = nullptr;
			}
			return {false, "CURL request failed: " + std::string(curl_easy_strerror(res))};
		}
		std::cout << "CURL request completed successfully" << std::endl;

		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		std::cout << "HTTP response code: " << http_code << std::endl;

		// Cleanup
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);

		// Clear the global callbacks with mutex protection
		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_currentTokenCallback = nullptr;
		}
		{
			std::lock_guard<std::mutex> responseLock(g_responseMutex);
			g_responseCallback = nullptr;
		}

		if (http_code != 200)
		{
			std::cerr << "=== HTTP ERROR ===" << std::endl;
			std::cerr << "HTTP Status Code: " << http_code << " ("
					  << getHttpStatusDescription(http_code) << ")" << std::endl;
			std::cerr << "Function: jsonPayloadStreamWithResponse" << std::endl;
			std::cerr << "URL: http://localhost:11434/api/chat" << std::endl;
			std::cerr << "Response body length: " << response_body.length() << " bytes"
					  << std::endl;
			if (!response_body.empty())
			{
				std::cerr << "=== ACTUAL API ERROR RESPONSE ===" << std::endl;
				std::cerr << response_body << std::endl;
				std::cerr << "=== END ACTUAL API ERROR RESPONSE ===" << std::endl;
			}

			// Get detailed error response
			std::string detailedError = getDetailedErrorResponse(json_str, api_key);
			std::cerr << "Detailed Error Response: " << detailedError << std::endl;
			std::cerr << "=== END HTTP ERROR ===" << std::endl;

			// Return specific error messages for common HTTP errors
			std::string errorMessage;
			if (http_code == 404)
			{
				errorMessage = "Model not found. Please ensure DeepSeek model is "
							   "available in Ollama.";
			} else if (http_code == 500)
			{
				errorMessage = "Ollama server error. Please check if Ollama is running.";
			} else
			{
				errorMessage = "HTTP error " + std::to_string(http_code) + " (" +
							   getHttpStatusDescription(http_code) +
							   "). Check Ollama connection.";
			}

			return {false, errorMessage};
		} else
		{
			std::cout << "HTTP request successful (200 OK)" << std::endl;
		}

		bool success = http_code == 200;
		std::cout << "=== OLLAMA: STREAMING REQUEST "
				  << (success ? "SUCCEEDED" : "FAILED") << " ===" << std::endl;
		return {success, ""};
	} catch (const std::exception &e)
	{
		std::cerr << "EXCEPTION in jsonPayloadStreamWithResponse: " << e.what()
				  << std::endl;
		std::cerr << "Exception type: " << typeid(e).name() << std::endl;
		return {false, "Exception occurred: " + std::string(e.what())};
	} catch (...)
	{
		std::cerr << "UNKNOWN EXCEPTION in jsonPayloadStreamWithResponse" << std::endl;
		return {false, "Unknown error occurred"};
	}
}

std::vector<std::string> Ollama::getAvailableModels()
{
	std::vector<std::string> models;

	if (!initializeCURL())
	{
		return models;
	}

	CURL *curl = curl_easy_init();
	if (!curl)
	{
		return models;
	}

	std::string response;
	long http_code = 0;

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/tags");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteData);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	if (res == CURLE_OK && http_code == 200)
	{
		try
		{
			json result = json::parse(response);
			if (result.contains("models") && result["models"].is_array())
			{
				for (const auto &model : result["models"])
				{
					if (model.contains("name") && model["name"].is_string())
					{
						std::string modelName = model["name"].get<std::string>();
						models.push_back(modelName);
					}
				}
			}
		} catch (const json::exception &e)
		{
			// Fallback to default models if parsing fails
			std::cout << "Warning: Could not parse Ollama models response: " << e.what()
					  << std::endl;
		}
	} else
	{
		std::cout << "Warning: Could not connect to Ollama (" << http_code
				  << "). Using default models." << std::endl;
	}

	// If no models found, add some common defaults
	if (models.empty())
	{
		models.push_back("deepseek-coder:latest");
		models.push_back("deepseek-coder:6.7b");
		models.push_back("deepseek-coder:1.3b");
		models.push_back("llama3.2:latest");
		models.push_back("codellama:latest");
	}

	return models;
}

std::string Ollama::sanitize_completion(const std::string &completion)
{
	if (completion.empty())
	{
		return "";
	}

	// Remove markdown code blocks
	size_t code_start = completion.find("```");
	if (code_start != std::string::npos)
	{
		size_t content_start = completion.find('\n', code_start);
		if (content_start == std::string::npos)
		{
			return completion; // Return original if no newline found
		}
		content_start++;
		size_t code_end = completion.rfind("```");
		if (code_end != std::string::npos && code_end > content_start)
		{
			return completion.substr(content_start, code_end - content_start);
		}
	}

	// Trim whitespace
	auto front = completion.find_first_not_of(" \t\n\r");
	if (front == std::string::npos)
	{
		return completion; // Return original if only whitespace
	}
	auto back = completion.find_last_not_of(" \t\n\r");
	return completion.substr(front, back - front + 1);
}

// Helper function to get detailed error response
std::string getDetailedErrorResponse(const std::string &payload,
									 const std::string &api_key)
{
	CURL *curl = curl_easy_init();
	if (!curl)
	{
		return "Failed to initialize CURL";
	}

	std::string response_body;
	long http_code = 0;

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(curl, CURLOPT_URL, "http://localhost:11434/api/chat");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteDataRaw);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, gSettings.getAgentTimeout());
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	curl_easy_cleanup(curl);
	curl_slist_free_all(headers);

	return response_body;
}