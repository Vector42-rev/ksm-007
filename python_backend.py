#!/usr/bin/env python3
"""
Python backend for loop analysis in NED editor.
This script receives Python code via stdin and returns loop parallelizability analysis via stdout.
"""

import sys
import json
import os

# Add the loop analyzer directory to Python path
sys.path.insert(0, '/home/veera/CD_files/pro')

# Try to import the loop analyzer
LOOP_ANALYZER_AVAILABLE = False
IMPORT_ERROR_MSG = ""

try:
    from loop_analyzer import AdvancedLoopAnalyzer
    LOOP_ANALYZER_AVAILABLE = True
except ImportError as e:
    IMPORT_ERROR_MSG = f"Could not import loop_analyzer: {e}\nPath: /home/veera/CD_files/pro\nPlease ensure loop_analyzer.py and its dependencies (astroid) are available."
except Exception as e:
    IMPORT_ERROR_MSG = f"Error loading loop_analyzer: {e}"

def process_text(text):
    """
    Process the input text through the loop analyzer.

    Args:
        text (str): The selected Python code from the editor
        
    Returns:
        str: The loop analysis report
    """
    # Check if analyzer is available
    if not LOOP_ANALYZER_AVAILABLE:
        return f"Error: Loop analyzer not available.\n\n{IMPORT_ERROR_MSG}"
    
    try:
        # Create analyzer instance
        analyzer = AdvancedLoopAnalyzer(text)
        
        # Run analysis
        results = analyzer.analyze()
        
        # Format results as a readable report
        if not results:
            return "No loops found in the selected code."
        
        output_lines = []
        output_lines.append("="*80)
        output_lines.append("PARALLEL LOOP ANALYSIS REPORT")
        output_lines.append("="*80)
        output_lines.append("")
        
        parallelizable = [r for r in results if r.is_parallelizable]
        not_parallelizable = [r for r in results if not r.is_parallelizable]
        
        output_lines.append("Summary:")
        output_lines.append(f"  Total loops analyzed: {len(results)}")
        output_lines.append(f"  ✓ Parallelizable: {len(parallelizable)}")
        output_lines.append(f"  ✗ Not parallelizable: {len(not_parallelizable)}")
        output_lines.append("")
        
        # Print each loop analysis
        for i, result in enumerate(results, 1):
            output_lines.append("─"*80)
            output_lines.append(f"Loop #{i} (Line {result.line_number})")
            output_lines.append(f"Loop variable: {result.loop_variable}")
            output_lines.append("─"*80)
            
            if result.is_parallelizable:
                confidence_symbol = {
                    "high": "✓✓✓",
                    "medium": "✓✓",
                    "low": "✓"
                }.get(result.confidence, "✓")
                output_lines.append(f"{confidence_symbol} PARALLELIZABLE (Confidence: {result.confidence.upper()})")
            else:
                output_lines.append("✗ NOT PARALLELIZABLE")
            
            output_lines.append("")
            output_lines.append("Analysis:")
            for reason in result.reasons:
                output_lines.append(f"  • {reason}")
            
            # Detailed variable information
            output_lines.append("")
            output_lines.append("Detailed Information:")
            
            if result.modified_vars:
                output_lines.append(f"  Variables modified: {', '.join(sorted(result.modified_vars))}")
            else:
                output_lines.append("  Variables modified: None")
            
            if result.read_vars:
                output_lines.append(f"  Variables read: {', '.join(sorted(result.read_vars))}")
            else:
                output_lines.append("  Variables read: None")
            
            if result.function_calls:
                output_lines.append(f"  Function calls: {', '.join(result.function_calls)}")
            else:
                output_lines.append("  Function calls: None")
            
            # Flags
            output_lines.append("")
            output_lines.append("Flags:")
            output_lines.append(f"  I/O operations: {'Yes' if result.has_io else 'No'}")
            output_lines.append(f"  In-place mutations: {'Yes' if result.has_mutations else 'No'}")
            output_lines.append(f"  Loop-carried dependencies: {'Yes' if result.has_dependencies else 'No'}")
            
            output_lines.append("")
        
        return "\n".join(output_lines)
        
    except Exception as e:
        return f"Error analyzing code: {str(e)}\n\nPlease ensure the selected text is valid Python code."



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
