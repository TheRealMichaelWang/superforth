#include <stdlib.h>
#include <string.h>
#include "compiler.h"

#define LOC_REG(INDEX) (compiler_reg_t){.reg = (INDEX), .offset = 1}
#define GLOB_REG(INDEX) (compiler_reg_t){.reg = (INDEX), .offset = 0}

#define INS0(OP) (machine_ins_t){.op_code = OP}
#define INS1(OP, REG) (machine_ins_t){.op_code = OP, .a = REG.reg, .a_flag = REG.offset}
#define INS2(OP, REG, REG1) (machine_ins_t){.op_code = OP, .a = REG.reg, .a_flag = REG.offset, .b = REG1.reg, .b_flag = REG1.offset}
#define INS3(OP, REG, REG1, REG2) (machine_ins_t){.op_code = OP, .a = REG.reg, .a_flag = REG.offset, .b = REG1.reg, .b_flag = REG1.offset, .c = REG2.reg, .c_flag = REG2.offset}

#define EMIT_INS(INS) PANIC_ON_FAIL(ins_builder_append_ins(&compiler->ins_builder, INS), compiler, ERROR_MEMORY)

int init_ins_builder(ins_builder_t* ins_builder) {
	ESCAPE_ON_FAIL(ins_builder->instructions = malloc((ins_builder->alloced_ins = 64) * sizeof(machine_ins_t)));
	ins_builder->instruction_count = 0;
	return 1;
}

int ins_builder_append_ins(ins_builder_t* ins_builder, machine_ins_t ins) {
	if (ins_builder->instruction_count == ins_builder->alloced_ins) {
		machine_ins_t* new_ins = realloc(ins_builder->instructions, (ins_builder->alloced_ins *= 2) * sizeof(machine_ins_t));
		ESCAPE_ON_FAIL(new_ins);
		ins_builder->instructions = new_ins;
	}
	ins_builder->instructions[ins_builder->instruction_count++] = ins;
	return 1;
}

static void allocate_code_block_regs(compiler_t* compiler, ast_code_block_t code_block, uint16_t current_reg);

