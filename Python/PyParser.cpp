/******************************************************************************
 * Copyright (c) 2014-2015 Leandro T. C. Melo (ltcmelo@gmail.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *****************************************************************************/

/*--------------------------*/
/*--- The UaiSo! Project ---*/
/*--------------------------*/

#include "Python/PyParser.h"
#include "Python/PyLexer.h"
#include "Ast/Ast.h"
#include "Common/Assert.h"
#include "Common/Trace__.h"
#include "Common/Util__.h"
#include "Parsing/ParsingContext.h"
#include <tuple>

#define TRACE_NAME "PyParser"

using namespace uaiso;

PyParser::PyParser()
{}

void PyParser::consumeToken()
{
    if (ahead_ == TK_EOP)
        return;

    // Track previous token location.
    lastLoc_ = lexer_->tokenLoc();
    lastLoc_.fileName_ = context_->fileName();
    ahead_ = lexer_->lex();
}

bool PyParser::maybeConsume(Token tk)
{
    if (ahead_ == tk) {
        consumeToken();
        return true;
    }
    return false;
}

void PyParser::skipTo(Token tk)
{
    while (!(ahead_ == tk || ahead_ == TK_EOP))
        consumeToken();
}

bool PyParser::match(Token tk)
{
    auto actual = ahead_;
    consumeToken(); // Move on, regardless of a match.
    if (actual != tk) {
        failMatch(false);
        return false;
    }
    return true;
}

void PyParser::failMatch(bool consume)
{
    // Location in the report is always from the lastly consumed token.
    if (consume)
        consumeToken();

    DEBUG_TRACE("error at %d:%d unexpected token (%s)\n",
                lastLoc_.lastLine_, lastLoc_.lastCol_, lastLoc_.fileName_.c_str());
    context_->trackReport(Diagnostic::UnexpectedToken, lastLoc_);
}

bool PyParser::isTestAhead() const
{
    switch (ahead_) {
    case TK_LAMBDA:
        return true;

    default:
        return isNonLambdaTestAhead();
    }
}

bool PyParser::isNonLambdaTestAhead() const
{
    switch (ahead_) {
    case TK_NOT:
        return true;

    default:
        return isExprAhead();
    }
}

bool PyParser::isExprAhead() const
{
    return isFactorAhead();
}

bool PyParser::isFactorAhead() const
{
    switch (ahead_) {
    case TK_PLUS:
    case TK_MINUS:
    case TK_TILDE:
        return true;

    default:
        return isAtomAhead();
    }
}

bool PyParser::isAtomAhead() const
{
    switch (ahead_) {
    case TK_LPAREN:
    case TK_LBRACKET:
    case TK_LBRACE:
    case TK_BACKTICK:
    case TK_IDENTIFIER:
    case TK_INTEGER_LITERAL:
    case TK_FLOAT_LITERAL:
    case TK_STRING_LITERAL:
    case TK_NULL_LITERAL:
    case TK_TRUE_LITERAL:
    case TK_FALSE_LITERAL:
        return true;

    default:
        return false;
    }
}

bool PyParser::isArgAhead() const
{
    switch (ahead_) {
    case TK_STAR:
    case TK_STAR_STAR:
        return true;

    default:
        return isTestAhead();
    }
}

bool PyParser::isSubscriptAhead() const
{
    switch (ahead_) {
    case TK_DOT_DOT_DOT:
    case TK_COLON:
        return true;

    default:
        return isTestAhead();
    }
}

bool PyParser::isNameAhead() const
{
    return ahead_ == TK_IDENTIFIER;
}

std::pair<PyParser::Precedence, std::unique_ptr<BinaryExprAst>>
PyParser::fetchPrecAhead() const
{
    switch (ahead_) {
    case TK_PIPE:
        return std::make_pair(Precedence::Or, makeAst<BitOrExprAst>());

    case TK_CARET:
        return std::make_pair(Precedence::Xor, makeAst<BitXorExprAst>());

    case TK_AMPER:
        return std::make_pair(Precedence::And, makeAst<BitAndExprAst>());

    case TK_LESS_LESS:
    case TK_GREATER_GREATER:
        return std::make_pair(Precedence::Shift, makeAst<ShiftExprAst>());

    case TK_PLUS:
        return std::make_pair(Precedence::Term, makeAst<AddExprAst>());

    case TK_MINUS:
        return std::make_pair(Precedence::Term, makeAst<SubExprAst>());

    case TK_STAR:
        return std::make_pair(Precedence::Factor, makeAst<MulExprAst>());

    case TK_SLASH:
        return std::make_pair(Precedence::Factor, makeAst<DivExprAst>());

    case TK_SLASH_SLASH:
        return std::make_pair(Precedence::Factor, makeAst<DivExprAst>());

    case TK_PERCENT:
        return std::make_pair(Precedence::Factor, makeAst<ModExprAst>());

    default:
        return std::make_pair(Precedence::Zero, std::unique_ptr<BinaryExprAst>());
    }
}

/*
 * file_input: (NEWLINE | stmt)* ENDMARKER
 */
bool PyParser::parse(Lexer* lexer, ParsingContext* context)
{
    UAISO_ASSERT(lexer, return false);
    UAISO_ASSERT(context && context->fileName(), return false);

    lexer_ = lexer;
    context_ = context;

    consumeToken();
    std::unique_ptr<StmtAstList> stmts;
    while (ahead_ != TK_EOP) {
        if (maybeConsume(TK_NEWLINE))
            continue;
        addToList(stmts, parseStmt().release());
    }

    if (stmts) {
        auto prog = makeAst<ProgramAst>();
        prog->setStmts(stmts.release());
        context->takeAst(std::unique_ptr<Ast>(prog.release()));
        return true;
    }

    return false;
}

/*
 * stmt: simple_stmt | compound_stmt
 * simple_stmt: small_stmt (';' small_stmt)* [';'] NEWLINE
 * small_stmt: expr_stmt | print_stmt  | del_stmt | pass_stmt | flow_stmt |
 *             import_stmt | global_stmt | exec_stmt | assert_stmt
 * compound_stmt: if_stmt | while_stmt | for_stmt | try_stmt | with_stmt |
 *                funcdef | classdef | decorated
 */
std::unique_ptr<StmtAst> PyParser::parseStmt()
{
    switch (ahead_) {
    case TK_PRINT:
    case TK_DELETE:
    case TK_PASS:
    case TK_IMPORT:
    case TK_GLOBAL:
    case TK_EXEC:
    case TK_ASSERT:
    case TK_BREAK:
    case TK_CONTINUE:
    case TK_RETURN:
    case TK_THROW:
    case TK_YIELD:
        return parseSimpleStmt();

    case TK_IF:
        return parseIfStmt();

    case TK_WHILE:
        return parseWhileStmt();

    case TK_FOR:
        return parseForStmt();

    case TK_TRY:
        return parseTryStmt();

    case TK_WITH:
        return parseWithStmt();

    case TK_FUNC:
        return parseFuncDef();

    case TK_CLASS:
        return parseClassDef();

    case TK_AT_SYMBOL:
        return parseDecorated();

    default:
        return parseSimpleStmt();
    }
}

/*
 * simple_stmt: small_stmt (';' small_stmt)* [';'] NEWLINE
 */
std::unique_ptr<StmtAst> PyParser::parseSimpleStmt()
{
    auto stmt = parseSmallStmt();
    if (maybeConsume(TK_NEWLINE))
        return stmt;

    auto block = makeAst<BlockStmtAst>();
    block->addStmt(stmt.release());
    while (maybeConsume(TK_SEMICOLON)) {
        if (ahead_ == TK_NEWLINE || ahead_ == TK_EOP)
            break;
        block->addStmt(parseSmallStmt().release());
    }

    if (!match(TK_NEWLINE)) {
        DEBUG_TRACE("parseSimpleStmt, skip to TK_NEWLINE\n");
        skipTo(TK_NEWLINE);
    }

    return Stmt(block.release());
}

/*
 * small_stmt: expr_stmt | print_stmt  | del_stmt | pass_stmt | flow_stmt |
 *             import_stmt | global_stmt | exec_stmt | assert_stmt
 */
