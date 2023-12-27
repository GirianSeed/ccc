// This file is part of the Chaos Compiler Collection.
// SPDX-License-Identifier: MIT

#include "ccc/ccc.h"
#include "platform/file.h"
#define HAVE_DECL_BASENAME 1
#include "demangle.h"

using namespace ccc;

enum Flags {
	NO_FLAGS = 0,
	FLAG_PER_FILE = 1 << 0,
	FLAG_OMIT_ACCESS_SPECIFIERS = 1 << 1,
	FLAG_OMIT_MEMBER_FUNCTIONS = 1 << 2,
	FLAG_INCLUDE_GENERATED_FUNCTIONS = 1 << 3,
	FLAG_LOCAL_SYMBOLS = 1 << 4,
	FLAG_EXTERNAL_SYMBOLS = 1 << 5,
	FLAG_MANGLED = 1 << 6
};

struct Options {
	void (*function)(FILE* out, const Options& options) = nullptr;
	fs::path input_file;
	fs::path output_file;
	u32 flags = NO_FLAGS;
	std::optional<std::string> section;
	std::optional<SymbolTableFormat> format;
};

static void identify_symbol_tables(FILE* out, const Options& options);
static void identify_symbol_tables_in_file(FILE* out, u32* totals, u32* unknown_total, const fs::path& file_path);
static void print_functions(FILE* out, const Options& options);
static void print_globals(FILE* out, const Options& options);
static void print_types(FILE* out, const Options& options);
static void print_types_deduplicated(FILE* out, SymbolDatabase& database, const Options& options);
static void print_types_per_file(FILE* out, SymbolDatabase& database, const Options& options);
static void print_type_graph(FILE* out, const Options& options);
static void print_labels(FILE* out, const Options& options);
static void print_json(FILE* out, const Options& options);
static void print_symbols(FILE* out, const Options& options);
static void print_headers(FILE* out, const Options& options);
static u32 command_line_flags_to_importer_flags(u32 flags);
static void print_files(FILE* out, const Options& options);
static void print_sections(FILE* out, const Options& options);
static SymbolDatabase read_symbol_table(SymbolFile& symbol_file, const Options& options);
static Options parse_command_line_arguments(int argc, char** argv);
static void print_help(FILE* out);
static const char* get_version();

struct StdumpCommand {
	void (*function)(FILE* out, const Options& options);
	const char* name;
	std::initializer_list<const char*> help_text;
};

static const StdumpCommand commands[] = {
	{identify_symbol_tables, "identify", {
		"Identify the symbol tables present in the input file(s). If the input path",
		"is a directory, it will be walked recursively."
	}},
	{print_functions, "functions", {
		"Print all the functions recovered from the symbol table as C++."
	}},
	{print_globals, "globals", {
		"Print all the global variables recovered from the symbol table as C++."
	}},
	{print_types, "types", {
		"Print all the types recovered from the symbol table as C++.",
		"",
		"--per-file                    Do not deduplicate types from files.",
		"--omit-access-specifiers      Do not print access specifiers.",
		"--omit-member-functions       Do not print member functions.",
		"--include-generated-functions Include member functions that are likely",
		"                              auto-generated."
	}},
	{print_type_graph, "type_graph", {
		"Print out a dependency graph of all the types in graphviz DOT format."
	}},
	{print_labels, "labels", {
		"Print all the labels recovered from the symbol table. Note that this may",
		"include other symbols where their type is not recoverable."
	}},
	{print_json, "json", {
		"Print all of the above as JSON.",
		"",
		"--per-file                    Do not deduplicate types from files."
	}},
	{print_symbols, "symbols", {
		"Print all of the symbols in a given symbol table.",
		"",
		"--locals                      Only print local .mdebug symbols.",
		"--externals                   Only print external .mdebug symbols."
	}},
	{print_headers, "headers", {
		"Print out the contents of the .mdebug header."
	}},
	{print_files, "files", {
		"Print a list of all the source files."
	}},
	{print_sections, "sections", {
		"List the names of the source files associated with each ELF section."
	}}
};

