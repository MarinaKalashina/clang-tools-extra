//===--- HeaderGuard.cpp - clang-tidy -------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "HeaderGuard.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/Path.h"

namespace clang {
namespace tidy {

/// \brief canonicalize a path by removing ./ and ../ components.
// FIXME: Consider moving this to llvm::sys::path.
static std::string cleanPath(StringRef Path) {
  SmallString<256> NewPath;
  for (auto I = llvm::sys::path::begin(Path), E = llvm::sys::path::end(Path);
       I != E; ++I) {
    if (*I == ".")
      continue;
    if (*I == "..") {
      // Drop the last component.
      NewPath.resize(llvm::sys::path::parent_path(NewPath).size());
    } else {
      if (!NewPath.empty())
        NewPath += '/';
      NewPath += *I;
    }
  }
  return NewPath.str();
}

namespace {
class HeaderGuardPPCallbacks : public PPCallbacks {
public:
  explicit HeaderGuardPPCallbacks(Preprocessor *PP, HeaderGuardCheck *Check)
      : PP(PP), Check(Check) {}

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind FileType,
                   FileID PrevFID) override {
    // Record all files we enter. We'll need them to diagnose headers without
    // guards.
    SourceManager &SM = PP->getSourceManager();
    if (Reason == EnterFile && FileType == SrcMgr::C_User) {
      if (const FileEntry *FE = SM.getFileEntryForID(SM.getFileID(Loc))) {
        std::string FileName = cleanPath(FE->getName());
        Files[FileName] = FE;
      }
    }
  }

  void Ifndef(SourceLocation Loc, const Token &MacroNameTok,
              const MacroDirective *MD) override {
    if (MD)
      return;

    // Record #ifndefs that succeeded. We also need the Location of the Name.
    Ifndefs[MacroNameTok.getIdentifierInfo()] =
        std::make_pair(Loc, MacroNameTok.getLocation());
  }

  void MacroDefined(const Token &MacroNameTok,
                    const MacroDirective *MD) override {
    // Record all defined macros. We store the whole token to get info on the
    // name later.
    Macros.emplace_back(MacroNameTok, MD);
  }

  void Endif(SourceLocation Loc, SourceLocation IfLoc) override {
    // Record all #endif and the corresponding #ifs (including #ifndefs).
    EndIfs[IfLoc] = Loc;
  }

  void EndOfMainFile() override {
    // Now that we have all this information from the preprocessor, use it!
    SourceManager &SM = PP->getSourceManager();

    for (const auto &MacroEntry : Macros) {
      const MacroInfo *MI = MacroEntry.second->getMacroInfo();

      // We use clang's header guard detection. This has the advantage of also
      // emitting a warning for cases where a pseudo header guard is found but
      // preceeded by something blocking the header guard optimization.
      if (!MI->isUsedForHeaderGuard())
        continue;

      const FileEntry *FE =
          SM.getFileEntryForID(SM.getFileID(MI->getDefinitionLoc()));
      std::string FileName = cleanPath(FE->getName());
      Files.erase(FileName);

      // See if we should check and fix this header guard.
      if (!Check->shouldFixHeaderGuard(FileName))
        continue;

      // Look up Locations for this guard.
      SourceLocation Ifndef =
          Ifndefs[MacroEntry.first.getIdentifierInfo()].second;
      SourceLocation Define = MacroEntry.first.getLocation();
      SourceLocation EndIf =
          EndIfs[Ifndefs[MacroEntry.first.getIdentifierInfo()].first];

      // If the macro Name is not equal to what we can compute, correct it in
      // the
      // #ifndef and #define.
      StringRef CurHeaderGuard =
          MacroEntry.first.getIdentifierInfo()->getName();
      std::string NewGuard =
          checkHeaderGuardDefinition(Ifndef, Define, FileName, CurHeaderGuard);

      // Now look at the #endif. We want a comment with the header guard. Fix it
      // at the slightest deviation.
      if (Check->shouldSuggestEndifComment(FileName))
        checkEndifComment(EndIf, NewGuard);
    }

    // Emit warnings for headers that are missing guards.
    checkGuardlessHeaders();

    // Clear all state.
    Macros.clear();
    Files.clear();
    Ifndefs.clear();
    EndIfs.clear();
  }

