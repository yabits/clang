//===--- EBC.cpp - EBC ToolChain Implementation -*- C++ -*-===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "EBC.h"
#include "CommonArgs.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

void EBC::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                               const InputInfo &Output,
                               const InputInfoList &Inputs,
                               const ArgList &Args,
                               const char *LinkingOutput) const {
  ArgStringList CmdArgs;

  auto &TC = static_cast<const toolchains::EBCToolChain &>(getToolChain());

  assert((Output.isFilename() || Output.isNothing()) && "invalid output");
  if (Output.isFilename())
    CmdArgs.push_back(
        Args.MakeArgString(std::string("-out:") + Output.getFilename()));

  CmdArgs.push_back(Args.MakeArgString(std::string("-entry:main")));

  CmdArgs.push_back("-nologo");

  Args.AddAllArgValues(CmdArgs, options::OPT__SLASH_link);

  // Add filenames, libraries, and other linker inputs.
  for (const auto &Input : Inputs) {
    if (Input.isFilename()) {
      CmdArgs.push_back(Input.getFilename());
      continue;
    }

    const Arg &A = Input.getInputArg();

    // Render -l options differently for the MSVC linker.
    if (A.getOption().matches(options::OPT_l)) {
      StringRef Lib = A.getValue();
      const char *LinkLibArg;
      if (Lib.endswith(".lib"))
        LinkLibArg = Args.MakeArgString(Lib);
      else
        LinkLibArg = Args.MakeArgString(Lib + ".lib");
      CmdArgs.push_back(LinkLibArg);
      continue;
    }

    // Otherwise, this is some other kind of linker input option like -Wl, -z,
    // or -L. Render it even if MSVC doesn't understand it.
    A.renderAsInput(Args, CmdArgs);
  }

  TC.addProfileRTLibs(Args, CmdArgs);

  // We need to special case some linker paths. In the case of lld, we need to
  // translate 'lld' into 'lld-link'.
  llvm::SmallString<128> linkPath;
  StringRef Linker = Args.getLastArgValue(options::OPT_fuse_ld_EQ, "lld-link");
  if (Linker.equals_lower("lld"))
    Linker = "lld-link";

  linkPath = TC.GetProgramPath(Linker.str().c_str());

  auto LinkCmd = llvm::make_unique<Command>(
      JA, *this, Args.MakeArgString(linkPath), CmdArgs, Inputs);
  C.addCommand(std::move(LinkCmd));
}

EBCToolChain::EBCToolChain(const Driver &D, const llvm::Triple &Triple,
                           const ArgList &Args)
    : ToolChain(D, Triple, Args) {
  getProgramPaths().push_back(getDriver().getInstalledDir());
  if (getDriver().getInstalledDir() != getDriver().Dir)
    getProgramPaths().push_back(getDriver().Dir);
}

Tool *EBCToolChain::buildLinker() const {
  return new tools::EBC::Linker(*this);
}

bool EBCToolChain::IsIntegratedAssemblerDefault() const {
  return true;
}

bool EBCToolChain::isPICDefault() const {
  return false;
}

bool EBCToolChain::isPIEDefault() const {
  return false;
}

bool EBCToolChain::isPICDefaultForced() const {
  return false;
}

void EBCToolChain::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                             ArgStringList &CC1Args) const {
  // FIXME: Add system include paths.
}

void EBCToolChain::AddClangCXXStdlibIncludeArgs(const ArgList &DriverArgs,
                                                ArgStringList &CC1Args) const {
  // FIXME: C++ is not supported yet.
}