std::unique_ptr<StmtAst> PyParser::parseSmallStmt()
{
    switch (ahead_) {
    case TK_PRINT:
        return parsePrintStmt();

    case TK_DELETE:
        return parseDelStmt();

    case TK_PASS:
        return parsePassStmt();

    case TK_IMPORT:
    case TK_FROM:
        return parseImportStmt();

    case TK_GLOBAL:
        return parseGlobalStmt();

    case TK_EXEC:
        return parseExecStmt();

    case TK_ASSERT:
        return parseAssertStmt();

    case TK_BREAK:
    case TK_CONTINUE:
    case TK_RETURN:
    case TK_THROW:
    case TK_YIELD:
        return parseFlowStmt();

    default:
        return parseExprStmt();
    }
}

/*
 * expr_stmt: testlist (augassign (yield_expr|testlist) |
 *                      ('=' (yield_expr|testlist))*)
 * augassign: ('+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' |
 *             '<<=' | '>>=' | '**=' | '//=')
 */
std::unique_ptr<StmtAst> PyParser::parseExprStmt()
{
    auto exprs = parseTestList();
    bool augmented = false;
    while (true) {
        switch (ahead_) {
        case TK_PLUS_EQUAL:
        case TK_MINUS_EQUAL:
        case TK_STAR_EQUAL:
        case TK_SLASH_EQUAL:
        case TK_PERCENT_EQUAL:
        case TK_AMPER_EQUAL:
        case TK_PIPE_EQUAL:
        case TK_CARET_EQUAL:
        case TK_LESS_LESS_EQUAL:
        case TK_GREATER_GREATER_EQUAL:
        case TK_STAR_STAR_EQUAL:
        case TK_SLASH_SLASH_EQUAL:
            augmented = true;
            // Fallthrough

        case TK_EQUAL: {
            consumeToken();
            auto assign = makeAst<AssignExprAst>();
            assign->setOprLoc(lastLoc_);
            assign->setExpr1s(exprs.release());
            if (ahead_ == TK_YIELD)
                assign->setExpr2s(ExprAstList::create(parseYieldExpr().release()));
            else
                assign->setExpr2s(parseTestList().release());
            exprs.reset(ExprAstList::create(assign.release()));

            if (augmented || ahead_ != TK_EQUAL)
                return Stmt(makeAstRaw<ExprStmtAst>()->setExprs(exprs.release()));

            break;
        }

        default:
            return Stmt(makeAstRaw<ExprStmtAst>()->setExprs(exprs.release()));
        }
    }
}

/*
 * print_stmt: 'print' ( [ test (',' test)* [','] ] |
 *                       '>>' test [ (',' test)+ [','] ] )
 */
std::unique_ptr<StmtAst> PyParser::parsePrintStmt()
{
    UAISO_ASSERT(ahead_ == TK_PRINT, return Stmt());

    match(TK_PRINT);
    auto print = makeAst<PrintExprAst>();
    print->setKeyLoc(lastLoc_);

    bool wantTest = false;
    if (maybeConsume(TK_GREATER_GREATER)) {
        print->setOprLoc(lastLoc_);
        print->addExpr(parseTest().release());
        if (!maybeConsume(TK_COMMA))
            return Stmt(makeAstRaw<ExprStmtAst>()->addExpr(print.release()));

        if (print->exprs())
            print->exprs()->lastSubList()->delim_ = lastLoc_;
        wantTest = true;
    }

    if (wantTest || isTestAhead()) {
        print->addExpr(parseTest().release());
        if (maybeConsume(TK_COMMA)) {
            if (print->exprs())
                print->exprs()->lastSubList()->delim_ = lastLoc_;
            if (isTestAhead())
                print->mergeExprs(parseTestList().release());
        }
    }

    return Stmt(makeAstRaw<ExprStmtAst>()->addExpr(print.release()));
}

/*
 * del_stmt: 'del' exprlist
 */
std::unique_ptr<StmtAst> PyParser::parseDelStmt()
{
    UAISO_ASSERT(ahead_ == TK_DELETE, return Stmt());

    match(TK_DELETE);
    auto del = makeAst<DelExprAst>();
    del->setKeyLoc(lastLoc_);
    del->setExprs(parseExprList().release());
    return Stmt(makeAstRaw<ExprStmtAst>()->addExpr(del.release()));
}

/*
 * pass_stmt: 'pass'
 */
std::unique_ptr<StmtAst> PyParser::parsePassStmt()
{
    UAISO_ASSERT(ahead_ == TK_PASS, return Stmt());

    match(TK_PASS);
    return Stmt(makeAstRaw<EmptyStmtAst>()->setKeyLoc(lastLoc_));
}

/*
 * flow_stmt: break_stmt | continue_stmt | return_stmt | raise_stmt | yield_stmt
 */
std::unique_ptr<StmtAst> PyParser::parseFlowStmt()
{
    UAISO_ASSERT(ahead_ == TK_BREAK
                 || ahead_ == TK_CONTINUE
                 || ahead_ == TK_RETURN
                 || ahead_ == TK_THROW
                 || ahead_ == TK_YIELD, return Stmt());

    switch (ahead_) {
    case TK_BREAK:
        return parseBreakStmt();

    case TK_CONTINUE:
        return parseContinueStmt();

    case TK_RETURN:
        return parseReturnStmt();

    case TK_THROW:
        return parseRaiseStmt();

    case TK_YIELD:
        return parseYieldStmt();

    default:
        failMatch(true);
        return Stmt();
    }
}

/*
 * import_stmt: import_name | import_from
 * import_name: 'import' dotted_as_names
 * import_from: 'from' ('.'* dotted_name | '.'+) 'import' sub_import
 */
std::unique_ptr<StmtAst> PyParser::parseImportStmt()
{
    UAISO_ASSERT(ahead_ == TK_IMPORT || ahead_ == TK_FROM, return Stmt());

    switch (ahead_) {
    case TK_IMPORT: {
        consumeToken();
        auto import = makeAst<ImportClauseDeclAst>();
        import->setKeyLoc(lastLoc_);
        do {
            if (import->modules_)
                import->modules_->delim_ = lastLoc_;
            auto module = makeAst<ImportModuleDeclAst>();
            module->setExpr(makeAstRaw<IdentExprAst>()->setName(parseDottedName().release()));
            if (maybeConsume(TK_AS)) {
                module->setAsLoc(lastLoc_);
                module->setLocalName(parseName().release());
            }
            import->addModule(module.release());
        } while (maybeConsume(TK_COMMA));
        if (!import->modules_)
            failMatch(true);

        return Stmt(makeAstRaw<DeclStmtAst>()->setDecl(import.release()));
    }

    case TK_FROM: {
        consumeToken();
        auto import = makeAst<ImportClauseDeclAst>();
        import->setKeyLoc(lastLoc_);

        // DESIGN: Store the dots (relative location info).
        bool wantName = true;
        while (maybeConsume(TK_DOT) || maybeConsume(TK_DOT_DOT_DOT))
            wantName = false;

        // Confusing rules... The thing comming after 'from' might be a module,
        // preceeded or not by dots (relative indication), or dots alone. In
        // this latter case, specifying the folder of the upcoming 'import'.
        // Details in PEP 0328:
        //
        // package/
        //     __init__.py
        //     subpackage1/
        //         __init__.py
        //         moduleX.py
        //         moduleY.py
        //     subpackage2/
        //         __init__.py
        //         moduleZ.py
        //     moduleA.py
        //
        // from .moduleY import spam
        // from .moduleY import spam as ham
        // from . import moduleY
        // from ..subpackage1 import moduleY
        // from ..subpackage2.moduleZ import eggs
        // from ..moduleA import foo
        // from ...package import bar
        // from ...sys import path

        if (wantName || isNameAhead()) {
            // A selective import, members specified after 'import'.
            auto module = makeAst<ImportModuleDeclAst>();
            module->setExpr(makeAstRaw<IdentExprAst>()->setName(parseDottedName().release()));
            match(TK_IMPORT);
            module->setSelectLoc(lastLoc_);
            module->setMembers(parseSubImports(true).release());
            import->addModule(module.release());
        } else {
            // An "ordinary" (non-selective) import, 'from' is just to indicate
            // the module after 'import' is relative.
            match(TK_IMPORT);
            import->setHintLoc(lastLoc_);
            import->setModules(parseSubImports(false).release());
        }

        return Stmt(makeAstRaw<DeclStmtAst>()->setDecl(import.release()));
    }

    default:
        failMatch(true);
        return Stmt();
    }
}

