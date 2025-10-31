#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class PythonCompletion
{
  public:
	PythonCompletion();
	~PythonCompletion();
	void python_complete();		  // Trigger completion
	void python_output_to_pane(); // Send output to pane (Ctrl+Y)
	void update();				  // Call this in main loop
	void accept_completion();
	void dismiss_completion();
	void cancel_request();

	bool request_done = false;
	bool has_ghost_text = false;
	std::string ghost_text;
	int ghost_text_start = 0;
	int ghost_text_end = 0;

	std::string response;

	std::atomic<bool> request_active{false};

	std::thread worker_thread;
	std::atomic<bool> should_cancel;

	std::atomic<bool> pending_request;

  private:
	void insert(const std::string &code);
	std::string get_selected_text() const;
	std::string execute_python_script(const std::string &text);
	std::string execute_rf3_script(const std::string &text);
	std::string execute_script_helper(const std::string &text,
									  const std::string &script_path);

	std::mutex thread_mutex;
	std::condition_variable thread_cv;

	static constexpr int MAX_CONCURRENT_THREADS = 3;
	std::atomic<int> active_thread_count{0};

	void cleanup_old_threads();
	bool can_start_new_thread();
	void increment_thread_count();
	void decrement_thread_count();

	// ADD THESE TWO LINES:
	int selection_start_stored = 0;
	int selection_end_stored = 0;
};

extern PythonCompletion gPythonCompletion;