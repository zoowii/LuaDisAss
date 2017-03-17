﻿#include "Assembler.h"

#include <cstring>
#include <limits>
#include "util.h"
#include "opcodes.h"


Assembler::Assembler(Buffer *rbuffer, WriteBufferPtr wbuffer) : rbuffer_(rbuffer), wbuffer_(wbuffer), parseStatus_(PARSE_NONE), funcid_(-1), bUpvalues_(false) {

}

inline const char *parseLabel(std::string &out, const char *start, const char *end) {
	start = std::find_if_not(start, end, std::ptr_fun<int, int>(std::isblank));
	if (start == end || *start == ';') {
		return nullptr;
	}

	const char *lend = std::find_if_not(start, end, [](char c) {return std::isalnum(c) || c == '_'; });
	if (lend == start) {
		return nullptr;
	}

	out.assign(start, lend);
	return lend;
}

template<typename T>
inline const char *parseInt(T &out, const char *start, const char *end) {
	start = std::find_if_not(start, end, std::ptr_fun<int, int>(std::isblank));
	if (start == end || *start == ';') {
		return nullptr;
	}

	bool negative = false;
	if (std::numeric_limits<T>::is_signed) {
		if (*start == '-') {
			negative = true;
			if (++start == end) {
				return nullptr;
			}
		}
	}

	if (*start == '+') {
		if (++start == end) {
			return nullptr;
		}
	}

	const char *iend = std::find_if_not(start, end, std::ptr_fun<int, int>(std::isdigit));
	if (iend == start) {
		return nullptr;
	}

	out = 0;
	
	for (; start != iend; ++start) {
		out = out * 10 + (*start - '0');
	}

	if (negative) {
		out *= -1;
	}

	return iend;
}

std::string Assembler::get_line_comment_from_asm_line_code(const char *line, size_t len)
{
  std::string line_comment;
  const char *comment_start = nullptr;
  for (auto i = line + 1; i < line + len; ++i) {
    if (*i == ';') {
      comment_start = i;
      break;
    }
  }
  if (comment_start) {
    line_comment.assign(comment_start, line + len);
  }
  return line_comment;
}

int Assembler::get_linenumber_from_asm_line_comment(std::string line_comment)
{
  int linenumber = -1;
  if (line_comment.size() >= 3 && line_comment[0] == ';' && line_comment[1] == 'L')
  {
    std::vector<char> line_number_chars;
    for (size_t i = 2; i < line_comment.length(); ++i)
    {
      auto ic = line_comment[i];
      if (isdigit(ic)) {
        line_number_chars.push_back(ic);
      }
      else {
        break;
      }
    }
    line_number_chars.push_back('\0');
    if (line_comment.length() >= 2 + line_number_chars.size() - 1 + 1
      && line_comment[2 + line_number_chars.size() - 1] == ';' && line_number_chars.size() >= 2) {
      // this is ";L<digit>;" + other line comment style
      std::string line_number_str(line_number_chars.data());
      linenumber = std::stoi(line_number_str);
    }
  }
  return linenumber;
}

