/*
 * Copyright (c) 2015 Andrew Kelley
 *
 * This file is part of zig, which is MIT licensed.
 * See http://opensource.org/licenses/MIT
 */

#include "codegen.hpp"
#include "hash_map.hpp"
#include "zig_llvm.hpp"
#include "os.hpp"
#include "config.h"

#include <stdio.h>

#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>

struct FnTableEntry {
    LLVMValueRef fn_value;
    AstNode *proto_node;
};

enum TypeId {
    TypeIdUserDefined,
    TypeIdPointer,
    TypeIdU8,
    TypeIdI32,
    TypeIdVoid,
    TypeIdUnreachable,
};

struct TypeTableEntry {
    TypeId id;
    LLVMTypeRef type_ref;
    llvm::DIType *di_type;

    TypeTableEntry *pointer_child;
    bool pointer_is_const;
    int user_defined_id;
    Buf name;
    TypeTableEntry *pointer_const_parent;
    TypeTableEntry *pointer_mut_parent;
};

struct CodeGen {
    LLVMModuleRef mod;
    AstNode *root;
    HashMap<Buf *, AstNode *, buf_hash, buf_eql_buf> fn_defs;
    ZigList<ErrorMsg> errors;
    LLVMBuilderRef builder;
    llvm::DIBuilder *dbuilder;
    llvm::DICompileUnit *compile_unit;
    HashMap<Buf *, FnTableEntry *, buf_hash, buf_eql_buf> fn_table;
    HashMap<Buf *, LLVMValueRef, buf_hash, buf_eql_buf> str_table;
    HashMap<Buf *, TypeTableEntry *, buf_hash, buf_eql_buf> type_table;
    TypeTableEntry *invalid_type_entry;
    LLVMTargetDataRef target_data_ref;
    unsigned pointer_size_bytes;
    bool is_static;
    LLVMTargetMachineRef target_machine;
    Buf in_file;
    Buf in_dir;
};

struct TypeNode {
    TypeTableEntry *entry;
};

struct CodeGenNode {
    union {
        TypeNode type_node; // for NodeTypeType
    } data;
};

CodeGen *create_codegen(AstNode *root, bool is_static, Buf *in_full_path) {
    CodeGen *g = allocate<CodeGen>(1);
    g->root = root;
    g->fn_defs.init(32);
    g->fn_table.init(32);
    g->str_table.init(32);
    g->type_table.init(32);
    g->is_static = is_static;

    os_path_split(in_full_path, &g->in_dir, &g->in_file);
    return g;
}

static void add_node_error(CodeGen *g, AstNode *node, Buf *msg) {
    g->errors.add_one();
    ErrorMsg *last_msg = &g->errors.last();
    last_msg->line_start = node->line;
    last_msg->column_start = node->column;
    last_msg->line_end = -1;
    last_msg->column_end = -1;
    last_msg->msg = msg;
}

static LLVMTypeRef to_llvm_type(AstNode *type_node) {
    assert(type_node->type == NodeTypeType);
    assert(type_node->codegen_node);
    assert(type_node->codegen_node->data.type_node.entry);

    return type_node->codegen_node->data.type_node.entry->type_ref;
}

static llvm::DIType *to_llvm_debug_type(AstNode *type_node) {
    assert(type_node->type == NodeTypeType);
    assert(type_node->codegen_node);
    assert(type_node->codegen_node->data.type_node.entry);

    return type_node->codegen_node->data.type_node.entry->di_type;
}


static bool type_is_unreachable(AstNode *type_node) {
    assert(type_node->type == NodeTypeType);
    return type_node->data.type.type == AstNodeTypeTypePrimitive &&
            buf_eql_str(&type_node->data.type.primitive_name, "unreachable");
}

static void analyze_node(CodeGen *g, AstNode *node);

