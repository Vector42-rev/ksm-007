// python_output_pane.h
#pragma once
#include "textselect.hpp"
#include <imgui.h>
#include <mutex>
#include <string>
#include <vector>

class PythonOutputPane
{
  public:
	PythonOutputPane();
	~PythonOutputPane();
	void render(float paneWidth, ImFont *largeFont = nullptr);
	void setOutput(const std::string &output);
	std::string getOutput() const;
	void clearOutput();

  private:
	std::string outputText;
	mutable std::mutex outputMutex;
	std::vector<std::string> displayLines;
	TextSelect textSelect;
	bool needsRebuild;
	float lastKnownWidth;

	void rebuildDisplayLines();
};

extern PythonOutputPane gPythonOutputPane;