/*
 * sub_import: ('*' | '(' import_as_names ')' | import_as_names)
 * import_as_name: NAME ['as' NAME]
 * import_as_names: import_as_name (',' import_as_name)* [',']
 */
std::unique_ptr<DeclAstList> PyParser::parseSubImports(bool selective)
{
    bool wantParen = false;
    switch (ahead_) {
    case TK_STAR:
        consumeToken();
        if (selective) {
            auto star = makeAst<GenNameAst>();
            star->setGenLoc(lastLoc_);
            auto member = makeAst<ImportMemberDeclAst>();
            member->setActualName(star.release());
            return DeclList(DeclAstList::create(member.release()));
        }
        failMatch(false);
        return DeclList();

    case TK_LPAREN:
        consumeToken();
        wantParen = true;
        // Fallthrough

    default:
        DeclList decls;
        if (selective) {
            do {
                if (decls)
                    decls->delim_ = lastLoc_;
                auto member = makeAst<ImportMemberDeclAst>();
                member->setActualName(parseName().release());
                if (maybeConsume(TK_AS)) {
                    member->setAsLoc(lastLoc_);
                    member->setNickName(parseName().release());
                }
                addToList(decls, member.release());
            } while (maybeConsume(TK_COMMA));
        } else {
            do {
                if (decls)
                    decls->delim_ = lastLoc_;
                auto module = makeAst<ImportModuleDeclAst>();
                module->setExpr(makeAstRaw<IdentExprAst>()->setName(parseName().release()));
                if (maybeConsume(TK_AS)) {
                    module->setAsLoc(lastLoc_);
                    module->setLocalName(parseName().release());
                }
                addToList(decls, module.release());
            } while (maybeConsume(TK_COMMA));
        }
        if (wantParen && !match(TK_RPAREN)) {
            DEBUG_TRACE("parseSubImports, skip to TK_RPAREN\n");
            skipTo(TK_RPAREN);
        }
        if (!decls)
            failMatch(true);

        return decls;
    }
}

/*
 * global_stmt: 'global' NAME (',' NAME)*
 */
std::unique_ptr<StmtAst> PyParser::parseGlobalStmt()
{
    UAISO_ASSERT(ahead_ == TK_GLOBAL, return Stmt());

    match(TK_GLOBAL);
    auto group = makeAst<VarGroupDeclAst>();
    group->setKeyLoc(lastLoc_);
    do {
        if (group->decls_)
            group->decls_->delim_ = lastLoc_;
        group->addDecl(makeAstRaw<VarDeclAst>()->setName(parseName().release()));
    } while (maybeConsume(TK_COMMA));
    if (!group->decls())
        failMatch(true);

    return Stmt(makeAstRaw<DeclStmtAst>()->setDecl(group.release()));
}

/*
 * exec_stmt: 'exec' expr ['in' test [',' test]]
 */
std::unique_ptr<StmtAst> PyParser::parseExecStmt()
{
    UAISO_ASSERT(ahead_ == TK_EXEC, return Stmt());

    match(TK_EXEC);
    auto eval = makeAst<EvalStmtAst>();
    eval->setKeyLoc(lastLoc_);
    eval->setExpr(parseExpr().release());

    // TODO: Model remaining items.
    if (maybeConsume(TK_IN)) {
        parseTest();
        if (maybeConsume(TK_COMMA))
            parseTest();
    }

    return Stmt(eval.release());
}

/*
 * assert_stmt: 'assert' test [',' test]
 */
std::unique_ptr<StmtAst> PyParser::parseAssertStmt()
{
    UAISO_ASSERT(ahead_ == TK_ASSERT, return Stmt());

    match(TK_ASSERT);
    auto expr = makeAst<AssertExprAst>();
    expr->setKeyLoc(lastLoc_);
    expr->setExpr(parseTest().release());

    // TODO: Generalize "message" from AssertExprAst to "action".
    if (maybeConsume(TK_COMMA))
        parseTest();

    return Stmt(makeAstRaw<ExprStmtAst>()->addExpr(expr.release()));
}

/*
 * if_stmt: 'if' if_else
 */
std::unique_ptr<StmtAst> PyParser::parseIfStmt()
{
    UAISO_ASSERT(ahead_ == TK_IF, return Stmt());

    match(TK_IF);
    return parseIfElseIfStmt();
}

/*
 * if_else: test ':' suite ('elif' test ':' suite)* ['else' ':' suite]
 *
 * Note: 'elif' parsed as 'else' 'if'.
 */
std::unique_ptr<StmtAst> PyParser::parseIfElseIfStmt()
{
    auto ef = makeAst<IfStmtAst>();
    ef->setIfLoc(lastLoc_);
    ef->setExpr(parseTest().release());
    match(TK_COLON);
    ef->setThen(parseSuite().release());
    if (maybeConsume(TK_ELIF)) {
        ef->setElseLoc(lastLoc_);
        ef->setNotThen(parseIfElseIfStmt().release());
    } else if (maybeConsume(TK_ELSE)) {
        ef->setElseLoc(lastLoc_);
        match(TK_COLON);
        ef->setNotThen(parseSuite().release());
    }

    return Stmt(ef.release());
}

/*
 * while_stmt: 'while' test ':' suite ['else' ':' suite]
 */
std::unique_ptr<StmtAst> PyParser::parseWhileStmt()
{
    UAISO_ASSERT(ahead_ == TK_WHILE, return Stmt());

    match(TK_WHILE);
    auto whyle = makeAst<WhileStmtAst>();
    whyle->setWhileLoc(lastLoc_);
    whyle->setExpr(parseTest().release());
    match(TK_COLON);
    whyle->setStmt(parseSuite().release());

    if (maybeConsume(TK_ELSE)) {
        match(TK_COLON);
        // DESIGN: WhileStmtAst needs an else clause.
        parseSuite();
    }

    return Stmt(whyle.release());
}

/*
 * for_stmt: 'for' exprlist 'in' testlist ':' suite ['else' ':' suite]
 */
std::unique_ptr<StmtAst> PyParser::parseForStmt()
{
    UAISO_ASSERT(ahead_ == TK_FOR, return Stmt());

    match(TK_FOR);
    auto four = makeAst<ForeachStmtAst>();

    // Convert the exprs (when plain identifiers) into var decls.
    auto exprs = parseExprList();
    auto group = makeAst<VarGroupDeclAst>();
    for (auto expr : *exprs) {
        if (expr->kind() != Ast::Kind::IdentExpr)
            continue;
        auto name = IdentExpr_Cast(expr)->name_.release();
        group->addDecl(makeAstRaw<VarDeclAst>()->setName(name));
    }
    four->setDecl(group.release());

    match(TK_IN);
    auto test = parseTestList();
    // DESIGN: Extend ForeachStmtAst to allow exprs? For now, take the first one.
    if (test)
        four->setExpr(test->releaseHead().release());
    match(TK_COLON);
    four->setStmt(parseSuite().release());

    if (maybeConsume(TK_ELSE)) {
        match(TK_COLON);
        // DESIGN: ForeachStmtAst needs an else clause.
        parseSuite();
    }

    return Stmt(four.release());
}

/*
 * try_stmt: ('try' ':' suite
 *            ((except_clause ':' suite)+
 *             ['else' ':' suite]
 *             ['finally' ':' suite] |
 *             'finally' ':' suite))
 * except_clause: 'except' [test [('as' | ',') test]]
 */