static uint16_t allocate_value_regs(compiler_t* compiler, ast_value_t value, uint16_t current_reg, compiler_reg_t* target_reg) {
	uint16_t extra_regs = current_reg;
	switch (value.value_type)
	{
	case AST_VALUE_PRIMATIVE:
		memcpy(&compiler->target_machine->stack[compiler->current_constant], &value.data.primative.data, sizeof(uint64_t));
		compiler->eval_regs[value.id] = GLOB_REG(compiler->current_constant++);
		compiler->move_eval[value.id] = 1;
		return current_reg;
	case AST_VALUE_ALLOC_ARRAY:
		allocate_value_regs(compiler, value.data.alloc_array->size, current_reg, NULL);
		break;
	case AST_VALUE_ARRAY_LITERAL:
		for (uint_fast16_t i = 0; i < value.data.array_literal.element_count; i++)
			allocate_value_regs(compiler, value.data.array_literal.elements[i], current_reg, NULL);
		break;
	case AST_VALUE_PROC: {
		compiler->eval_regs[value.id] = GLOB_REG(compiler->ast->total_constants + compiler->current_global++);
		compiler->move_eval[value.id] = 1;
		for (uint_fast16_t i = 0; i < value.data.procedure->param_count; i++)
			compiler->var_regs[value.data.procedure->params[i].var_info.id] = LOC_REG(i);
		compiler->var_regs[value.data.procedure->thisproc->id] = compiler->eval_regs[value.id];
		allocate_code_block_regs(compiler, value.data.procedure->exec_block, value.data.procedure->param_count);
		return current_reg;
	}
	case AST_VALUE_VAR:
		compiler->eval_regs[value.id] = compiler->var_regs[value.data.variable->id];
		compiler->move_eval[value.id] = 1;
		return current_reg;
	case AST_VALUE_SET_VAR:
		compiler->eval_regs[value.id] = compiler->var_regs[value.data.set_var->var_info->id];
		allocate_value_regs(compiler, value.data.set_var->set_value, current_reg, &compiler->eval_regs[value.id]);
		compiler->move_eval[value.id] = 1;
		return current_reg;
	case AST_VALUE_SET_INDEX:
		extra_regs = allocate_value_regs(compiler, value.data.set_index->array, extra_regs, NULL);
		extra_regs = allocate_value_regs(compiler, value.data.set_index->index, extra_regs, NULL);
		extra_regs = allocate_value_regs(compiler, value.data.set_index->value, extra_regs, NULL);
		compiler->eval_regs[value.id] = compiler->eval_regs[value.data.set_index->value.id];
		compiler->move_eval[value.id] = compiler->move_eval[value.data.set_index->value.id];
		return current_reg;
	case AST_VALUE_GET_INDEX:
		extra_regs = allocate_value_regs(compiler, value.data.get_index->array, extra_regs, NULL);
		extra_regs = allocate_value_regs(compiler, value.data.get_index->index, extra_regs, NULL);
		break;
	case AST_VALUE_BINARY_OP:
		extra_regs = allocate_value_regs(compiler, value.data.binary_op->lhs, extra_regs, NULL);
		extra_regs = allocate_value_regs(compiler, value.data.binary_op->rhs, extra_regs, NULL);
		break;
	case AST_VALUE_UNARY_OP:
		allocate_value_regs(compiler, value.data.unary_op->operand, current_reg, NULL);
		break;
	case AST_VALUE_PROC_CALL: {
		compiler->eval_regs[value.id] = LOC_REG(compiler->proc_call_offsets[value.data.proc_call->id] = extra_regs);
		compiler->move_eval[value.id] = !(value.type.type == TYPE_NOTHING || !target_reg || (target_reg->offset && target_reg->reg == current_reg));
		for (uint_fast8_t i = 0; i < value.data.proc_call->argument_count; i++) {
			compiler_reg_t arg_reg = LOC_REG(extra_regs);
			allocate_value_regs(compiler, value.data.proc_call->arguments[i], extra_regs++, &arg_reg);
		}
		allocate_value_regs(compiler, value.data.proc_call->procedure, extra_regs, NULL);
		return current_reg + 1;
	}
	case AST_VALUE_FOREIGN:
		extra_regs = allocate_value_regs(compiler, value.data.foreign->op_id, extra_regs, NULL);
		if(value.data.foreign->has_input)
			extra_regs = allocate_value_regs(compiler, value.data.foreign->input, extra_regs, NULL);
		break;
	}
	if (target_reg) {
		compiler->eval_regs[value.id] = *target_reg;
		compiler->move_eval[value.id] = 0;
	}
	else {
		compiler->eval_regs[value.id] = LOC_REG(current_reg++);
		compiler->move_eval[value.id] = 1;
	}
	return current_reg;
}

static void allocate_code_block_regs(compiler_t* compiler, ast_code_block_t code_block, uint16_t current_reg) {
	for (uint_fast32_t i = 0; i < code_block.instruction_count; i++)
		switch (code_block.instructions[i].type)
		{
		case AST_STATEMENT_DECL_VAR: {
			ast_decl_var_t var_decl = code_block.instructions[i].data.var_decl;
			if (var_decl.var_info->is_readonly &&
				(var_decl.set_value.value_type == AST_VALUE_PRIMATIVE ||
					var_decl.set_value.value_type == AST_VALUE_PROC ||
					(var_decl.set_value.value_type == AST_VALUE_VAR && var_decl.set_value.data.variable->is_readonly))) {
				current_reg = allocate_value_regs(compiler, var_decl.set_value, current_reg, NULL);
				compiler->var_regs[var_decl.var_info->id] = compiler->eval_regs[var_decl.set_value.id];
				compiler->move_eval[var_decl.set_value.id] = 0;
			}
			else {
				if (var_decl.var_info->is_global) {
					compiler->var_regs[var_decl.var_info->id] = GLOB_REG(compiler->ast->total_constants + compiler->current_global++);
					allocate_value_regs(compiler, var_decl.set_value, current_reg, &compiler->var_regs[var_decl.var_info->id]);
				}
				else {
					compiler->var_regs[var_decl.var_info->id] = LOC_REG(current_reg);
					allocate_value_regs(compiler, var_decl.set_value, current_reg, &compiler->var_regs[var_decl.var_info->id]);
					current_reg++;
				}
			}
			break;
		}
		case AST_STATEMENT_COND: {
			ast_cond_t* conditional = code_block.instructions[i].data.conditional;
			while (conditional)
			{
				if (conditional->has_cond_val)
					allocate_value_regs(compiler, conditional->condition, current_reg, NULL);
				allocate_code_block_regs(compiler, conditional->exec_block, current_reg);
				conditional = conditional->next_if_false;
			}
			break;
		}
		case AST_STATEMENT_VALUE:
			allocate_value_regs(compiler, code_block.instructions[i].data.value, current_reg, NULL);
			break;
		case AST_STATEMENT_RETURN_VALUE: {
			compiler_reg_t return_reg = LOC_REG(0);
			allocate_value_regs(compiler, code_block.instructions[i].data.value, current_reg, &return_reg);
			break;
		}
	}
}

