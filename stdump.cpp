#include "ccc/ccc.h"

#include <algorithm>
#include <set>

void print_address(const char* name, u64 address) {
	fprintf(stderr, "%32s @ 0x%08lx\n", name, address);
}

enum OutputMode : u32 {
	OUTPUT_HELP = 0,
	OUTPUT_SYMBOLS = 1,
	OUTPUT_TYPES = 2,
	OUTPUT_TEST = 4
};

struct Options {
	OutputMode mode = OUTPUT_HELP;
	fs::path input_file;
	bool verbose = false;
};

static Options parse_args(int argc, char** argv);
static void print_symbols(Program& program, SymbolTable& symbol_table);
static void print_types(const SymbolTable& symbol_table, bool verbose);
static void print_test(const SymbolTable& symbol_table, bool verbose);
static void print_help();

int main(int argc, char** argv) {
	Options options = parse_args(argc, argv);
	if(options.mode == OUTPUT_HELP) {
		print_help();
		exit(1);
	}
	
	Program program;
	program.images.emplace_back(read_program_image(options.input_file));
	parse_elf_file(program, 0);
	
	SymbolTable symbol_table;
	bool has_symbol_table = false;
	for(ProgramSection& section : program.sections) {
		if(section.type == ProgramSectionType::MIPS_DEBUG) {
			if(options.verbose) {
				print_address("mdebug section", section.file_offset);
			}
			symbol_table = parse_symbol_table(program.images[0], section);
			has_symbol_table = true;
		}
	}
	verify(has_symbol_table, "No symbol table.\n");
	if(options.verbose) {
		print_address("procedure descriptor table", symbol_table.procedure_descriptor_table_offset);
		print_address("local symbol table", symbol_table.local_symbol_table_offset);
		print_address("file descriptor table", symbol_table.file_descriptor_table_offset);
	}
	
	if(options.mode & OUTPUT_SYMBOLS) {
		print_symbols(program, symbol_table);
	}
	if(options.mode & OUTPUT_TYPES) {
		print_types(symbol_table, options.verbose);
	}
	if(options.mode & OUTPUT_TEST) {
		print_test(symbol_table, options.verbose);
	}
}

static Options parse_args(int argc, char** argv) {
	Options options;
	for(int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if(arg == "--symbols" || arg == "-s") {
			(u32&) options.mode |= OUTPUT_SYMBOLS;
		}
		if(arg == "--types" || arg == "-t") {
			(u32&) options.mode |= OUTPUT_TYPES;
		}
		if(arg == "--test") {
			(u32&) options.mode |= OUTPUT_TEST;
		}
		if(arg == "--verbose" || arg == "-v") {
			options.verbose = true;
		}
	}
	for(int i = 1; i < argc; i++) {
		std::string arg = argv[i];
		if(arg == "--symbols" || arg == "-s") {
			continue;
		}
		if(arg == "--types" || arg == "-t") {
			continue;
		}
		if(arg == "--test") {
			continue;
		}
		if(arg == "--verbose" || arg == "-v") {
			continue;
		}
		verify(options.input_file.empty(), "error: Multiple input files specified.\n");
		options.input_file = arg;
	}
	return options;
}

static void print_symbols(Program& program, SymbolTable& symbol_table) {
	for(SymFileDescriptor& fd : symbol_table.files) {
		printf("FILE %s:\n", fd.name.c_str());
		for(Symbol& sym : fd.symbols) {
			const char* symbol_type_str = symbol_type(sym.storage_type);
			const char* symbol_class_str = symbol_class(sym.storage_class);
			printf("\t%8x ", sym.value);
			if(symbol_type_str) {
				printf("%11s ", symbol_type_str);
			} else {
				printf("ST(%5d) ", (u32) sym.storage_type);
			}
			if(symbol_class_str) {
				printf("%6s ", symbol_class_str);
			} else if ((u32)sym.storage_class == 0) {
				printf("       ");
			} else {
				printf("SC(%2d) ", (u32) sym.storage_class);
			}
			printf("%8d %s\n", sym.index, sym.string.c_str());
		}
	}
}