std::unique_ptr<StmtAst> PyParser::parseTryStmt()
{
    UAISO_ASSERT(ahead_ == TK_TRY, return Stmt());

    match(TK_TRY);
    auto trie = makeAst<TryStmtAst>();
    trie->setKeyLoc(lastLoc_);
    match(TK_COLON);
    trie->setStmt(parseSuite().release());

    bool seenElse = false;
    while (true) {
        switch (ahead_) {
        case TK_CATCH: {
            consumeToken();
            auto catche = makeAst<CatchClauseStmtAst>();
            catche->setKeyLoc(lastLoc_);
            if (isTestAhead()) {
                auto test = parseTest();
                if (test) {
                    auto group = makeAst<ParamGroupDeclAst>();
                    if (test->kind() != Ast::Kind::IdentExpr) {
                        // TODO: This is an error, right?
                    } else {
                        group->setSpec(makeAstRaw<NamedSpecAst>()->setName(
                                IdentExpr_Cast(test.get())->name_.release()));
                    }
                    if (maybeConsume(TK_AS) || maybeConsume(TK_COMMA)) {
                        auto ident = parseTest();
                        if (ident) {
                            if (ident->kind() != Ast::Kind::IdentExpr) {
                                context_->trackReport(Diagnostic::NameRequired,
                                                      lastLoc_);
                            } else {
                                group->addDecl(makeAstRaw<ParamDeclAst>()
                                    ->setName(IdentExpr_Cast(ident.get())->name_.release()));
                            }
                        }
                    }
                    catche->setDecl(group.release());
                }
            }
            match(TK_COLON);
            catche->setStmt(parseSuite().release());
            trie->addCatch(catche.release());
            break;
        }

        case TK_FINALLY: {
            consumeToken();
            auto finaly = makeAst<FinallyClauseStmtAst>();
            finaly->setKeyLoc(lastLoc_);
            match(TK_COLON);
            finaly->setStmt(parseSuite().release());
            trie->setFinal(finaly.release());
            return Stmt(trie.release());
        }

        case TK_ELSE: {
            if (!trie->catchs() || seenElse) {
                // TODO: Once else is added to stmt, check against it (no seenElse).
                failMatch(true);
            } else {
                consumeToken();
                match(TK_COLON);
                // DESIGN: ForeachStmtAst needs an else clause.
                parseSuite();
                seenElse = true;
            }
            break;
        }

        default:
            // Check for 'except' only ('finally' always returns).
            if (!trie->catchs())
                failMatch(true);
            return Stmt(trie.release());
        }
    }
}

/*
 * with_stmt: 'with' with_item (',' with_item)* ':' suite
 */
std::unique_ptr<StmtAst> PyParser::parseWithStmt()
{
    UAISO_ASSERT(ahead_ == TK_WITH, return Stmt());

    match(TK_WITH);
    auto with = makeAst<WithStmtAst>();
    with->setKeyLoc(lastLoc_);
    with->setExprs(parseList<ExprAstList>(TK_COMMA,
                                          &PyParser::isTestAhead,
                                          &PyParser::parseWithItem,
                                          false).first.release());
    if (!with->exprs())
        failMatch(true);

    match(TK_COLON);
    with->setStmt(parseSuite().release());

    return Stmt(with.release());
}

/*
 * with_item: test ['as' expr]
 */
std::unique_ptr<ExprAst> PyParser::parseWithItem()
{
    auto test = parseTest();
    if (maybeConsume(TK_AS)) {
        auto assign = makeAst<AssignExprAst>();
        assign->setOprLoc(lastLoc_);
        assign->addExpr1(test.release());
        assign->addExpr2(parseExpr().release());
        return Expr(assign.release());
    }

    return test;
}

/*
 * varargslist: (fpdef ['=' test] ',')*
 *              ('*' NAME [',' '**' NAME] | '**' NAME) |
 *              fpdef ['=' test] (',' fpdef ['=' test])* [',']
 * fpdef: NAME | '(' fplist ')'
 * fplist: fpdef (',' fpdef)* [',']
 */
std::unique_ptr<DeclAst> PyParser::parseVarArgsList(bool wantParen)
{
    auto clause = makeAst<ParamClauseDeclAst>();
    if (wantParen) {
        match(TK_LPAREN);
        clause->setLDelimLoc(lastLoc_);
    }

    bool seenStar = false;
    bool takeComma = true;
    do {
        if (clause->decls_)
            clause->decls_->lastSubList()->delim_ = lastLoc_;

        auto group = makeAst<ParamGroupDeclAst>();
        group->setSpec(makeAstRaw<InferredSpecAst>());
        switch (ahead_) {
        case TK_IDENTIFIER: {
            if (seenStar)
                break;
            auto name = parseName();
            if (maybeConsume(TK_EQUAL)) {
                auto param = makeAst<ParamDeclAst__<ParamVariadic__Empty__,
                                                    ParamDefaultArg__>>();
                param->setAssignLoc(lastLoc_);
                param->setName(name.release());
                param->setDefaultArg(parseTest().release());
                group->addDecl(param.release());
            } else {
                group->addDecl(makeAstRaw<ParamDeclAst>()->setName(name.release()));
            }
            clause->addDecl(group.release());
            break;
        }

        case TK_STAR: {
            if (seenStar)
                break;
            seenStar = true;
            consumeToken();
            clause->addDecl(completeParam(std::move(group)).release());
            break;
        }

        case TK_STAR_STAR: {
            consumeToken();
            clause->addDecl(completeParam(std::move(group)).release());
            // Fallthrough
        }

        default:
            takeComma = false;
            break;
        }
    } while (takeComma && maybeConsume(TK_COMMA));

    if (wantParen && !match(TK_RPAREN)) {
        DEBUG_TRACE("parseFuncDef, skip to TK_RPAREN\n");
        skipTo(TK_RPAREN);
    }

    return Decl(clause.release());
}

/*
 * funcdef: 'def' NAME parameters ':' suite
 * parameters: '(' [varargslist] ')'
 */
std::unique_ptr<StmtAst> PyParser::parseFuncDef()
{
    UAISO_ASSERT(ahead_ == TK_FUNC, return Stmt());

    match(TK_FUNC);
    auto spec = makeAst<FuncSpecAst__<>>();
    spec->setKeyLoc(lastLoc_);
    auto decl = makeAst<FuncDeclAst>();
    decl->setName(parseName().release());
    spec->setParam(parseVarArgsList(true).release());

    match(TK_COLON);
    spec->setLDelimLoc(lastLoc_);
    decl->setStmt(parseSuite().release());
    decl->setSpec(spec.release());

    return Stmt(makeAstRaw<DeclStmtAst>()->setDecl(decl.release()));
}

/*
 * classdef: 'class' NAME ['(' [testlist] ')'] ':' suite
 */
std::unique_ptr<StmtAst> PyParser::parseClassDef()
{
    UAISO_ASSERT(ahead_ == TK_CLASS, return Stmt());

    match(TK_CLASS);
    auto spec = makeAst<RecordSpecAst>();
    spec->setKeyLoc(lastLoc_);
    auto decl = makeAst<RecordDeclAst>();
    decl->setName(parseName().release());

    if (maybeConsume(TK_LPAREN)) {
        auto bases = parseTestList();
        for (auto base : *bases) {
            if (base->kind() != Ast::Kind::IdentExpr)
                continue;
            auto name = IdentExpr_Cast(base)->name_.release();
            spec->addBase(makeAstRaw<BaseDeclAst>()->setName(name));
        }
        if (!match(TK_RPAREN)) {
            DEBUG_TRACE("parseClassDef, skip to TK_RPAREN\n");
            skipTo(TK_RPAREN);
        }
    }

    match(TK_COLON);
    spec->setLDelimLoc(lastLoc_);
    spec->setProto(parseSuite().release());
    decl->setSpec(spec.release());

    return Stmt(makeAstRaw<DeclStmtAst>()->setDecl(decl.release()));
}

/*
 * decorated: decorators (classdef | funcdef)
 * decorators: decorator+
 * decorator: '@' dotted_name [ '(' [arglist] ')' ] NEWLINE
 */
std::unique_ptr<StmtAst> PyParser::parseDecorated()
{
    UAISO_ASSERT(ahead_ == TK_AT_SYMBOL, return Stmt());

    match(TK_AT_SYMBOL);
    do {
        // TODO: Work on decorators.
        parseDottedName();
        if (maybeConsume(TK_LPAREN)) {
            parseArgList();
            if (!match(TK_RPAREN)) {
                DEBUG_TRACE("parseDecorated, skip to TK_RPAREN\n");
                skipTo(TK_RPAREN);
            }
        }
        if (!match(TK_NEWLINE)) {
            DEBUG_TRACE("parseDecorated, skip to TK_NEWLINE\n");
            skipTo(TK_NEWLINE);
        }
    } while (maybeConsume(TK_AT_SYMBOL));

    switch (ahead_) {
    case TK_CLASS:
        return parseClassDef();

    case TK_FUNC:
        return parseFuncDef();

    default:
        failMatch(true);
        return Stmt();
    }
}

/*
 * continue_stmt: 'continue'
 */
std::unique_ptr<StmtAst> PyParser::parseContinueStmt()
{
    UAISO_ASSERT(ahead_ == TK_CONTINUE, return Stmt());

    match(TK_CONTINUE);
    return Stmt(makeAstRaw<ContinueStmtAst>()->setKeyLoc(lastLoc_));
}

