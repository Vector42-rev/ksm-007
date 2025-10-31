#include "python_output_pane.h"
#include "../util/settings.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#include <cassert>
#ifdef assert
#undef assert
#endif
#include <utf8.h>
#ifdef _WIN32
#define assert(expr) ((void)0)
#endif
#else
#include <utf8.h>
#endif

PythonOutputPane gPythonOutputPane;

PythonOutputPane::PythonOutputPane()
	: textSelect(
		  [this](size_t idx) -> std::string {
			  if (idx < displayLines.size())
				  return displayLines[idx];
			  return "";
		  },
		  [this]() -> size_t { return displayLines.size(); },
		  true), // word wrap enabled
	  needsRebuild(true), lastKnownWidth(0.0f)
{
}

PythonOutputPane::~PythonOutputPane() {}

void PythonOutputPane::setOutput(const std::string &output)
{
	std::lock_guard<std::mutex> lock(outputMutex);
	outputText = output;
	needsRebuild = true;
}

std::string PythonOutputPane::getOutput() const
{
	std::lock_guard<std::mutex> lock(outputMutex);
	return outputText;
}

void PythonOutputPane::clearOutput()
{
	std::lock_guard<std::mutex> lock(outputMutex);
	outputText.clear();
	displayLines.clear();
	needsRebuild = true;
}

void PythonOutputPane::rebuildDisplayLines()
{
	std::lock_guard<std::mutex> lock(outputMutex);
	displayLines.clear();

	if (outputText.empty())
	{
		displayLines.push_back("No output yet. Select text and press Ctrl+Y to process.");
		return;
	}

	// Split output by newlines
	std::istringstream stream(outputText);
	std::string line;
	while (std::getline(stream, line))
	{
		displayLines.push_back(line);
	}

	if (displayLines.empty())
	{
		displayLines.push_back("");
	}
}

void PythonOutputPane::render(float paneWidth, ImFont *largeFont)
{
	float inputWidth = ImGui::GetContentRegionAvail().x;
	float windowHeight = ImGui::GetWindowHeight();
	float horizontalPadding = 16.0f;
	float textBoxWidth = inputWidth;
	if (textBoxWidth < 50.0f)
		textBoxWidth = 50.0f;

	ImGui::BeginGroup();
	ImGui::Dummy(ImVec2(horizontalPadding - 10, 0));
	ImGui::SameLine();

	ImGui::BeginGroup();

	// Title
	if (largeFont)
		ImGui::PushFont(largeFont);
	ImGui::Text("PARA TEST");
	if (largeFont)
		ImGui::PopFont();

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Calculate output area dimensions
	float scrollbarWidth = ImGui::GetStyle().ScrollbarSize;
	float outputWidth = textBoxWidth - 2 * horizontalPadding - scrollbarWidth + 45.0f;
	if (outputWidth < 50.0f)
		outputWidth = 50.0f;

	float outputHeight = windowHeight - 120.0f;
	if (outputHeight < 100.0f)
		outputHeight = 100.0f;

	// Check if width changed or needs rebuild
	if (std::abs(outputWidth - lastKnownWidth) > 1.0f || needsRebuild)
	{
		lastKnownWidth = outputWidth;
		needsRebuild = false;
		rebuildDisplayLines();
	}

	// Render output area with text selection
	ImGui::PushStyleColor(
		ImGuiCol_ChildBg,
		ImVec4(gSettings.getSettings()["backgroundColor"][0].get<float>() * 0.9f,
			   gSettings.getSettings()["backgroundColor"][1].get<float>() * 0.9f,
			   gSettings.getSettings()["backgroundColor"][2].get<float>() * 0.9f,
			   1.0f));

	ImGui::BeginChild(
		"PythonOutput", ImVec2(outputWidth, outputHeight), true, ImGuiWindowFlags_None);

	// Don't use textSelect for now to avoid crashes
	// textSelect.update();

	// Render text (read-only for now)
	for (const auto &line : displayLines)
	{
		ImGui::TextUnformatted(line.c_str());
	}

	ImGui::EndChild();
	ImGui::PopStyleColor();

	ImGui::Spacing();

	// Clear button
	if (ImGui::Button("Clear Output"))
	{
		clearOutput();
	}

	ImGui::EndGroup();

	ImGui::SameLine();
	ImGui::Dummy(ImVec2(horizontalPadding, 0));
	ImGui::EndGroup();
}