static void resolve_type_and_recurse(CodeGen *g, AstNode *node) {
    assert(!node->codegen_node);
    node->codegen_node = allocate<CodeGenNode>(1);
    TypeNode *type_node = &node->codegen_node->data.type_node;
    switch (node->data.type.type) {
        case AstNodeTypeTypePrimitive:
            {
                Buf *name = &node->data.type.primitive_name;
                auto table_entry = g->type_table.maybe_get(name);
                if (table_entry) {
                    type_node->entry = table_entry->value;
                } else {
                    add_node_error(g, node,
                            buf_sprintf("invalid type name: '%s'", buf_ptr(name)));
                    type_node->entry = g->invalid_type_entry;
                }
                break;
            }
        case AstNodeTypeTypePointer:
            {
                analyze_node(g, node->data.type.child_type);
                TypeNode *child_type_node = &node->data.type.child_type->codegen_node->data.type_node;
                if (child_type_node->entry->id == TypeIdUnreachable) {
                    add_node_error(g, node,
                            buf_create_from_str("pointer to unreachable not allowed"));
                }
                TypeTableEntry **parent_pointer = node->data.type.is_const ?
                    &child_type_node->entry->pointer_const_parent :
                    &child_type_node->entry->pointer_mut_parent;
                const char *const_or_mut_str = node->data.type.is_const ? "const" : "mut";
                if (*parent_pointer) {
                    type_node->entry = *parent_pointer;
                } else {
                    TypeTableEntry *entry = allocate<TypeTableEntry>(1);
                    entry->id = TypeIdPointer;
                    entry->type_ref = LLVMPointerType(child_type_node->entry->type_ref, 0);
                    buf_appendf(&entry->name, "*%s %s", const_or_mut_str, buf_ptr(&child_type_node->entry->name));
                    entry->di_type = g->dbuilder->createPointerType(child_type_node->entry->di_type,
                            g->pointer_size_bytes * 8, g->pointer_size_bytes * 8, buf_ptr(&entry->name));
                    g->type_table.put(&entry->name, entry);
                    type_node->entry = entry;
                    *parent_pointer = entry;
                }
                break;
            }
    }
}