inline Util::BoolRes Assembler::parseDirective(const char *line, size_t len) {
	std::string name;
    // line's first char is '.'
	const char *c = line + 1;
	const char *end = line + len;
    const char *comment_start = nullptr; // line comment start position
	for (; c != end; c++) {
		if (!(std::isalnum(*c) || *c == '_')) {
			if (std::isblank(*c) || *c == ';') {
				if (*c == ';') {
					end = --c;
				}
				break;
			}
			return Util::BoolRes(false, std::string("could not parse directive: illegal character '") + *c + "'");
		}
	}
    for (auto i = line + 1; i < line + len; ++i) {
      if (*i == ';') {
        comment_start = i;
        break;
      }
    }
    // here c is the end position of directive name
	name.assign(&line[1], c);
    
	if (name.empty()) {
		return Util::BoolRes(false, "could not parse directive");
	}
    auto line_comment = get_line_comment_from_asm_line_code(line, len);
    int linenumber = get_linenumber_from_asm_line_comment(line_comment);

	Util::lower(name);
	if (name == "upvalues") {
		if (bUpvalues_) {
			return Util::BoolRes(false, "already declared amount of upvalues");
		}
		if ((c = parseInt(nUpvalues_, c, end)) == nullptr) {
			return Util::BoolRes(false, "invalid args for directive .upvalues");
		}
		bUpvalues_ = true;
	} else if (name == "func") {
		if (parseStatus_ != PARSE_FUNC && parseStatus_ != PARSE_NONE) {
			return Util::BoolRes(false, "func declaration cannot be inside a code or const segment");
		}

		if (!instructions_.empty()) {
			auto res = finalizeFunction();
			if (!res.success()) {
				return res;
			}
		}

		if ((c = parseLabel(funcname_, c, end)) == nullptr) {
			return Util::BoolRes(false, "invalid args for directive .func");
		}

		if ((c = parseInt(f_maxstacksize_, c, end)) == nullptr) {
			return Util::BoolRes(false, "invalid args for directive .func");
		}
		if ((c = parseInt(f_params_, c, end)) == nullptr) {
			return Util::BoolRes(false, "invalid args for directive .func");
		}
		if ((c = parseInt(f_vararg_, c, end)) == nullptr) {
			return Util::BoolRes(false, "invalid args for directive .func");
		}
		if (f_vararg_ > 2) {
			return Util::BoolRes(false, "vararg cannot be greater than 2");
		}

		funcid_++;
		parseStatus_ = PARSE_FUNC;

	} else if (name == "begin_const") {
		if (parseStatus_ != PARSE_FUNC) {
			return Util::BoolRes(false, "const declaration must be inside function");
		}

		parseStatus_ = PARSE_CONST;
	} else if (name == "end_const") {
		if (parseStatus_ != PARSE_CONST) {
			return Util::BoolRes(false, "end_const must be inside const segment");
		}

		parseStatus_ = PARSE_FUNC;
	} else if (name == "begin_code") {
		if (parseStatus_ != PARSE_FUNC) {
			return Util::BoolRes(false, "code declaration must be inside function");
		}

		parseStatus_ = PARSE_CODE;
	} else if (name == "end_code") {
		if (parseStatus_ != PARSE_CODE) {
			return Util::BoolRes(false, "end_code must be inside code segment");
		}

		parseStatus_ = PARSE_FUNC;
	} else if (name == "begin_upvalue") {
		if (parseStatus_ != PARSE_FUNC) {
			return Util::BoolRes(false, "upvalue declaration must be inside function");
		}

		parseStatus_ = PARSE_UPVALUE;
	} else if (name == "end_upvalue") {
		if (parseStatus_ != PARSE_UPVALUE) {
			return Util::BoolRes(false, "end_upvalue must be inside upvalue segment");
		}

		parseStatus_ = PARSE_FUNC;
	}

	return Util::BoolRes(true, "");
}