static int compile_code_block(compiler_t* compiler, ast_code_block_t code_block, ast_proc_t* proc, uint16_t break_ip, uint16_t continue_ip);

static int compile_value(compiler_t* compiler, ast_value_t value) {
	switch (value.value_type)
	{
	case AST_VALUE_ALLOC_ARRAY:
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.alloc_array->size));
		EMIT_INS(INS3(OP_CODE_HEAP_ALLOC, compiler->eval_regs[value.id], compiler->eval_regs[value.data.alloc_array->size.id], LOC_REG(value.data.alloc_array->elem_type->type == TYPE_SUPER_ARRAY)));
		break;
	case AST_VALUE_ARRAY_LITERAL:
		EMIT_INS(INS2(OP_CODE_HEAP_ALLOC_I, compiler->eval_regs[value.id], GLOB_REG(value.data.array_literal.element_count)));
		for (uint_fast32_t i = 0; i < value.data.array_literal.element_count; i++) {
			ESCAPE_ON_FAIL(compile_value(compiler, value.data.array_literal.elements[i]));
			EMIT_INS(INS3(OP_CODE_STORE_HEAP_I, compiler->eval_regs[value.id], GLOB_REG(i), compiler->eval_regs[value.data.array_literal.elements[i].id]));
		}
		break;
	case AST_VALUE_PROC: {
		uint16_t start_ip = compiler->ins_builder.instruction_count;
		EMIT_INS(INS1(OP_CODE_LABEL, compiler->eval_regs[value.id]));
		EMIT_INS(INS0(OP_CODE_JUMP));
		compiler->ins_builder.instructions[start_ip].b = compiler->ins_builder.instruction_count;
		EMIT_INS(INS0(OP_CODE_HEAP_NEW_FRAME));
		compile_code_block(compiler, value.data.procedure->exec_block, value.data.procedure, 0 ,0);
		EMIT_INS(INS0(OP_CODE_ABORT));
		compiler->ins_builder.instructions[start_ip + 1].a = compiler->ins_builder.instruction_count;
		break;
	}
	case AST_VALUE_SET_VAR:
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.set_var->set_value));
		if (compiler->move_eval[value.data.set_var->set_value.id])
			EMIT_INS(INS2(OP_CODE_MOVE, compiler->var_regs[value.data.set_var->var_info->id], compiler->eval_regs[value.data.set_var->set_value.id]));
		if (value.from_gctrace && value.data.set_var->set_value.type.type == TYPE_SUPER_ARRAY)
			EMIT_INS(INS1(OP_CODE_HEAP_TRACE, compiler->var_regs[value.data.set_var->var_info->id]));
		break;
	case AST_VALUE_SET_INDEX:
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.set_index->array));
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.set_index->index));
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.set_index->value));
		EMIT_INS(INS3(OP_CODE_STORE_HEAP, compiler->eval_regs[value.data.set_index->array.id], compiler->eval_regs[value.data.set_index->index.id], compiler->eval_regs[value.data.set_index->value.id]));
		if (value.from_gctrace && value.data.set_index->value.type.type == TYPE_SUPER_ARRAY)
			EMIT_INS(INS1(OP_CODE_HEAP_TRACE, compiler->var_regs[value.data.set_index->value.id]));
		break;
	case AST_VALUE_GET_INDEX:
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.get_index->array));
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.get_index->index));
		EMIT_INS(INS3(OP_CODE_LOAD_HEAP, compiler->eval_regs[value.data.get_index->array.id], compiler->eval_regs[value.data.get_index->index.id], compiler->eval_regs[value.id]));
		break;
	case AST_VALUE_BINARY_OP: {
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.binary_op->lhs));
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.binary_op->rhs));
		compiler_reg_t lhs = compiler->eval_regs[value.data.binary_op->lhs.id];
		compiler_reg_t rhs = compiler->eval_regs[value.data.binary_op->rhs.id];
 		if (value.data.binary_op->operator == TOK_EQUALS || value.data.binary_op->operator == TOK_NOT_EQUAL) {
			EMIT_INS(INS3(OP_CODE_BOOL_EQUAL + value.data.binary_op->lhs.type.type - TYPE_PRIMATIVE_BOOL, lhs, rhs, compiler->eval_regs[value.id]));
			if (value.data.binary_op->operator == TOK_NOT_EQUAL)
				EMIT_INS(INS2(OP_CODE_NOT, compiler->eval_regs[value.id], compiler->eval_regs[value.id]));
		}
		else if (value.data.binary_op->operator == TOK_AND || value.data.binary_op->operator == TOK_OR)
			EMIT_INS(INS3(OP_CODE_AND + value.data.binary_op->operator - TOK_AND, rhs, lhs, compiler->eval_regs[value.id]))
		else {
			if (value.data.binary_op->lhs.type.type == TYPE_PRIMATIVE_LONG)
				EMIT_INS(INS3(OP_CODE_LONG_MORE + (value.data.binary_op->operator - TOK_MORE), lhs, rhs, compiler->eval_regs[value.id]))
			else
				EMIT_INS(INS3(OP_CODE_FLOAT_MORE + (value.data.binary_op->operator - TOK_MORE), lhs, rhs, compiler->eval_regs[value.id]))
		}
		break;
	}
	case AST_VALUE_UNARY_OP:
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.unary_op->operand));
		if (value.data.unary_op->operator == TOK_SUBTRACT)
			EMIT_INS(INS2(OP_CODE_LONG_NEGATE + value.type.type - TYPE_PRIMATIVE_LONG, compiler->eval_regs[value.id], compiler->eval_regs[value.data.unary_op->operand.id]))
		else
			EMIT_INS(INS2(OP_CODE_NOT + value.data.unary_op->operator - TOK_NOT, compiler->eval_regs[value.id], compiler->eval_regs[value.data.unary_op->operand.id]))
		break;
	case AST_VALUE_PROC_CALL: {
		for (uint_fast8_t i = 0; i < value.data.proc_call->argument_count; i++) {
			ESCAPE_ON_FAIL(compile_value(compiler, value.data.proc_call->arguments[i]));
			if (compiler->move_eval[value.data.proc_call->arguments[i].id])
				EMIT_INS(INS2(OP_CODE_MOVE, LOC_REG(compiler->proc_call_offsets[value.data.proc_call->id] + i), compiler->eval_regs[value.data.proc_call->arguments[i].id]));
		}
		ESCAPE_ON_FAIL(compile_value(compiler, value.data.proc_call->procedure));
		EMIT_INS(INS2(OP_CODE_JUMP_HIST, compiler->eval_regs[value.data.proc_call->procedure.id], GLOB_REG(compiler->proc_call_offsets[value.data.proc_call->id])));
		if (compiler->proc_call_offsets[value.data.proc_call->id])
			EMIT_INS(INS1(OP_CODE_STACK_DEOFFSET, GLOB_REG(compiler->proc_call_offsets[value.data.proc_call->id])));
		break; 
	}
	case AST_VALUE_FOREIGN:
		compile_value(compiler, value.data.foreign->op_id);
		if (value.data.foreign->has_input) {
			compile_value(compiler, value.data.foreign->input);
			EMIT_INS(INS3(OP_CODE_FOREIGN, compiler->eval_regs[value.data.foreign->op_id.id], compiler->eval_regs[value.data.foreign->input.id], compiler->eval_regs[value.id]));
		}
		else
			EMIT_INS(INS3(OP_CODE_FOREIGN, compiler->eval_regs[value.data.foreign->op_id.id], LOC_REG(0), compiler->eval_regs[value.id]));
	}
	return 1;
}

