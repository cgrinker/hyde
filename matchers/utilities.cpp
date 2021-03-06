/*
Copyright 2018 Adobe
All Rights Reserved.

NOTICE: Adobe permits you to use, modify, and distribute this file in
accordance with the terms of the Adobe license agreement accompanying
it. If you have received this file from a source other than Adobe,
then your use, modification, or distribution of it requires the prior
written permission of Adobe.
*/

// identity
#include "utilities.hpp"

// stdc++
#include <iostream>
#include <sstream>

// boost
#include "boost/filesystem.hpp"

// clang
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/ArrayRef.h"

// application
#include "json.hpp"

using namespace clang;
using namespace clang::ast_matchers;

/**************************************************************************************************/

namespace {

/**************************************************************************************************/

void trim_back(std::string& src) {
    std::size_t start(src.size());

    while (start != 0 && (std::isspace(src[start - 1]) || src[start - 1] == '\n'))
        start--;

    src.erase(start, std::string::npos);
}

/**************************************************************************************************/

std::string to_string(const ASTContext* n, SourceRange range, bool as_token) {
    const auto char_range = as_token ? clang::CharSourceRange::getTokenRange(range) :
                                       clang::CharSourceRange::getCharRange(range);
    std::string result =
        Lexer::getSourceText(char_range, n->getSourceManager(), n->getLangOpts()).str();
    trim_back(result);
    return result;
}

/**************************************************************************************************/

std::string to_string(const ASTContext* n, const clang::TemplateDecl* template_decl) {
    std::size_t count{0};
    std::string result = "template <";
    for (const auto& parameter_decl : *template_decl->getTemplateParameters()) {
        if (count++) result += ", ";
        if (const auto& template_type = dyn_cast<TemplateTypeParmDecl>(parameter_decl)) {
            result += template_type->wasDeclaredWithTypename() ? "typename" : "class";
            if (template_type->isParameterPack()) result += "...";
            result += " " + template_type->getNameAsString();
        } else if (const auto& non_template_type =
                       dyn_cast<NonTypeTemplateParmDecl>(parameter_decl)) {
            result += hyde::to_string(n, non_template_type->getType());
            if (non_template_type->isParameterPack()) result += "...";
            result += " " + non_template_type->getNameAsString();
        }
    }
    result += "> ";

    return result;
}

/**************************************************************************************************/
// Taken from https://clang.llvm.org/doxygen/IssueHash_8cpp_source.html#l00031
// and cleaned up a bit.

// Get a string representation of the parts of the signature that can be
// overloaded on.
std::string GetSignature(const ASTContext* n, const FunctionDecl* function, bool fully_qualified, bool named_args) {
    if (!function) return "";

    bool isTrailing = false;
    std::string signature;

    if (const auto* fp = function->getType()->getAs<FunctionProtoType>()) {
        isTrailing = fp->hasTrailingReturn();
    }

    if (auto template_decl = function->getDescribedFunctionTemplate()) {
        signature = to_string(n, template_decl);
    }

    if (!isa<CXXConstructorDecl>(function) && !isa<CXXDestructorDecl>(function) &&
        !isa<CXXConversionDecl>(function)) {
        if (function->isConstexpr()) {
            signature.append("constexpr ");
        }
        auto storage = function->getStorageClass();
        switch (storage) {
            case SC_Static:
                signature.append("static ");
            case SC_Extern:
                signature.append("extern ");
            default:
                break;
        }

        if (isTrailing) {
            signature.append("auto").append(" ");
        } else {
            signature.append(hyde::to_string(n, function->getReturnType())).append(" ");
        }
    }
    signature
        .append(fully_qualified ? function->getQualifiedNameAsString() :
                                  function->getNameAsString())
        .append("(");

    for (int i = 0, paramsCount = function->getNumParams(); i < paramsCount; ++i) {
        if (i) signature.append(", ");
        signature.append(hyde::to_string(n, function->getParamDecl(i)->getType()));
        if (named_args) {
            auto arg_name = function->getParamDecl(i)->getNameAsString();
            if (!arg_name.empty()) {
                signature.append(" ");
                signature.append(std::move(arg_name));
            }
        }
    }

    if (function->isVariadic()) signature.append(", ...");
    signature.append(")");
    const auto* functionT = llvm::dyn_cast_or_null<FunctionType>(function->getType().getTypePtr());
    bool canHaveCV = functionT || isa<CXXMethodDecl>(function);
    if (isTrailing) {
        if (canHaveCV) {
            // bit of repetition but hey not much.
            if (functionT->isConst()) signature.append(" const");
            if (functionT->isVolatile()) signature.append(" volatile");
            if (functionT->isRestrict()) signature.append(" restrict");
        }
        signature.append(" -> ").append(hyde::to_string(n, function->getReturnType())).append("");
    }

    if (!canHaveCV) return signature;

    if (!isTrailing) {
        if (functionT->isConst()) signature.append(" const");
        if (functionT->isVolatile()) signature.append(" volatile");
        if (functionT->isRestrict()) signature.append(" restrict");
    }

    if (const auto* functionPT =
            dyn_cast_or_null<FunctionProtoType>(function->getType().getTypePtr())) {
        switch (functionPT->getRefQualifier()) {
            case RQ_LValue:
                signature.append(" &");
                break;
            case RQ_RValue:
                signature.append(" &&");
                break;
            default:
                break;
        }
    }

    return signature;
}

/**************************************************************************************************/

template <typename NodeType>
hyde::json GetParents(const ASTContext* n, const Decl* d) {
    hyde::json result = hyde::json::array();

    if (!n || !d) return result;

    auto parent = const_cast<ASTContext*>(n)->getParents(*d);

    while (true) {
        if (parent.size() == 0) break;
        if (parent.size() != 1) {
            assert(false && "What exactly is going on here?");
        }

        auto node = parent.begin()->get<NodeType>();
        if (node) {
            std::string name = node->getNameAsString();
            if (auto specialization = dyn_cast_or_null<ClassTemplateSpecializationDecl>(node)) {
                name += hyde::GetArgumentList(
                    n, specialization->getTemplateInstantiationArgs().asArray());
            } else if (auto cxxrecord = dyn_cast_or_null<CXXRecordDecl>(node)) {
                if (auto template_decl = cxxrecord->getDescribedClassTemplate()) {
                    name +=
                        hyde::GetArgumentList(n, template_decl->getTemplateParameters()->asArray());
                }
            } else {
                assert(false && "What kind of a parent is this?");
            }

            result.push_back(std::move(name));
        }

        parent = const_cast<ASTContext*>(n)->getParents(*parent.begin());
    }

    std::reverse(result.begin(), result.end());

    return result;
}

/**************************************************************************************************/

} // namespace

