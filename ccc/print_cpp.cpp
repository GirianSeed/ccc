#include "print_cpp.h"

#include <cmath>
#include <chrono>

namespace ccc {

enum VariableNamePrintFlags {
	NO_VAR_PRINT_FLAGS = 0,
	INSERT_SPACE_TO_LEFT = (1 << 0),
	BRACKETS_IF_POINTER = (1 << 2)
};

// print_cpp_comment_block_beginning
// print_cpp_comment_block_compiler_version_info
// print_cpp_comment_block_builtin_types
// print_cpp_ast_nodes
// print_cpp_ast_node
static void print_cpp_storage_class(FILE* dest, ast::StorageClass storage_class);
static void print_cpp_variable_name(FILE* dest, VariableName& name, u32 flags);
static void print_cpp_offset(FILE* dest, const ast::Node& node, s32 digits_for_offset);
// print_variable_storage_comment
static void indent(FILE* dest, s32 level);

void print_cpp_comment_block_beginning(FILE* dest, const fs::path& input_file) {
	fprintf(dest, "// File written by stdump");
	time_t cftime = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	tm* t = std::localtime(&cftime);
	if(t) {
		fprintf(dest, " on %04d-%02d-%02d", 1900 + t->tm_year, t->tm_mon + 1, t->tm_mday);
	}
	fprintf(dest, "\n// \n");
	fprintf(dest, "// Input file:\n");
	fprintf(dest, "//   %s\n", input_file.filename().string().c_str());
}

void print_cpp_comment_block_compiler_version_info(FILE* dest, const SymbolTable& symbol_table) {
	std::set<std::string> compiler_version_info;
	for(const SymFileDescriptor& fd : symbol_table.files) {
		bool known = false;
		for(const Symbol& symbol : fd.symbols) {
			if(symbol.storage_class == SymbolClass::COMPILER_VERSION_INFO && symbol.string != "@stabs") {
				known = true;
				compiler_version_info.emplace(symbol.string);
			}
		}
		if(!known) {
			compiler_version_info.emplace("unknown");
		}
	}
	
	fprintf(dest, "// Toolchain version(s):\n");
	for(const std::string& string : compiler_version_info) {
		fprintf(dest, "//   %s\n", string.c_str());
	}
}

void print_cpp_comment_block_builtin_types(FILE* dest, const std::vector<std::unique_ptr<ast::Node>>& ast_nodes) {
	std::set<std::pair<std::string, BuiltInClass>> builtins;
	for(const std::unique_ptr<ast::Node>& node : ast_nodes) {
		if(node->descriptor == ast::BUILTIN) {
			builtins.emplace(node->name, node->as<ast::BuiltIn>().bclass);
		}
	}
	
	if(!builtins.empty()) {
		fprintf(dest, "// Built-in types:\n");
		
		for(const auto& [type, bclass] : builtins) {
			fprintf(dest, "//   %-25s%s\n", type.c_str(), builtin_class_to_string(bclass));
		}
	}
}

void print_cpp_ast_nodes(FILE* dest, const std::vector<std::unique_ptr<ast::Node>>& nodes, bool verbose) {
	bool last_was_multiline = true;
	for(size_t i = 0; i < nodes.size(); i++) {
		const std::unique_ptr<ast::Node>& node = nodes[i];
		assert(node.get());
		if(node->descriptor == ast::BUILTIN) {
			continue;
		}
		bool multiline =
			node->descriptor == ast::INLINE_ENUM ||
			node->descriptor == ast::INLINE_STRUCT_OR_UNION;
		if(!last_was_multiline && multiline) {
			fprintf(dest, "\n");
		}
		if(node->compare_fail_reason) {
			fprintf(dest, "// warning: multiple differing types with the same name (%s not equal)\n", node->compare_fail_reason);
		}
		if(verbose && node->symbol != nullptr) {
			fprintf(dest, "// symbol: %s\n", node->symbol->raw->string.c_str());
		}
		VariableName name{nullptr};
		s32 digits_for_offset = 0;
		if(node->descriptor == ast::INLINE_STRUCT_OR_UNION && node->size_bits > 0) {
			digits_for_offset = (s32) ceilf(log2(node->size_bits / 8.f) / 4.f);
		}
		print_cpp_ast_node(stdout, *node.get(), name, 0, digits_for_offset);
		fprintf(dest, ";\n");
		if(multiline && i != nodes.size() - 1) {
			fprintf(dest, "\n");
		}
		last_was_multiline = multiline;
	}
}

void print_cpp_ast_node(FILE* dest, const ast::Node& node, VariableName& parent_name, s32 indentation_level, s32 digits_for_offset) {
	VariableName this_name{&node.name};
	VariableName& name = node.name.empty() ? parent_name : this_name;
	
	print_cpp_storage_class(dest, node.storage_class);
	
	switch(node.descriptor) {
		case ast::ARRAY: {
			const ast::Array& array = node.as<ast::Array>();
			assert(array.element_type.get());
			name.array_indices.emplace_back(array.element_count);
			print_cpp_ast_node(dest, *array.element_type.get(), name, indentation_level, digits_for_offset);
			break;
		}
		case ast::BITFIELD: {
			const ast::BitField& bit_field = node.as<ast::BitField>();
			assert(bit_field.underlying_type.get());
			print_cpp_ast_node(dest, *bit_field.underlying_type.get(), name, indentation_level, digits_for_offset);
			printf(" : %d", bit_field.size_bits);
			break;
		}
		case ast::BUILTIN: {
			const ast::BuiltIn& builtin = node.as<ast::BuiltIn>();
			if(builtin.bclass == BuiltInClass::VOID) {
				printf("void");
			} else {
				printf("CCC_BUILTIN(%s)", builtin_class_to_string(builtin.bclass));
			}
			print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			break;
		}
		case ast::FUNCTION: {
			const ast::Function& function = node.as<ast::Function>();
			if(function.modifier == MemberFunctionModifier::STATIC) {
				fprintf(dest, "static ");
			} else if(function.modifier == MemberFunctionModifier::VIRTUAL) {
				fprintf(dest, "virtual ");
			}
			if(!function.is_constructor) {
				assert(function.return_type.get());
				VariableName dummy{nullptr};
				print_cpp_ast_node(dest, *function.return_type.get(), dummy, indentation_level, digits_for_offset);
				fprintf(dest, " ");
			}
			print_cpp_variable_name(dest, name, BRACKETS_IF_POINTER);
			fprintf(dest, "(");
			if(function.parameters.has_value()) {
				for(size_t i = 0; i < function.parameters->size(); i++) {
					assert((*function.parameters)[i].get());
					VariableName dummy{nullptr};
					print_cpp_ast_node(dest, *(*function.parameters)[i].get(), dummy, indentation_level, digits_for_offset);
					if(i != function.parameters->size() - 1) {
						fprintf(dest, ", ");
					}
				}
			} else {
				fprintf(dest, "/* parameters unknown */");
			}
			fprintf(dest, ")");
			break;
		}
		case ast::INLINE_ENUM: {
			const ast::InlineEnum& inline_enum = node.as<ast::InlineEnum>();
			fprintf(dest, "enum");
			bool name_on_top = (indentation_level == 0) && (inline_enum.storage_class != ast::StorageClass::TYPEDEF);
			if(name_on_top) {
				print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			}
			fprintf(dest, " {");
			if(inline_enum.size_bits > -1) {
				printf(" // 0x%x", inline_enum.size_bits / 8);
			}
			fprintf(dest, "\n");
			for(size_t i = 0; i < inline_enum.constants.size(); i++) {
				s32 number = inline_enum.constants[i].first;
				const std::string& name = inline_enum.constants[i].second;
				bool is_last = i == inline_enum.constants.size() - 1;
				indent(dest, indentation_level + 1);
				fprintf(dest, "%s = %d%s\n", name.c_str(), number, is_last ? "" : ",");
			}
			indent(dest, indentation_level);
			fprintf(dest, "}");
			if(!name_on_top) {
				print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			}
			break;
		}
		case ast::INLINE_STRUCT_OR_UNION: {
			const ast::InlineStructOrUnion& struct_or_union = node.as<ast::InlineStructOrUnion>();
			if(struct_or_union.is_struct) {
				fprintf(dest, "struct");
			} else {
				fprintf(dest, "union");
			}
			bool name_on_top = (indentation_level == 0) && (struct_or_union.storage_class != ast::StorageClass::TYPEDEF);
			if(name_on_top) {
				print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			}
			// Print base classes.
			if(!struct_or_union.base_classes.empty()) {
				fprintf(dest, " :");
				for(const ast::BaseClass& base_class : struct_or_union.base_classes) {
					if(base_class.offset > -1) {
						fprintf(dest, " /* 0x%0*x */", digits_for_offset, base_class.offset);
					}
					fprintf(dest, " %s", base_class.type_name.c_str());
				}
			}
			fprintf(dest, " { // 0x%x\n", struct_or_union.size_bits / 8);
			// Print fields.
			for(const std::unique_ptr<ast::Node>& field : struct_or_union.fields) {
				assert(field.get());
				indent(dest, indentation_level + 1);
				print_cpp_offset(dest, *field.get(), digits_for_offset);
				print_cpp_ast_node(dest, *field.get(), name, indentation_level + 1, digits_for_offset);
				fprintf(dest, ";\n");
			}
			// Print member functions.
			if(!struct_or_union.member_functions.empty()) {
				if(!struct_or_union.fields.empty()) {
					indent(dest, indentation_level + 1);
					fprintf(dest, "\n");
				}
				for(size_t i = 0; i < struct_or_union.member_functions.size(); i++) {
					ast::Function& member_func = struct_or_union.member_functions[i]->as<ast::Function>();
					assert(struct_or_union.member_functions[i].get());
					indent(dest, indentation_level + 1);
					print_cpp_ast_node(dest, *struct_or_union.member_functions[i].get(), name, indentation_level + 1, digits_for_offset);
					fprintf(dest, ";\n");
				}
			}
			indent(dest, indentation_level);
			fprintf(dest, "}");
			if(!name_on_top) {
				print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			}
			break;
		}
		case ast::POINTER: {
			const ast::Pointer& pointer = node.as<ast::Pointer>();
			assert(pointer.value_type.get());
			name.pointer_chars.emplace_back('*');
			print_cpp_ast_node(dest, *pointer.value_type.get(), name, indentation_level, digits_for_offset);
			print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			break;
		}
		case ast::REFERENCE: {
			const ast::Reference& reference = node.as<ast::Reference>();
			assert(reference.value_type.get());
			name.pointer_chars.emplace_back('&');
			print_cpp_ast_node(dest, *reference.value_type.get(), name, indentation_level, digits_for_offset);
			print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			break;
		}
		case ast::TYPE_NAME: {
			const ast::TypeName& type_name = node.as<ast::TypeName>();
			fprintf(dest, "%s", type_name.type_name.c_str());
			print_cpp_variable_name(dest, name, INSERT_SPACE_TO_LEFT);
			break;
		}
	}
}

static void print_cpp_storage_class(FILE* dest, ast::StorageClass storage_class) {
	switch(storage_class) {
		case ast::StorageClass::NONE: break;
		case ast::StorageClass::TYPEDEF: fprintf(dest, "typedef "); break;
		case ast::StorageClass::EXTERN: fprintf(dest, "extern "); break;
		case ast::StorageClass::STATIC: fprintf(dest, "static "); break;
		case ast::StorageClass::AUTO: fprintf(dest, "auto "); break;
		case ast::StorageClass::REGISTER: fprintf(dest, "register "); break;
	}
}

static void print_cpp_variable_name(FILE* dest, VariableName& name, u32 flags) {
	bool has_name = name.identifier != nullptr && !name.identifier->empty();
	bool has_brackets = (flags & BRACKETS_IF_POINTER) && !name.pointer_chars.empty();
	if(has_name && (flags & INSERT_SPACE_TO_LEFT)) {
		fprintf(dest, " ");
	}
	if(has_brackets) {
		fprintf(dest, "(");
	}
	for(s32 i = (s32) name.pointer_chars.size() - 1; i >= 0; i--) {
		fprintf(dest, "%c", name.pointer_chars[i]);
	}
	name.pointer_chars.clear();
	if(has_name) {
		fprintf(dest, "%s", name.identifier->c_str());
		name.identifier = nullptr;
	}
	for(s32 index : name.array_indices) {
		fprintf(dest, "[%d]", index);
	}
	name.array_indices.clear();
	if(has_brackets) {
		fprintf(dest, ")");
	}
}

static void print_cpp_offset(FILE* dest, const ast::Node& node, s32 digits_for_offset) {
	if(node.storage_class != ast::StorageClass::STATIC && node.absolute_offset_bytes > -1) {
		assert(digits_for_offset > -1 && digits_for_offset < 100);
		fprintf(dest, "/* 0x%0*x", digits_for_offset, node.absolute_offset_bytes);
		if(node.bitfield_offset_bits > -1) {
			fprintf(dest, ":%d", node.bitfield_offset_bits);
		}
		fprintf(dest, " */ ");
	}
}

void print_variable_storage_comment(FILE* dest, const VariableStorage& storage) {
	fprintf(dest, "/* ");
	if(storage.location == VariableStorageLocation::REGISTER) {
		const char** name_table = mips::REGISTER_STRING_TABLES[(s32) storage.register_class];
		assert(storage.register_index_relative < mips::REGISTER_STRING_TABLE_SIZES[(s32) storage.register_class]);
		const char* register_name = name_table[storage.register_index_relative];
		fprintf(dest, "%s %d", register_name, storage.dbx_register_number);
	} else {
		if(storage.stack_pointer_offset >= 0) {
			fprintf(dest, "0x%x(sp)", storage.stack_pointer_offset);
		} else {
			fprintf(dest, "-0x%x(sp)", -storage.stack_pointer_offset);
		}
	}
	fprintf(dest, " */ ");
}

static void indent(FILE* dest, s32 level) {
	for(s32 i = 0; i < level; i++) {
		fputc('\t', dest);
	}
}

}