static int compile_conditional(compiler_t* compiler, ast_cond_t* conditional, ast_proc_t* proc, uint16_t break_ip, uint16_t continue_ip) {
	if (conditional->next_if_true) {
		uint16_t this_continue_ip = compiler->ins_builder.instruction_count;
		ESCAPE_ON_FAIL(compile_value(compiler, conditional->condition));
		EMIT_INS(INS1(OP_CODE_CHECK, compiler->eval_regs[conditional->condition.id]));
		uint16_t this_break_ip = compiler->ins_builder.instruction_count;
		EMIT_INS(INS0(OP_CODE_JUMP));
		ESCAPE_ON_FAIL(compile_code_block(compiler, conditional->exec_block, proc, this_break_ip, this_continue_ip));
		EMIT_INS(INS1(OP_CODE_JUMP, GLOB_REG(this_continue_ip)));
		compiler->ins_builder.instructions[this_break_ip].a = compiler->ins_builder.instruction_count;
	}
	else {
		uint16_t escape_jump_count = 0;
		ast_cond_t* count_cond = conditional;
		while (count_cond) {
			if (count_cond->has_cond_val)
				escape_jump_count++;
			count_cond = count_cond->next_if_false;
		}
		escape_jump_count--;
		uint16_t* escape_jumps = malloc(escape_jump_count * sizeof(uint16_t));
		PANIC_ON_FAIL(escape_jumps, compiler, ERROR_MEMORY);
		uint16_t current_escape_jump = 0;
		while (conditional) {
			if (conditional->has_cond_val) {
				ESCAPE_ON_FAIL(compile_value(compiler, conditional->condition));
				EMIT_INS(INS1(OP_CODE_CHECK, compiler->eval_regs[conditional->condition.id]));
				uint16_t move_next_ip = compiler->ins_builder.instruction_count;
				EMIT_INS(INS0(OP_CODE_JUMP));
				ESCAPE_ON_FAIL(compile_code_block(compiler, conditional->exec_block, proc, break_ip, continue_ip));
				if (current_escape_jump != escape_jump_count) {
					escape_jumps[current_escape_jump++] = compiler->ins_builder.instruction_count;
					EMIT_INS(INS0(OP_CODE_JUMP));
				}
				compiler->ins_builder.instructions[move_next_ip].a = compiler->ins_builder.instruction_count;
			}
			else
				ESCAPE_ON_FAIL(compile_code_block(compiler, conditional->exec_block, proc, break_ip, continue_ip));
			conditional = conditional->next_if_false;
		}
		for (uint_fast16_t i = 0; i < escape_jump_count; i++)
			compiler->ins_builder.instructions[escape_jumps[i]].a = compiler->ins_builder.instruction_count;
	}
	return 1;
}

