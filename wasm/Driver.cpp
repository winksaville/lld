//===- Driver.cpp ---------------------------------------------------------===//
//
//                             The LLVM Linker
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Driver.h"

#include "Config.h"
#include "Error.h"
#include "Memory.h"
#include "SymbolTable.h"
#include "Writer.h"
#include "lld/Config/Version.h"
#include "lld/Driver/Driver.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

using namespace llvm;
using namespace llvm::sys;
using llvm::sys::Process;

namespace lld {
namespace wasm {

std::vector<SpecificAllocBase *> SpecificAllocBase::Instances;
Configuration *Config;
LinkerDriver *Driver;

// Create OptTable

// Create prefix string literals used in Options.td
#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Options.inc"
#undef PREFIX

// Create table mapping all options defined in Options.td
static const opt::OptTable::Info OptInfo[] = {
#define OPTION(X1, X2, ID, KIND, GROUP, ALIAS, X6, X7, X8, X9, X10)            \
  {X1, X2, X9,          X10,         OPT_##ID, opt::Option::KIND##Class,       \
   X8, X7, OPT_##GROUP, OPT_##ALIAS, X6},
#include "Options.inc"
#undef OPTION
};

static StringRef getString(opt::InputArgList &Args, unsigned Key,
                           StringRef Default = "") {
  if (auto *Arg = Args.getLastArg(Key))
    return Arg->getValue();
  return Default;
}

static std::vector<StringRef> getArgs(opt::InputArgList &Args, int Id) {
  std::vector<StringRef> V;
  for (auto *Arg : Args.filtered(Id))
    V.push_back(Arg->getValue());
  return V;
}

static int getInteger(opt::InputArgList &Args, unsigned Key, int Default) {
  int V = Default;
  if (auto *Arg = Args.getLastArg(Key)) {
    StringRef S = Arg->getValue();
    if (S.getAsInteger(10, V))
      error(Arg->getSpelling() + ": number expected, but got " + S);
  }
  return V;
}

static uint64_t getZOptionValue(opt::InputArgList &Args, StringRef Key,
                                uint64_t Default) {
  for (auto *Arg : Args.filtered(OPT_z)) {
    StringRef Value = Arg->getValue();
    size_t Pos = Value.find("=");
    if (Pos != StringRef::npos && Key == Value.substr(0, Pos)) {
      Value = Value.substr(Pos + 1);
      uint64_t Result;
      if (Value.getAsInteger(0, Result))
        error("invalid " + Key + ": " + Value);
      return Result;
    }
  }
  return Default;
}

static bool parseUndefinedFile(StringRef Filename) {
  Optional<MemoryBufferRef> Buffer = readFile(Filename);
  if (!Buffer.hasValue())
    return false;
  StringRef Str = Buffer->getBuffer();
  size_t Pos = 0;
  while (1) {
    size_t NextLine = Str.find('\n', Pos);
    StringRef SymbolName = Str.slice(Pos, NextLine);
    if (!SymbolName.empty())
      Config->AllowUndefinedSymbols.insert(SymbolName);
    if (NextLine == StringRef::npos)
      break;
    Pos = NextLine + 1;
  }


  return true;
}

// Parse -color-diagnostics={auto,always,never} or -no-color-diagnostics.
static bool getColorDiagnostics(opt::InputArgList &Args) {
  bool Default = (ErrorOS == &errs() && Process::StandardErrHasColors());

  auto *Arg = Args.getLastArg(OPT_color_diagnostics, OPT_color_diagnostics_eq,
                              OPT_no_color_diagnostics);
  if (!Arg)
    return Default;
  if (Arg->getOption().getID() == OPT_color_diagnostics)
    return true;
  if (Arg->getOption().getID() == OPT_no_color_diagnostics)
    return false;

  StringRef S = Arg->getValue();
  if (S == "auto")
    return Default;
  if (S == "always")
    return true;
  if (S != "never")
    error("unknown option: -color-diagnostics=" + S);
  return false;
}

WasmOptTable::WasmOptTable() : OptTable(OptInfo) {}

opt::InputArgList WasmOptTable::parse(ArrayRef<const char *> Argv) {
  SmallVector<const char *, 256> Vec(Argv.data(), Argv.data() + Argv.size());

  unsigned MissingIndex;
  unsigned MissingCount;
  opt::InputArgList Args = this->ParseArgs(Vec, MissingIndex, MissingCount);

  for (auto *Arg : Args.filtered(OPT_UNKNOWN))
    fatal(Twine("unknown argument: ") + Arg->getSpelling());
  return Args;
}

void LinkerDriver::addFile(StringRef Path) {
  Optional<MemoryBufferRef> Buffer = readFile(Path);
  if (!Buffer.hasValue())
    return;
  MemoryBufferRef MBRef = *Buffer;

  switch (identify_magic(MBRef.getBuffer())) {
  case file_magic::archive:
    Files.push_back(make<ArchiveFile>(MBRef));
    break;
  default:
    Files.push_back(make<ObjectFile>(MBRef));
    break;
  }
}

void LinkerDriver::addArchiveBuffer(MemoryBufferRef MB, StringRef SymName,
                                    StringRef ParentName) {
  InputFile *Obj;
  file_magic Magic = identify_magic(MB.getBuffer());
  if (Magic == file_magic::wasm_object) {
    Obj = make<ObjectFile>(MB);
  } else {
    error("unknown file type: " + MB.getBufferIdentifier());
    return;
  }

  Obj->ParentName = ParentName;
  Symtab->addFile(Obj);
  log("Loaded " + toString(Obj) + " for " + SymName);
}

// Add a given library by searching it from input search paths.
void LinkerDriver::addLibrary(StringRef Name) {
  if (Optional<std::string> Path = searchLibrary(Name))
    addFile(*Path);
  else
    error("unable to find library -l" + Name);
}

void LinkerDriver::addSyntheticGlobal(StringRef Name, int32_t Value) {
  log("injecting global: " + Twine(Name));
  Symbol* S = Symtab->addDefinedGlobal(Name);
  S->setOutputIndex(Config->SyntheticGlobals.size());

  WasmGlobal Global;
  Global.Mutable = true;
  Global.Type = WASM_TYPE_I32;
  Global.InitExpr.Opcode = WASM_OPCODE_I32_CONST;
  Global.InitExpr.Value.Int32 = Value;
  Config->SyntheticGlobals.emplace_back(S, Global);
}

void LinkerDriver::addSyntheticUndefinedFunction(StringRef Name) {
  log("injecting undefined func: " + Twine(Name));
  Symtab->addUndefinedFunction(Config->Entry);
}

void LinkerDriver::createFiles(opt::InputArgList &Args) {
  for (auto *Arg : Args) {
    switch (Arg->getOption().getID()) {
    case OPT_l:
      addLibrary(Arg->getValue());
      break;
    case OPT_INPUT:
      addFile(Arg->getValue());
    }
  }

  if (Files.empty())
    error("no input files");
}

void LinkerDriver::link(ArrayRef<const char *> ArgsArr) {
  SymbolTable Symtab;
  wasm::Symtab = &Symtab;

  WasmOptTable Parser;
  opt::InputArgList Args = Parser.parse(ArgsArr.slice(1));

  // Handle /help
  if (Args.hasArg(OPT_help)) {
    printHelp(ArgsArr[0]);
    return;
  }

  // Parse and evaluate -mllvm options.
  std::vector<const char *> V;
  V.push_back("lld-link (LLVM option parsing)");
  for (auto *Arg : Args.filtered(OPT_mllvm))
    V.push_back(Arg->getValue());
  cl::ParseCommandLineOptions(V.size(), V.data());

  Config->ColorDiagnostics = getColorDiagnostics(Args);

  // GNU linkers disagree here. Though both -version and -v are mentioned
  // in help to print the version information, GNU ld just normally exits,
  // while gold can continue linking. We are compatible with ld.bfd here.
  if (Args.hasArg(OPT_version) || Args.hasArg(OPT_v))
    outs() << getLLDVersion() << "\n";
  if (Args.hasArg(OPT_version))
    return;

  Config->AllowUndefined = Args.hasArg(OPT_allow_undefined);
  Config->Entry = getString(Args, OPT_entry);
  Config->EmitRelocs = Args.hasArg(OPT_emit_relocs);
  Config->Relocatable = Args.hasArg(OPT_relocatable);
  Config->OutputFile = getString(Args, OPT_o);
  Config->SearchPaths = getArgs(Args, OPT_L);
  Config->StripAll = Args.hasArg(OPT_strip_all);
  Config->StripDebug = Args.hasArg(OPT_strip_debug);
  Config->Sysroot = getString(Args, OPT_sysroot);
  Config->Verbose = Args.hasArg(OPT_verbose);

  Config->InitialMemory = getInteger(Args, OPT_initial_memory, 0);
  Config->MaxMemory = getInteger(Args, OPT_max_memory, 0);
  Config->ZStackSize = getZOptionValue(Args, "stack-size", WasmPageSize);

  StringRef AllowUndefinedFilename = getString(Args, OPT_allow_undefined_file);
  if (!AllowUndefinedFilename.empty()) {
    if (!parseUndefinedFile(AllowUndefinedFilename))
      return;
  }

  // Default output filename is "a.out" by the Unix tradition.
  if (Config->OutputFile.empty())
    Config->OutputFile = "a.out";

  if (!Args.hasArgNoClaim(OPT_INPUT))
    fatal("no input files");

  if (!Config->Relocatable) {
    if (Config->Entry.empty())
      Config->Entry = "_start";
    addSyntheticUndefinedFunction(Config->Entry);

    addSyntheticGlobal("__stack_pointer", 0);
  }

  createFiles(Args);
  if (ErrorCount)
    return;

  // Add all files to the symbol table. This will add almost all
  // symbols that we need to the symbol table.
  for (InputFile *F : Files)
    Symtab.addFile(F);

  // Make sure we have resolved all symbols.
  if (!Config->AllowUndefined && !Config->Relocatable)
    Symtab.reportRemainingUndefines();

  // Write the result to the file.
  writeResult(&Symtab);
}

void printHelp(const char *Argv0) {
  WasmOptTable Table;
  Table.PrintHelp(outs(), Argv0, "LLVM Linker", false);
}

bool link(ArrayRef<const char *> Args, raw_ostream &Error) {
  ErrorCount = 0;
  Argv0 = Args[0];
  ErrorOS = &Error;
  Config = make<Configuration>();
  Driver = make<LinkerDriver>();
  Driver->link(Args);
  return !ErrorCount;
}

// Find a file by concatenating given paths. If a resulting path
// starts with "=", the character is replaced with a --sysroot value.
static Optional<std::string> findFile(StringRef Path1, const Twine &Path2) {
  SmallString<128> S;
  if (Path1.startswith("="))
    path::append(S, Config->Sysroot, Path1.substr(1), Path2);
  else
    path::append(S, Path1, Path2);

  if (fs::exists(S))
    return S.str().str();
  return None;
}

static Optional<std::string> findFromSearchPaths(StringRef Path) {
  for (StringRef Dir : Config->SearchPaths)
    if (Optional<std::string> S = findFile(Dir, Path))
      return S;
  return None;
}

// This is for -lfoo. We'll look for libfoo.so or libfoo.a from
// search paths.
Optional<std::string> searchLibrary(StringRef Name) {
  if (Name.startswith(":"))
    return findFromSearchPaths(Name.substr(1));

  for (StringRef Dir : Config->SearchPaths) {
    if (Optional<std::string> S = findFile(Dir, "lib" + Name + ".a"))
      return S;
  }
  return None;
}

} // namespace wasm
} // namespace lld