const char *Assembler::parseConstant(const char *start, const char *end, size_t *id) {
	const char *c = start;
	const char *bend;

	char cf = std::tolower(start[0]);

	TValuePtr tval;

	if (cf == '\'' || cf == '"') { // parse string
		std::string string;

		c++;
		for (; c != end; ++c) {
			if (*c == '\\') {
				if (c + 1 == end) {
					return nullptr;
				}
				switch (*(++c)) {
					case 'a':
						string += '\a';
						break;
					case 'b':
						string += '\b';
						break;
					case 'f':
						string += '\f';
						break;
					case 'n':
						string += '\n';
						break;
					case 'r':
						string += '\r';
						break;
					case 't':
						string += '\t';
						break;
					case 'v':
						string += '\v';
						break;
					case '\\':
						string += '\\';
						break;
					case '"':
						string += '"';
						break;
					case '\'':
						string += '\'';
						break;
					case '[':
						string += '[';
						break;
					case ']':
						string += ']';
						break;
				}
			} else if (*c == cf) {
				tval.reset(new TString(string));
				bend = c + 1;
				break;
			} else {
				string += *c;
			}
		}
	}
	else if (std::isdigit(cf) || (cf == '-' || cf == '+')) { // parse number
		bool negative = false;
		lua_Number num = 0;

		if (cf == '-' || cf == '+') {
			if (cf == '-') {
				negative = true;
			}
			if (++c == end) {
				return nullptr;
			}
		}

		bool hex = false;
		if (cf == '0' && c + 1 != end && *(c + 1) == 'x') {
			// hexadecimal
			hex = true;
			c+=2;
		}

		if (hex) {
			for (; c != end; ++c) {
				if (std::isxdigit(*c)) {
					cf = *c;
					if (cf <= '9' && cf >= '0') {
						num = num * 16 + (cf - '0');
					} else {
						cf = std::tolower(cf);
						num = num * 16 + (cf - 'a' + 10);
					}
				} else {
					break;
				}
			}
		} else {
			lua_Number frac = 0;
			bool dpart = false;
			for (; c != end; ++c) {
				if (std::isdigit(*c)) {
					if (dpart) {
						num += frac * (*c - '0');
						frac *= 0.1;
					} else {
						num = num * 10 + (*c - '0');
					}
				} else if (*c == '.' && !dpart) {
					dpart = true;
					frac = 0.1;
				} else {
					break;
				}
			}
		}

		if (negative) {
			num *= -1;
		}

		tval.reset(new TNumber(num));
		bend = c;

	} else if (cf == 't' || cf == 'f' || cf == 'n') { // possibly true, false, or nil
		bend = std::find_if_not(c, end, std::ptr_fun<int, int>(std::isalpha));
		if (bend == c) {
			return nullptr;
		}

		std::string cval(c, bend);
		Util::lower(cval);
		if (cval == "true") {
			tval.reset(new TBool(true));
		} else if (cval == "false") {
			tval.reset(new TBool(false));
		} else if (cval == "nil") {
			tval.reset(new TValue());
		} else {
			return nullptr;
		}
	}

	if (!tval) {
		return nullptr;
	}

	if (parseStatus_ == PARSE_CONST) {
		if (bend != end && *bend != ';') {
			bend = std::find_if_not(bend, end, std::ptr_fun<int, int>(std::isblank));
			if (bend != end && *bend != ';') {
				return nullptr;
			}
		}
	}

	bool found = false;
	for (int i = 0; i < constants_.size(); i++) {
		if (*constants_[i] == *tval) {
			// use this constant instead of creating a duplicate
			found = true;
			if (id != nullptr) {
				*id = i;
			}
			break;
		}
	}

	if (!found) {
		constants_.push_back(tval);
		if (id != nullptr) {
			*id = constants_.size() - 1;
		}
	}

	return bend;
}

#define LIMIT_STACKIDX 1
#define LIMIT_UPVALUE 2
#define LIMIT_LOCATION 4
#define LIMIT_CONSTANT 8
#define LIMIT_EMBED 0x10
#define LIMIT_PROTO 0x20
#define LIMIT_CONST_STACK LIMIT_CONSTANT | LIMIT_STACKIDX