static int compile_code_block(compiler_t* compiler, ast_code_block_t code_block, ast_proc_t* proc, uint16_t break_ip, uint16_t continue_ip) {
	for (uint_fast32_t i = 0; i < code_block.instruction_count; i++)
		switch (code_block.instructions[i].type) {
		case AST_STATEMENT_DECL_VAR:
			ESCAPE_ON_FAIL(compile_value(compiler, code_block.instructions[i].data.var_decl.set_value));
			if (compiler->move_eval[code_block.instructions[i].data.var_decl.set_value.id])
				EMIT_INS(INS2(OP_CODE_MOVE, compiler->var_regs[code_block.instructions[i].data.var_decl.var_info->id], compiler->eval_regs[code_block.instructions[i].data.var_decl.set_value.id]));
			break;
		case AST_STATEMENT_COND: 
			ESCAPE_ON_FAIL(compile_conditional(compiler, code_block.instructions[i].data.conditional, proc, break_ip, continue_ip));
			break;
		case AST_STATEMENT_VALUE:
			ESCAPE_ON_FAIL(compile_value(compiler, code_block.instructions[i].data.value));
			break;
		case AST_STATEMENT_RETURN_VALUE:
			ESCAPE_ON_FAIL(compile_value(compiler, code_block.instructions[i].data.value));
			if (compiler->move_eval[code_block.instructions[i].data.value.id])
				EMIT_INS(INS2(OP_CODE_MOVE, LOC_REG(0), compiler->eval_regs[code_block.instructions[i].data.value.id]));
			if ((!code_block.instructions[i].data.value.from_gctrace && code_block.instructions[i].data.value.type.type == TYPE_SUPER_ARRAY)
				|| code_block.instructions[i].data.value.value_type == AST_VALUE_FOREIGN)
				EMIT_INS(INS1(OP_CODE_HEAP_TRACE, LOC_REG(0)));
		case AST_STATEMENT_RETURN:
			for (uint_fast8_t i = 0; i < proc->param_count; i++)
				if (proc->params[i].var_info.type.type == TYPE_SUPER_ARRAY && proc->params[i].var_info.type.sub_types->type == TYPE_SUPER_ARRAY)
					EMIT_INS(INS1(OP_CODE_HEAP_TRACE, compiler->var_regs[proc->params[i].var_info.id]));
			EMIT_INS(INS0(OP_CODE_HEAP_CLEAN));
			EMIT_INS(INS0(OP_CODE_JUMP_BACK));
			break;
		case AST_STATEMENT_BREAK:
			EMIT_INS(INS1(OP_CODE_JUMP, GLOB_REG(break_ip)));
			break;
		case AST_STATEMENT_CONTINUE:
			EMIT_INS(INS1(OP_CODE_JUMP, GLOB_REG(continue_ip)));
			break;
		}
	return 1;
}