static void analyze_node(CodeGen *g, AstNode *node) {
    switch (node->type) {
        case NodeTypeRoot:
            for (int i = 0; i < node->data.root.top_level_decls.length; i += 1) {
                AstNode *child = node->data.root.top_level_decls.at(i);
                analyze_node(g, child);
            }
            break;
        case NodeTypeExternBlock:
            for (int fn_decl_i = 0; fn_decl_i < node->data.extern_block.fn_decls.length; fn_decl_i += 1) {
                AstNode *fn_decl = node->data.extern_block.fn_decls.at(fn_decl_i);
                analyze_node(g, fn_decl);

                AstNode *fn_proto = fn_decl->data.fn_decl.fn_proto;
                Buf *name = &fn_proto->data.fn_proto.name;
                ZigList<AstNode *> *params = &fn_proto->data.fn_proto.params;

                LLVMTypeRef *fn_param_values = allocate<LLVMTypeRef>(params->length);
                for (int param_i = 0; param_i < params->length; param_i += 1) {
                    AstNode *param_node = params->at(param_i);
                    assert(param_node->type == NodeTypeParamDecl);
                    AstNode *param_type = param_node->data.param_decl.type;
                    fn_param_values[param_i] = to_llvm_type(param_type);
                }
                AstNode *return_type_node = fn_proto->data.fn_proto.return_type;
                LLVMTypeRef return_type = to_llvm_type(return_type_node);

                LLVMTypeRef fn_type = LLVMFunctionType(return_type, fn_param_values, params->length, 0);
                LLVMValueRef fn_val = LLVMAddFunction(g->mod, buf_ptr(name), fn_type);
                LLVMSetLinkage(fn_val, LLVMExternalLinkage);
                LLVMSetFunctionCallConv(fn_val, LLVMCCallConv);

                if (type_is_unreachable(return_type_node)) {
                    LLVMAddFunctionAttr(fn_val, LLVMNoReturnAttribute);
                }

                FnTableEntry *fn_table_entry = allocate<FnTableEntry>(1);
                fn_table_entry->fn_value = fn_val;
                fn_table_entry->proto_node = fn_proto;
                g->fn_table.put(name, fn_table_entry);
            }
            break;
        case NodeTypeFnDef:
            {
                AstNode *proto_node = node->data.fn_def.fn_proto;
                assert(proto_node->type = NodeTypeFnProto);
                Buf *proto_name = &proto_node->data.fn_proto.name;
                auto entry = g->fn_defs.maybe_get(proto_name);
                if (entry) {
                    add_node_error(g, node,
                            buf_sprintf("redefinition of '%s'", buf_ptr(proto_name)));
                } else {
                    g->fn_defs.put(proto_name, node);
                    analyze_node(g, proto_node);
                }
                break;
            }
        case NodeTypeFnDecl:
            {
                AstNode *proto_node = node->data.fn_decl.fn_proto;
                assert(proto_node->type == NodeTypeFnProto);
                analyze_node(g, proto_node);
                break;
            }
        case NodeTypeFnProto:
            {
                for (int i = 0; i < node->data.fn_proto.params.length; i += 1) {
                    AstNode *child = node->data.fn_proto.params.at(i);
                    analyze_node(g, child);
                }
                analyze_node(g, node->data.fn_proto.return_type);
                break;
            }
        case NodeTypeParamDecl:
            analyze_node(g, node->data.param_decl.type);
            break;

        case NodeTypeType:
            {
                resolve_type_and_recurse(g, node);
                break;
            }
        case NodeTypeBlock:
            for (int i = 0; i < node->data.block.statements.length; i += 1) {
                AstNode *child = node->data.block.statements.at(i);
                analyze_node(g, child);
            }
            break;
        case NodeTypeStatement:
            switch (node->data.statement.type) {
                case AstNodeStatementTypeExpression:
                    analyze_node(g, node->data.statement.data.expr.expression);
                    break;
                case AstNodeStatementTypeReturn:
                    analyze_node(g, node->data.statement.data.retrn.expression);
                    break;
            }
            break;
        case NodeTypeExpression:
            switch (node->data.expression.type) {
                case AstNodeExpressionTypeNumber:
                    break;
                case AstNodeExpressionTypeString:
                    break;
                case AstNodeExpressionTypeFnCall:
                    analyze_node(g, node->data.expression.data.fn_call);
                    break;
                case AstNodeExpressionTypeUnreachable:
                    break;
            }
            break;
        case NodeTypeFnCall:
            for (int i = 0; i < node->data.fn_call.params.length; i += 1) {
                AstNode *child = node->data.fn_call.params.at(i);
                analyze_node(g, child);
            }
            break;
    }
}

static void add_types(CodeGen *g) {
    {
        TypeTableEntry *entry = allocate<TypeTableEntry>(1);
        entry->id = TypeIdU8;
        entry->type_ref = LLVMInt8Type();
        buf_init_from_str(&entry->name, "u8");
        entry->di_type = g->dbuilder->createBasicType(buf_ptr(&entry->name), 8, 8, llvm::dwarf::DW_ATE_unsigned);
        g->type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = allocate<TypeTableEntry>(1);
        entry->id = TypeIdI32;
        entry->type_ref = LLVMInt32Type();
        buf_init_from_str(&entry->name, "i32");
        entry->di_type = g->dbuilder->createBasicType(buf_ptr(&entry->name), 32, 32,
                llvm::dwarf::DW_ATE_signed);
        g->type_table.put(&entry->name, entry);
    }
    {
        TypeTableEntry *entry = allocate<TypeTableEntry>(1);
        entry->id = TypeIdVoid;
        entry->type_ref = LLVMVoidType();
        buf_init_from_str(&entry->name, "void");
        entry->di_type = g->dbuilder->createBasicType(buf_ptr(&entry->name), 0, 0,
                llvm::dwarf::DW_ATE_unsigned);
        g->type_table.put(&entry->name, entry);

        // invalid types are void
        g->invalid_type_entry = entry;
    }
    {
        TypeTableEntry *entry = allocate<TypeTableEntry>(1);
        entry->id = TypeIdUnreachable;
        entry->type_ref = LLVMVoidType();
        buf_init_from_str(&entry->name, "unreachable");
        entry->di_type = g->invalid_type_entry->di_type;
        g->type_table.put(&entry->name, entry);
    }
}