/*
 * break_stmt: 'break'
 */
std::unique_ptr<StmtAst> PyParser::parseBreakStmt()
{
    UAISO_ASSERT(ahead_ == TK_BREAK, return Stmt());

    match(TK_BREAK);
    return Stmt(makeAstRaw<BreakStmtAst>()->setKeyLoc(lastLoc_));
}

/*
 * yield_stmt: yield_expr
 */
std::unique_ptr<StmtAst> PyParser::parseYieldStmt()
{
    UAISO_ASSERT(ahead_ == TK_YIELD, return Stmt());

    return Stmt(makeAstRaw<YieldStmtAst>()->setExpr(parseYieldExpr().release()));
}

/*
 * raise_stmt: 'raise' [test [',' test [',' test]]]
 */
std::unique_ptr<StmtAst> PyParser::parseRaiseStmt()
{
    UAISO_ASSERT(ahead_ == TK_THROW, return Stmt());

    match(TK_THROW);
    auto stmt = makeAst<ThrowStmtAst>();
    stmt->setKeyLoc(lastLoc_);
    if (isTestAhead()) {
        stmt->setExpr(parseTest().release());

        // TODO: Model remaining exprs.
        if (maybeConsume(TK_COMMA)) {
            parseTest();
            if (maybeConsume(TK_COMMA))
                parseTest();
        }
    }

    return Stmt(stmt.release());
}

/*
 * return_stmt: 'return' [testlist]
 */
std::unique_ptr<StmtAst> PyParser::parseReturnStmt()
{
    UAISO_ASSERT(ahead_ == TK_RETURN, return Stmt());

    match(TK_RETURN);
    auto stmt = makeAst<ReturnStmtAst>();
    stmt->setKeyLoc(lastLoc_);
    if (isTestAhead())
        stmt->setExprs(parseTestList().release());
    return Stmt(stmt.release());
}

/*
 * suite: simple_stmt | NEWLINE INDENT stmt+ DEDENT
 */
std::unique_ptr<StmtAst> PyParser::parseSuite()
{
    if (!maybeConsume(TK_NEWLINE))
        return parseSimpleStmt();

    match(TK_INDENT);
    auto block = makeAst<BlockStmtAst>();
    block->addStmt(parseStmt().release());
    while (!maybeConsume(TK_DEDENT) && ahead_ != TK_EOP)
        block->addStmt(parseStmt().release());

    return Stmt(block.release());
}

/*
 * exprlist: expr (',' expr)* [',']
 */
std::unique_ptr<ExprAstList> PyParser::parseExprList()
{
    return parseList<ExprAstList>(TK_COMMA,
                                  &PyParser::isExprAhead,
                                  &PyParser::parseExpr).first;
}

/*
 * test: or_test ['if' or_test 'else' test] | lambdef
 */
std::unique_ptr<ExprAst> PyParser::parseTest()
{
    if (ahead_ == TK_LAMBDA)
        return parseLambdaDef();

    auto orTest = parseOrTest();
    if (maybeConsume(TK_IF)) {
        // Must be a conditional.
        auto condExpr = makeAst<CondExprAst>();
        condExpr->setYes(orTest.release());
        condExpr->setQuestionLoc(lastLoc_);
        condExpr->setCond(parseOrTest().release());
        match(TK_ELSE);
        condExpr->setDelimLoc(lastLoc_);
        condExpr->setNo(parseTest().release());
        orTest.reset(condExpr.release());
    }

    return orTest;
}

/*
 * old_test: or_test | old_lambdef
 */
std::unique_ptr<ExprAst> PyParser::parseOldTest()
{
    if (ahead_ == TK_LAMBDA)
        return parseOldLambdaDef();
    return parseOrTest();
}

/*
 * testlist: test (',' test)* [',']
 */
std::unique_ptr<ExprAstList> PyParser::parseTestList()
{
    return parseList<ExprAstList>(TK_COMMA,
                                  &PyParser::isTestAhead,
                                  &PyParser::parseTest).first;
}

/*
 * testlist1: test (',' test)*
 */
std::unique_ptr<ExprAstList> PyParser::parseTestList1()
{
    return parseList<ExprAstList>(TK_COMMA,
                                  &PyParser::isTestAhead,
                                  &PyParser::parseTest,
                                  false).first;
}

/*
 * testlist_safe: old_test [(',' old_test)+ [',']]
 */
std::unique_ptr<ExprAstList> PyParser::parseTestListSafe()
{
    // BUG: The trailing comma is allowed only for non-singleton lists. This
    // not handled by the helper parseList function currently.

    return parseList<ExprAstList>(TK_COMMA,
                                  &PyParser::isTestAhead,
                                  &PyParser::parseOldTest).first;
}

/*
 * argument: test [comp_for] | test '=' test
 */
std::unique_ptr<ExprAst> PyParser::parseArg()
{
    auto test = parseTest();
    switch (ahead_) {
    case TK_FOR: {
        auto listCompre = parseCompFor(makeAst<ListCompreExprAst>());
        listCompre->setExpr(test.release());
        return Expr(listCompre.release());
    }

    case TK_EQUAL:
        consumeToken();
        return completeAssignExpr(std::move(test), &PyParser::parseTest);

    default:
        return test;
    }
}

/*
 * arglist: (argument ',')* (argument [','] |
 *                           '*' test (',' argument)* [',' '**' test] |
 *                           '**' test)
 */
std::unique_ptr<ExprAstList> PyParser::parseArgList()
{
    ExprList args;
    if (isTestAhead()) {
        auto p = parseList<ExprAstList>(TK_COMMA, &PyParser::isTestAhead,
                                        &PyParser::parseArg);
        args = std::move(p.first);
        // If there's no trailing comma, this arg must be the last one.
        if (!p.second)
            return args;
    }

    bool wantStarStar = false;
    if (maybeConsume(TK_STAR)) {
        auto unpack = makeAst<UnpackExprAst>();
        unpack->setKeyLoc(lastLoc_);
        unpack->setExpr(parseTest().release());
        addToList(args, unpack.release());

        if (maybeConsume(TK_COMMA)) {
            if (args)
                args->lastSubList()->delim_ = lastLoc_;
            if (isTestAhead()) {
                auto p = parseList<ExprAstList>(TK_COMMA, &PyParser::isTestAhead,
                                                &PyParser::parseArg);
                mergeList(args, p.first.release());
                if (p.second)
                    wantStarStar = true;
            } else {
                wantStarStar = true;
            }
        }
    }

    if (maybeConsume(TK_STAR_STAR)) {
        auto unpack = makeAst<UnpackExprAst>();
        unpack->setKeyLoc(lastLoc_);
        unpack->setExpr(parseTest().release());
        addToList(args, unpack.release());
    } else if (wantStarStar) {
        failMatch(true);
    }

    return args;
}

/*
 * comp_iter: comp_for | comp_if
 * comp_for: 'for' exprlist 'in' or_test [comp_iter]
 */
std::unique_ptr<ListCompreExprAst>
PyParser::parseCompFor(std::unique_ptr<ListCompreExprAst> listCompre)
{
    UAISO_ASSERT(ahead_ == TK_FOR, return listCompre);
    UAISO_ASSERT(listCompre, return listCompre);

    match(TK_FOR);
    listCompre->addGen(makeAstRaw<GeneratorAst>());
    auto gen = listCompre->gens()->back();
    gen->setPatterns(parseExprList().release());

    match(TK_IN);
    gen->setOprLoc(lastLoc_);
    gen->setRange(parseOrTest().release());

    return completeListCompre(std::move(listCompre),
                              &PyParser::parseCompFor, &PyParser::parseCompIf);
}

/*
 * list_iter: list_for | list_if
 * list_for: 'for' exprlist 'in' testlist_safe [list_iter]
 */
std::unique_ptr<ListCompreExprAst>
PyParser::parseListFor(std::unique_ptr<ListCompreExprAst> listCompre)
{
    UAISO_ASSERT(ahead_ == TK_FOR, return listCompre);
    UAISO_ASSERT(listCompre, return listCompre);

    match(TK_FOR);
    listCompre->addGen(makeAstRaw<GeneratorAst>());
    auto gen = listCompre->gens()->back();
    gen->setPatterns(parseExprList().release());

    match(TK_IN);
    gen->setOprLoc(lastLoc_);

    // DESIGN: An expr list as the expr of a list comprehension sounds weird.
    // Perhaps wrap this into a comma expr?
    auto tests = parseTestListSafe();
    if (tests)
        gen->setRange(tests->releaseHead().release());

    return completeListCompre(std::move(listCompre),
                              &PyParser::parseListFor, &PyParser::parseListIf);
}

