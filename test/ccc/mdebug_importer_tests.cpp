// This file is part of the Chaos Compiler Collection.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include "ccc/mdebug_importer.h"

using namespace ccc;
using namespace ccc::mdebug;

// Tests for the whole .mdebug parsing and analysis pipeline. They are based on
// real compiler outputs from the old homebrew toolchain (GCC 3.2.3) except
// where otherwise stated.

static Result<SymbolDatabase> run_importer(const char* name, const mdebug::File& input)
{
	SymbolDatabase database;
	
	Result<SymbolSource*> symbol_source = database.symbol_sources.create_symbol(name, SymbolSourceHandle());
	CCC_RETURN_IF_ERROR(symbol_source);
	
	AnalysisContext context;
	context.symbol_source = (*symbol_source)->handle();
	
	Result<void> result = import_file(database, input, context);
	CCC_RETURN_IF_ERROR(result);
	
	return database;
}

#define MDEBUG_IMPORTER_TEST(name, symbols) \
	static void mdebug_importer_test_##name(SymbolDatabase& database); \
	TEST(CCCMdebugImporter, name) \
	{ \
		mdebug::File input = {std::vector<mdebug::Symbol>symbols}; \
		Result<SymbolDatabase> database = run_importer(#name, input); \
		CCC_GTEST_FAIL_IF_ERROR(database); \
		mdebug_importer_test_##name(*database); \
	} \
	static void mdebug_importer_test_##name(SymbolDatabase& database)

// ee-g++ -gstabs
// enum Enum {};
MDEBUG_IMPORTER_TEST(Enum,
	({
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "Enum:t(1,1)=e;"}
	}))
{
	EXPECT_EQ(database.data_types.size(), 1);
	DataTypeHandle handle = database.data_types.first_handle_from_name("Enum");
	DataType* data_type = database.data_types.symbol_from_handle(handle);
	ASSERT_TRUE(data_type && data_type->type());
	EXPECT_EQ(data_type->type()->descriptor, ast::ENUM);
	EXPECT_EQ(data_type->type()->storage_class, ast::SC_NONE);
}

// ee-g++ -gstabs
// typedef enum NamedTypedefedEnum {} NamedTypedefedEnum;
MDEBUG_IMPORTER_TEST(NamedTypedefedEnum,
	({
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "Enum:t(1,1)=e;"},
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "Enum:t(1,2)=(1,1)"}
	}))
{
	EXPECT_EQ(database.data_types.size(), 1);
	DataTypeHandle handle = database.data_types.first_handle_from_name("Enum");
	DataType* data_type = database.data_types.symbol_from_handle(handle);
	ASSERT_TRUE(data_type && data_type->type());
	EXPECT_EQ(data_type->type()->descriptor, ast::ENUM);
	EXPECT_EQ(data_type->type()->storage_class, ast::SC_TYPEDEF);
}

// ee-g++ -gstabs
// struct Struct {};
MDEBUG_IMPORTER_TEST(Struct,
	({
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "Struct:T(1,1)=s1;"},
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "Struct:t(1,1)"}
	}))
{
	EXPECT_EQ(database.data_types.size(), 1);
	DataTypeHandle handle = database.data_types.first_handle_from_name("Struct");
	DataType* data_type = database.data_types.symbol_from_handle(handle);
	ASSERT_TRUE(data_type && data_type->type());
	EXPECT_EQ(data_type->type()->descriptor, ast::STRUCT_OR_UNION);
	EXPECT_EQ(data_type->type()->storage_class, ast::SC_NONE);
}

// ee-g++ -gstabs
// typedef struct {} TypedefedStruct;
MDEBUG_IMPORTER_TEST(TypedefedStruct,
	({
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "TypedefedStruct:t(1,1)=s1;"}
	}))
{
	EXPECT_EQ(database.data_types.size(), 1);
	DataTypeHandle handle = database.data_types.first_handle_from_name("TypedefedStruct");
	DataType* data_type = database.data_types.symbol_from_handle(handle);
	ASSERT_TRUE(data_type && data_type->type());
	EXPECT_EQ(data_type->type()->descriptor, ast::STRUCT_OR_UNION);
	EXPECT_EQ(data_type->type()->storage_class, ast::SC_TYPEDEF);
}

// ee-g++ -gstabs
// typedef struct NamedTypedefStruct {} NamedTypedefStruct;
MDEBUG_IMPORTER_TEST(NamedTypedefStruct,
	({
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "NamedTypedefedStruct:T(1,1)=s1;"},
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "NamedTypedefedStruct:t(1,1)"},
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "NamedTypedefedStruct:t(1,2)=(1,1)"}
	}))
{
	EXPECT_EQ(database.data_types.size(), 1);
	DataTypeHandle handle = database.data_types.first_handle_from_name("NamedTypedefedStruct");
	DataType* data_type = database.data_types.symbol_from_handle(handle);
	ASSERT_TRUE(data_type && data_type->type());
	EXPECT_EQ(data_type->type()->descriptor, ast::STRUCT_OR_UNION);
	EXPECT_EQ(data_type->type()->storage_class, ast::SC_TYPEDEF);
}