void semantic_analyze(CodeGen *g) {
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeNativeTarget();

    char *native_triple = LLVMGetDefaultTargetTriple();

    LLVMTargetRef target_ref;
    char *err_msg = nullptr;
    if (LLVMGetTargetFromTriple(native_triple, &target_ref, &err_msg)) {
        zig_panic("unable to get target from triple: %s", err_msg);
    }

    char *native_cpu = LLVMZigGetHostCPUName();
    char *native_features = LLVMZigGetNativeFeatures();

    LLVMCodeGenOptLevel opt_level = LLVMCodeGenLevelNone;

    LLVMRelocMode reloc_mode = g->is_static ? LLVMRelocStatic : LLVMRelocPIC;

    g->target_machine = LLVMCreateTargetMachine(target_ref, native_triple,
            native_cpu, native_features, opt_level, reloc_mode, LLVMCodeModelDefault);

    g->target_data_ref = LLVMGetTargetMachineData(g->target_machine);


    g->mod = LLVMModuleCreateWithName("ZigModule");

    g->pointer_size_bytes = LLVMPointerSize(g->target_data_ref);

    g->builder = LLVMCreateBuilder();
    g->dbuilder = new llvm::DIBuilder(*llvm::unwrap(g->mod), true);


    add_types(g);

    // Pass 1.
    analyze_node(g, g->root);
}

static LLVMValueRef gen_expr(CodeGen *g, AstNode *expr_node);

static LLVMValueRef gen_fn_call(CodeGen *g, AstNode *fn_call_node) {
    assert(fn_call_node->type == NodeTypeFnCall);

    Buf *name = &fn_call_node->data.fn_call.name;

    auto entry = g->fn_table.maybe_get(name);
    if (!entry) {
        add_node_error(g, fn_call_node,
                buf_sprintf("undefined function: '%s'", buf_ptr(name)));
        return LLVMConstNull(LLVMInt32Type());
    }
    FnTableEntry *fn_table_entry = entry->value;
    assert(fn_table_entry->proto_node->type == NodeTypeFnProto);
    int expected_param_count = fn_table_entry->proto_node->data.fn_proto.params.length;
    int actual_param_count = fn_call_node->data.fn_call.params.length;
    if (expected_param_count != actual_param_count) {
        add_node_error(g, fn_call_node,
                buf_sprintf("wrong number of arguments. Expected %d, got %d.",
                    expected_param_count, actual_param_count));
        return LLVMConstNull(LLVMInt32Type());
    }

    LLVMValueRef *param_values = allocate<LLVMValueRef>(actual_param_count);
    for (int i = 0; i < actual_param_count; i += 1) {
        AstNode *expr_node = fn_call_node->data.fn_call.params.at(i);
        param_values[i] = gen_expr(g, expr_node);
    }

    LLVMValueRef result = LLVMBuildCall(g->builder, fn_table_entry->fn_value,
            param_values, actual_param_count, "");

    if (type_is_unreachable(fn_table_entry->proto_node->data.fn_proto.return_type)) {
        return LLVMBuildUnreachable(g->builder);
    } else {
        return result;
    }
}