inline const char *Assembler::parseOperand(Operand &operand, const char *start, const char *end, unsigned int limit) {
	start = std::find_if_not(start, end, std::ptr_fun<int, int>(std::isblank));
	if (start == end || *start == ';') {
		return nullptr;
	}

	switch (*start) {
		case '%': { // stack index
			if (!(limit & LIMIT_STACKIDX) || ++start == end) {
				return nullptr;
			}

			size_t val;
			const char *bend = parseInt(val, start, end);
			if (bend == nullptr || (bend != end && (!std::isblank(*bend) && *bend != ';'))) {
				return nullptr;
			}

			operand.setType(Operand::STACKIDX);
			operand.setValue(val);

			return bend;
			break;
		}
		case '@': { // upvalue index.
			if (!(limit & LIMIT_UPVALUE) || ++start == end) {
				return nullptr;
			}

			size_t val;
			const char *bend = parseInt(val, start, end);
			if (bend == nullptr || (bend != end && (!std::isblank(*bend) && *bend != ';'))) {
				return nullptr;
			}

			operand.setType(Operand::UPVALUE);
			operand.setValue(val);
			
			return bend;
			break;
		}
		case '$': { // jmp location
			if (!(limit & LIMIT_LOCATION) || ++start == end) {
				return nullptr;
			}

			std::string label;
			const char *bend = parseLabel(label, start, end);
			if (bend == nullptr) {
				return nullptr;
			}

			operand.setType(Operand::LOCATION);

			auto it = locations_.find(label);
			if (it == locations_.end()) {
				operand.setValue(-1);
				neededLocations_.push_back(std::make_pair(label, (int)instructions_.size()));
			} else {
				operand.setValue(it->second - instructions_.size() - 1);
			}

			return bend;

			break;
		}
		default: {
			if (*start == 'c') { // could be const
				const char *bend = std::find_if_not(start, end, std::ptr_fun<int, int>(std::isalpha));
				if (bend != end && std::strncmp(start, "const", bend - start) == 0) {
					if (!(limit & LIMIT_CONSTANT)) {
						return nullptr;
					}
					bend = std::find_if_not(bend, end, std::ptr_fun<int, int>(std::isblank));
					if (bend == end || *bend == ';') {
						return nullptr;
					}

					size_t id;
					bend = parseConstant(bend, end, &id);
					if (bend == nullptr) {
						return nullptr;
					}
					operand.setType(Operand::CONSTANT);
					if (limit & LIMIT_STACKIDX) {
						operand.setValue(RKASK(id));
					} else {
						operand.setValue(id);
					}

					return bend;
				}
			}

			const char *bend;
			if (limit & LIMIT_PROTO) {
				std::string label;
				if ((bend = parseLabel(label, start, end)) == nullptr) {
					return nullptr;
				}
				auto u = usedSubroutines_.find(label);
				if (u != usedSubroutines_.end() && u->second != funcid_) {
					return nullptr;
				}
				auto it = subroutines_.find(label);
				operand.setValue(-1);
				neededSubroutines_.push_back(std::make_pair(label, (int)instructions_.size()));

				if (u == usedSubroutines_.end()) {
					usedSubroutines_[label] = funcid_;
					f_usedSubroutines_.push_back(label);
				}

				return bend;
			}

			if (!(limit & (LIMIT_EMBED | LIMIT_CONSTANT))) { 
				return nullptr;
			}

			int val;

			char cf = std::tolower(*start);
			if (cf == 't' || cf == 'f') {
				if ((bend = std::find_if_not(start, end, std::ptr_fun<int, int>(std::isalpha))) == start) {
					return nullptr;
				}

				std::string s(start, bend);
				Util::lower(s);

				if (s == "true") {
					val = 1;
				} else if (s == "false") {
					val = 0;
				} else {
					return nullptr;
				}
			} else {
				if ((bend = parseInt(val, start, end)) == nullptr) {
					return nullptr;
				}
			}

			operand.setType(Operand::EMBEDDED);
			operand.setValue(val);

			return bend;
		}
	}

	return nullptr;
}


enum OpPos {
	OPP_A,
	OPP_B,
	OPP_C,
	OPP_Ax,
	OPP_Bx,
	OPP_sBx,
	OPP_ARG,
	OPP_C_ARG
};

struct OpInfo {
	OpPos position;
	int limit;
};

const unsigned char opcount[NUM_OPCODES] = {
	2, // MOVE
	2, // LOADK
	2, // LOADKX
	3, // LOADBOOL
	2, // LOADNIL
	2, // GETUPVAL
	3, // GETTABUP
	3, // GETTABLE
	3, // SETTABUP
	2, // SETUPVAL
	3, // SETTABLE
	3, // NEWTABLE
	3, // SELF
	3, // ADD
	3, // SUB
	3, // MUL
	3, // DIV
	3, // BAND
	3, // BOR
	3, // BXOR
	3, // SHL
	3, // SHR
	3, // MOD
	3, // IDIV
	3, // POW
	2, // UNM
	2, // BNOT
	2, // NOT
	2, // LEN
	3, // CONCAT
	2, // JMP
	3, // EQ
	3, // LT
	3, // LE
	2, // TEST
	3, // TESTSET
	3, // CALL
	3, // TAILCALL
	2, // RETURN
	2, // FORLOOP
	2, // FORPREP
	2, // TFORCALL
	2, // TFORLOOP
	3, // SETLIST
	2, // CLOSURE
	2, // VARARG
	1 // EXTRAARG
};

