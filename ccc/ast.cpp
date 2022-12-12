#include "ast.h"

namespace ccc::ast {

std::set<std::pair<std::string, RangeClass>> symbols_to_builtins(const std::vector<StabsSymbol>& symbols) {
	std::set<std::pair<std::string, RangeClass>> builtins;
	for(const StabsSymbol& symbol : symbols) {
		if(is_data_type(symbol) && is_builtin_type(symbol)) {
			builtins.emplace(symbol.name, symbol.type.range_type.range_class);
		}
	}
	return builtins;
}

std::vector<std::unique_ptr<ast::Node>> symbols_to_ast(const std::vector<StabsSymbol>& symbols, const std::map<s32, const StabsType*>& stabs_types) {
	std::vector<std::unique_ptr<ast::Node>> ast_nodes;
	for(const StabsSymbol& symbol : symbols) {
		if(is_data_type(symbol) && symbol.name != "void") {
			std::unique_ptr<ast::Node> node = ast::stabs_symbol_to_ast(symbol, stabs_types);
			if(node != nullptr) {
				ast_nodes.emplace_back(std::move(node));
			}
		}
	}
	return ast_nodes;
}

bool is_data_type(const StabsSymbol& symbol) {
	return symbol.mdebug_symbol.storage_type == SymbolType::NIL
		&& (u32) symbol.mdebug_symbol.storage_class == 0
		&& (symbol.descriptor == StabsSymbolDescriptor::ENUM_STRUCT_OR_TYPE_TAG
			|| symbol.descriptor == StabsSymbolDescriptor::TYPE_NAME);
}


bool is_builtin_type(const StabsSymbol& symbol) {
	return (symbol.type.descriptor == StabsTypeDescriptor::RANGE
		&& symbol.type.range_type.range_class != RangeClass::UNKNOWN_PROBABLY_ARRAY);
}

std::unique_ptr<Node> stabs_symbol_to_ast(const StabsSymbol& symbol, const std::map<s32, const StabsType*>& stabs_types) {
	if(!symbol.type.has_body) {
		auto node = std::make_unique<TypeName>();
		node->type_name = symbol.name;
		return node;
	}
	
	try {
		auto node = stabs_type_to_ast(symbol.type, stabs_types, 0, 0);
		if(node != nullptr) {
			node->name = (symbol.name == " ") ? "" : symbol.name;
			node->symbol = &symbol;
			if(symbol.descriptor == StabsSymbolDescriptor::TYPE_NAME) {
				node->storage_class = StorageClass::TYPEDEF;
			}
		}
		return node;
	} catch(std::runtime_error& e) {
		auto error = std::make_unique<ast::TypeName>();
		error->type_name = e.what();
		return error;
	}
}

std::unique_ptr<Node> stabs_type_to_ast(const StabsType& type, const std::map<s32, const StabsType*>& stabs_types, s32 absolute_parent_offset_bytes, s32 depth) {
	if(depth > 1000) {
		throw std::runtime_error("CCC_BADRECURSION");
	}
	
	// This makes sure that if types are referenced by their number, their name
	// is shown instead their entire contents.
	if(depth > 0 && type.name.has_value() && type.name != "" && type.name != " ") {
		auto type_name = std::make_unique<ast::TypeName>();
		type_name->type_name = *type.name;
		return type_name;
	}
	
	if(!type.has_body) {
		auto stabs_type = stabs_types.find(type.type_number);
		if(type.anonymous || stabs_type == stabs_types.end() || !stabs_type->second || !stabs_type->second->has_body) {
			auto type_name = std::make_unique<ast::TypeName>();
			type_name->type_name = stringf("CCC_BADTYPELOOKUP(%d)", type.type_number);
			return type_name;
		}
		return stabs_type_to_ast(*stabs_type->second, stabs_types, absolute_parent_offset_bytes, depth + 1);
	}
	
	std::unique_ptr<Node> result;
	
	switch(type.descriptor) {
		case StabsTypeDescriptor::TYPE_REFERENCE: {
			assert(type.type_reference.type.get());
			result = stabs_type_to_ast(*type.type_reference.type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(result == nullptr) {
				return nullptr;
			}
			break;
		}
		case StabsTypeDescriptor::ARRAY: {
			auto array = std::make_unique<ast::Array>();
			assert(type.array_type.element_type.get());
			array->element_type = stabs_type_to_ast(*type.array_type.element_type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(array->element_type == nullptr) {
				return nullptr;
			}
			const StabsType* index = type.array_type.index_type.get();
			// The low and high values are not wrong in this case.
			verify(index && index->descriptor == StabsTypeDescriptor::RANGE && index->range_type.low_maybe_wrong == 0,
				"Invalid index type for array.");
			array->element_count = index->range_type.high_maybe_wrong + 1;
			result = std::move(array);
			break;
		}
		case StabsTypeDescriptor::ENUM: {
			auto inline_enum = std::make_unique<ast::InlineEnum>();
			inline_enum->constants = type.enum_type.fields;
			result = std::move(inline_enum);
			break;
		}
		case StabsTypeDescriptor::FUNCTION: {
			auto function = std::make_unique<ast::Function>();
			assert(type.function_type.type.get());
			function->return_type = stabs_type_to_ast(*type.function_type.type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(function->return_type == nullptr) {
				return nullptr;
			}
			result = std::move(function);
			break;
		}
		case StabsTypeDescriptor::RANGE: {
			if(depth >= 2) {
				auto type_name = std::make_unique<ast::TypeName>();
				if(type.name.has_value()) {
					type_name->type_name = *type.name;
				} else {
					type_name->type_name = "CCC_RANGE";
				}
				return type_name;
			} else {
				return nullptr;
			}
		}
		case StabsTypeDescriptor::STRUCT: {
			auto inline_struct = std::make_unique<ast::InlineStructOrUnion>();
			inline_struct->is_union = false;
			inline_struct->size_bits = (s32) type.struct_or_union.size * 8;
			for(const StabsBaseClass& stabs_base_class : type.struct_or_union.base_classes) {
				ast::BaseClass& ast_base_class = inline_struct->base_classes.emplace_back();
				ast_base_class.visibility = stabs_base_class.visibility;
				ast_base_class.offset = stabs_base_class.offset;
				auto base_class_type = stabs_type_to_ast(stabs_base_class.type, stabs_types, absolute_parent_offset_bytes, depth + 1);
				if(base_class_type == nullptr) {
					return nullptr;
				}
				assert(base_class_type->descriptor == TYPE_NAME);
				ast_base_class.type_name = base_class_type->as<TypeName>().type_name;
			}
			for(const StabsField& field : type.struct_or_union.fields) {
				inline_struct->fields.emplace_back(stabs_field_to_ast(field, stabs_types, absolute_parent_offset_bytes, depth + 1));
			}
			for(const StabsMemberFunctionSet& function_set : type.struct_or_union.member_functions) {
				for(const StabsMemberFunctionOverload& overload : function_set.overloads) {
					auto node = stabs_type_to_ast(overload.type, stabs_types, absolute_parent_offset_bytes, depth + 1);
					if(node == nullptr) {
						return nullptr;
					}
					node->name = function_set.name;
					inline_struct->member_functions.emplace_back(std::move(node));
				}
			}
			result = std::move(inline_struct);
			break;
		}
		case StabsTypeDescriptor::UNION: {
			auto inline_union = std::make_unique<ast::InlineStructOrUnion>();
			inline_union->is_union = true;
			inline_union->size_bits = (s32) type.struct_or_union.size * 8;
			for(const StabsField& field : type.struct_or_union.fields) {
				auto node = stabs_field_to_ast(field, stabs_types, absolute_parent_offset_bytes, depth + 1);
				if(node == nullptr) {
					return nullptr;
				}
				inline_union->fields.emplace_back(std::move(node));
			}
			for(const StabsMemberFunctionSet& function_set : type.struct_or_union.member_functions) {
				for(const StabsMemberFunctionOverload& overload : function_set.overloads) {
					auto node = stabs_type_to_ast(overload.type, stabs_types, absolute_parent_offset_bytes, depth + 1);
					if(node == nullptr) {
						return nullptr;
					}
					node->name = function_set.name;
					inline_union->member_functions.emplace_back(std::move(node));
				}
			}
			result = std::move(inline_union);
			break;
		}
		case StabsTypeDescriptor::CROSS_REFERENCE: {
			auto type_name = std::make_unique<ast::TypeName>();
			type_name->type_name = type.cross_reference.identifier;
			result = std::move(type_name);
			break;
		}
		case StabsTypeDescriptor::METHOD: {
			assert(type.method.return_type.get());
			auto function = std::make_unique<ast::Function>();
			function->return_type = stabs_type_to_ast(*type.method.return_type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(function->return_type == nullptr) {
				return nullptr;
			}
			function->parameters.emplace();
			for(const StabsType& parameter_type : type.method.parameter_types) {
				auto node = stabs_type_to_ast(parameter_type, stabs_types, absolute_parent_offset_bytes, depth + 1);
				if(node == nullptr) {
					return nullptr;
				}
				function->parameters->emplace_back(std::move(node));
			}
			result = std::move(function);
			break;
		}
		case StabsTypeDescriptor::POINTER: {
			auto pointer = std::make_unique<ast::Pointer>();
			assert(type.reference_or_pointer.value_type.get());
			pointer->value_type = stabs_type_to_ast(*type.reference_or_pointer.value_type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(pointer->value_type == nullptr) {
				return nullptr;
			}
			result = std::move(pointer);
			break;
		}
		case StabsTypeDescriptor::REFERENCE: {
			auto reference = std::make_unique<ast::Reference>();
			assert(type.reference_or_pointer.value_type.get());
			reference->value_type = stabs_type_to_ast(*type.reference_or_pointer.value_type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(reference->value_type == nullptr) {
				return nullptr;
			}
			result = std::move(reference);
			break;
		}
		case StabsTypeDescriptor::TYPE_ATTRIBUTE: {
			assert(type.size_type_attribute.type.get());
			result = stabs_type_to_ast(*type.size_type_attribute.type.get(), stabs_types, absolute_parent_offset_bytes, depth + 1);
			if(result == nullptr) {
				return nullptr;
			}
			result->size_bits = type.size_type_attribute.size_bits;
			break;
		}
		case StabsTypeDescriptor::BUILT_IN: {
			if(depth >= 2) {
				auto type_name = std::make_unique<ast::TypeName>();
				type_name->type_name = "CCC_BUILTIN";
				return type_name;
			} else {
				return nullptr;
			}
		}
	}
	
	if(result == nullptr) {
		auto bad = std::make_unique<ast::TypeName>();
		bad->type_name = "CCC_BADTYPEINFO";
		return bad;
	}
	
	return result;
}

std::unique_ptr<Node> stabs_field_to_ast(const StabsField& field, const std::map<s32, const StabsType*>& stabs_types, s32 absolute_parent_offset_bytes, s32 depth) {
	// Bitfields
	if(field.offset_bits % 8 != 0 || field.size_bits % 8 != 0) {
		std::unique_ptr<BitField> bitfield = std::make_unique<BitField>();
		bitfield->name = (field.name == " ") ? "" : field.name;
		bitfield->relative_offset_bytes = field.offset_bits / 8;
		bitfield->absolute_offset_bytes = absolute_parent_offset_bytes + bitfield->relative_offset_bytes;
		bitfield->size_bits = field.size_bits;
		bitfield->underlying_type = stabs_type_to_ast(field.type, stabs_types, bitfield->absolute_offset_bytes, depth + 1);
		bitfield->bitfield_offset_bits = field.offset_bits % 8;
		if(field.is_static) {
			bitfield->storage_class = ast::StorageClass::STATIC;
		}
		return bitfield;
	}
	
	// Normal fields
	s32 relative_offset_bytes = field.offset_bits / 8;
	s32 absolute_offset_bytes = absolute_parent_offset_bytes + relative_offset_bytes;
	std::unique_ptr<Node> child = stabs_type_to_ast(field.type, stabs_types, absolute_offset_bytes, depth + 1);
	child->name = (field.name == " ") ? "" : field.name;
	child->relative_offset_bytes = relative_offset_bytes;
	child->absolute_offset_bytes = absolute_offset_bytes;
	child->size_bits = field.size_bits;
	if(field.is_static) {
		child->storage_class = ast::StorageClass::STATIC;
	}
	return child;
}

// Some enums have two symbols associated with them: One named " " and another
// one referencing the first.
void remove_duplicate_enums(std::vector<std::unique_ptr<Node>>& ast_nodes) {
	for(size_t i = 0; i < ast_nodes.size(); i++) {
		Node& node = *ast_nodes[i].get();
		if(node.descriptor == NodeDescriptor::INLINE_ENUM && (node.name == "" || node.name == " ")) {
			bool match = false;
			for(std::unique_ptr<Node>& other : ast_nodes) {
				bool is_match = other.get() != &node
					&& other->descriptor == NodeDescriptor::INLINE_ENUM
					&& (other->name != "" && other->name != " ")
					&& other->as<InlineEnum>().constants == node.as<InlineEnum>().constants;
				if(is_match) {
					match = true;
					break;
				}
			}
			if(match) {
				ast_nodes.erase(ast_nodes.begin() + i);
				i--;
			}
		}
	}
}

std::vector<std::vector<std::unique_ptr<Node>>> deduplicate_ast(std::vector<std::pair<std::string, std::vector<std::unique_ptr<ast::Node>>>>& per_file_ast) {
	std::vector<std::vector<std::unique_ptr<Node>>> deduplicated_nodes;
	std::map<std::string, size_t> name_to_deduplicated_index;
	for(auto& [file_name, ast_nodes] : per_file_ast) {
		for(std::unique_ptr<Node>& node : ast_nodes) {
			auto existing_node_index = name_to_deduplicated_index.find(node->name);
			if(existing_node_index == name_to_deduplicated_index.end()) {
				std::string name = node->name;
				size_t index = deduplicated_nodes.size();
				deduplicated_nodes.emplace_back().emplace_back(std::move(node));
				name_to_deduplicated_index[name] = index;
			} else {
				std::vector<std::unique_ptr<Node>>& existing_nodes = deduplicated_nodes[existing_node_index->second];
				bool match = false;
				for(std::unique_ptr<Node>& existing_node : existing_nodes) {
					auto compare_result = compare_ast_nodes(*existing_node.get(), *node.get());
					if(compare_result.has_value()) {
						bool is_anonymous_enum = existing_node->descriptor == INLINE_ENUM
							&& existing_node->name.empty();
						if(!is_anonymous_enum) {
							existing_node->compare_fail_reason = compare_fail_reason_to_string(*compare_result);
							node->compare_fail_reason = compare_fail_reason_to_string(*compare_result);
						}
					} else {
						match = true;
					}
				}
				if(!match) {
					existing_nodes.emplace_back(std::move(node));
				}
			}
		}
	}
	return deduplicated_nodes;
}

static std::optional<CompareFailReason> compare_ast_nodes(const ast::Node& lhs, const ast::Node& rhs) {
	if(lhs.descriptor != rhs.descriptor) return CompareFailReason::DESCRIPTOR;
	if(lhs.storage_class != rhs.storage_class) return CompareFailReason::STORAGE_CLASS;
	if(lhs.name != rhs.name) return CompareFailReason::NAME;
	if(lhs.relative_offset_bytes != rhs.relative_offset_bytes) return CompareFailReason::RELATIVE_OFFSET_BYTES;
	if(lhs.absolute_offset_bytes != rhs.absolute_offset_bytes) return CompareFailReason::ABSOLUTE_OFFSET_BYTES;
	if(lhs.bitfield_offset_bits != rhs.bitfield_offset_bits) return CompareFailReason::BITFIELD_OFFSET_BITS;
	if(lhs.size_bits != rhs.size_bits) return CompareFailReason::SIZE_BITS;
	switch(lhs.descriptor) {
		case ARRAY: {
			const Array& array_lhs = lhs.as<Array>();
			const Array& array_rhs = rhs.as<Array>();
			auto element_compare = compare_ast_nodes(*array_lhs.element_type.get(), *array_rhs.element_type.get());
			if(element_compare.has_value()) return element_compare;
			if(array_lhs.element_count != array_rhs.element_count) return CompareFailReason::ARRAY_ELEMENT_COUNT;
			break;
		}
		case BITFIELD: {
			const BitField& bitfield_lhs = lhs.as<BitField>();
			const BitField& bitfield_rhs = rhs.as<BitField>();
			auto bitfield_compare = compare_ast_nodes(*bitfield_lhs.underlying_type.get(), *bitfield_rhs.underlying_type.get());
			if(bitfield_compare.has_value()) return bitfield_compare;
			break;
		}
		case FUNCTION: {
			const Function& function_lhs = lhs.as<Function>();
			const Function& function_rhs = rhs.as<Function>();
			auto return_compare = compare_ast_nodes(*function_lhs.return_type.get(), *function_rhs.return_type.get());
			if(return_compare.has_value()) return return_compare;
			if(function_lhs.parameters.has_value() && function_rhs.parameters.has_value()) {
				if(function_lhs.parameters->size() != function_rhs.parameters->size()) return CompareFailReason::FUNCTION_PARAMAETER_SIZE;
				for(size_t i = 0; i < function_lhs.parameters->size(); i++) {
					auto parameter_compare = compare_ast_nodes(*(*function_lhs.parameters)[i].get(), *(*function_rhs.parameters)[i].get());
					if(parameter_compare.has_value()) return parameter_compare;
				}
			} else if(function_lhs.parameters.has_value() != function_rhs.parameters.has_value()) {
				return CompareFailReason::FUNCTION_PARAMETERS_HAS_VALUE;
			}
			break;
		}
		case INLINE_ENUM: {
			const InlineEnum& enum_lhs = lhs.as<InlineEnum>();
			const InlineEnum& enum_rhs = rhs.as<InlineEnum>();
			if(enum_lhs.constants != enum_rhs.constants) return CompareFailReason::ENUM_CONSTANTS;
			break;
		}
		case INLINE_STRUCT_OR_UNION: {
			const InlineStructOrUnion& struct_lhs = lhs.as<InlineStructOrUnion>();
			const InlineStructOrUnion& struct_rhs = rhs.as<InlineStructOrUnion>();
			if(struct_lhs.base_classes.size() != struct_rhs.base_classes.size()) return CompareFailReason::BASE_CLASS_SIZE;
			for(size_t i = 0; i < struct_lhs.base_classes.size(); i++) {
				const BaseClass& base_class_lhs = struct_lhs.base_classes[i];
				const BaseClass& base_class_rhs = struct_rhs.base_classes[i];
				if(base_class_lhs.visibility != base_class_rhs.visibility) return CompareFailReason::BASE_CLASS_VISIBILITY;
				if(base_class_lhs.offset != base_class_rhs.offset) return CompareFailReason::BASE_CLASS_OFFSET;
				if(base_class_lhs.type_name != base_class_rhs.type_name) return CompareFailReason::BASE_CLASS_TYPE_NAME;
			}
			if(struct_lhs.fields.size() != struct_rhs.fields.size()) return CompareFailReason::FIELDS_SIZE;
			for(size_t i = 0; i < struct_lhs.fields.size(); i++) {
				auto field_compare = compare_ast_nodes(*struct_lhs.fields[i].get(), *struct_rhs.fields[i].get());
				if(field_compare.has_value()) return field_compare;
			}
			if(struct_lhs.member_functions.size() != struct_rhs.member_functions.size()) return CompareFailReason::MEMBER_FUNCTION_SIZE;
			for(size_t i = 0; i < struct_lhs.member_functions.size(); i++) {
				auto member_function_compare = compare_ast_nodes(*struct_lhs.member_functions[i].get(), *struct_rhs.member_functions[i].get());
				if(member_function_compare.has_value()) return member_function_compare;
			}
			break;
		}
		case POINTER: {
			const Pointer& pointer_lhs = lhs.as<Pointer>();
			const Pointer& pointer_rhs = rhs.as<Pointer>();
			auto pointer_compare = compare_ast_nodes(*pointer_lhs.value_type.get(), *pointer_rhs.value_type.get());
			if(pointer_compare.has_value()) return pointer_compare;
			break;
		}
		case REFERENCE: {
			const Reference& reference_lhs = lhs.as<Reference>();
			const Reference& reference_rhs = rhs.as<Reference>();
			auto reference_compare = compare_ast_nodes(*reference_lhs.value_type.get(), *reference_rhs.value_type.get());
			if(reference_compare.has_value()) return reference_compare;
			break;
		}
		case TYPE_NAME: {
			const TypeName& typename_lhs = lhs.as<TypeName>();
			const TypeName& typename_rhs = rhs.as<TypeName>();
			if(typename_lhs.type_name != typename_rhs.type_name) return CompareFailReason::TYPE_NAME;
			break;
		}
	}
	return std::nullopt;
}

static const char* compare_fail_reason_to_string(CompareFailReason reason) {
	switch(reason) {
		case CompareFailReason::DESCRIPTOR: return "descriptors";
		case CompareFailReason::STORAGE_CLASS: return "storage classes";
		case CompareFailReason::NAME: return "names";
		case CompareFailReason::RELATIVE_OFFSET_BYTES: return "relative offsets";
		case CompareFailReason::ABSOLUTE_OFFSET_BYTES: return "absolute offsets";
		case CompareFailReason::BITFIELD_OFFSET_BITS: return "bitfield offsets";
		case CompareFailReason::SIZE_BITS: return "sizes";
		case CompareFailReason::ARRAY_ELEMENT_COUNT: return "array element counts";
		case CompareFailReason::FUNCTION_PARAMAETER_SIZE: return "function paramaeter sizes";
		case CompareFailReason::FUNCTION_PARAMETERS_HAS_VALUE: return "function parameters";
		case CompareFailReason::ENUM_CONSTANTS: return "enum constants";
		case CompareFailReason::BASE_CLASS_SIZE: return "base class sizes";
		case CompareFailReason::BASE_CLASS_VISIBILITY: return "base class visibility values";
		case CompareFailReason::BASE_CLASS_OFFSET: return "base class offsets";
		case CompareFailReason::BASE_CLASS_TYPE_NAME: return "base class type names";
		case CompareFailReason::FIELDS_SIZE: return "fields sizes";
		case CompareFailReason::MEMBER_FUNCTION_SIZE: return "member function sizes";
		case CompareFailReason::TYPE_NAME: return "type name";
	}
	return "";
}

}