static LLVMValueRef find_or_create_string(CodeGen *g, Buf *str) {
    auto entry = g->str_table.maybe_get(str);
    if (entry) {
        return entry->value;
    }
    LLVMValueRef text = LLVMConstString(buf_ptr(str), buf_len(str), false);
    LLVMValueRef global_value = LLVMAddGlobal(g->mod, LLVMTypeOf(text), "");
    LLVMSetLinkage(global_value, LLVMPrivateLinkage);
    LLVMSetInitializer(global_value, text);
    LLVMSetGlobalConstant(global_value, true);
    LLVMSetUnnamedAddr(global_value, true);
    g->str_table.put(str, global_value);

    return global_value;
}

static LLVMValueRef gen_expr(CodeGen *g, AstNode *expr_node) {
    assert(expr_node->type == NodeTypeExpression);
    switch (expr_node->data.expression.type) {
        case AstNodeExpressionTypeNumber:
            {
                Buf *number_str = &expr_node->data.expression.data.number;
                LLVMTypeRef number_type = LLVMInt32Type();
                LLVMValueRef number_val = LLVMConstIntOfStringAndSize(number_type,
                        buf_ptr(number_str), buf_len(number_str), 10);
                return number_val;
            }
        case AstNodeExpressionTypeString:
            {
                Buf *str = &expr_node->data.expression.data.string;
                LLVMValueRef str_val = find_or_create_string(g, str);
                LLVMValueRef indices[] = {
                    LLVMConstInt(LLVMInt32Type(), 0, false),
                    LLVMConstInt(LLVMInt32Type(), 0, false)
                };
                LLVMValueRef ptr_val = LLVMBuildInBoundsGEP(g->builder, str_val,
                        indices, 2, "");

                return ptr_val;
            }
        case AstNodeExpressionTypeFnCall:
            return gen_fn_call(g, expr_node->data.expression.data.fn_call);
        case AstNodeExpressionTypeUnreachable:
            return LLVMBuildUnreachable(g->builder);
    }
    zig_unreachable();
}

static void gen_block(CodeGen *g, AstNode *block_node) {
    assert(block_node->type == NodeTypeBlock);

    for (int i = 0; i < block_node->data.block.statements.length; i += 1) {
        AstNode *statement_node = block_node->data.block.statements.at(i);
        assert(statement_node->type == NodeTypeStatement);
        switch (statement_node->data.statement.type) {
            case AstNodeStatementTypeReturn:
                {
                    AstNode *expr_node = statement_node->data.statement.data.retrn.expression;
                    LLVMValueRef value = gen_expr(g, expr_node);
                    LLVMBuildRet(g->builder, value);
                    break;
                }
            case AstNodeStatementTypeExpression:
                {
                    AstNode *expr_node = statement_node->data.statement.data.expr.expression;
                    gen_expr(g, expr_node);
                    break;
                }
        }
    }
}

static llvm::DISubroutineType *create_di_function_type(CodeGen *g, AstNodeFnProto *fn_proto, llvm::DIFile *unit) {
    llvm::SmallVector<llvm::Metadata *, 8> types;

    llvm::DIType *return_type = to_llvm_debug_type(fn_proto->return_type);
    types.push_back(return_type);

    for (int i = 0; i < fn_proto->params.length; i += 1) {
        AstNode *param_node = fn_proto->params.at(i);
        llvm::DIType *param_type = to_llvm_debug_type(param_node);
        types.push_back(param_type);
    }

    return g->dbuilder->createSubroutineType(unit, g->dbuilder->getOrCreateTypeArray(types));
}