/**************************************************************************************************/

namespace hyde {

/**************************************************************************************************/

std::string GetSignature(const ASTContext* n, const Decl* d, bool fully_qualified, bool named_args) {
    if (!d) return "";

    const auto* nd = dyn_cast<NamedDecl>(d);
    if (!nd) return "";

    switch (nd->getKind()) {
        case Decl::Namespace:
        case Decl::Record:
        case Decl::CXXRecord:
        case Decl::Enum:
            return fully_qualified ? nd->getQualifiedNameAsString() : nd->getNameAsString();
        case Decl::CXXConstructor:
        case Decl::CXXDestructor:
        case Decl::CXXConversion:
        case Decl::CXXMethod:
        case Decl::Function:
            return ::GetSignature(n, dyn_cast_or_null<FunctionDecl>(nd), fully_qualified, named_args);
        case Decl::ObjCMethod:
            // ObjC Methods can not be overloaded, qualified name uniquely identifies
            // the method.
            return fully_qualified ? nd->getQualifiedNameAsString() : nd->getNameAsString();
        default:
            return "";
    }
}

/**************************************************************************************************/

json GetParentNamespaces(const ASTContext* n, const Decl* d) { return GetParents<NamespaceDecl>(n, d); }

/**************************************************************************************************/

json GetParentCXXRecords(const ASTContext* n, const Decl* d) { return GetParents<CXXRecordDecl>(n, d); }

/**************************************************************************************************/

json DetailCXXRecordDecl(const ASTContext* n, const clang::CXXRecordDecl* cxx) {
    json info = StandardDeclInfo(n, cxx);

    if (auto s = llvm::dyn_cast_or_null<ClassTemplateSpecializationDecl>(cxx)) {
        std::string arguments = GetArgumentList(n, s->getTemplateInstantiationArgs().asArray());
        info["qualified_name"] =
            static_cast<const std::string&>(info["qualified_name"]) + arguments;
        info["name"] = static_cast<const std::string&>(info["name"]) + arguments;
    } else if (auto template_decl = cxx->getDescribedClassTemplate()) {
        std::string arguments =
            GetArgumentList(n, template_decl->getTemplateParameters()->asArray());
        info["qualified_name"] =
            static_cast<const std::string&>(info["qualified_name"]) + arguments;
        info["name"] = static_cast<const std::string&>(info["name"]) + arguments;
    } else {
        assert(false && "What kind of a record is this?");
    }

    return info;
}

/**************************************************************************************************/

json GetTemplateParameters(const ASTContext* n, const clang::TemplateDecl* d) {
    json result = json::array();

    for (const auto& parameter_decl : *d->getTemplateParameters()) {
        json parameter_info = json::object();

        if (const auto& template_type = dyn_cast<TemplateTypeParmDecl>(parameter_decl)) {
            parameter_info["type"] =
                template_type->wasDeclaredWithTypename() ? "typename" : "class";
            if (template_type->isParameterPack()) parameter_info["parameter_pack"] = "true";
            parameter_info["name"] = template_type->getNameAsString();
        } else if (const auto& non_template_type =
                       dyn_cast<NonTypeTemplateParmDecl>(parameter_decl)) {
            parameter_info["type"] = hyde::to_string(n, non_template_type->getType());
            if (non_template_type->isParameterPack()) parameter_info["parameter_pack"] = "true";
            parameter_info["name"] = non_template_type->getNameAsString();
        } else if (const auto& template_template_type =
                       dyn_cast<TemplateTemplateParmDecl>(parameter_decl)) {
            parameter_info["type"] =
                ::to_string(n, template_template_type->getSourceRange(), false);
            if (template_template_type->isParameterPack())
                parameter_info["parameter_pack"] = "true";
            parameter_info["name"] = template_template_type->getNameAsString();
        } else {
            std::cerr << "What type is this thing, exactly?\n";
        }

        result.push_back(std::move(parameter_info));
    }

    return result;
}

/**************************************************************************************************/

json DetailFunctionDecl(const ASTContext* n, const FunctionDecl* f) {
    json info = StandardDeclInfo(n, f);
    info["return_type"] = hyde::to_string(n, f->getReturnType());
    info["arguments"] = json::array();
    info["signature"] = GetSignature(n, f, false, false);
    info["signature_with_names"] = GetSignature(n, f, false, true);
    if (f->isConstexpr()) info["constexpr"] = true;

    auto storage = f->getStorageClass();
    switch (storage) {
        case SC_Static:
            info["static"] = true;
        case SC_Extern:
            info["extern"] = true;
        default:
            break;
    }

    if (const auto* method = llvm::dyn_cast_or_null<CXXMethodDecl>(f)) {
        if (method->isConst()) info["const"] = true;
        if (method->isVolatile()) info["volatile"] = true;
        if (method->isStatic()) info["static"] = true;

        bool is_ctor = isa<CXXConstructorDecl>(method);
        bool is_dtor = isa<CXXDestructorDecl>(method);

        if (is_ctor || is_dtor) {
            if (is_ctor) info["is_ctor"] = true;
            if (is_dtor) info["is_dtor"] = true;
            if (method->isDeleted()) info["delete"] = true;
            if (method->isDefaulted()) info["default"] = true;
        }
    }

    if (auto template_decl = f->getDescribedFunctionTemplate()) {
        info["template_parameters"] = GetTemplateParameters(n, template_decl);
    }

    for (const auto& p : f->parameters()) {
        json argument = json::object();

        argument["type"] = to_string(n, p->getOriginalType());
        argument["name"] = p->getNameAsString();

        info["arguments"].push_back(std::move(argument));
    }
    return info;
}

/**************************************************************************************************/

std::string GetArgumentList(const ASTContext* n,
                            const llvm::ArrayRef<clang::TemplateArgument> args) {
    bool first = true;
    std::stringstream str;
    str << "<";

    // There is a better approach coming
    // https://en.cppreference.com/w/cpp/experimental/ostream_joiner/make_ostream_joiner

    for (const auto& arg : args) {
        if (first) {
            first = false;
        } else {
            str << ", ";
        }

        switch(arg.getKind()) {
            case TemplateArgument::ArgKind::Type: {
                str << to_string(n, arg.getAsType());
            } break;
            case TemplateArgument::ArgKind::Integral: {
                str << arg.getAsIntegral().toString(10);
            } break;
            case TemplateArgument::ArgKind::Template: {
                str << ::to_string(n, arg.getAsTemplate().getAsTemplateDecl());
            } break;
            case TemplateArgument::ArgKind::Expression: {
                str << ::to_string(n, arg.getAsExpr()->getSourceRange(), true);
            } break;
            default: {
                str << "XXXXXX";
            } break;
        }
    }

    str << ">";

    return str.str();
}

/**************************************************************************************************/

std::string GetArgumentList(const ASTContext*, const llvm::ArrayRef<clang::NamedDecl*> args) {
    std::size_t count{0};
    std::string result("<");

    for (const auto& arg : args) {
        if (count++) {
            result += ", ";
        }
        result += arg->getNameAsString();
    }

    result += ">";

    return result;
}

/**************************************************************************************************/

bool PathCheck(const std::vector<std::string>& paths, const Decl* d, ASTContext* n) {
    auto beginLoc = d->getBeginLoc();
    auto location = beginLoc.printToString(n->getSourceManager());
    std::string path = location.substr(0, location.find(':'));
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

/**************************************************************************************************/

} // namespace hyde

/**************************************************************************************************/