const OpInfo opinfo[NUM_OPCODES][3] = { // Maximum of 3 operands
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}}, // MOVE
	{{OPP_A, LIMIT_STACKIDX}, {OPP_Bx, LIMIT_CONSTANT}}, // LOADK
	{{OPP_A, LIMIT_STACKIDX}, {OPP_ARG, LIMIT_CONSTANT}}, // LOADKX
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}, {OPP_C, LIMIT_EMBED}}, // LOADBOOL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}}, // LOADNIL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_UPVALUE}}, // GETUPVAL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_UPVALUE}, {OPP_C, LIMIT_CONST_STACK}}, // GETTABUP
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}, {OPP_C, LIMIT_CONST_STACK}}, // GETTABLE
	{{OPP_A, LIMIT_UPVALUE}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // SETTABUP
	{{OPP_B, LIMIT_UPVALUE}, {OPP_A, LIMIT_STACKIDX}}, /// SETUPVAL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // SETTABLE
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}, {OPP_C, LIMIT_EMBED}}, // NEWTABLE
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}, {OPP_C, LIMIT_CONST_STACK}}, // SELF
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // ADD
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // SUB
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // MUL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // DIV
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // BAND
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // BOR
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // BXOR
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // SHL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // SHR
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // MOD
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // IDIV
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // POW
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}}, // UNM
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}}, // BNOT
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}}, // NOT
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}}, // LEN
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}, {OPP_C, LIMIT_STACKIDX}}, // CONCAT
	{{OPP_A, LIMIT_EMBED}, {OPP_sBx, LIMIT_LOCATION}}, // JMP
	{{OPP_A, LIMIT_EMBED}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // EQ
	{{OPP_A, LIMIT_EMBED}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // LT
	{{OPP_A, LIMIT_EMBED}, {OPP_B, LIMIT_CONST_STACK}, {OPP_C, LIMIT_CONST_STACK}}, // LE
	{{OPP_A, LIMIT_STACKIDX}, {OPP_C, LIMIT_EMBED}}, // TEST
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_STACKIDX}, {OPP_C, LIMIT_EMBED}}, // TESTSET
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}, {OPP_C, LIMIT_EMBED}}, // CALL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}, {OPP_C, LIMIT_EMBED}}, // TAILCALL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}}, // RETURN
	{{OPP_A, LIMIT_STACKIDX}, {OPP_sBx, LIMIT_LOCATION}}, // FORLOOP
	{{OPP_A, LIMIT_STACKIDX}, {OPP_sBx, LIMIT_LOCATION}}, // FORPREP
	{{OPP_A, LIMIT_STACKIDX}, {OPP_C, LIMIT_EMBED}}, // TFORCALL
	{{OPP_A, LIMIT_STACKIDX}, {OPP_sBx, LIMIT_LOCATION}}, // TFORLOOP
	{{OPP_A, LIMIT_STACKIDX}, {OPP_B, LIMIT_EMBED}, {OPP_C_ARG, LIMIT_EMBED}}, // SETLIST
	{{OPP_A, LIMIT_STACKIDX}, {OPP_Bx, LIMIT_PROTO}}, // CLOSURE
	{{OPP_A, LIMIT_STACKIDX}, {OPP_Bx, LIMIT_EMBED}}, // VARARG
	{{OPP_Ax, LIMIT_EMBED}} // EXTRAARG
};