int compile(compiler_t* compiler, machine_t* target_machine, ast_t* ast) {
	compiler->target_machine = target_machine;
	compiler->ast = ast;
	compiler->last_err = ERROR_NONE;
	compiler->current_constant = 0;
	compiler->current_global = 0;
	
	PANIC_ON_FAIL(compiler->eval_regs = malloc(ast->value_count * sizeof(compiler_reg_t)), compiler, ERROR_MEMORY);
	PANIC_ON_FAIL(compiler->move_eval = malloc(ast->value_count * sizeof(int)), compiler, ERROR_MEMORY);
	PANIC_ON_FAIL(compiler->var_regs = malloc(ast->total_var_decls * sizeof(compiler_reg_t)), compiler, ERROR_MEMORY);
	PANIC_ON_FAIL(compiler->proc_call_offsets = malloc(ast->proc_call_count * sizeof(uint16_t)), compiler, ERROR_MEMORY);

	PANIC_ON_FAIL(init_machine(target_machine, UINT16_MAX, 1000, 1000), compiler, target_machine->last_err);
	allocate_code_block_regs(compiler, ast->exec_block, 0);

	PANIC_ON_FAIL(init_ins_builder(&compiler->ins_builder), compiler, ERROR_MEMORY);
	
	EMIT_INS(INS1(OP_CODE_STACK_OFFSET, GLOB_REG(compiler->ast->total_constants + compiler->current_global)));
	EMIT_INS(INS0(OP_CODE_HEAP_NEW_FRAME));
	ESCAPE_ON_FAIL(compile_code_block(compiler, ast->exec_block, NULL, 0, 0));
	EMIT_INS(INS0(OP_CODE_HEAP_CLEAN));

	free(compiler->eval_regs);
	free(compiler->move_eval);
	free(compiler->var_regs);
	free(compiler->proc_call_offsets);

	return 1;
}