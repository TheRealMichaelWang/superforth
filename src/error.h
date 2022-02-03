#pragma once

#ifndef ERROR_H

#include <stdint.h>

typedef enum error {
	ERROR_NONE,
	ERROR_MEMORY,
	ERROR_INTERNAL,

	//syntax errors
	ERROR_UNEXPECTED_TOK,

	ERROR_READONLY,
	ERROR_TYPE_NOT_ALLOWED,

	ERROR_EXPECTED_SUB_TYPES,

	ERROR_UNDECLARED,
	ERROR_REDECLARATION,

	ERROR_UNEXPECTED_TYPE,
	ERROR_UNEXPECTED_ARGUMENT_SIZE,

	ERROR_CANNOT_RETURN,
	ERROR_CANNOT_CONTINUE,
	ERROR_CANNOT_BREAK,

	//virtual-machine errors
	ERROR_INDEX_OUT_OF_RANGE,
	ERROR_DIVIDE_BY_ZERO,
	ERROR_STACK_OVERFLOW,
	ERROR_READ_UNINIT,

	ERROR_UNRETURNED_FUNCTION,
	
	ERROR_ABORT,
	ERROR_FOREIGN,

	ERROR_CANNOT_OPEN_FILE
} error_t;

#define PANIC(OBJ, ERROR){ OBJ->last_err = ERROR; return 0; }
#define ESCAPE_ON_FAIL(PTR) {if(!(PTR)) { return 0; }}
#define PANIC_ON_FAIL(PTR, OBJ, ERROR) {if(!(PTR)) PANIC(OBJ, ERROR)}

#endif // !ERROR_H
