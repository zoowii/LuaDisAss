﻿#ifndef ASSEMBLER_H
#define ASSEMBLER_H

#include <utility>
#include <string>
#include <vector>
#include <unordered_map>

#include "WriteBuffer.h"
#include "Buffer.h"
#include "lconfig.h"
#include "Function.h"

struct ParsedFunction;
typedef std::shared_ptr<ParsedFunction> ParsedFunctionPtr;

struct ParsedFunction {
	std::string name;
	std::vector<Instruction> instructions;
	std::vector<Upvalue> upvalues;
	std::vector<std::string> usedSubroutines;
    std::vector<int> lineinfos;
	std::vector<std::pair<std::string, int> > neededSubroutines;
	std::vector<TValuePtr> constants;
	unsigned char maxstacksize, params, vararg;
};

class Assembler {
public:
	Assembler(Buffer *rbuffer, WriteBufferPtr wbuffer);
    Util::BoolRes assemble();

private:
	class Operand {
	public:
		enum Type {
			STACKIDX,
			CONSTANT,
			LOCATION,
			UPVALUE,
			EMBEDDED
		};

		inline Type type() {return type_; };
		inline void setType(Type type) {type_ = type; };

		inline int value() {return value_; };
		inline void setValue(int value) {value_ = value; };
	private:
		Type type_;
		int value_; // stack index, const id, location id (or -1 if unknown), upvalue index, embedded integer
	};

    Util::BoolRes writeHeader();

    Util::BoolRes parseLine(const char *line, size_t len);
    Util::BoolRes parseDirective(const char *line, size_t len);
    Util::BoolRes finalizeFunction();
	const char *parseConstant(const char *start, const char *end, size_t *id); // returns nullptr if the operand could not be parsed
    Util::BoolRes parseCode(const char *line, size_t len);
    Util::BoolRes parseUpvalue(const char *line, size_t len);
	const char *parseOperand(Operand &operand, const char *start, const char *end, unsigned int limit = 0xFFFFFFFF); // returns nullptr if the operand could not be parsed

    Util::BoolRes writeFunction(ParsedFunctionPtr function);
    Util::BoolRes writeString(const std::string &string);

	WriteBufferPtr wbuffer_;
	BufferPtr rbuffer_;

	enum ParseStatus {
		PARSE_FUNC,
		PARSE_CODE,
		PARSE_CONST,
		PARSE_UPVALUE,
		PARSE_NONE
	};

	ParseStatus parseStatus_;

	unsigned int nUpvalues_;
	bool bUpvalues_;


	std::unordered_map<std::string, ParsedFunctionPtr> functions_;

	// TODO: automatically detect which subroutines are protos of subroutines through the CLOSURE instruction

	std::unordered_map<std::string, int> subroutines_;
	std::unordered_map<std::string, int> usedSubroutines_; // subroutine name, function id

	int funcid_;
	/* The following should be save on a new function declaration or end of file */
	std::vector<std::string> f_usedSubroutines_;
	std::vector<Upvalue> upvalues_;
	std::string funcname_;

	std::vector<Instruction> instructions_;
	std::vector<std::pair<std::string, int> > neededSubroutines_;
	std::vector<std::pair<std::string, int> > neededLocations_; // for jmps to the future
	std::unordered_map<std::string, int> locations_;

    std::vector<int> lineinfos_;

	unsigned int f_maxstacksize_, f_params_, f_vararg_;
	std::vector<TValuePtr> constants_;

    std::string get_line_comment_from_asm_line_code(const char *line, size_t len);

    // fetch linenumber from line comment of ";L<digits>;<other_line_comment>" style. -1 for not exist defined line number
    int get_linenumber_from_asm_line_comment(std::string line_comment);
	/* end of function-specific data */
};

#endif