// Synthetic example. Something like:
// typedef struct {} StrangeStruct;
MDEBUG_IMPORTER_TEST(StrangeStruct,
	({
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "StrangeStruct:T(1,1)=s1;"},
		{0x00000000, SymbolType::NIL, SymbolClass::NIL, STABS_CODE(N_LSYM), "StrangeStruct:t(1,2)=(1,1)"}
	}))
{
	EXPECT_EQ(database.data_types.size(), 1);
	DataTypeHandle handle = database.data_types.first_handle_from_name("StrangeStruct");
	DataType* data_type = database.data_types.symbol_from_handle(handle);
	ASSERT_TRUE(data_type && data_type->type());
	EXPECT_EQ(data_type->type()->descriptor, ast::STRUCT_OR_UNION);
	EXPECT_EQ(data_type->type()->storage_class, ast::SC_TYPEDEF);
}

// ee-g++ -gstabs
// void SimpleFunction() {}
MDEBUG_IMPORTER_TEST(SimpleFunction,
	({
		{0x00000000, SymbolType::LABEL, SymbolClass::TEXT, STABS_CODE(N_FUN), "Z14SimpleFunctionv:F(0,23)"},
		{0x00000000, SymbolType::LABEL, SymbolClass::TEXT, 1,                 "$LM1"},
		{0x00000000, SymbolType::PROC,  SymbolClass::TEXT, 1,                 "_Z14SimpleFunctionv"},
		{0x0000000c, SymbolType::LABEL, SymbolClass::TEXT, 1,                 "$LM2"},
		{0x00000020, SymbolType::END,   SymbolClass::TEXT, 31,                "_Z14SimpleFunctionv"}
	}))
{
	EXPECT_EQ(database.functions.size(), 1);
	FunctionHandle handle = database.functions.first_handle_from_name("Z14SimpleFunctionv");
	Function* function = database.functions.symbol_from_handle(handle);
	ASSERT_TRUE(function);
}

// iop-gcc -gstabs
// void SimpleFunction() {}
MDEBUG_IMPORTER_TEST(SimpleFunctionIOP,
	({
		{0x00000000,  SymbolType::PROC,  SymbolClass::TEXT, 1,                 "SimpleFunction"},
		{0x0000000c,  SymbolType::LABEL, SymbolClass::TEXT, 1,                 "$LM2"},
		{0x00000020, SymbolType::END,   SymbolClass::TEXT, 27,                "SimpleFunction"},
		{0x00000000,  SymbolType::LABEL, SymbolClass::TEXT, STABS_CODE(N_FUN), "SimpleFunction:F22"}
	}))
{
	EXPECT_EQ(database.functions.size(), 1);
	FunctionHandle handle = database.functions.first_handle_from_name("Z14SimpleFunctionv");
	Function* function = database.functions.symbol_from_handle(handle);
	ASSERT_TRUE(function);
}

// ee-g++ -gstabs
// int ComplicatedFunction(int a, float b, char* c) {
// 	int x = b < 0;
// 	if(a) { int y = b + *c; return y; }
// 	for(int i = 0; i < 5; i++) { int z = b + i; x += z; }
// 	return x;
// }
MDEBUG_IMPORTER_TEST(ComplicatedFunction,
	({
		{0x00000000, SymbolType::LABEL, SymbolClass::TEXT, STABS_CODE(N_FUN),  "_Z19ComplicatedFunctionifPc:F(0,1)"},
		{0xffffffd0, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_PSYM), "a:p(0,1)"},
		{0xffffffd4, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_PSYM), "b:p(0,14)"},
		{0xffffffd8, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_PSYM), "c:p(1,1)=*(0,2)"},
		{0x00000000, SymbolType::LABEL, SymbolClass::TEXT, 1,                  "$LM1"},
		{0x00000000, SymbolType::PROC,  SymbolClass::TEXT, 1,                  "_Z19ComplicatedFunctionifPc"},
		{0x00000018, SymbolType::LABEL, SymbolClass::TEXT, 2,                  "$LM2"},
		{0x00000048, SymbolType::LABEL, SymbolClass::TEXT, 3,                  "$LM3"},
		{0x00000088, SymbolType::LABEL, SymbolClass::TEXT, 4,                  "$LM4"},
		{0x000000e0, SymbolType::LABEL, SymbolClass::TEXT, 5,                  "$LM5"},
		{0x000000e8, SymbolType::LABEL, SymbolClass::TEXT, 6,                  "$LM6"},
		{0x00000100, SymbolType::END,   SymbolClass::TEXT, 34,                 "_Z19ComplicatedFunctionifPc"},
		{0xffffffdc, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LSYM), "x:(0,1)"},
		{0x00000018, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LBRAC), ""},
		{0xffffffe0, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LSYM), "y:(0,1)"},
		{0x00000054, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LBRAC), ""},
		{0x00000088, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_RBRAC), ""},
		{0xffffffe0, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LSYM), "i:(0,1)"},
		{0x00000088, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LBRAC), ""},
		{0xffffffe4, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LSYM), "z:(0,1)"},
		{0x000000a4, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_LBRAC), ""},
		{0x000000cc, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_RBRAC), ""},
		{0x000000e0, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_RBRAC), ""},
		{0x000000e8, SymbolType::NIL,   SymbolClass::NIL,  STABS_CODE(N_RBRAC), ""},
	}))
{
	EXPECT_EQ(database.functions.size(), 1);
	EXPECT_EQ(database.local_variables.size(), 3);
	EXPECT_EQ(database.parameter_variables.size(), 3);
}
