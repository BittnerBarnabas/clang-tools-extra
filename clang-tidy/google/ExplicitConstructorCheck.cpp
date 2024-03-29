//===--- ExplicitConstructorCheck.cpp - clang-tidy ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "ExplicitConstructorCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Lex/Lexer.h"

using namespace clang::ast_matchers;

namespace clang {
namespace tidy {
namespace google {

void ExplicitConstructorCheck::registerMatchers(MatchFinder *Finder) {
  // Only register the matchers for C++; the functionality currently does not
  // provide any benefit to other languages, despite being benign.
  if (!getLangOpts().CPlusPlus)
    return;
  Finder->addMatcher(cxxConstructorDecl(unless(isInstantiated())).bind("ctor"),
                     this);
  Finder->addMatcher(cxxConversionDecl(unless(isExplicit())).bind("conversion"),
                     this);
}

// Looks for the token matching the predicate and returns the range of the found
// token including trailing whitespace.
static SourceRange FindToken(const SourceManager &Sources,
                             const LangOptions &LangOpts,
                             SourceLocation StartLoc, SourceLocation EndLoc,
                             bool (*Pred)(const Token &)) {
  if (StartLoc.isMacroID() || EndLoc.isMacroID())
    return SourceRange();
  FileID File = Sources.getFileID(Sources.getSpellingLoc(StartLoc));
  StringRef Buf = Sources.getBufferData(File);
  const char *StartChar = Sources.getCharacterData(StartLoc);
  Lexer Lex(StartLoc, LangOpts, StartChar, StartChar, Buf.end());
  Lex.SetCommentRetentionState(true);
  Token Tok;
  do {
    Lex.LexFromRawLexer(Tok);
    if (Pred(Tok)) {
      Token NextTok;
      Lex.LexFromRawLexer(NextTok);
      return SourceRange(Tok.getLocation(), NextTok.getLocation());
    }
  } while (Tok.isNot(tok::eof) && Tok.getLocation() < EndLoc);

  return SourceRange();
}

static bool declIsStdInitializerList(const NamedDecl *D) {
  // First use the fast getName() method to avoid unnecessary calls to the
  // slow getQualifiedNameAsString().
  return D->getName() == "initializer_list" &&
         D->getQualifiedNameAsString() == "std::initializer_list";
}

static bool isStdInitializerList(QualType Type) {
  Type = Type.getCanonicalType();
  if (const auto *TS = Type->getAs<TemplateSpecializationType>()) {
    if (const TemplateDecl *TD = TS->getTemplateName().getAsTemplateDecl())
      return declIsStdInitializerList(TD);
  }
  if (const auto *RT = Type->getAs<RecordType>()) {
    if (const auto *Specialization =
            dyn_cast<ClassTemplateSpecializationDecl>(RT->getDecl()))
      return declIsStdInitializerList(Specialization->getSpecializedTemplate());
  }
  return false;
}

void ExplicitConstructorCheck::check(const MatchFinder::MatchResult &Result) {
  constexpr char WarningMessage[] =
      "%0 must be marked explicit to avoid unintentional implicit conversions";

  if (const auto *Conversion =
      Result.Nodes.getNodeAs<CXXConversionDecl>("conversion")) {
    SourceLocation Loc = Conversion->getLocation();
    diag(Loc, WarningMessage)
        << Conversion << FixItHint::CreateInsertion(Loc, "explicit ");
    return;
  }

  const auto *Ctor = Result.Nodes.getNodeAs<CXXConstructorDecl>("ctor");
  // Do not be confused: isExplicit means 'explicit' keyword is present,
  // isImplicit means that it's a compiler-generated constructor.
  if (Ctor->isOutOfLine() || Ctor->isImplicit() || Ctor->isDeleted() ||
      Ctor->getNumParams() == 0 || Ctor->getMinRequiredArguments() > 1)
    return;

  bool takesInitializerList = isStdInitializerList(
      Ctor->getParamDecl(0)->getType().getNonReferenceType());
  if (Ctor->isExplicit() &&
      (Ctor->isCopyOrMoveConstructor() || takesInitializerList)) {
    auto isKWExplicit = [](const Token &Tok) {
      return Tok.is(tok::raw_identifier) &&
             Tok.getRawIdentifier() == "explicit";
    };
    SourceRange ExplicitTokenRange =
        FindToken(*Result.SourceManager, getLangOpts(),
                  Ctor->getOuterLocStart(), Ctor->getLocEnd(), isKWExplicit);
    StringRef ConstructorDescription;
    if (Ctor->isMoveConstructor())
      ConstructorDescription = "move";
    else if (Ctor->isCopyConstructor())
      ConstructorDescription = "copy";
    else
      ConstructorDescription = "initializer-list";

    auto Diag = diag(Ctor->getLocation(),
                     "%0 constructor should not be declared explicit")
                << ConstructorDescription;
    if (ExplicitTokenRange.isValid()) {
      Diag << FixItHint::CreateRemoval(
          CharSourceRange::getCharRange(ExplicitTokenRange));
    }
    return;
  }

  if (Ctor->isExplicit() || Ctor->isCopyOrMoveConstructor() ||
      takesInitializerList)
    return;

  bool SingleArgument =
      Ctor->getNumParams() == 1 && !Ctor->getParamDecl(0)->isParameterPack();
  SourceLocation Loc = Ctor->getLocation();
  diag(Loc, WarningMessage)
      << (SingleArgument
              ? "single-argument constructors"
              : "constructors that are callable with a single argument")
      << FixItHint::CreateInsertion(Loc, "explicit ");
}

} // namespace google
} // namespace tidy
} // namespace clang