int main(int argc, char** argv)
{
	Options options = parse_command_line_arguments(argc, argv);
	
	FILE* out = stdout;
	if(!options.output_file.empty()) {
		out = fopen(options.output_file.string().c_str(), "w");
		CCC_CHECK_FATAL(out, "Failed to open output file '%s'.", options.output_file.string().c_str());
	}
	
	if(options.function) {
		options.function(out, options);
	} else {
		print_help(out);
		return 1;
	}
}

static void identify_symbol_tables(FILE* out, const Options& options)
{
	if(fs::is_regular_file(options.input_file)) {
		identify_symbol_tables_in_file(out, nullptr, nullptr, options.input_file);
	} else if(fs::is_directory(options.input_file)) {
		std::vector<u32> totals(SYMBOL_TABLE_FORMAT_COUNT, 0);
		u32 unknown_total = 0;
		
		for(auto entry : fs::recursive_directory_iterator(options.input_file)) {
			if(entry.is_regular_file()) {
				identify_symbol_tables_in_file(out, totals.data(), &unknown_total, entry.path());
			}
		}
		
		fprintf(out, "\n");
		fprintf(out, "Totals:\n");
		for(u32 i = 0; i < SYMBOL_TABLE_FORMAT_COUNT; i++) {
			fprintf(out, "  %4d %s sections\n", totals[i], SYMBOL_TABLE_FORMATS[i].section_name);
		}
		fprintf(out, "  %4d unknown\n", unknown_total);
	} else {
		CCC_FATAL("Input path '%s' is neither a regular file nor a directory.", options.input_file.string().c_str());
	}
}

static void identify_symbol_tables_in_file(FILE* out, u32* totals, u32* unknown_total, const fs::path& file_path)
{
	fprintf(out, "%100s:", file_path.string().c_str());
	
	Result<std::vector<u8>> file = platform::read_binary_file(file_path);
	CCC_EXIT_IF_ERROR(file);
	
	const u32* fourcc = get_packed<u32>(*file, 0);
	if(!fourcc) {
		fprintf(out, " file too small\n");
		return;
	}
	
	switch(*fourcc) {
		case CCC_FOURCC("\x7f""ELF"): {
			Result<ElfFile> elf = parse_elf_file(std::move(*file));
			CCC_EXIT_IF_ERROR(elf);
			
			bool print_none = true;
			for(u32 i = 0; i < SYMBOL_TABLE_FORMAT_COUNT; i++) {
				if(elf->lookup_section(SYMBOL_TABLE_FORMATS[i].section_name)) {
					fprintf(out, " %s", SYMBOL_TABLE_FORMATS[i].section_name);
					if(totals) {
						totals[i]++;
					}
					print_none = false;
				}
			}
			
			if(print_none) {
				fprintf(out, " none");
			}
			
			fprintf(out, "\n");
			
			break;
		}
		case CCC_FOURCC("SNR1"):
		case CCC_FOURCC("SNR2"): {
			if(totals) {
				totals[SNDLL]++;
			}
			fprintf(out, " sndll\n");
			break;
		}
		default: {
			if(unknown_total) {
				(*unknown_total)++;
			}
			fprintf(out, " unknown format\n");
			break;
		}
	}
}

static void print_functions(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	CppPrinterConfig config;
	CppPrinter printer(out, config);
	
	bool first_iteration = true;
	SourceFileHandle source_file_handle;
	for(const Function& function : database.functions) {
		if(function.source_file() != source_file_handle || first_iteration) {
			SourceFile* source_file = database.source_files.symbol_from_handle(function.source_file());
			if(source_file) {
				printer.comment_block_file(source_file->full_path().c_str());
				source_file_handle = source_file->handle();
			} else {
				printer.comment_block_file("(unknown)");
				source_file_handle = SourceFileHandle();
			}
			first_iteration = false;
		}
		
		printer.function(function, database, nullptr);
	}
}

