import libcst as cst
from typing import Optional, List
import json
import sys


class LoopParallelizer(cst.CSTTransformer):
    """
    Transform various loop patterns into parallelized versions using joblib.
    Handles: for-loops, list comprehensions, map() calls, and nested loops.
    """

    def __init__(self):
        super().__init__()
        self.joblib_imported = False
        self.helper_map = {}
        self.counter = 0

    def leave_Import(self, original_node, updated_node):
        """Check if joblib is already imported"""
        names = [n.name.value for n in original_node.names]
        if "joblib" in names:
            self.joblib_imported = True
        return updated_node

    def leave_ImportFrom(self, original_node, updated_node):
        """Check if joblib components are already imported"""
        if original_node.module and original_node.module.value == "joblib":
            self.joblib_imported = True
        return updated_node

    def _extract_loop_body(self, body: cst.IndentedBlock) -> List[cst.BaseStatement]:
        """Extract statements from loop body"""
        return list(body.body)

    def _create_helper_function(self, loop_var: cst.Name, body_stmts: List[cst.BaseStatement], 
                                return_value: Optional[cst.BaseExpression] = None) -> cst.FunctionDef:
        """Create a helper function for the loop body"""
        self.counter += 1
        fn_name = f"_loop_body_{self.counter}"
        
        # Prepare function body
        fn_body = []
        for stmt in body_stmts:
            fn_body.append(stmt)
        
        # Add return statement
        if return_value:
            fn_body.append(cst.SimpleStatementLine([cst.Return(value=return_value)]))
        
        return cst.FunctionDef(
            name=cst.Name(fn_name),
            params=cst.Parameters(params=[cst.Param(loop_var)]),
            body=cst.IndentedBlock(body=fn_body)
        ), fn_name

    def _is_simple_assignment_loop(self, node: cst.For) -> bool:
        """Check if loop is a simple assignment pattern (e.g., l[i] = expr)"""
        if len(node.body.body) != 1:
            return False
        
        stmt = node.body.body[0]
        if not isinstance(stmt, cst.SimpleStatementLine):
            return False
        
        if len(stmt.body) != 1:
            return False
            
        return isinstance(stmt.body[0], (cst.Assign, cst.AugAssign))

    def leave_For(self, original_node: cst.For, updated_node: cst.For) -> cst.BaseStatement:
        """Transform for-loops into parallelized versions"""
        
        # Handle simple assignment loops (e.g., l[i] = l[i] * 2)
        if self._is_simple_assignment_loop(updated_node):
            return self._transform_simple_assignment_loop(updated_node)
        
        # Handle accumulation loops with multiple statements
        else:
            return self._transform_general_loop(updated_node)

    def _transform_simple_assignment_loop(self, node: cst.For) -> cst.BaseStatement:
        """Transform simple assignment loops like: for i in range(n): l[i] = expr"""
        stmt = node.body.body[0].body[0]
        
        # Extract target variable name
        target_name = None
        if isinstance(stmt, cst.Assign):
            tgt = stmt.targets[0].target
            if isinstance(tgt, cst.Subscript) and isinstance(tgt.value, cst.Name):
                target_name = tgt.value.value
                return_expr = stmt.value
            else:
                return node  # Not a pattern we can parallelize
        else:
            return node
        
        # Create helper function
        fn_def, fn_name = self._create_helper_function(
            node.target,
            [],
            return_expr
        )
        
        # Build Parallel call
        loop_var = cst.Module([]).code_for_node(node.target)
        loop_iter = cst.Module([]).code_for_node(node.iter)
        
        parallel_stmt = cst.parse_statement(
            f"{target_name} = list(Parallel(n_jobs=-1)(delayed({fn_name})({loop_var}) "
            f"for {loop_var} in {loop_iter}))\n"
        )
        
        self.helper_map[id(parallel_stmt)] = fn_def
        return parallel_stmt

    def _transform_general_loop(self, node: cst.For) -> cst.BaseStatement:
        """Transform general loops with multiple statements or complex bodies"""
        
        # Extract all statements except the last one
        body_stmts = list(node.body.body)
        
        # Try to find what's being accumulated/returned
        last_stmt = body_stmts[-1] if body_stmts else None
        return_expr = None
        
        if isinstance(last_stmt, cst.SimpleStatementLine) and len(last_stmt.body) == 1:
            if isinstance(last_stmt.body[0], cst.Expr):
                # If last statement is an expression, use it as return value
                return_expr = last_stmt.body[0].value
                body_stmts = body_stmts[:-1]
        
        # Create helper function with all statements
        fn_def, fn_name = self._create_helper_function(
            node.target,
            body_stmts,
            return_expr
        )
        
        # Build Parallel call
        loop_var = cst.Module([]).code_for_node(node.target)
        loop_iter = cst.Module([]).code_for_node(node.iter)
        
        parallel_stmt = cst.parse_statement(
            f"results = list(Parallel(n_jobs=-1)(delayed({fn_name})({loop_var}) "
            f"for {loop_var} in {loop_iter}))\n"
        )
        
        self.helper_map[id(parallel_stmt)] = fn_def
        return parallel_stmt

    def leave_ListComp(self, original_node: cst.ListComp, updated_node: cst.ListComp) -> cst.BaseExpression:
        """Transform list comprehensions into parallel Parallel calls"""
        
        # Only handle simple list comprehensions with one for clause
        if len(updated_node.for_in.ifs) > 0:
            # Has filters - keep as is for now
            return updated_node
        
        # Extract comprehension parts
        loop_var = updated_node.for_in.target
        loop_iter = updated_node.for_in.iter
        element_expr = updated_node.elt
        
        # Create helper function
        fn_def, fn_name = self._create_helper_function(
            loop_var,
            [],
            element_expr
        )
        
        # Build the parallel expression
        loop_var_str = cst.Module([]).code_for_node(loop_var)
        loop_iter_str = cst.Module([]).code_for_node(loop_iter)
        
        parallel_expr = cst.parse_expression(
            f"list(Parallel(n_jobs=-1)(delayed({fn_name})({loop_var_str}) "
            f"for {loop_var_str} in {loop_iter_str}))"
        )
        
        # Store helper function
        self.helper_map[id(parallel_expr)] = fn_def
        
        return parallel_expr

    def leave_Call(self, original_node: cst.Call, updated_node: cst.Call) -> cst.BaseExpression:
        """Transform map() calls into parallel versions"""
        
        # Check if this is a map() call
        if isinstance(updated_node.func, cst.Name) and updated_node.func.value == "map":
            if len(updated_node.args) == 2:
                func_arg = updated_node.args[0].value
                iter_arg = updated_node.args[1].value
                
                # Handle lambda functions
                if isinstance(func_arg, cst.Lambda):
                    # Create helper function from lambda
                    fn_def, fn_name = self._create_helper_function(
                        func_arg.params.params[0].name,
                        [],
                        func_arg.body
                    )
                    
                    loop_var_str = cst.Module([]).code_for_node(func_arg.params.params[0].name)
                    iter_str = cst.Module([]).code_for_node(iter_arg)
                    
                    parallel_expr = cst.parse_expression(
                        f"list(Parallel(n_jobs=-1)(delayed({fn_name})({loop_var_str}) "
                        f"for {loop_var_str} in {iter_str}))"
                    )
                    
                    self.helper_map[id(parallel_expr)] = fn_def
                    return parallel_expr
                
                # Handle named functions
                elif isinstance(func_arg, cst.Name):
                    func_name = func_arg.value
                    iter_str = cst.Module([]).code_for_node(iter_arg)
                    
                    parallel_expr = cst.parse_expression(
                        f"list(Parallel(n_jobs=-1)(delayed({func_name})(x) for x in {iter_str}))"
                    )
                    
                    return parallel_expr
        
        return updated_node

    def leave_Module(self, original_node, updated_node):
        """Insert imports and helper functions before their usage"""
        body = []
        added_import = False
        
        for stmt in updated_node.body:
            # Add import at the beginning if needed
            if not added_import and not self.joblib_imported:
                body.append(cst.parse_statement("from joblib import Parallel, delayed\n"))
                added_import = True
            
            # Check if we need to insert helper function(s) before this statement
            self._insert_helpers_recursive(stmt, body)
            
            body.append(stmt)
        
        return updated_node.with_changes(body=body)

    def _insert_helpers_recursive(self, node, body):
        """Recursively find and insert helper functions for a node and its children"""
        # Check current node
        helper = self.helper_map.get(id(node))
        if helper:
            body.append(helper)
        
        # Check children recursively
        for child in node.children:
            if isinstance(child, (cst.CSTNode,)):
                self._insert_helpers_recursive(child, body)


def parallelize_code(code: str) -> str:
    """Transform serial code patterns into parallel versions using joblib"""
    tree = cst.parse_module(code)
    transformer = LoopParallelizer()
    new_tree = tree.visit(transformer)
    return new_tree.code


if __name__ == "__main__":
    # Read input from stdin (JSON format like python_backend.py)
    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            
            try:
                # Parse JSON input
                data = json.loads(line)
                input_text = data.get("text", "")
                
                # Process the code - parallelize it
                try:
                    parallelized_code = parallelize_code(input_text)
                    
                    # Return JSON output
                    result = {"completion": parallelized_code}
                    print(json.dumps(result), flush=True)
                except Exception as e:
                    error_result = {"error": f"Parallelization error: {str(e)}"}
                    print(json.dumps(error_result), flush=True)
                
            except json.JSONDecodeError as e:
                error_result = {"error": f"Invalid JSON: {str(e)}"}
                print(json.dumps(error_result), flush=True)
    
    except KeyboardInterrupt:
        pass
    except Exception as e:
        error_result = {"error": f"Unexpected error: {str(e)}"}
        print(json.dumps(error_result), flush=True)
        sys.exit(1)