inline Util::BoolRes Assembler::parseCode(const char *line, size_t len) {
	const char *c = line;
	const char *end = line + len;

	std::string opcodestr;
	const char *bend = parseLabel(opcodestr, c, end);
	if (bend == nullptr) {
		return Util::BoolRes(false, "invalid opcode");
	}

    auto line_comment = get_line_comment_from_asm_line_code(line, len);
    auto linenumber = get_linenumber_from_asm_line_comment(line_comment);
    if (linenumber >= 0)
    {
      lineinfos_.push_back(linenumber);
    }

	if (bend != end && *bend == ':') { // location
		locations_[opcodestr] = instructions_.size();

		for (auto it = neededLocations_.begin(); it != neededLocations_.end(); ) {
			if ((*it).first == opcodestr) {
				/* Fix entry in instructions_[(*it).second] */

				Instruction *ins = &instructions_[(it->second)];
				// all jmp instructons use sBx

				SETARG_sBx(*ins, instructions_.size() - it->second - 1);

				neededLocations_.erase(it);
			} else {
				it++;
			}
		}

		return Util::BoolRes(true, "");
	}
	Util::lower(opcodestr);



	OpCode opcode;

	int i;
	for (i = 0; i < NUM_OPCODES; i++) {
		if (opcodestr == luaP_opnames[i]) {
			opcode = (OpCode)i;
			break;
		}
	}
	if (i == NUM_OPCODES) {
		return Util::BoolRes(false, "invalid opcode");
	}

	Instruction ins = 0;
	int extended;
	bool useExtended = false;
	SET_OPCODE(ins, opcode);

	unsigned char count = opcount[opcode];
	const OpInfo *info = opinfo[opcode];

	for (int i = 0; i < count; i++) {
		Operand op;
		if ((bend = parseOperand(op, bend, end, info[i].limit)) == nullptr) {
			return Util::BoolRes(false, "invalid operand(s)");
		}

		switch (info[i].position) {
			case OPP_A:
				SETARG_A(ins, op.value());
				break;
			case OPP_B:
				SETARG_B(ins, op.value());
				break;
			case OPP_C:
				SETARG_C(ins, op.value());
				break;
			case OPP_Bx:
				SETARG_Bx(ins, op.value());
				break;
			case OPP_Ax:
				SETARG_Ax(ins, op.value());
				break;
			case OPP_sBx:
				SETARG_sBx(ins, op.value());
				break;
			case OPP_ARG:
				extended = op.value();
				useExtended = true;
				break;
			case OPP_C_ARG:
				if (op.value() > MAXARG_C) {
					SETARG_C(ins, 0);
					extended = op.value();
					useExtended = true;
				} else {
					SETARG_C(ins, op.value());
				}
				break;
		}
	}

	bend = std::find_if_not(bend, end, std::ptr_fun<int, int>(std::isblank));
	if (bend != end && *bend != ';') {
		return Util::BoolRes(false, "too many operands in instruction");
	}

	instructions_.push_back(ins);

	if (useExtended) {
		Instruction extended = 0;
		SET_OPCODE(extended, OP_EXTRAARG);
		SETARG_Ax(extended, extended);
		instructions_.push_back(extended);
	}

	return Util::BoolRes(true, "");
}

inline Util::BoolRes Assembler::parseUpvalue(const char *line, size_t len) {
	const char *end = line + len;
	const char *c = line;
	unsigned char instack, idx;

	if ((c = parseInt(instack, c, end)) == nullptr) {
		return Util::BoolRes(false, "could not parse instack");
	}
	if ((c = parseInt(idx, c, end)) == nullptr) {
		return Util::BoolRes(false, "could not parse idx");
	}

	c = std::find_if_not(c, end, std::ptr_fun<int, int>(std::isblank));
	if (c != end && *c != ';') {
		return Util::BoolRes(false, "invalid upvalue");
	}

	upvalues_.push_back(Upvalue{instack, idx, ""});

	return Util::BoolRes(true, "");
}

Util::BoolRes Assembler::parseLine(const char *line, size_t len) {
	if (line[0] == '.') { // directive
		return parseDirective(line, len);
	}

	switch(parseStatus_) {
		case PARSE_CONST:
			if (parseConstant(line, line + len, nullptr) == nullptr) {
				return Util::BoolRes(false, "could not parse constant");
			}
			return Util::BoolRes(true, "");
		case PARSE_CODE:
			return parseCode(line, len);
		case PARSE_UPVALUE:
			return parseUpvalue(line, len);
		default:
			return Util::BoolRes(false, "unimplemented");
	}
}

