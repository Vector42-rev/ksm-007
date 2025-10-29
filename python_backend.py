#!/usr/bin/env python3
"""
Dummy Python backend for custom text completion in NED editor.
This script receives text via stdin and returns completion suggestions via stdout.
"""

import sys
import json


def process_text(text):
    """
    Process the input text and return a completion suggestion,
    adding a comment with the line number after each line.

    Args:
        text (str): The selected text from the editor
        
    Returns:
        str: The completion suggestion with line-number comments
    """
    lines = text.splitlines()
    processed_lines = []

    for idx, line in enumerate(lines, start=1):
        # Add line number comment after each line
        if line.strip():  # Only add comment if line is not empty
            processed_lines.append(f"{line}  # Line {idx}")
        else:
            processed_lines.append(line)  # Keep empty lines as-is

    return "\n".join(processed_lines)



def main():
    """
    Main loop that reads from stdin and writes to stdout.
    Expects JSON input: {"text": "selected text here"}
    Returns JSON output: {"completion": "suggested completion"}
    """
    try:
        # Read input from stdin
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
                
            try:
                # Parse JSON input
                data = json.loads(line)
                input_text = data.get("text", "")
                
                # Process the text
                completion = process_text(input_text)
                
                # Return JSON output
                result = {"completion": completion}
                print(json.dumps(result), flush=True)
                
            except json.JSONDecodeError as e:
                error_result = {"error": f"Invalid JSON: {str(e)}"}
                print(json.dumps(error_result), flush=True)
                
    except KeyboardInterrupt:
        pass
    except Exception as e:
        error_result = {"error": f"Unexpected error: {str(e)}"}
        print(json.dumps(error_result), flush=True)
        sys.exit(1)


if __name__ == "__main__":
    main()