void code_gen(CodeGen *g) {
    Buf *producer = buf_sprintf("zig %s", ZIG_VERSION_STRING);
    bool is_optimized = false;
    const char *flags = "";
    unsigned runtime_version = 0;
    g->compile_unit = g->dbuilder->createCompileUnit(llvm::dwarf::DW_LANG_C99,
            buf_ptr(&g->in_file), buf_ptr(&g->in_dir),
            buf_ptr(producer), is_optimized, flags, runtime_version);

    auto it = g->fn_defs.entry_iterator();
    for (;;) {
        auto *entry = it.next();
        if (!entry)
            break;

        AstNode *fn_def_node = entry->value;
        AstNodeFnDef *fn_def = &fn_def_node->data.fn_def;
        assert(fn_def->fn_proto->type == NodeTypeFnProto);
        AstNodeFnProto *fn_proto = &fn_def->fn_proto->data.fn_proto;

        LLVMTypeRef ret_type = to_llvm_type(fn_proto->return_type);
        LLVMTypeRef *param_types = allocate<LLVMTypeRef>(fn_proto->params.length);
        for (int param_decl_i = 0; param_decl_i < fn_proto->params.length; param_decl_i += 1) {
            AstNode *param_node = fn_proto->params.at(param_decl_i);
            assert(param_node->type == NodeTypeParamDecl);
            AstNode *type_node = param_node->data.param_decl.type;
            param_types[param_decl_i] = to_llvm_type(type_node);
        }
        LLVMTypeRef function_type = LLVMFunctionType(ret_type, param_types, fn_proto->params.length, 0);
        LLVMValueRef fn = LLVMAddFunction(g->mod, buf_ptr(&fn_proto->name), function_type);

        bool internal_linkage = false;
        LLVMSetLinkage(fn, internal_linkage ? LLVMPrivateLinkage : LLVMExternalLinkage);

        if (type_is_unreachable(fn_proto->return_type)) {
            LLVMAddFunctionAttr(fn, LLVMNoReturnAttribute);
        }
        LLVMAddFunctionAttr(fn, LLVMNoUnwindAttribute);

        // Add debug info.
        llvm::DIFile *unit = g->dbuilder->createFile(g->compile_unit->getFilename(),
                g->compile_unit->getDirectory());
        llvm::DIScope *fn_scope = unit;
        unsigned line_number = fn_def_node->line + 1;
        unsigned scope_line = line_number;
        bool is_definition = true;
        unsigned flags = 0;
        llvm::Function *unwrapped_function = reinterpret_cast<llvm::Function*>(llvm::unwrap(fn));
        g->dbuilder->createFunction(
            fn_scope, buf_ptr(&fn_proto->name), "", unit, line_number,
            create_di_function_type(g, fn_proto, unit), internal_linkage, 
            is_definition, scope_line, flags, is_optimized, unwrapped_function);



        LLVMBasicBlockRef entry_block = LLVMAppendBasicBlock(fn, "entry");
        LLVMPositionBuilderAtEnd(g->builder, entry_block);

        gen_block(g, fn_def->body);
    }

    g->dbuilder->finalize();

    LLVMDumpModule(g->mod);

    char *error = nullptr;
    LLVMVerifyModule(g->mod, LLVMAbortProcessAction, &error);
}

ZigList<ErrorMsg> *codegen_error_messages(CodeGen *g) {
    return &g->errors;
}


void code_gen_link(CodeGen *g, const char *out_file) {
    LLVMPassRegistryRef registry = LLVMGetGlobalPassRegistry();
    LLVMInitializeCore(registry);
    LLVMInitializeCodeGen(registry);
    LLVMZigInitializeLoopStrengthReducePass(registry);
    LLVMZigInitializeLowerIntrinsicsPass(registry);
    LLVMZigInitializeUnreachableBlockElimPass(registry);

    Buf out_file_o = BUF_INIT;
    buf_init_from_str(&out_file_o, out_file);
    buf_append_str(&out_file_o, ".o");

    char *err_msg = nullptr;
    if (LLVMTargetMachineEmitToFile(g->target_machine, g->mod, buf_ptr(&out_file_o), LLVMObjectFile, &err_msg)) {
        zig_panic("unable to write object file: %s", err_msg);
    }

    ZigList<const char *> args = {0};
    args.append("-o");
    args.append(out_file);
    args.append((const char *)buf_ptr(&out_file_o));
    args.append("-lc");
    os_spawn_process("ld", args, false);
}