static std::vector<AstNode> symbols_to_ast(const std::vector<StabsSymbol>& symbols, const std::map<s32, TypeName>& type_names) {
	auto is_data_type = [&](const StabsSymbol& symbol) {
		return symbol.mdebug_symbol.storage_type == SymbolType::NIL
			&& (u32) symbol.mdebug_symbol.storage_class == 0
			&& (symbol.descriptor == StabsSymbolDescriptor::ENUM_STRUCT_OR_TYPE_TAG
				|| symbol.descriptor == StabsSymbolDescriptor::TYPE_NAME);
	};
	
	std::vector<AstNode> ast_nodes;
	for(const StabsSymbol& symbol : symbols) {
		if(is_data_type(symbol)) {
			std::optional<AstNode> node = stabs_symbol_to_ast(symbol, type_names);
			if(node.has_value()) {
				node->top_level = true;
				node->symbol = &symbol;
				ast_nodes.emplace_back(std::move(*node));
			}
		}
	}
	return ast_nodes;
}

static void print_types(const SymbolTable& symbol_table, bool verbose) {
	for(const SymFileDescriptor& fd : symbol_table.files) {
		const std::vector<StabsSymbol> symbols = parse_stabs_symbols(fd.symbols);
		const std::map<s32, const StabsType*> types = enumerate_numbered_types(symbols);
		const std::map<s32, TypeName> type_names = resolve_c_type_names(types);
		const std::vector<AstNode> ast_nodes = symbols_to_ast(symbols, type_names);
		
		for(const AstNode& node : ast_nodes) {
			assert(node.symbol);
			printf("// %s\n", node.symbol->raw.c_str());
			print_ast_node(stdout, node, 0);
			printf("\n");
		}
	}
}

static void print_test(const SymbolTable& symbol_table, bool verbose) {
	const SymFileDescriptor& fd = symbol_table.files.at(1);
	const std::vector<StabsSymbol> symbols = parse_stabs_symbols(fd.symbols);
	const std::map<s32, const StabsType*> types = enumerate_numbered_types(symbols);
	const std::map<s32, TypeName> type_names = resolve_c_type_names(types);
	const std::vector<AstNode> ast_nodes = symbols_to_ast(symbols, type_names);
	
	for(const AstNode& node : ast_nodes) {
		bool print = true;
		switch(node.descriptor) {
			case AstNodeDescriptor::ENUM: printf("enum"); break;
			case AstNodeDescriptor::STRUCT: printf("struct"); break;
			case AstNodeDescriptor::UNION: printf("union"); break;
			default:
				print = false;
		}
		if(print) {
			printf(" %s;\n", node.name.c_str());
		}
	}
	for(const AstNode& node : ast_nodes) {
		assert(node.symbol);
		printf("// %s\n", node.symbol->raw.c_str());
		print_ast_node(stdout, node, 0);
		printf("\n");
	}
	int test_case = 0;
	printf("#define CCC_OFFSETOF(type, field) ((int) &((type*) 0)->field)\n");
	for(const AstNode& node : ast_nodes) {
		if(node.descriptor == AstNodeDescriptor::STRUCT) {
			for(const AstNode& field : node.struct_or_union.fields) {
				printf("typedef int testcase__%d__%s__%s[(CCC_OFFSETOF(%s,%s) == %d) ? 1 : -1];\n",
					test_case++, node.name.c_str(), field.name.c_str(),
					node.name.c_str(), field.name.c_str(), field.offset);
			}
		}
	}
}

void print_help() {
	puts("stdump: MIPS/GCC symbol table parser.");
	puts("");
	puts("OPTIONS:");
	puts(" --symbols, -s      Print a list of all the local symbols, grouped");
	puts("                    by file descriptor.");
	puts("");
	puts(" --types, -t        TODO");
	puts("");
	puts(" --test             TODO");
	puts("");
	puts(" --verbose, -v      Print out addition information e.g. the offsets of");
	puts("                    various data structures in the input file.");
}