/*
 * comp_iter: comp_for | comp_if
 * comp_if: 'if' old_test [comp_iter]
 */
std::unique_ptr<ListCompreExprAst>
PyParser::parseCompIf(std::unique_ptr<ListCompreExprAst> listCompre)
{
    return parseListOrCompIf(std::move(listCompre),
                             &PyParser::parseCompFor, &PyParser::parseCompIf);
}

/*
 * list_iter: list_for | list_if
 * list_if: 'if' old_test [list_iter]
 */
std::unique_ptr<ListCompreExprAst>
PyParser::parseListIf(std::unique_ptr<ListCompreExprAst> listCompre)
{
    return parseListOrCompIf(std::move(listCompre),
                             &PyParser::parseListFor, &PyParser::parseListIf);
}

std::unique_ptr<ListCompreExprAst>
PyParser::parseListOrCompIf(std::unique_ptr<ListCompreExprAst> listCompre,
                            ListCompre (PyParser::*genFunc) (ListCompre),
                            ListCompre (PyParser::*filterFunc) (ListCompre))
{
    UAISO_ASSERT(ahead_ == TK_IF, return listCompre);
    UAISO_ASSERT(listCompre, return listCompre);
    UAISO_ASSERT(listCompre->gens(), return listCompre);

    match(TK_IF);
    auto gen = listCompre->gens()->back();
    gen->addFilter(parseOldTest().release());

    return completeListCompre(std::move(listCompre), genFunc, filterFunc);
}

std::unique_ptr<ListCompreExprAst>
PyParser::completeListCompre(ListCompre listCompre,
                             ListCompre (PyParser::*genFunc) (ListCompre),
                             ListCompre (PyParser::*filterFunc) (ListCompre))
{
    if (ahead_ == TK_FOR)
        return ((this)->*(genFunc))(std::move(listCompre));

    if (ahead_ == TK_IF)
        return ((this)->*(filterFunc))(std::move(listCompre));

    return std::move(listCompre);
}

/*
 * or_test: and_test ('or' and_test)*
 */
std::unique_ptr<ExprAst> PyParser::parseOrTest()
{
    auto andTest = parseAndTest();
    while (maybeConsume(TK_OR)) {
        andTest = completeBinaryExpr<LogicOrExprAst>(std::move(andTest),
                                                     &PyParser::parseAndTest);
    }
    return andTest;
}

/*
 * and_test: not_test ('and' not_test)*
 */
std::unique_ptr<ExprAst> PyParser::parseAndTest()
{
    auto notTest = parseNotTest();
    while (maybeConsume(TK_AND)) {
        notTest = completeBinaryExpr<LogicAndExprAst>(std::move(notTest),
                                                      &PyParser::parseNotTest);
    }
    return notTest;
}

/*
 * not_test: 'not' not_test | comparison
 */
std::unique_ptr<ExprAst> PyParser::parseNotTest()
{
    if (maybeConsume(TK_NOT)) {
        auto notTest = makeAst<LogicNotExprAst>();
        notTest->setOprLoc(lastLoc_);
        notTest->setExpr(parseNotTest().release());
        return Expr(notTest.release());
    }

    return parseComparison();
}

/*
 * comparison: expr (comp_op expr)*
 * comp_op: '<'|'>'|'=='|'>='|'<='|'<>'|'!='|'in'|'not' 'in'|'is'|'is' 'not'
 */
std::unique_ptr<ExprAst> PyParser::parseComparison()
{
    auto expr = parseExpr();
    while (true) {
        switch (ahead_) {
        case TK_LESS:
        case TK_GREATER:
        case TK_EQUAL_EQUAL:
        case TK_GREATER_EQUAL:
        case TK_LESS_EQUAL:
        case TK_LESS_GREATER:
        case TK_EXCLAM_EQUAL:
            consumeToken();
            expr = completeBinaryExpr<RelExprAst>(std::move(expr), &PyParser::parseExpr);
            break;

        case TK_IN:
            consumeToken();
            expr = completeBinaryExpr<InExprAst>(std::move(expr), &PyParser::parseExpr);
            break;

        case TK_IS: // May be followed by 'not'.
            consumeToken();
            maybeConsume(TK_NOT);
            expr = completeBinaryExpr<IsExprAst>(std::move(expr), &PyParser::parseExpr);
            break;

        case TK_NOT: // Must be followed by 'in'.
            consumeToken();
            match(TK_IN);
            expr = completeBinaryExpr<InExprAst>(std::move(expr), &PyParser::parseExpr);
            break;

        default:
            return expr;
        }
    }
}

/*
 * expr: xor_expr ('|' xor_expr)*
 * xor_expr: and_expr ('^' and_expr)*
 * and_expr: shift_expr ('&' shift_expr)*
 * shift_expr: arith_expr (('<<'|'>>') arith_expr)*
 * arith_expr: term (('+'|'-') term)*
 * term: factor (('*'|'/'|'%'|'//') factor)*
 */
std::unique_ptr<ExprAst> PyParser::parseExpr()
{
    return parseBinaryExpr(Precedence::Or);
}

std::unique_ptr<ExprAst> PyParser::parseBinaryExpr(Precedence curPrec)
{
    auto factor = parseFactor();
    while (true) {
        Precedence prec;
        std::unique_ptr<BinaryExprAst> bin;
        std::tie(prec, bin) = fetchPrecAhead();
        if (prec < curPrec)
            break;

        consumeToken();
        bin->setOprLoc(lastLoc_);
        bin->setExpr1(factor.release());
        bin->setExpr2(parseBinaryExpr(Precedence(curPrec + 1)).release());
        factor.reset(bin.release());
    }

    return factor;
}

/*
 * factor: ('+'|'-'|'~') factor | power
 */
std::unique_ptr<ExprAst> PyParser::parseFactor()
{
    std::unique_ptr<UnaryExprAst> expr;
    switch (ahead_) {
    case TK_PLUS:
        consumeToken();
        return completeUnaryExpr<PlusExprAst>(&PyParser::parseFactor);

    case TK_MINUS:
        consumeToken();
        return completeUnaryExpr<MinusExprAst>(&PyParser::parseFactor);

    case TK_TILDE:
        consumeToken();
        return completeUnaryExpr<BitCompExprAst>(&PyParser::parseFactor);

    default:
        return parsePower();
    }
}

/*
 * power: atom trailer* ['**' factor]
 * trailer: '(' [arglist] ')' | '[' subscriptlist ']' | '.' NAME
 */
std::unique_ptr<ExprAst> PyParser::parsePower()
{
    auto atom = parseAtom();
    bool trailer = true;
    do {
        switch (ahead_) {
        case TK_LPAREN: {
            consumeToken();
            auto call = makeAst<CallExprAst>();
            call->setLDelimLoc(lastLoc_);
            call->setBase(atom.release());
            if (isArgAhead())
                call->setArgs(parseArgList().release());
            if (!match(TK_RPAREN)) {
                DEBUG_TRACE("parsePower, skip to TK_RPAREN\n");
                skipTo(TK_RPAREN);
            }
            call->setRDelimLoc(lastLoc_);
            atom.reset(call.release());
            break;
        }

        case TK_LBRACKET: {
            consumeToken();
            auto arrayAccess = makeAst<ArraySliceExprAst>(); // See comment in this AST.
            arrayAccess->setLDelimLoc(lastLoc_);
            arrayAccess->setBase(atom.release());
            arrayAccess->setRange(parseSubscript().release());
            if (!match(TK_RBRACKET)) {
                DEBUG_TRACE("parsePower, skip to TK_RBRACKET\n");
                skipTo(TK_RBRACKET);
            }
            atom.reset(arrayAccess.release());
            break;
        }

        case TK_DOT: {
            consumeToken();
            auto member = makeAst<MemberAccessExprAst>();
            member->setOprLoc(lastLoc_);
            member->setExpr(atom.release());
            member->setName(parseName().release());
            atom.reset(member.release());
            break;
        }

        default:
            trailer = false;
            break;
        }
    } while (trailer);

    if (maybeConsume(TK_STAR_STAR))
        return completeBinaryExpr<PowerExprAst>(std::move(atom), &PyParser::parseFactor);

    return atom;
}