Util::BoolRes Assembler::finalizeFunction() {
	if (!neededLocations_.empty()) {
		std::string locList = std::string("undeclared locations: ") + neededLocations_[0].first;
		for (int i = 1; i < neededLocations_.size(); i++) {
			locList += ", " + neededLocations_[i].first;
		}
		return Util::BoolRes(false, locList);
	}

	ParsedFunctionPtr func(new ParsedFunction);
	func->name = std::move(funcname_);
	func->instructions = std::move(instructions_);
	func->upvalues = std::move(upvalues_);
	func->constants = std::move(constants_);
	func->neededSubroutines = std::move(neededSubroutines_);
	func->usedSubroutines = std::move(f_usedSubroutines_);
	func->maxstacksize = f_maxstacksize_;
	func->params = f_params_;
	func->vararg = f_vararg_;

    func->lineinfos = lineinfos_;
    lineinfos_.clear();

	functions_[func->name] = func;

	return Util::BoolRes(true, "");
}

#define WRITE_ASSERT(f, msg) if (!f) return Util::BoolRes(false, msg);

inline Util::BoolRes Assembler::writeHeader() {
	if (wbuffer_->writeBytes(LUA_SIGNATURE, sizeof(LUA_SIGNATURE)-1) != sizeof(LUA_SIGNATURE)-1) {
		return Util::BoolRes(false, "failed to write signature");
	}
	WRITE_ASSERT(wbuffer_->write<unsigned char>(LUAC_VERSION).success(), "failed to write version");
	WRITE_ASSERT(wbuffer_->write<unsigned char>(LUAC_FORMAT).success(), "failed to write format");
	if (wbuffer_->writeBytes(LUAC_DATA, sizeof(LUAC_DATA)-1) != sizeof(LUAC_DATA)-1) {
		return Util::BoolRes(false, "failed to write LUAC_DATA");
	}
	WRITE_ASSERT(wbuffer_->write<unsigned char>(sizeof(int)).success(), "failed to write int size");
	WRITE_ASSERT(wbuffer_->write<unsigned char>(sizeof(LUA_SIZE_T_TYPE)).success(), "failed to write size_t size");
	WRITE_ASSERT(wbuffer_->write<unsigned char>(sizeof(Instruction)).success(), "failed to write instruction size");
	WRITE_ASSERT(wbuffer_->write<unsigned char>(sizeof(lua_Integer)).success(), "failed to write integer size");
	WRITE_ASSERT(wbuffer_->write<unsigned char>(sizeof(lua_Number)).success(), "failed to write number size");
	WRITE_ASSERT(wbuffer_->write<lua_Integer>(LUAC_INT).success(), "failed to write LUAC_INT");
	WRITE_ASSERT(wbuffer_->write<lua_Number>(LUAC_NUM).success(), "failed to write LUAC_NUM");

	return Util::BoolRes(true, "");
}


Util::BoolRes Assembler::writeString(const std::string &string) {
	size_t len = string.size();
	Util::BoolRes res;
	if (len < 0xFE) {
		res = wbuffer_->write<unsigned char>(len + 1);
	} else {
		res = wbuffer_->write<unsigned char>(0xFF);
		if (!res.success()) {
			return res;
		}
		res = wbuffer_->write(len + 1);
	}
	if (!res.success()) {
		return res;
	}

	if (wbuffer_->writeBytes(string.c_str(), len) != len) {
		return Util::BoolRes(false, "could not write string");
	}
	return Util::BoolRes(true, "");
}

