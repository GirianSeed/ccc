// This file is part of the Chaos Compiler Collection.
// SPDX-License-Identifier: MIT

#include "stabs.h"

namespace ccc {

#define STABS_DEBUG(...) //__VA_ARGS__
#define STABS_DEBUG_PRINTF(...) STABS_DEBUG(printf(__VA_ARGS__);)

static bool validate_symbol_descriptor(StabsSymbolDescriptor descriptor);
static Result<std::unique_ptr<StabsType>> parse_stabs_type(const char*& input);
static Result<std::vector<StabsStructOrUnionType::Field>> parse_field_list(const char*& input);
static Result<std::vector<StabsStructOrUnionType::MemberFunctionSet>> parse_member_functions(const char*& input);
STABS_DEBUG(static void print_field(const StabsStructOrUnionType::Field& field);)

const char* STAB_TRUNCATED_ERROR_MESSAGE =
	"STABS symbol truncated. This was probably caused by a GCC bug. "
	"Other symbols from the same translation unit may be invalid.";

Result<StabsSymbol> parse_stabs_symbol(const char* input) {
	STABS_DEBUG_PRINTF("PARSING %s\n", input);
	
	StabsSymbol symbol;
	
	Result<std::string> name = eat_dodgy_stabs_identifier(input);
	CCC_RETURN_IF_ERROR(name);
	
	symbol.name = *name;
	
	CCC_EXPECT_CHAR(input, ':', "identifier");
	CCC_CHECK(*input != '\0', "Unexpected end of input.");
	if((*input >= '0' && *input <= '9') || *input == '(') {
		symbol.descriptor = StabsSymbolDescriptor::LOCAL_VARIABLE;
	} else {
		std::optional<char> symbol_descriptor = eat_char(input);
		CCC_CHECK(symbol_descriptor.has_value(), "Cannot parse symbol descriptor.");
		symbol.descriptor = (StabsSymbolDescriptor) *symbol_descriptor;
	}
	CCC_CHECK(validate_symbol_descriptor(symbol.descriptor),
		"Invalid symbol descriptor '%c'.",
		(char) symbol.descriptor);
	CCC_CHECK(*input != '\0', "Unexpected end of input.");
	if(*input == 't') {
		input++;
	}
	
	auto type = parse_top_level_stabs_type(input);
	CCC_RETURN_IF_ERROR(type);
	
	// Handle nested functions.
	bool is_function =
		symbol.descriptor == StabsSymbolDescriptor::LOCAL_FUNCTION ||
		symbol.descriptor == StabsSymbolDescriptor::GLOBAL_FUNCTION;
	if(is_function && input[0] == ',') {
		input++;
		while(*input != ',' && *input != '\0') input++; // enclosing function
		CCC_EXPECT_CHAR(input, ',', "nested function suffix");
		while(*input != ',' && *input != '\0') input++; // function
	}
	
	CCC_CHECK(*input == '\0', "Unknown data '%s' at the end of the '%s' stab.", input, name->c_str());
	symbol.type = std::move(*type);
	
	// Make sure that variable names aren't used as type names e.g. the STABS
	// symbol "somevar:P123=*456" may be referenced by the type number 123, but
	// the type name is not "somevar".
	bool is_type = symbol.descriptor == StabsSymbolDescriptor::TYPE_NAME
		|| symbol.descriptor == StabsSymbolDescriptor::ENUM_STRUCT_OR_TYPE_TAG; 
	if(is_type) {
		symbol.type->name = symbol.name;
	}
	
	symbol.type->is_typedef = symbol.descriptor == StabsSymbolDescriptor::TYPE_NAME;
	symbol.type->is_root = true;
	
	return symbol;
}

static bool validate_symbol_descriptor(StabsSymbolDescriptor descriptor) {
	bool valid;
	switch(descriptor) {
		case StabsSymbolDescriptor::LOCAL_VARIABLE:
		case StabsSymbolDescriptor::REFERENCE_PARAMETER_A:
		case StabsSymbolDescriptor::LOCAL_FUNCTION:
		case StabsSymbolDescriptor::GLOBAL_FUNCTION:
		case StabsSymbolDescriptor::GLOBAL_VARIABLE:
		case StabsSymbolDescriptor::REGISTER_PARAMETER:
		case StabsSymbolDescriptor::VALUE_PARAMETER:
		case StabsSymbolDescriptor::REGISTER_VARIABLE:
		case StabsSymbolDescriptor::STATIC_GLOBAL_VARIABLE:
		case StabsSymbolDescriptor::TYPE_NAME:
		case StabsSymbolDescriptor::ENUM_STRUCT_OR_TYPE_TAG:
		case StabsSymbolDescriptor::STATIC_LOCAL_VARIABLE:
		case StabsSymbolDescriptor::REFERENCE_PARAMETER_V:
			valid = true;
			break;
		default:
			valid = false;
			break;
	}
	return valid;
}

Result<std::unique_ptr<StabsType>> parse_top_level_stabs_type(const char*& input) {
	Result<std::unique_ptr<StabsType>> type = parse_stabs_type(input);
	CCC_RETURN_IF_ERROR(type);
	
	// Handle first base class suffixes.
	if((*type)->descriptor == StabsTypeDescriptor::STRUCT && input[0] == '~' && input[1] == '%') {
		input += 2;
		
		Result<std::unique_ptr<StabsType>> first_base_class = parse_stabs_type(input);
		CCC_RETURN_IF_ERROR(first_base_class);
		(*type)->as<StabsStructType>().first_base_class = std::move(*first_base_class);
		
		CCC_EXPECT_CHAR(input, ';', "first base class suffix");
	}
	
	// Handle extra live range information.
	if(input[0] == ';' && input[1] == 'l') {
		input += 2;
		CCC_EXPECT_CHAR(input, '(', "live range suffix");
		CCC_EXPECT_CHAR(input, '#', "live range suffix");
		std::optional<s32> start = eat_s32_literal(input);
		CCC_CHECK(start.has_value(), "Failed to parse live range suffix.");
		CCC_EXPECT_CHAR(input, ',', "live range suffix");
		CCC_EXPECT_CHAR(input, '#', "live range suffix");
		std::optional<s32> end = eat_s32_literal(input);
		CCC_CHECK(end.has_value(), "Failed to parse live range suffix.");
		CCC_EXPECT_CHAR(input, ')', "live range suffix");
	}
	
	return type;
}

static Result<std::unique_ptr<StabsType>> parse_stabs_type(const char*& input) {
	StabsTypeInfo info;
	CCC_CHECK(*input != '\0', "Unexpected end of input.");
	if(*input == '(') {
		// This file has type numbers made up of two pieces: an include file
		// index and a type number.
		
		input++;
		
		std::optional<s32> file_number = eat_s32_literal(input);
		CCC_CHECK(file_number.has_value(), "Cannot parse file number.");
		
		CCC_EXPECT_CHAR(input, ',', "type number");
		
		std::optional<s32> type_number = eat_s32_literal(input);
		CCC_CHECK(type_number.has_value(), "Cannot parse type number.");
		
		CCC_EXPECT_CHAR(input, ')', "type number");
		
		info.anonymous = false;
		info.type_number.file = *file_number;
		info.type_number.type = *type_number;
		if(*input != '=') {
			info.has_body = false;
			return std::make_unique<StabsType>(info);
		}
		input++;
	} else if(*input >= '0' && *input <= '9') {
		// This file has type numbers which are just a single number. This is
		// the more common case for games.
		
		info.anonymous = false;
		
		std::optional<s32> type_number = eat_s32_literal(input);
		CCC_CHECK(type_number.has_value(), "Cannot parse type number.");
		info.type_number.type = *type_number;
		
		if(*input != '=') {
			info.has_body = false;
			return std::make_unique<StabsType>(info);
		}
		input++;
	} else {
		info.anonymous = true;
	}
	info.has_body = true;
	
	CCC_CHECK(*input != '\0', "Unexpected end of input.");
	
	StabsTypeDescriptor descriptor;
	if((*input >= '0' && *input <= '9') || *input == '(') {
		descriptor = StabsTypeDescriptor::TYPE_REFERENCE;
	} else {
		std::optional<char> descriptor_char = eat_char(input);
		CCC_CHECK(descriptor_char.has_value(), "Cannot parse type descriptor.");
		descriptor = (StabsTypeDescriptor) *descriptor_char;
	}
	
	std::unique_ptr<StabsType> out_type;
	
	switch(descriptor) {
		case StabsTypeDescriptor::TYPE_REFERENCE: { // 0..9
			auto type_reference = std::make_unique<StabsTypeReferenceType>(info);
			
			auto type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(type);
			type_reference->type = std::move(*type);
			
			out_type = std::move(type_reference);
			break;
		}
		case StabsTypeDescriptor::ARRAY: { // a
			auto array = std::make_unique<StabsArrayType>(info);
			
			auto index_type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(index_type);
			array->index_type = std::move(*index_type);
			
			auto element_type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(element_type);
			array->element_type = std::move(*element_type);
			
			out_type = std::move(array);
			break;
		}
		case StabsTypeDescriptor::ENUM: { // e
			auto enum_type = std::make_unique<StabsEnumType>(info);
			STABS_DEBUG_PRINTF("enum {\n");
			while(*input != ';') {
				std::optional<std::string> name = eat_stabs_identifier(input);
				CCC_CHECK(name.has_value(), "Cannot parse enum field name.");
				
				CCC_EXPECT_CHAR(input, ':', "enum");
				
				std::optional<s32> value = eat_s32_literal(input);
				CCC_CHECK(value.has_value(), "Cannot parse enum value.");
				
				enum_type->fields.emplace_back(*value, std::move(*name));
				
				CCC_EXPECT_CHAR(input, ',', "enum");
			}
			input++;
			STABS_DEBUG_PRINTF("}\n");
			
			out_type = std::move(enum_type);
			break;
		}
		case StabsTypeDescriptor::FUNCTION: { // f
			auto function = std::make_unique<StabsFunctionType>(info);
			
			auto return_type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(return_type);
			function->return_type = std::move(*return_type);
			
			out_type = std::move(function);
			break;
		}
		case StabsTypeDescriptor::VOLATILE_QUALIFIER: { // k
			auto volatile_qualifier = std::make_unique<StabsVolatileQualifierType>(info);
			
			auto type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(type);
			volatile_qualifier->type = std::move(*type);
			
			out_type = std::move(volatile_qualifier);
			break;
		}
		case StabsTypeDescriptor::CONST_QUALIFIER: { // k
			auto const_qualifier = std::make_unique<StabsConstQualifierType>(info);
			
			auto type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(type);
			const_qualifier->type = std::move(*type);
			
			out_type = std::move(const_qualifier);
			break;
		}
		case StabsTypeDescriptor::RANGE: { // r
			auto range = std::make_unique<StabsRangeType>(info);
			
			auto type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(type);
			range->type = std::move(*type);
			
			CCC_EXPECT_CHAR(input, ';', "range type descriptor");
			
			std::optional<std::string> low = eat_stabs_identifier(input);
			CCC_CHECK(low.has_value(), "Cannot parse low part of range.");
			CCC_EXPECT_CHAR(input, ';', "low range value");
			
			std::optional<std::string> high = eat_stabs_identifier(input);
			CCC_CHECK(high.has_value(), "Cannot parse high part of range.");
			CCC_EXPECT_CHAR(input, ';', "high range value");
			
			range->low = std::move(*low);
			range->high = std::move(*high);
			
			out_type = std::move(range);
			break;
		}
		case StabsTypeDescriptor::STRUCT: { // s
			auto struct_type = std::make_unique<StabsStructType>(info);
			STABS_DEBUG_PRINTF("struct {\n");
			
			std::optional<s64> struct_size = eat_s64_literal(input);
			CCC_CHECK(struct_size.has_value(), "Cannot parse struct size.");
			struct_type->size = *struct_size;
			
			if(*input == '!') {
				input++;
				std::optional<s32> base_class_count = eat_s32_literal(input);
				CCC_CHECK(base_class_count.has_value(), "Cannot parse base class count.");
				CCC_EXPECT_CHAR(input, ',', "base class section");
				for(s64 i = 0; i < *base_class_count; i++) {
					StabsStructOrUnionType::BaseClass base_class;
					eat_char(input);
					
					std::optional<char> visibility = eat_char(input);
					CCC_CHECK(visibility.has_value(), "Cannot parse base class visibility.");
					base_class.visibility = (StabsStructOrUnionType::Visibility) *visibility;
					
					std::optional<s32> offset = eat_s32_literal(input);
					CCC_CHECK(offset.has_value(), "Cannot parse base class offset.");
					base_class.offset = (s32) *offset;
					
					CCC_EXPECT_CHAR(input, ',', "base class section");
					
					auto base_class_type = parse_stabs_type(input);
					CCC_RETURN_IF_ERROR(base_class_type);
					base_class.type = std::move(*base_class_type);
					
					CCC_EXPECT_CHAR(input, ';', "base class section");
					struct_type->base_classes.emplace_back(std::move(base_class));
				}
			}
			
			auto fields = parse_field_list(input);
			CCC_RETURN_IF_ERROR(fields);
			struct_type->fields = std::move(*fields);
			
			auto member_functions = parse_member_functions(input);
			CCC_RETURN_IF_ERROR(member_functions);
			struct_type->member_functions = std::move(*member_functions);
			
			STABS_DEBUG_PRINTF("}\n");
			
			out_type = std::move(struct_type);
			break;
		}
		case StabsTypeDescriptor::UNION: { // u
			auto union_type = std::make_unique<StabsUnionType>(info);
			STABS_DEBUG_PRINTF("union {\n");
			
			std::optional<s64> union_size = eat_s64_literal(input);
			CCC_CHECK(union_size.has_value(), "Cannot parse struct size.");
			union_type->size = *union_size;
			
			auto fields = parse_field_list(input);
			CCC_RETURN_IF_ERROR(fields);
			union_type->fields = std::move(*fields);
			
			auto member_functions = parse_member_functions(input);
			CCC_RETURN_IF_ERROR(member_functions);
			union_type->member_functions = std::move(*member_functions);
			
			STABS_DEBUG_PRINTF("}\n");
			
			out_type = std::move(union_type);
			break;
		}
		case StabsTypeDescriptor::CROSS_REFERENCE: { // x
			auto cross_reference = std::make_unique<StabsCrossReferenceType>(info);
			
			std::optional<char> c = eat_char(input);
			CCC_CHECK(c.has_value(), "Cannot parse cross reference type.");
			
			switch(*c) {
				case 'e': cross_reference->type = ast::ForwardDeclaredType::ENUM; break;
				case 's': cross_reference->type = ast::ForwardDeclaredType::STRUCT; break;
				case 'u': cross_reference->type = ast::ForwardDeclaredType::UNION; break;
				default:
					return CCC_FAILURE("Invalid cross reference type '%c'.", cross_reference->type);
			}
			
			Result<std::string> identifier = eat_dodgy_stabs_identifier(input);
			CCC_RETURN_IF_ERROR(identifier);
			cross_reference->identifier = std::move(*identifier);
			
			cross_reference->name = cross_reference->identifier;
			CCC_EXPECT_CHAR(input, ':', "cross reference");
			
			out_type = std::move(cross_reference);
			break;
		}
		case StabsTypeDescriptor::FLOATING_POINT_BUILTIN: { // R
			auto fp_builtin = std::make_unique<StabsFloatingPointBuiltInType>(info);
			
			std::optional<s32> fpclass = eat_s32_literal(input);
			CCC_CHECK(fpclass.has_value(), "Cannot parse floating point built-in class.");
			fp_builtin->fpclass = *fpclass;
			
			CCC_EXPECT_CHAR(input, ';', "floating point builtin");
			
			std::optional<s32> bytes = eat_s32_literal(input);
			CCC_CHECK(bytes.has_value(), "Cannot parse floating point built-in.");
			fp_builtin->bytes = *bytes;
			
			CCC_EXPECT_CHAR(input, ';', "floating point builtin");
			
			std::optional<s32> value = eat_s32_literal(input);
			CCC_CHECK(value.has_value(), "Cannot parse floating point built-in.");
			
			CCC_EXPECT_CHAR(input, ';', "floating point builtin");
			
			out_type = std::move(fp_builtin);
			break;
		}
		case StabsTypeDescriptor::METHOD: { // #
			auto method = std::make_unique<StabsMethodType>(info);
			
			if(*input == '#') {
				input++;
				
				auto return_type = parse_stabs_type(input);
				CCC_RETURN_IF_ERROR(return_type);
				method->return_type = std::move(*return_type);
				
				if(*input == ';') {
					input++;
				}
			} else {
				auto class_type = parse_stabs_type(input);
				CCC_RETURN_IF_ERROR(class_type);
				method->class_type = std::move(*class_type);
				
				CCC_EXPECT_CHAR(input, ',', "method");
				
				auto return_type = parse_stabs_type(input);
				CCC_RETURN_IF_ERROR(return_type);
				method->return_type = std::move(*return_type);
				
				while(*input != '\0') {
					if(*input == ';') {
						input++;
						break;
					}
					CCC_EXPECT_CHAR(input, ',', "method");
					
					auto parameter_type = parse_stabs_type(input);
					CCC_RETURN_IF_ERROR(parameter_type);
					method->parameter_types.emplace_back(std::move(*parameter_type));
				}
			}
			
			out_type = std::move(method);
			break;
		}
		case StabsTypeDescriptor::REFERENCE: { // &
			auto reference = std::make_unique<StabsReferenceType>(info);
			
			auto value_type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(value_type);
			reference->value_type = std::move(*value_type);
			
			out_type = std::move(reference);
			break;
		}
		case StabsTypeDescriptor::POINTER: { // *
			auto pointer = std::make_unique<StabsPointerType>(info);
			
			auto value_type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(value_type);
			pointer->value_type = std::move(*value_type);
			
			out_type = std::move(pointer);
			break;
		}
		case StabsTypeDescriptor::TYPE_ATTRIBUTE: { // @
			if((*input >= '0' && *input <= '9') || *input == '(') {
				auto member_pointer = std::make_unique<StabsPointerToNonStaticDataMember>(info);
				
				auto class_type = parse_stabs_type(input);
				CCC_RETURN_IF_ERROR(class_type);
				member_pointer->class_type = std::move(*class_type);
				
				CCC_EXPECT_CHAR(input, ',', "pointer to non-static data member");
				
				auto member_type = parse_stabs_type(input);
				CCC_RETURN_IF_ERROR(member_type);
				member_pointer->member_type = std::move(*member_type);
				
				out_type = std::move(member_pointer);
			} else {
				auto type_attribute = std::make_unique<StabsSizeTypeAttributeType>(info);
				CCC_CHECK(*input == 's', "Weird value following '@' type descriptor.");
				input++;
				
				std::optional<s64> size_bits = eat_s64_literal(input);
				CCC_CHECK(size_bits.has_value(), "Cannot parse type attribute.")
				type_attribute->size_bits = *size_bits;
				CCC_EXPECT_CHAR(input, ';', "type attribute");
				
				auto type = parse_stabs_type(input);
				CCC_RETURN_IF_ERROR(type);
				type_attribute->type = std::move(*type);
				
				out_type = std::move(type_attribute);
			}
			break;
		}
		case StabsTypeDescriptor::BUILTIN: { // -
			auto built_in = std::make_unique<StabsBuiltInType>(info);
			
			std::optional<s64> type_id = eat_s64_literal(input);
			CCC_CHECK(type_id.has_value(), "Cannot parse built-in.");
			built_in->type_id = *type_id;
			
			CCC_EXPECT_CHAR(input, ';', "builtin");
			
			out_type = std::move(built_in);
			break;
		}
		default: {
			return CCC_FAILURE(
				"Invalid type descriptor '%c' (%02x).",
				(u32) descriptor, (u32) descriptor);
		}
	}
	
	return out_type;
}

static Result<std::vector<StabsStructOrUnionType::Field>> parse_field_list(const char*& input) {
	std::vector<StabsStructOrUnionType::Field> fields;
	
	while(*input != '\0') {
		if(*input == ';') {
			input++;
			break;
		}
		
		const char* before_field = input;
		StabsStructOrUnionType::Field field;
		
		std::optional<std::string> name = eat_stabs_identifier(input);
		CCC_CHECK(name.has_value(), "Cannot parse field name.");
		field.name = std::move(*name);
		
		CCC_EXPECT_CHAR(input, ':', "identifier");
		if(*input == '/') {
			input++;
			
			std::optional<char> visibility = eat_char(input);
			CCC_CHECK(visibility.has_value(), "Cannot parse field visibility.");
			field.visibility = (StabsStructOrUnionType::Visibility) *visibility;
			
			switch(field.visibility) {
				case StabsStructOrUnionType::Visibility::NONE:
				case StabsStructOrUnionType::Visibility::PRIVATE:
				case StabsStructOrUnionType::Visibility::PROTECTED:
				case StabsStructOrUnionType::Visibility::PUBLIC:
				case StabsStructOrUnionType::Visibility::PUBLIC_OPTIMIZED_OUT:
					break;
				default:
					return CCC_FAILURE("invalid field visibility");
			}
		}
		if(*input == ':') {
			input = before_field;
			break;
		}
		auto type = parse_stabs_type(input);
		CCC_RETURN_IF_ERROR(type);
		field.type = std::move(*type);
		
		if(field.name.size() >= 1 && field.name[0] == '$') {
			// Virtual table pointers.
			CCC_EXPECT_CHAR(input, ',', "field type");
			
			std::optional<s32> offset_bits = eat_s32_literal(input);
			CCC_CHECK(offset_bits.has_value(), "Cannot parse field offset.");
			field.offset_bits = *offset_bits;
			
			CCC_EXPECT_CHAR(input, ';', "field offset");
		} else if(*input == ':') {
			input++;
			field.is_static = true;
			
			std::optional<std::string> type_name = eat_stabs_identifier(input);
			CCC_CHECK(type_name.has_value(), "Cannot parse static field type name.");
			field.type_name = std::move(*type_name);
			
			CCC_EXPECT_CHAR(input, ';', "identifier");
		} else if(*input == ',') {
			input++;
			
			std::optional<s32> offset_bits = eat_s32_literal(input);
			CCC_CHECK(offset_bits.has_value(), "Cannot parse field offset.");
			field.offset_bits = *offset_bits;
			
			CCC_EXPECT_CHAR(input, ',', "field offset");
			
			std::optional<s32> size_bits = eat_s32_literal(input);
			CCC_CHECK(size_bits.has_value(), "Cannot parse field size.");
			field.size_bits = *size_bits;
			
			CCC_EXPECT_CHAR(input, ';', "field size");
		} else {
			return CCC_FAILURE("Expected ':' or ',', got '%c' (%hhx).", *input, *input);
		}

		STABS_DEBUG(print_field(field);)

		fields.emplace_back(std::move(field));
	}
	
	return fields;
}

static Result<std::vector<StabsStructOrUnionType::MemberFunctionSet>> parse_member_functions(const char*& input) {
	// Check for if the next character is from an enclosing field list. If this
	// is the case, the next character will be ',' for normal fields and ':' for
	// static fields (see above).
	if(*input == ',' || *input == ':') {
		return std::vector<StabsStructOrUnionType::MemberFunctionSet>();
	}
	
	std::vector<StabsStructOrUnionType::MemberFunctionSet> member_functions;
	while(*input != '\0') {
		if(*input == ';') {
			input++;
			break;
		}
		StabsStructOrUnionType::MemberFunctionSet member_function_set;
		
		std::optional<std::string> name = eat_stabs_identifier(input);
		CCC_CHECK(name.has_value(), "Cannot parse member function name.");
		member_function_set.name = std::move(*name);
		
		CCC_EXPECT_CHAR(input, ':', "member function");
		CCC_EXPECT_CHAR(input, ':', "member function");
		while(*input != '\0') {
			if(*input == ';') {
				input++;
				break;
			}
			
			StabsStructOrUnionType::MemberFunction function;
			
			auto type = parse_stabs_type(input);
			CCC_RETURN_IF_ERROR(type);
			function.type = std::move(*type);
			
			CCC_EXPECT_CHAR(input, ':', "member function");
			std::optional<std::string> identifier = eat_stabs_identifier(input);
			CCC_CHECK(identifier.has_value(), "Invalid member function identifier.");
			CCC_EXPECT_CHAR(input, ';', "member function");
			
			std::optional<char> visibility = eat_char(input);
			CCC_CHECK(visibility.has_value(), "Cannot parse member function visibility.");
			function.visibility = (StabsStructOrUnionType::Visibility) *visibility;
			
			switch(function.visibility) {
				case StabsStructOrUnionType::Visibility::PRIVATE:
				case StabsStructOrUnionType::Visibility::PROTECTED:
				case StabsStructOrUnionType::Visibility::PUBLIC:
				case StabsStructOrUnionType::Visibility::PUBLIC_OPTIMIZED_OUT:
					break;
				default:
					return CCC_FAILURE("Invalid visibility for member function.");
			}
			
			std::optional<char> modifiers = eat_char(input);
			CCC_CHECK(modifiers.has_value(), "Cannot parse member function modifiers.");
			switch(*modifiers) {
				case 'A':
					function.is_const = false;
					function.is_volatile = false;
					break;
				case 'B':
					function.is_const = true;
					function.is_volatile = false;
					break;
				case 'C':
					function.is_const = false;
					function.is_volatile = true;
					break;
				case 'D':
					function.is_const = true;
					function.is_volatile = true;
					break;
				case '?':
				case '.':
					break;
				default:
					return CCC_FAILURE("Invalid member function modifiers.");
			}
			
			std::optional<char> flag = eat_char(input);
			CCC_CHECK(flag.has_value(), "Cannot parse member function type.");
			switch(*flag) {
				case '.': { // normal member function
					function.modifier = ast::MemberFunctionModifier::NONE;
					break;
				}
				case '?': { // static member function
					function.modifier = ast::MemberFunctionModifier::STATIC;
					break;
				}
				case '*': { // virtual member function
					std::optional<s32> vtable_index = eat_s32_literal(input);
					CCC_CHECK(vtable_index.has_value(), "Cannot parse vtable index.");
					function.vtable_index = *vtable_index;
					
					CCC_EXPECT_CHAR(input, ';', "virtual member function");
					
					auto virtual_type = parse_stabs_type(input);
					CCC_RETURN_IF_ERROR(virtual_type);
					function.virtual_type = std::move(*virtual_type);
					
					CCC_EXPECT_CHAR(input, ';', "virtual member function");
					function.modifier = ast::MemberFunctionModifier::VIRTUAL;
					break;
				}
				default:
					return CCC_FAILURE("Invalid member function type.");
			}
			member_function_set.overloads.emplace_back(std::move(function));
		}
		STABS_DEBUG_PRINTF("member func: %s\n", member_function_set.name.c_str());
		member_functions.emplace_back(std::move(member_function_set));
	}
	return member_functions;
}

std::optional<char> eat_char(const char*& input) {
	if(*input == '\0') {
		return std::nullopt;
	}
	return *(input++);
}

std::optional<s32> eat_s32_literal(const char*& input) {
	char* end;
	s64 value = strtoll(input, &end, 10);
	if(end == input) {
		return std::nullopt;
	}
	input = end;
	return (s32) value;
}

std::optional<s64> eat_s64_literal(const char*& input) {
	char* end;
	s64 value = strtoll(input, &end, 10);
	if(end == input) {
		return std::nullopt;
	}
	input = end;
	return value;
}

std::optional<std::string> eat_stabs_identifier(const char*& input) {
	std::string identifier;
	bool first = true;
	for(; *input != '\0'; input++) {
		bool valid_char = false;
		valid_char |= *input != ':' && *input != ';';
		valid_char |= !first && isalnum(*input);
		if(valid_char) {
			identifier += *input;
		} else {
			return identifier;
		}
		first = false;
	}
	return std::nullopt;
}

// The complexity here is because the input may contain an unescaped namespace
// separator '::' even if the field terminator is supposed to be a colon.
Result<std::string> eat_dodgy_stabs_identifier(const char*& input) {
	std::string identifier;
	bool first = true;
	s32 template_depth = 0;
	for(; *input != '\0'; input++) {
		if(*input == '<') {
			template_depth++;
		}
		if(*input == '>') {
			template_depth--;
		}
		bool valid_char = false;
		valid_char |= (*input != ':' || template_depth != 0) && *input != ';';
		valid_char |= !first && isalnum(*input);
		if(valid_char) {
			identifier += *input;
		} else {
			return identifier;
		}
		first = false;
	}
	return CCC_FAILURE(STAB_TRUNCATED_ERROR_MESSAGE);
}

STABS_DEBUG(

static void print_field(const StabsStructOrUnionType::Field& field) {
	printf("\t%04x %04x %04x %04x %s\n", field.offset_bits / 8, field.size_bits / 8, field.offset_bits, field.size_bits, field.name.c_str());
}

)

const char* stabs_field_visibility_to_string(StabsStructOrUnionType::Visibility visibility) {
	switch(visibility) {
		case StabsStructOrUnionType::Visibility::PRIVATE: return "private";
		case StabsStructOrUnionType::Visibility::PROTECTED: return "protected";
		case StabsStructOrUnionType::Visibility::PUBLIC: return "public";
		case StabsStructOrUnionType::Visibility::PUBLIC_OPTIMIZED_OUT: return "public_optimizedout";
		default: return "none";
	}
	return "";
}

}