/*
 * atom: ('(' [yield_expr|testlist_comp] ')' |
 *        '[' [listmaker] ']' |
 *        '{' [dictorsetmaker] '}' |
 *        '`' testlist1 '`' |
 *        NAME | NUMBER | STRING+)
 *
 * Note: Accept 'None', 'True', and 'False, as well. The lexer process them
 * as keywords.
 */
std::unique_ptr<ExprAst> PyParser::parseAtom()
{
    switch (ahead_) {
    case TK_LPAREN:
        return parseWrappedOrTuple();

    case TK_LBRACE:
        return parseDictOrSetMaker();

    case TK_LBRACKET:
        return parseListMaker();

    case TK_BACKTICK: {
        consumeToken();
        auto str = makeAst<StrLitExprAst>();
        auto loc = lastLoc_;
        parseTestList1(); // Let it die.
        if (!match(TK_BACKTICK)) {
            DEBUG_TRACE("parseAtom, skip to TK_BACKTICK\n");
            skipTo(TK_BACKTICK);
        }
        str->setLitLoc(joinedLoc(loc, lastLoc_));
        return Expr(str.release());
    }

    case TK_IDENTIFIER: {
        auto ident = makeAst<IdentExprAst>();
        ident->setName(parseName().release());
        return Expr(ident.release());
    }

    case TK_INTEGER_LITERAL:
    case TK_FLOAT_LITERAL:
        consumeToken();
        return Expr(makeAstRaw<NumLitExprAst>()->setLitLoc(lastLoc_));

    case TK_NULL_LITERAL:
        consumeToken();
        return Expr(makeAstRaw<NullLitExprAst>()->setLitLoc(lastLoc_));

    case TK_TRUE_LITERAL:
    case TK_FALSE_LITERAL:
        consumeToken();
        return Expr(makeAstRaw<BoolLitExprAst>()->setLitLoc(lastLoc_));

    case TK_STRING_LITERAL:
        return parseStrLit();

    default:
        failMatch(true);
        return Expr();
    }
}

/*
 * subscript: '.' '.' '.' | test | [test] ':' [test] [sliceop]
 * sliceop: ':' [test]
 */
std::unique_ptr<ExprAst> PyParser::parseSubscript()
{
    switch (ahead_) {
    case TK_DOT_DOT_DOT:
        // TODO: Model this kind of slice.
        consumeToken();
        return makeAst<SubrangeExprAst>();

    case TK_COLON:
        consumeToken();
        return completeSubrangeExpr(Expr());

    default:
        auto test = parseTest();
        if (maybeConsume(TK_COLON))
            return completeSubrangeExpr(std::move(test));
        return test;
    }
}

/*
 * subscriptlist: subscript (',' subscript)* [',']
 */
std::unique_ptr<ExprAstList> PyParser::parseSubscriptList()
{
    return parseList<ExprAstList>(TK_COMMA,
                                  &PyParser::isSubscriptAhead,
                                  &PyParser::parseSubscript).first;
}

/*
 * dictorsetmaker: ( (test ':' test (comp_for | (',' test ':' test)* [','])) |
 *                   (test (comp_for | (',' test)* [','])) )
 *
 * Note: This will actually parse '{' [dictorsetmaker] '}'
 */
std::unique_ptr<ExprAst> PyParser::parseDictOrSetMaker()
{
    UAISO_ASSERT(ahead_ == TK_LBRACE, return Expr());

    match(TK_LBRACE);
    auto dictOrSet = makeAst<ArrayInitExprAst>();
    // DESIGN: Differentiate a set literal, '{1, 2}', from a list literal,
    // '(1, 2)'. Both are parsed as ArrayInitExprAst. Perhaps add a variety
    // to the AST or handle through Syntax.
    dictOrSet->setLDelimLoc(lastLoc_);
    if (maybeConsume(TK_RBRACE)) {
        dictOrSet->setRDelimLoc(lastLoc_);
        return Expr(dictOrSet.release());
    }

    auto test = parseTest();
    switch (ahead_) {
    case TK_COLON: {
        consumeToken();
        auto desig = makeAst<DesignateExprAst>();
        desig->setDelimLoc(lastLoc_);
        desig->setId(test.release());
        desig->setValue(parseTest().release());

        if (ahead_ == TK_FOR) {
            auto listCompre = parseListFor(makeAst<ListCompreExprAst>());
            listCompre->setLDelimLoc(dictOrSet->lDelimLoc());
            listCompre->setExpr(desig.release());
            if (!match(TK_RBRACE)) {
                DEBUG_TRACE("parseDictOrSetMaker, skip to TK_RBRACE\n");
                skipTo(TK_RBRACE);
            }
            listCompre->setRDelimLoc(lastLoc_);
            return Expr(listCompre.release());
        }

        dictOrSet->addInit(desig.release());
        while (maybeConsume(TK_COMMA)) {
            if (!isTestAhead())
                break;
            if (dictOrSet->inits())
                dictOrSet->inits()->delim_ = lastLoc_;
            desig = makeAst<DesignateExprAst>();
            desig->setId(parseTest().release());
            match(TK_COLON);
            desig->setDelimLoc(lastLoc_);
            desig->setValue(parseTest().release());
            dictOrSet->addInit(desig.release());
        }
        if (!match(TK_RBRACE)) {
            DEBUG_TRACE("parseDictOrSetMaker, skip to TK_RBRACE\n");
            skipTo(TK_RBRACE);
        }
        dictOrSet->setRDelimLoc(lastLoc_);
        return Expr(dictOrSet.release());
    }

    case TK_FOR:  {
        auto listCompre = parseListFor(makeAst<ListCompreExprAst>());
        listCompre->setLDelimLoc(dictOrSet->lDelimLoc());
        listCompre->setExpr(test.release());
        if (!match(TK_RBRACE)) {
            DEBUG_TRACE("parseDictOrSetMaker, skip to TK_RBRACE\n");
            skipTo(TK_RBRACE);
        }
        listCompre->setRDelimLoc(lastLoc_);
        return Expr(listCompre.release());
    }

    case TK_COMMA:
        consumeToken();
        if (dictOrSet->inits())
            dictOrSet->inits()->delim_ = lastLoc_;
        dictOrSet->addInit(test.release());
        dictOrSet->mergeInits(parseTestList().release());
        // Fallthrough

    default:
        if (test)
            dictOrSet->addInit(test.release());
        if (!match(TK_RBRACE)) {
            DEBUG_TRACE("parseDictOrSetMaker, skip to TK_RBRACE\n");
            skipTo(TK_RBRACE);
        }
        dictOrSet->setRDelimLoc(lastLoc_);
        return Expr(dictOrSet.release());
    }
}

/*
 * listmaker: test ( list_for | (',' test)* [','] )
 *
 * Note: This will actually parse '[' [listmaker] ']'
 */
std::unique_ptr<ExprAst> PyParser::parseListMaker()
{
    UAISO_ASSERT(ahead_ == TK_LBRACKET, return Expr());

    match(TK_LBRACKET);
    auto list = makeAst<ArrayInitExprAst>();
    list->setLDelimLoc(lastLoc_);
    if (maybeConsume(TK_RBRACKET)) {
        list->setRDelimLoc(lastLoc_);
        return Expr(list.release());
    }

    auto test = parseTest();
    switch (ahead_) {
    case TK_FOR: {
        auto listCompre = parseListFor(makeAst<ListCompreExprAst>());
        listCompre->setLDelimLoc(list->lDelimLoc());
        listCompre->setExpr(test.release());
        if (!match(TK_RBRACKET)) {
            DEBUG_TRACE("parseListMaker, skip to TK_RBRACKET\n");
            skipTo(TK_RBRACKET);
        }
        listCompre->setRDelimLoc(lastLoc_);
        return Expr(listCompre.release());
    }

    case TK_COMMA:
        consumeToken();
        if (list->inits())
            list->inits()->delim_ = lastLoc_;
        list->addInit(test.release());
        if (isTestAhead())
            list->mergeInits(parseTestList().release());
        // Fallthrough

    default:
        if (test)
            list->addInit(test.release());
        if (!match(TK_RBRACKET)) {
            DEBUG_TRACE("parseListMaker, skip to TK_RBRACKET\n");
            skipTo(TK_RBRACKET);
        }
        list->setRDelimLoc(lastLoc_);
        return Expr(list.release());
    }
}