Util::BoolRes Assembler::writeFunction(ParsedFunctionPtr function) {
	auto res = writeString(function->name);
	if (!res.success()) {
		return res;
	}

	if (!(res = wbuffer_->write<int>(0)).success()) { // linedefined (unimplemented)
		return res;
	}
	if (!(res = wbuffer_->write<int>(0)).success()) { // lastlinedefined (unimplemented)
		return res;
	}
	if (!(res = wbuffer_->write(function->params)).success()) { // numparams
		return res;
	}
	if (!(res = wbuffer_->write(function->vararg)).success()) { // is_vararg
		return res;
	}
	if (!(res = wbuffer_->write(function->maxstacksize)).success()) { // maxstacksize
		return res;
	}

	std::vector<ParsedFunctionPtr> protos;

	// fix missing closure pointers
	for (int i = 0; i < function->usedSubroutines.size(); i++) {
		std::string pName = function->usedSubroutines[i];
		auto sub = functions_.find(pName);
		if (sub == functions_.end()) {
			return Util::BoolRes(false, std::string("no such function: ") + pName);
		}

		protos.push_back(sub->second);

		for (auto it = function->neededSubroutines.begin(); it != function->neededSubroutines.end();) {
			if (it->first == pName) {
				SETARG_Bx(function->instructions[it->second], i);
				it = function->neededSubroutines.erase(it);
			} else {
				it++;
			}
		}
	}


	if (!(res = wbuffer_->write<int>(function->instructions.size())).success()) { // code length	
		return res;
	}
	if (wbuffer_->writeBytes((const char*)function->instructions.data(), function->instructions.size() * sizeof(Instruction))  != function->instructions.size() * sizeof(Instruction)) { // code
		return Util::BoolRes(false, "failed to write instructions");
	}


	if (!(res = wbuffer_->write<int>(function->constants.size())).success()) { // constants length	
		return res;
	}

	for (TValuePtr constant : function->constants) {
		if (!(res = wbuffer_->write<unsigned char>(constant->type())).success()) { // constant type
			return res;
		}
		switch (constant->type()) {
			case LUA_TSTRING: {
				if (!(res = writeString(reinterpret_cast<TString*>(constant.get())->string())).success()) {
					return res;
				}
				break;
			}
			case LUA_TNUMBER: {
				if (!(res = wbuffer_->write(reinterpret_cast<TNumber*>(constant.get())->number())).success()) {
					return res;
				}
				break;
			}
			case LUA_TBOOLEAN: {
				if (!(res = wbuffer_->write(reinterpret_cast<TBool*>(constant.get())->value())).success()) {
					return res;
				}
				break;
			}
		}
	}

	if (!(res = wbuffer_->write<int>(function->upvalues.size())).success()) { // upvalues length	
		return res;
	}

	for (Upvalue upvalue : function->upvalues) {
		if (!(res = wbuffer_->write(upvalue.instack)).success()) { // instack
			return res;
		}
		if (!(res = wbuffer_->write(upvalue.idx)).success()) { // idx
			return res;
		}
	}

	if (!(res = wbuffer_->write<int>(protos.size())).success()) { // protos length	
		return res;
	}
	for (ParsedFunctionPtr proto : protos) {
		if (!(res = writeFunction(proto)).success()) {
			return res;
		}
	}

    // dumpSizeLineinfos and dumpVector(lineinfos)
	if (!(res = wbuffer_->write<int>(function->lineinfos.size())).success()) { // line info size
		return res;
	}
    // dumpVector(lineinfos)
    if (function->lineinfos.size() > 0)
    {
        wbuffer_->writeBytes((const char *)function->lineinfos.data(), function->lineinfos.size() * sizeof(function->lineinfos[0]));
    }

	if (!(res = wbuffer_->write<int>(0)).success()) { // local var size (unimplemented)
		return res;
	}
	if (!(res = wbuffer_->write<int>(0)).success()) { // upvalue name size (unimplemented)
		return res;
	}

	return Util::BoolRes(true, "");
}


Util::BoolRes Assembler::assemble() {
	if (!rbuffer_) {
		return Util::BoolRes(false, "invalid read buffer");
	}
	if (!wbuffer_) {
		return Util::BoolRes(false, "invalid write buffer");
	}

	std::string line;
	unsigned int linen = 0;
	while (rbuffer_->readLine(line).success()) {
		linen++;

		Util::trim(line);
		if (line.empty() || line.front() == ';') {
			continue;
		}

		auto res = parseLine(line.c_str(), line.length());
		if (!res.success()) {
			return Util::BoolRes(false, std::string("error parsing line ") + std::to_string(linen) + ": " + res.error_msg());
		}
	}

	if (!instructions_.empty()) {
		auto res = finalizeFunction();
		if (!res.success()) {
			return res;
		}
	}

	if (!bUpvalues_) {
		return Util::BoolRes(false, "amount of upvalues never declared");
	}

	auto  res = writeHeader();
	if (!res.success()) {
		return res;
	}

	WRITE_ASSERT(wbuffer_->write<unsigned char>(nUpvalues_).success(), "failed to write num upvalues");

	auto it = functions_.find("main");
	if (it == functions_.end()) {
		return Util::BoolRes(false, "no main function");
	}

	return writeFunction(it->second);

	// return Util::BoolRes(true, "");
}