  /// \brief Look for header guards that don't match the preferred style. Emit
  /// fix-its and return the suggested header guard (or the original if no
  /// change was made.
  std::string checkHeaderGuardDefinition(SourceLocation Ifndef,
                                         SourceLocation Define,
                                         StringRef FileName,
                                         StringRef CurHeaderGuard) {
    std::string CPPVar = Check->getHeaderGuard(FileName, CurHeaderGuard);
    std::string CPPVarUnder = CPPVar + '_'; // Allow a trailing underscore.
    if (Ifndef.isValid() && CurHeaderGuard != CPPVar &&
        CurHeaderGuard != CPPVarUnder) {
      Check->diag(Ifndef, "header guard does not follow preferred style")
          << FixItHint::CreateReplacement(
                 CharSourceRange::getTokenRange(
                     Ifndef, Ifndef.getLocWithOffset(CurHeaderGuard.size())),
                 CPPVar)
          << FixItHint::CreateReplacement(
                 CharSourceRange::getTokenRange(
                     Define, Define.getLocWithOffset(CurHeaderGuard.size())),
                 CPPVar);
      return CPPVar;
    }
    return CurHeaderGuard;
  }

  /// \brief Checks the comment after the #endif of a header guard and fixes it
  /// if it doesn't match \c HeaderGuard.
  void checkEndifComment(SourceLocation EndIf, StringRef HeaderGuard) {
    const char *EndIfData = PP->getSourceManager().getCharacterData(EndIf);
    size_t EndIfLen = std::strcspn(EndIfData, "\r\n");

    StringRef EndIfStr(EndIfData, EndIfLen);
    if (EndIf.isValid() && !EndIfStr.endswith("// " + HeaderGuard.str()) &&
        !EndIfStr.endswith("/* " + HeaderGuard.str() + " */")) {
      std::string Correct = "endif  // " + HeaderGuard.str();
      Check->diag(EndIf, "#endif for a header guard should reference the "
                         "guard macro in a comment")
          << FixItHint::CreateReplacement(
              CharSourceRange::getCharRange(EndIf,
                                            EndIf.getLocWithOffset(EndIfLen)),
              Correct);
    }
  }

  /// \brief Looks for files that were visited but didn't have a header guard.
  /// Emits a warning with fixits suggesting adding one.
  void checkGuardlessHeaders() {
    // Look for header files that didn't have a header guard. Emit a warning and
    // fix-its to add the guard.
    // TODO: Insert the guard after top comments.
    for (const auto &FE : Files) {
      StringRef FileName = FE.getKey();
      if (!Check->shouldSuggestToAddHeaderGuard(FileName))
        continue;

      SourceManager &SM = PP->getSourceManager();
      FileID FID = SM.translateFile(FE.getValue());
      SourceLocation StartLoc = SM.getLocForStartOfFile(FID);
      if (StartLoc.isInvalid())
        continue;

      std::string CPPVar = Check->getHeaderGuard(FileName);
      std::string CPPVarUnder = CPPVar + '_'; // Allow a trailing underscore.
      // If there is a header guard macro but it's not in the topmost position
      // emit a plain warning without fix-its. This often happens when the guard
      // macro is preceeded by includes.
      // FIXME: Can we move it into the right spot?
      bool SeenMacro = false;
      for (const auto &MacroEntry : Macros) {
        StringRef Name = MacroEntry.first.getIdentifierInfo()->getName();
        SourceLocation DefineLoc = MacroEntry.first.getLocation();
        if ((Name == CPPVar || Name == CPPVarUnder) &&
            SM.isWrittenInSameFile(StartLoc, DefineLoc)) {
          Check->diag(
              DefineLoc,
              "Header guard after code/includes. Consider moving it up.");
          SeenMacro = true;
          break;
        }
      }

      if (SeenMacro)
        continue;

      Check->diag(StartLoc, "header is missing header guard")
          << FixItHint::CreateInsertion(
                 StartLoc, "#ifndef " + CPPVar + "\n#define " + CPPVar + "\n\n")
          << FixItHint::CreateInsertion(
                 SM.getLocForEndOfFile(FID),
                 Check->shouldSuggestEndifComment(FileName)
                     ? "\n#endif  // " + CPPVar + "\n"
                     : "\n#endif\n");
    }
  }

private:
  std::vector<std::pair<Token, const MacroDirective *>> Macros;
  llvm::StringMap<const FileEntry *> Files;
  std::map<const IdentifierInfo *, std::pair<SourceLocation, SourceLocation>>
      Ifndefs;
  std::map<SourceLocation, SourceLocation> EndIfs;

  Preprocessor *PP;
  HeaderGuardCheck *Check;
};
} // namespace

void HeaderGuardCheck::registerPPCallbacks(CompilerInstance &Compiler) {
  Compiler.getPreprocessor().addPPCallbacks(
      new HeaderGuardPPCallbacks(&Compiler.getPreprocessor(), this));
}

bool HeaderGuardCheck::shouldSuggestEndifComment(StringRef FileName) {
  return FileName.endswith(".h");
}

bool HeaderGuardCheck::shouldFixHeaderGuard(StringRef FileName) { return true; }

bool HeaderGuardCheck::shouldSuggestToAddHeaderGuard(StringRef FileName) {
  return FileName.endswith(".h");
}

} // namespace tidy
} // namespace clang
