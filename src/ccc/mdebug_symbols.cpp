// This file is part of the Chaos Compiler Collection.
// SPDX-License-Identifier: MIT

#include "mdebug_symbols.h"

#include "symbol_table.h"

namespace ccc::mdebug {

static void mark_duplicate_symbols(std::vector<ParsedSymbol>& symbols);

Result<std::vector<ParsedSymbol>> parse_symbols(const std::vector<mdebug::Symbol>& input, u32& parser_flags)
{
	std::vector<ParsedSymbol> output;
	std::string prefix;
	for(const mdebug::Symbol& symbol : input) {
		if(symbol.is_stabs()) {
			switch(symbol.code()) {
				case mdebug::N_GSYM: // Global variable
				case mdebug::N_FUN: // Function
				case mdebug::N_STSYM: // Data section static global variable
				case mdebug::N_LCSYM: // BSS section static global variable
				case mdebug::N_RSYM: // Register variable
				case mdebug::N_LSYM: // Automatic variable or type definition
				case mdebug::N_PSYM: { // Parameter variable
					// Some STABS symbols are split between multiple strings.
					if(symbol.string[0] != '\0') {
						if(symbol.string[strlen(symbol.string) - 1] == '\\') {
							prefix += std::string(symbol.string, symbol.string + strlen(symbol.string) - 1);
						} else {
							std::string symbol_string = prefix + symbol.string;
							prefix.clear();
							
							const char* input = symbol_string.c_str();
							Result<StabsSymbol> parse_result = parse_stabs_symbol(input);
							if(parse_result.success()) {
								if(*input != '\0') {
									if(parser_flags & STRICT_PARSING) {
										return CCC_FAILURE("Unknown data '%s' at the end of the '%s' stab.", input, parse_result->name.c_str());
									} else {
										CCC_WARN("Unknown data '%s' at the end of the '%s' stab.", input, parse_result->name.c_str());
									}
								}
								
								ParsedSymbol& parsed = output.emplace_back();
								parsed.type = ParsedSymbolType::NAME_COLON_TYPE;
								parsed.raw = &symbol;
								parsed.name_colon_type = std::move(*parse_result);
							} else if(parse_result.error().message == STAB_TRUNCATED_ERROR_MESSAGE) {
								// Symbol truncated due to a GCC bug. Report a
								// warning and try to tolerate further faults
								// caused as a result of this.
								CCC_WARN("%s Symbol string: %s", STAB_TRUNCATED_ERROR_MESSAGE, symbol_string.c_str());
								parser_flags &= ~STRICT_PARSING;
							} else {
								return parse_result;
							}
						}
					} else {
						CCC_CHECK(prefix.empty(), "Invalid STABS continuation.");
						if(symbol.code() == mdebug::N_FUN) {
							ParsedSymbol& func_end = output.emplace_back();
							func_end.type = ParsedSymbolType::FUNCTION_END;
							func_end.raw = &symbol;
						}
					}
					break;
				}
				case mdebug::N_SOL: { // Sub-source file
					ParsedSymbol& sub = output.emplace_back();
					sub.type = ParsedSymbolType::SUB_SOURCE_FILE;
					sub.raw = &symbol;
					break;
				}
				case mdebug::N_LBRAC: { // Begin block
					ParsedSymbol& begin_block = output.emplace_back();
					begin_block.type = ParsedSymbolType::LBRAC;
					begin_block.raw = &symbol;
					break;
				}
				case mdebug::N_RBRAC: { // End block
					ParsedSymbol& end_block = output.emplace_back();
					end_block.type = ParsedSymbolType::RBRAC;
					end_block.raw = &symbol;
					break;
				}
				case mdebug::N_SO: { // Source filename
					ParsedSymbol& so_symbol = output.emplace_back();
					so_symbol.type = ParsedSymbolType::SOURCE_FILE;
					so_symbol.raw = &symbol;
					break;
				}
				case mdebug::STAB:
				case mdebug::N_OPT:
				case mdebug::N_BINCL: {
					break;
				}
				case mdebug::N_FNAME:
				case mdebug::N_MAIN:
				case mdebug::N_PC:
				case mdebug::N_NSYMS:
				case mdebug::N_NOMAP:
				case mdebug::N_OBJ:
				case mdebug::N_M2C:
				case mdebug::N_SLINE:
				case mdebug::N_DSLINE:
				case mdebug::N_BSLINE:
				case mdebug::N_EFD:
				case mdebug::N_EHDECL:
				case mdebug::N_CATCH:
				case mdebug::N_SSYM:
				case mdebug::N_EINCL:
				case mdebug::N_ENTRY:
				case mdebug::N_EXCL:
				case mdebug::N_SCOPE:
				case mdebug::N_BCOMM:
				case mdebug::N_ECOMM:
				case mdebug::N_ECOML:
				case mdebug::N_NBTEXT:
				case mdebug::N_NBDATA:
				case mdebug::N_NBBSS:
				case mdebug::N_NBSTS:
				case mdebug::N_NBLCS:
				case mdebug::N_LENG: {
					CCC_WARN("Unhandled N_%s symbol: %s", mdebug::stabs_code_to_string(symbol.code()), symbol.string);
					break;
				}
			}
		} else {
			ParsedSymbol& non_stabs_symbol = output.emplace_back();
			non_stabs_symbol.type = ParsedSymbolType::NON_STABS;
			non_stabs_symbol.raw = &symbol;
		}
	}
	
	mark_duplicate_symbols(output);
	
	return output;
}

static void mark_duplicate_symbols(std::vector<ParsedSymbol>& symbols)
{
	return;
	
	std::map<StabsTypeNumber, size_t> stabs_type_number_to_symbol;
	for(size_t i = 0; i < symbols.size(); i++) {
		ParsedSymbol& symbol = symbols[i];
		if(symbol.type == ParsedSymbolType::NAME_COLON_TYPE && !symbol.name_colon_type.type->anonymous) {
			stabs_type_number_to_symbol.emplace(symbol.name_colon_type.type->type_number, i);
		}
	}
	
	for(ParsedSymbol& symbol : symbols) {
		if(symbol.type == ParsedSymbolType::NAME_COLON_TYPE && symbol.name_colon_type.type->descriptor == StabsTypeDescriptor::TYPE_REFERENCE) {
			StabsTypeReferenceType& reference = symbol.name_colon_type.type->as<StabsTypeReferenceType>();
			auto referenced_index = stabs_type_number_to_symbol.find(reference.type->type_number);
			if(referenced_index != stabs_type_number_to_symbol.end()) {
				ParsedSymbol& referenced = symbols[referenced_index->second];
				if(referenced.name_colon_type.name == " " || referenced.name_colon_type.name == symbol.name_colon_type.name) {
					referenced.duplicate = true;
					symbol.dont_substitute_type_name = true;
				}
			}
		}
	}
}

}