static void print_globals(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	CppPrinterConfig config;
	CppPrinter printer(out, config);
	
	bool first_iteration = true;
	SourceFileHandle source_file_handle;
	for(const GlobalVariable& global_variable : database.global_variables) {
		if(global_variable.source_file() != source_file_handle || first_iteration) {
			SourceFile* source_file = database.source_files.symbol_from_handle(global_variable.source_file());
			if(source_file) {
				printer.comment_block_file(source_file->full_path().c_str());
				source_file_handle = source_file->handle();
			} else {
				printer.comment_block_file("(unknown)");
				source_file_handle = SourceFileHandle();
			}
			first_iteration = false;
		}
		
		printer.global_variable(global_variable, database, nullptr);
	}
}

static void print_types(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	if((options.flags & FLAG_PER_FILE) == 0) {
		print_types_deduplicated(out, database, options);
	} else {
		print_types_per_file(out, database, options);
	}
}

static void print_types_deduplicated(FILE* out, SymbolDatabase& database, const Options& options)
{
	CppPrinterConfig config;
	CppPrinter printer(out, config);
	printer.comment_block_beginning(options.input_file.filename().string().c_str(), "stdump", get_version());
	printer.comment_block_toolchain_version_info(database);
	printer.comment_block_builtin_types(database);
	for(const DataType& data_type : database.data_types) {
		printer.data_type(data_type, database);
	}
}

static void print_types_per_file(FILE* out, SymbolDatabase& database, const Options& options)
{
	CppPrinterConfig config;
	CppPrinter printer(out, config);
	printer.comment_block_beginning(options.input_file.filename().string().c_str(), "stdump", get_version());
	
	for(const SourceFile& source_file : database.source_files) {
		printer.comment_block_file(source_file.full_path().c_str());
		printer.comment_block_toolchain_version_info(database);
		printer.comment_block_builtin_types(database, source_file.handle());
		for(const DataType& data_type : database.data_types) {
			if(data_type.files.size() == 1 && data_type.files[0] == source_file.handle()) {
				printer.data_type(data_type, database);
			}
		}
	}
}

static void print_type_graph(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	TypeDependencyAdjacencyList graph = build_type_dependency_graph(database);
	print_type_dependency_graph(out, database, graph);
}

static void print_labels(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	for(const Label& label : database.labels) {
		fprintf(out, "%08x %s\n", label.address().value, label.name().c_str());
	}
}

static void print_json(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	rapidjson::StringBuffer buffer;
	JsonWriter writer(buffer);
	write_json(writer, database);
	fprintf(out, "%s", buffer.GetString());
}

static void print_symbols(FILE* out, const Options& options)
{
	Result<std::vector<u8>> image = platform::read_binary_file(options.input_file);
	CCC_EXIT_IF_ERROR(image);
	
	Result<SymbolFile> symbol_file = parse_symbol_file(*image);
	CCC_EXIT_IF_ERROR(symbol_file);
	
	SymbolTableConfig config;
	config.section = options.section;
	config.format = options.format;
	
	bool print_locals = options.flags & FLAG_LOCAL_SYMBOLS;
	bool print_externals = options.flags & FLAG_EXTERNAL_SYMBOLS;
	if(!print_locals && ! print_externals) {
		print_locals = true;
		print_externals = true;
	}
	
	Result<void> print_result = print_symbols(out, *symbol_file, config, print_locals, print_externals);
	CCC_EXIT_IF_ERROR(print_result);
}

static void print_headers(FILE* out, const Options& options)
{
	Result<std::vector<u8>> image = platform::read_binary_file(options.input_file);
	CCC_EXIT_IF_ERROR(image);
	
	Result<SymbolFile> symbol_file = parse_symbol_file(*image);
	CCC_EXIT_IF_ERROR(symbol_file);
	
	SymbolTableConfig config;
	config.section = options.section;
	config.format = options.format;
	
	Result<void> print_result = print_headers(out, *symbol_file, config);
	CCC_EXIT_IF_ERROR(print_result);
}

