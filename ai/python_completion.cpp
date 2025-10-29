// FIXED VERSION - Works exactly like AI completion
#include "python_completion.h"
#include "../editor/editor.h"
#include "../files/files.h"
#include "../lib/json.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

PythonCompletion gPythonCompletion;

PythonCompletion::PythonCompletion()
	: request_active(false), request_done(false), has_ghost_text(false),
	  ghost_text_start(0), ghost_text_end(0), should_cancel(false),
	  pending_request(false), active_thread_count(0)
{
}

PythonCompletion::~PythonCompletion()
{
	should_cancel = true;

	{
		std::lock_guard<std::mutex> lock(thread_mutex);
		if (worker_thread.joinable())
		{
			worker_thread.detach();
		}
	}
}

void PythonCompletion::python_complete()
{
	// Cancel any active request first
	if (request_active)
	{
		should_cancel = true;
		request_active = false;
		std::cout << "Python completion request canceled\n";
	}

	pending_request = true;

	cleanup_old_threads();

	if (!can_start_new_thread())
	{
		return;
	}

	should_cancel = false;
	increment_thread_count();

	worker_thread = std::thread([this]() {
		if (should_cancel)
		{
			request_active = false;
			decrement_thread_count();
			return;
		}

		request_active = true;

		std::string selected_text = get_selected_text();
		if (selected_text.empty() || should_cancel)
		{
			request_active = false;
			decrement_thread_count();
			return;
		}

		std::cout << "Requesting Python completion for selected text ("
				  << selected_text.length() << " chars)\n";
		std::string new_response = execute_python_script(selected_text);

		if (should_cancel)
		{
			request_active = false;
			decrement_thread_count();
			return;
		}

		// Process response
		if (!new_response.empty() && new_response.find("error") != 0)
		{
			std::lock_guard<std::mutex> lock(thread_mutex);
			if (!should_cancel)
			{
				response = std::move(new_response);
				request_done = true;
			}
		} else if (!new_response.empty())
		{
			std::cerr << "Error from Python: " << new_response << "\n";
		}

		request_active = false;
		decrement_thread_count();
	});
}

std::string PythonCompletion::get_selected_text() const
{
	// If there's a selection, use it
	if (editor_state.selection_active)
	{
		int start = std::min(editor_state.selection_start, editor_state.selection_end);
		int end = std::max(editor_state.selection_start, editor_state.selection_end);

		// Safety checks
		if (start < 0 || end > editor_state.fileContent.size() || start >= end)
		{
			return "";
		}

		return editor_state.fileContent.substr(start, end - start);
	}

	// If no selection, return empty
	return "";
}

std::string PythonCompletion::execute_python_script(const std::string &text)
{
	try
	{
		// Prepare JSON input
		json input_json;
		input_json["text"] = text;
		std::string input_str = input_json.dump() + "\n";

		std::cerr << "Sending to Python: " << input_str.substr(0, 100) << "...\n";

		// Create a temporary file for the input
		std::string temp_input = "/tmp/ned_python_input.json";
		FILE *input_file = fopen(temp_input.c_str(), "w");
		if (!input_file)
		{
			return "error: Failed to create temp input file";
		}
		fwrite(input_str.c_str(), 1, input_str.size(), input_file);
		fclose(input_file);

		// Execute Python script and read output
		std::string command = "python3 /home/veera/ned/python_backend.py < " + temp_input;
		FILE *pipe = popen(command.c_str(), "r");
		if (!pipe)
		{
			std::cerr << "popen failed (" << errno << "): " << strerror(errno) << "\n";
			return "error: Failed to execute Python script";
		}

		// Read the complete JSON response from stdout
		std::string result;
		char buffer[256];
		while (fgets(buffer, sizeof(buffer), pipe))
		{
			result += buffer;
		}

		int status = pclose(pipe);

		if (status != 0)
		{
			std::cerr << "Python script exited with status: " << status << "\n";
		}

		// Debug: see exactly what was returned
		std::cerr << "Python output:\n" << result << "\n---END---\n";

		// Parse the JSON result
		if (result.empty())
		{
			return "error: Empty response from Python script";
		}

		// Trim whitespace
		result.erase(0, result.find_first_not_of(" \n\r\t"));
		result.erase(result.find_last_not_of(" \n\r\t") + 1);

		// Try to parse the JSON
		json response_json;
		try
		{
			response_json = json::parse(result);
		} catch (const json::parse_error &e)
		{
			std::cerr << "JSON parse error: " << e.what() << "\n";
			std::cerr << "Tried to parse: " << result << "\n";
			return "error: Failed to parse JSON response";
		}

		if (response_json.contains("error"))
		{
			return "error: " + response_json["error"].get<std::string>();
		}

		if (response_json.contains("completion"))
		{
			std::string completion = response_json["completion"].get<std::string>();
			std::cerr << "✓ Got completion: " << completion.length() << " chars\n";
			return completion;
		}

		return "error: No completion in response";
	} catch (const std::exception &e)
	{
		std::cerr << "Exception in execute_python_script: " << e.what() << "\n";
		return "error: " + std::string(e.what());
	}
}