/*
 * wrappedortuple: '(' [yield_expr|testlist_comp] ')'
 * testlist_comp: test ( comp_for | (',' test)* [','] )
 *
 * '()'     - Tuple
 * '(1)'    - Wrapped expr
 * '(1,)'   - Tuple
 * '(1, 2)' - Tuple
 */
std::unique_ptr<ExprAst> PyParser::parseWrappedOrTuple()
{
    UAISO_ASSERT(ahead_ == TK_LPAREN, return Expr());

    match(TK_LPAREN);
    auto tuple = makeAst<TupleLitExprAst>();
    tuple->setLDelimLoc(lastLoc_);
    if (maybeConsume(TK_RPAREN)) {
        tuple->setRDelimLoc(lastLoc_);
        return Expr(tuple.release());
    }

    if (ahead_ == TK_YIELD)
        return completeWrapped([this] () { return parseYieldExpr(); });

    auto test = parseTest();
    switch (ahead_) {
    case TK_FOR:
        return completeWrapped([&test, this] () {
            auto listCompre = parseListFor(makeAst<ListCompreExprAst>());
            listCompre->setExpr(test.release());
            return listCompre;
        });

    case TK_COMMA:
        consumeToken();
        if (tuple->inits())
            tuple->inits()->delim_ = lastLoc_;
        tuple->addInit(test.release());
        if (isTestAhead())
            mergeList(tuple->inits_, parseTestList().release());
        if (!match(TK_RPAREN)) {
            DEBUG_TRACE("parseWrappedOrTuple, skip to TK_RPAREN\n");
            skipTo(TK_RPAREN);
        }
        tuple->setRDelimLoc(lastLoc_);
        return Expr(tuple.release());

    default:
        return completeWrapped([&test, this]() { return std::move(test); });
    }
}

/*
 * yield_expr: 'yield' [testlist]
 */
std::unique_ptr<ExprAst> PyParser::parseYieldExpr()
{
    UAISO_ASSERT(ahead_ == TK_YIELD, return Expr());

    match(TK_YIELD);
    auto yield = makeAst<YieldExprAst>();
    yield->setKeyLoc(lastLoc_);
    if (isTestAhead())
        yield->setExprs(parseTestList().release());
    return Expr(yield.release());
}

/*
 * lambdef: 'lambda' [varargslist] ':' test
 */
std::unique_ptr<ExprAst> PyParser::parseLambdaDef()
{
    return parseLambdaCore(&PyParser::parseTest);
}

/*
 * old_lambdef: 'lambda' [varargslist] ':' old_test
 */
std::unique_ptr<ExprAst> PyParser::parseOldLambdaDef()
{
    return parseLambdaCore(&PyParser::parseOldTest);
}

std::unique_ptr<ExprAst> PyParser::parseLambdaCore(Expr (PyParser::*parseFunc) ())
{
    UAISO_ASSERT(ahead_ == TK_LAMBDA, return Expr());

    match(TK_LAMBDA);
    auto spec = makeAst<FuncSpecAst__<>>();
    spec->setKeyLoc(lastLoc_);
    spec->setParam(parseVarArgsList(false).release());

    match(TK_COLON);
    spec->setLDelimLoc(lastLoc_);
    auto lambda = makeAst<FuncLitExprAst>();
    lambda->setSpec(spec.release());
    lambda->setStmt(makeAstRaw<ExprStmtAst>()->addExpr(((this)->*(parseFunc))().release()));

    return Expr(lambda.release());
}

/*
 * dotted_name: NAME ('.' NAME)*
 * dotted_as_name: dotted_name ['as' NAME]
 * dotted_as_names: dotted_as_name (',' dotted_as_name)*
 */
std::unique_ptr<NameAst> PyParser::parseDottedName()
{
    auto name = makeAst<NestedNameAst>();
    name->setNames(parseList<NameAstList>(TK_DOT,
                                          &PyParser::isNameAhead,
                                          &PyParser::parseName,
                                          false).first.release());
    return Name(name.release());
}

std::unique_ptr<NameAst> PyParser::parseName()
{
    // A name AST may only be created if the match succeeds. Otherwise,
    // it will have no corresponding identifier in the lexeme map.
    if (match(TK_IDENTIFIER)) {
        auto name = makeAst<SimpleNameAst>();
        name->setNameLoc(lastLoc_);
        return Name(name.release());
    }
    return Name();
}

std::unique_ptr<ExprAst> PyParser::parseStrLit()
{
    UAISO_ASSERT(ahead_ == TK_STRING_LITERAL, return Expr());

    match(TK_STRING_LITERAL);
    auto str = makeAst<StrLitExprAst>();
    str->setLitLoc(lastLoc_);
    if (ahead_ == TK_STRING_LITERAL) {
        auto concat = makeAst<ConcatExprAst>();
        concat->setExpr1(str.release());
        concat->setExpr2(parseStrLit().release());
        return Expr(concat.release());
    }

    return Expr(str.release());
}

std::unique_ptr<ExprAst> PyParser::completeSubrangeExpr(Expr expr)
{
    auto range = makeAst<SubrangeExprAst>();
    range->setDelim1Loc(lastLoc_);
    range->setLow(expr.release());
    if (isTestAhead())
        range->setHi(parseTest().release());
    if (maybeConsume(TK_COLON))
        range->setDelim2Loc(lastLoc_);
    if (isTestAhead())
        range->setMax(parseTest().release()); // TODO: In Go, this is max.
    return Expr(range.release());
}

std::unique_ptr<ExprAst>
PyParser::completeWrapped(const std::function<Expr ()> exprFunc)
{
    auto wrap = makeAst<WrappedExprAst>();
    wrap->setLDelimLoc(lastLoc_);
    wrap->setExpr(exprFunc().release());
    if (!match(TK_RPAREN)) {
        DEBUG_TRACE("completeWrapped, skip to TK_PAREN\n");
        skipTo(TK_RPAREN);
    }
    wrap->setRDelimLoc(lastLoc_);
    return Expr(wrap.release());
}

template <class UnaryAstT> std::unique_ptr<ExprAst>
PyParser::completeUnaryExpr(Expr (PyParser::*parseFunc) ())
{
    auto unary = makeAst<UnaryAstT>();
    unary->setOprLoc(lastLoc_);
    unary->setExpr((((this)->*(parseFunc))()).release());
    return Expr(unary.release());
}

template <class BinaryAstT> std::unique_ptr<ExprAst>
PyParser::completeBinaryExpr(Expr expr, Expr (PyParser::*parseFunc) ())
{
    auto bin = makeAst<BinaryAstT>();
    bin->setOprLoc(lastLoc_);
    bin->setExpr1(expr.release());
    bin->setExpr2((((this)->*(parseFunc))()).release());
    return Expr(bin.release());
}

std::unique_ptr<ExprAst>
PyParser::completeAssignExpr(Expr expr, Expr (PyParser::*parseFunc) ())
{
    auto assign = makeAst<AssignExprAst>();
    assign->setOprLoc(lastLoc_);
    assign->setExpr1s(ExprAstList::create(expr.release()));
    assign->setExpr2s(ExprAstList::create((((this)->*(parseFunc))()).release()));
    return Expr(assign.release());
}

std::unique_ptr<DeclAst> PyParser::completeParam(ParamGroup group)
{
    auto param = makeAst<ParamDeclAst__<ParamVariadic__>>();
    param->setVariadicLoc(lastLoc_);
    param->setName(parseName().release());
    group->addDecl(param.release());
    return std::move(group);
}

template <class AstListT> std::pair<std::unique_ptr<AstListT>, bool>
PyParser::parseList(Token tk,
                    bool (PyParser::*checkAhead) () const,
                    std::unique_ptr<typename AstListT::AstType> (PyParser::*parseFunc) (),
                    bool trailingOK)
{
    using List = std::unique_ptr<AstListT>;

    List list;
    auto item = ((this)->*(parseFunc))();
    addToList(list, item.release());
    while (maybeConsume(tk)) {
        if (list)
            list->delim_ = lastLoc_;
        if (trailingOK && !(((this)->*(checkAhead))()))
            return std::make_pair(std::move(list), true);

        addToList(list, (((this)->*(parseFunc))()).release());
    }

    return std::make_pair(std::move(list), false);
}