static u32 command_line_flags_to_importer_flags(u32 flags)
{
	u32 importer_flags = NO_IMPORTER_FLAGS;
	if(flags & FLAG_PER_FILE) importer_flags |= DONT_DEDUPLICATE_TYPES;
	if(flags & FLAG_OMIT_ACCESS_SPECIFIERS) importer_flags |= NO_ACCESS_SPECIFIERS;
	if(flags & FLAG_OMIT_MEMBER_FUNCTIONS) importer_flags |= NO_MEMBER_FUNCTIONS;
	if(!(flags & FLAG_INCLUDE_GENERATED_FUNCTIONS)) importer_flags |= NO_GENERATED_MEMBER_FUNCTIONS;
	return importer_flags;
}

static void print_files(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	for(const SourceFile& source_file : database.source_files) {
		fprintf(out, "%s\n", source_file.name().c_str());
	}
}

static void print_sections(FILE* out, const Options& options)
{
	SymbolFile symbol_file;
	SymbolDatabase database = read_symbol_table(symbol_file, options);
	
	for(const Section& section : database.sections) {
		if(!section.address().valid()) {
			continue;
		}
		
		u32 section_start = section.address().value;
		u32 section_end = section.address().value + section.size;
		
		fprintf(out, "%s:\n", section.name().c_str());
		
		for(const SourceFile& source_file : database.source_files) {
			if(source_file.text_address.valid() && source_file.text_address >= section_start && source_file.text_address < section_end) {
				fprintf(out, "\t%s\n", source_file.full_path().c_str());
			}
		}
	}
}

static SymbolDatabase read_symbol_table(SymbolFile& symbol_file, const Options& options)
{
	Result<std::vector<u8>> image = platform::read_binary_file(options.input_file);
	CCC_EXIT_IF_ERROR(image);
	
	Result<SymbolFile> symbol_file_result = parse_symbol_file(std::move(*image));
	CCC_EXIT_IF_ERROR(symbol_file_result);
	symbol_file = std::move(*symbol_file_result);
	
	SymbolDatabase database;
	
	if(ElfFile* elf = std::get_if<ElfFile>(&symbol_file)) {
		Result<SymbolSourceHandle> sections_source = import_elf_section_headers(database, *elf);
		CCC_EXIT_IF_ERROR(sections_source);
	}
	
	SymbolTableConfig config;
	config.section = options.section;
	config.format = options.format;
	config.importer_flags = command_line_flags_to_importer_flags(options.flags);
	
	if((options.flags & FLAG_MANGLED) == 0) {
		config.demangler.cplus_demangle = cplus_demangle;
		config.demangler.cplus_demangle_opname = cplus_demangle_opname;
	}
	
	Result<SymbolSourceHandle> symbol_source = import_symbol_table(database, symbol_file, config);
	CCC_EXIT_IF_ERROR(symbol_source);
	
	return database;
}

static Options parse_command_line_arguments(int argc, char** argv)
{
	Options options;
	if(argc < 2) {
		return options;
	}
	
	const char* name = argv[1];
	bool require_input_path = false;
	for(const StdumpCommand& command : commands) {
		if(strcmp(name, command.name) == 0) {
			options.function = command.function;
			require_input_path = true;
			break;
		}
	}
	
	bool input_path_provided = false;
	for(s32 i = 2; i < argc; i++) {
		const char* arg = argv[i];
		if(strcmp(arg, "--per-file") == 0) {
			options.flags |= FLAG_PER_FILE;
		} else if(strcmp(arg, "--omit-access-specifiers") == 0) {
			options.flags |= FLAG_OMIT_ACCESS_SPECIFIERS;
		} else if(strcmp(arg, "--omit-member-functions") == 0) {
			options.flags |= FLAG_OMIT_MEMBER_FUNCTIONS;
		} else if(strcmp(arg, "--include-generated-functions") == 0) {
			options.flags |= FLAG_INCLUDE_GENERATED_FUNCTIONS;
		} else if(strcmp(arg, "--locals") == 0) {
			options.flags |= FLAG_LOCAL_SYMBOLS;
		} else if(strcmp(arg, "--externals") == 0) {
			options.flags |= FLAG_EXTERNAL_SYMBOLS;
		} else if(strcmp(arg, "--mangled") == 0) {
			options.flags |= FLAG_MANGLED;
		} else if(strcmp(arg, "--output") == 0) {
			if(i + 1 < argc) {
				options.output_file = argv[++i];
			} else {
				CCC_FATAL("No output path specified.");
			}
		} else if(strcmp(arg, "--section") == 0) {
			if(i + 1 < argc) {
				options.section = argv[++i];
			} else {
				CCC_FATAL("No section name specified.");
			}
		} else if(strcmp(arg, "--format") == 0) {
			if(i + 1 < argc) {
				std::string format = argv[++i];
				const SymbolTableFormatInfo* info = symbol_table_format_from_name(format.c_str());
				CCC_CHECK_FATAL(info, "Invalid symbol table format specified.");
				options.format = info->format;
			} else {
				CCC_FATAL("No section name specified.");
			}
		} else if(strncmp(arg, "--", 2) == 0) {
			CCC_FATAL("Unknown option '%s'.", arg);
		} else if(input_path_provided) {
			CCC_FATAL("Multiple input paths specified.");
		} else {
			options.input_file = argv[i];
			input_path_provided = true;
		}
	}
	
	CCC_CHECK_FATAL(!require_input_path || !options.input_file.empty(), "No input path specified.");
	
	return options;
}