void PythonCompletion::update()
{
	if (request_done)
	{
		std::string current_response;
		{
			std::lock_guard<std::mutex> lock(thread_mutex);
			current_response = std::move(response);
			request_done = false;
		}

		if (!current_response.empty() && current_response.find("error") != 0)
		{
			std::cerr << "✓ Processing Python completion in update()\n";

			// Dismiss any existing ghost text first
			if (has_ghost_text)
			{
				dismiss_completion();
			}

			// Just insert as ghost text - DON'T delete anything
			// This is exactly how AI completion works
			std::cerr << "✓ Inserting Python ghost text at cursor\n";
			insert(current_response);
		}
	}
}

void PythonCompletion::insert(const std::string &code)
{
	if (code.empty())
		return;

	std::lock_guard<std::mutex> lock(thread_mutex);

	if (has_ghost_text)
	{
		dismiss_completion();
	}

	// Ensure cursor index is within bounds
	if (editor_state.cursor_index < 0 ||
		editor_state.cursor_index > editor_state.fileContent.size())
	{
		std::cerr << "✗ Invalid cursor position\n";
		return;
	}

	std::cerr << "✓ Inserting Python ghost text: " << code.length()
			  << " chars at position " << editor_state.cursor_index << "\n";

	ghost_text = code;
	ghost_text_start = editor_state.cursor_index;
	ghost_text_end = ghost_text_start + code.size();

	// Insert the code into the file content at cursor position
	editor_state.fileContent.insert(editor_state.cursor_index, code);

	// Use a distinct ghost color (gray/transparent)
	ImVec4 ghost_color = ImVec4(0.5f, 0.5f, 0.5f, 0.5f);

	// Ensure fileColors matches fileContent size
	if (editor_state.fileColors.size() < editor_state.fileContent.size())
	{
		editor_state.fileColors.resize(editor_state.fileContent.size(),
									   ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
	}

	// Insert ghost colors at the cursor position
	editor_state.fileColors.insert(editor_state.fileColors.begin() +
									   editor_state.cursor_index,
								   code.size(),
								   ghost_color);

	has_ghost_text = true;

	// CRITICAL: Keep cursor at the START of ghost text (like AI completion)
	editor_state.cursor_index = ghost_text_start;
	editor_state.selection_start = editor_state.selection_end = ghost_text_start;

	// CRITICAL: Use ghost_text_changed, NOT text_changed
	// This prevents saving/undo operations on ghost text
	editor_state.ghost_text_changed = true;
	gEditor.updateLineStarts();

	std::cerr << "✓ Python ghost text inserted successfully\n";
}

void PythonCompletion::accept_completion()
{
	if (!has_ghost_text)
		return;

	std::lock_guard<std::mutex> lock(thread_mutex);

	std::cerr << "✓ Accepting Python completion\n";

	// Change all ghost text colors to normal (white)
	for (int i = ghost_text_start;
		 i < ghost_text_end && i < editor_state.fileColors.size();
		 i++)
	{
		editor_state.fileColors[i] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
	}

	// Move cursor to end of inserted text
	editor_state.cursor_index = ghost_text_end;
	editor_state.selection_start = editor_state.selection_end = editor_state.cursor_index;

	has_ghost_text = false;
	ghost_text.clear();
	ghost_text_start = 0;
	ghost_text_end = 0;

	// NOW mark as text_changed to make it permanent and trigger save/undo
	editor_state.text_changed = true;
	gEditor.updateLineStarts();

	std::cerr << "✓ Python completion accepted and made permanent\n";
}

void PythonCompletion::dismiss_completion()
{
	if (!has_ghost_text)
		return;

	std::lock_guard<std::mutex> lock(thread_mutex);

	std::cerr << "✓ Dismissing Python completion\n";

	// Validate indices before accessing
	if (ghost_text_start < 0 || ghost_text_end > editor_state.fileContent.size() ||
		ghost_text_start >= ghost_text_end)
	{
		has_ghost_text = false;
		ghost_text.clear();
		ghost_text_start = 0;
		ghost_text_end = 0;
		return;
	}

	// Remove the ghost text from content and colors
	editor_state.fileContent.erase(ghost_text_start, ghost_text_end - ghost_text_start);

	if (ghost_text_start < editor_state.fileColors.size())
	{
		int erase_end = std::min(ghost_text_end, (int)editor_state.fileColors.size());
		editor_state.fileColors.erase(editor_state.fileColors.begin() + ghost_text_start,
									  editor_state.fileColors.begin() + erase_end);
	}

	has_ghost_text = false;
	ghost_text.clear();
	ghost_text_start = 0;
	ghost_text_end = 0;

	// Use ghost_text_changed for dismissal (not text_changed)
	editor_state.ghost_text_changed = true;
	gEditor.updateLineStarts();

	std::cerr << "✓ Python ghost text dismissed\n";
}

void PythonCompletion::cancel_request()
{
	should_cancel = true;
	request_active = false;
	std::cerr << "✓ Python completion request cancelled\n";
}

void PythonCompletion::cleanup_old_threads()
{
	std::lock_guard<std::mutex> lock(thread_mutex);
	if (worker_thread.joinable())
	{
		worker_thread.detach();
	}
}

bool PythonCompletion::can_start_new_thread()
{
	return active_thread_count < MAX_CONCURRENT_THREADS;
}

void PythonCompletion::increment_thread_count() { active_thread_count++; }

void PythonCompletion::decrement_thread_count()
{
	active_thread_count--;
	thread_cv.notify_one();
}