static void TranslateOptArg(Arg *A, llvm::opt::DerivedArgList &DAL,
                            const char *ExpandChar, const OptTable &Opts) {
  assert(A->getOption().matches(options::OPT__SLASH_O));

  StringRef OptStr = A->getValue();
  for (size_t I = 0, E = OptStr.size(); I != E; ++I) {
    const char &OptChar = *(OptStr.data() + I);
    switch (OptChar) {
    default:
      break;
    case '1':
    case '2':
    case 'x':
    case 'd':
      // Ignore /O[12xd] flags that aren't the last one on the command line.
      // Only the last one gets expanded.
      if (&OptChar != ExpandChar) {
        A->claim();
        break;
      }
      if (OptChar == 'd') {
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_O0));
      } else {
        if (OptChar == '1') {
          DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "s");
        } else if (OptChar == '2' || OptChar == 'x') {
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fbuiltin));
          DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "2");
        }
        if (!DAL.hasArgNoClaim(options::OPT_fno_omit_frame_pointer))
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fomit_frame_pointer));
        if (OptChar == '1' || OptChar == '2')
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_ffunction_sections));
      }
      break;
    case 'b':
      if (I + 1 != E && isdigit(OptStr[I + 1])) {
        switch (OptStr[I + 1]) {
        case '0':
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_inline));
          break;
        case '1':
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_finline_hint_functions));
          break;
        case '2':
          DAL.AddFlagArg(A, Opts.getOption(options::OPT_finline_functions));
          break;
        }
        ++I;
      }
      break;
    case 'g':
      A->claim();
      break;
    case 'i':
      if (I + 1 != E && OptStr[I + 1] == '-') {
        ++I;
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_builtin));
      } else {
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_fbuiltin));
      }
      break;
    case 's':
      DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "s");
      break;
    case 't':
      DAL.AddJoinedArg(A, Opts.getOption(options::OPT_O), "2");
      break;
    case 'y': {
      bool OmitFramePointer = true;
      if (I + 1 != E && OptStr[I + 1] == '-') {
        OmitFramePointer = false;
        ++I;
      }
      if (OmitFramePointer)
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_fomit_frame_pointer));
      else
        DAL.AddFlagArg(A, Opts.getOption(options::OPT_fno_omit_frame_pointer));
      break;
    }
    }
  }
}

static void TranslateDArg(Arg *A, llvm::opt::DerivedArgList &DAL,
                          const OptTable &Opts) {
  assert(A->getOption().matches(options::OPT_D));

  StringRef Val = A->getValue();
  size_t Hash = Val.find('#');
  if (Hash == StringRef::npos || Hash > Val.find('=')) {
    DAL.append(A);
    return;
  }

  std::string NewVal = Val;
  NewVal[Hash] = '=';
  DAL.AddJoinedArg(A, Opts.getOption(options::OPT_D), NewVal);
}

llvm::opt::DerivedArgList *
EBCToolChain::TranslateArgs(const llvm::opt::DerivedArgList &Args,
                            StringRef BoundArch, Action::OffloadKind) const {
  DerivedArgList *DAL = new DerivedArgList(Args.getBaseArgs());
  const OptTable &Opts = getDriver().getOpts();

  // The -O[12xd] flag actually expands to several flags.  We must desugar the
  // flags so that options embedded can be negated.  For example, the '-O2' flag
  // enables '-Oy'.  Expanding '-O2' into its constituent flags allows us to
  // correctly handle '-O2 -Oy-' where the trailing '-Oy-' disables a single
  // aspect of '-O2'.
  //
  // Note that this expansion logic only applies to the *last* of '[12xd]'.

  // First step is to search for the character we'd like to expand.
  const char *ExpandChar = nullptr;
  for (Arg *A : Args.filtered(options::OPT__SLASH_O)) {
    StringRef OptStr = A->getValue();
    for (size_t I = 0, E = OptStr.size(); I != E; ++I) {
      char OptChar = OptStr[I];
      char PrevChar = I > 0 ? OptStr[I - 1] : '0';
      if (PrevChar == 'b') {
        // OptChar does not expand; it's an argument to the previous char.
        continue;
      }
      if (OptChar == '1' || OptChar == '2' || OptChar == 'x' || OptChar == 'd')
        ExpandChar = OptStr.data() + I;
    }
  }

  for (Arg *A : Args) {
    if (A->getOption().matches(options::OPT__SLASH_O)) {
      // The -O flag actually takes an amalgam of other options. For example,
      // '/Ogyb2' is equivalent to '/Og' '/Oy' 'Ob2'.
      TranslateOptArg(A, *DAL, ExpandChar, Opts);
    } else if (A->getOption().matches(options::OPT_D)) {
      // Translate -Dfoo#bar into -Dfoo=bar.
      TranslateDArg(A, *DAL, Opts);
    } else {
      DAL->append(A);
    }
  }

  return DAL;
}