static void print_help(FILE* out)
{
	fprintf(out, "stdump %s -- https://github.com/chaoticgd/ccc\n", get_version());
	fprintf(out, "  PS2 symbol table parser and dumper.\n");
	fprintf(out, "\n");
	fprintf(out, "Commands:\n");
	fprintf(out, "\n");
	for(const StdumpCommand& command : commands) {
		fprintf(out, "  %s [options] <input file>\n", command.name);
		for(const char* line : command.help_text) {
			fprintf(out, "    %s\n", line);
		}
		fprintf(out, "\n");
	}
	fprintf(out, "  help | --help | -h\n");
	fprintf(out, "    Print this help message.\n");
	fprintf(out, "\n");
	fprintf(out, "Options:\n");
	fprintf(out, "\n");
	fprintf(out, "  --output <output file>        Write the output to the file specified instead\n");
	fprintf(out, "                                of to the standard output.\n");
	
	s32 column;
	
	fprintf(out, "  --section <section name>      Choose which symbol table you want to read from.\n");
	const char* common_section_names_are = "Common section names are: ";
	fprintf(out, "                                %s", common_section_names_are);
	column = 32 + strlen(common_section_names_are);
	for(u32 i = 0; i < SYMBOL_TABLE_FORMAT_COUNT; i++) {
		const SymbolTableFormatInfo& format = SYMBOL_TABLE_FORMATS[i];
		if(column + strlen(format.section_name) + 2 > 80) {
			fprintf(out, "\n                                ");
			column = 32;
		}
		fprintf(out, "%s", format.section_name);
		if(i + 1 == SYMBOL_TABLE_FORMAT_COUNT) {
			fprintf(out, ".\n");
		} else {
			fprintf(out, ", ");
		}
		column += strlen(format.section_name) + 2;
	}
	
	fprintf(out, "  --format <format name>        Explicitly specify the symbol table format.\n");
	const char* possible_options_are = "Possible options are: ";
	fprintf(out, "                                %s", possible_options_are);
	column = 32 + strlen(possible_options_are);
	for(u32 i = 0; i < SYMBOL_TABLE_FORMAT_COUNT; i++) {
		const SymbolTableFormatInfo& format = SYMBOL_TABLE_FORMATS[i];
		if(column + strlen(format.format_name) + 2 > 80) {
			fprintf(out, "\n                                ");
			column = 32;
		}
		fprintf(out, "%s", format.format_name);
		if(i + 1 == SYMBOL_TABLE_FORMAT_COUNT) {
			fprintf(out, ".\n");
		} else {
			fprintf(out, ", ");
		}
		column += strlen(format.format_name) + 2;
	}
	fprintf(out, "  --mangled                     Don't demangle function names, global variable\n");
	fprintf(out, "                                names, or overloaded operator names.\n");
}

extern const char* git_tag;

static const char* get_version() {
	return (git_tag && strlen(git_tag) > 0) ? git_tag : "development version";
}
