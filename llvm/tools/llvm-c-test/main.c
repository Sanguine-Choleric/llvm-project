/*===-- main.c - tool for testing libLLVM and llvm-c API ------------------===*\
|*                                                                            *|
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM          *|
|* Exceptions.                                                                *|
|* See https://llvm.org/LICENSE.txt for license information.                  *|
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception                    *|
 Modifications Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
 Notified per clause 4(b) of the license.
|*                                                                            *|
|*===----------------------------------------------------------------------===*|
|*                                                                            *|
|* Main file for llvm-c-tests. "Parses" arguments and dispatches.             *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#include "llvm-c-test.h"
#include <stdio.h>
#include <string.h>

static void print_usage(void) {
  fprintf(stderr, "llvm-c-test command\n\n");
  fprintf(stderr, " Commands:\n");
  fprintf(stderr, "  * --module-dump\n");
  fprintf(stderr, "    Read bitcode from stdin - print disassembly\n\n");
  fprintf(stderr, "  * --lazy-module-dump\n");
  fprintf(stderr,
          "    Lazily read bitcode from stdin - print disassembly\n\n");
  fprintf(stderr, "  * --new-module-dump\n");
  fprintf(stderr, "    Read bitcode from stdin - print disassembly\n\n");
  fprintf(stderr, "  * --lazy-new-module-dump\n");
  fprintf(stderr,
          "    Lazily read bitcode from stdin - print disassembly\n\n");
  fprintf(stderr, "  * --module-list-functions\n");
  fprintf(stderr,
          "    Read bitcode from stdin - list summary of functions\n\n");
  fprintf(stderr, "  * --module-list-globals\n");
  fprintf(stderr, "    Read bitcode from stdin - list summary of globals\n\n");
  fprintf(stderr, "  * --targets-list\n");
  fprintf(stderr, "    List available targets\n\n");
  fprintf(stderr, "  * --object-list-sections\n");
  fprintf(stderr, "    Read object file from stdin - list sections\n\n");
  fprintf(stderr, "  * --object-list-symbols\n");
  fprintf(stderr,
          "    Read object file from stdin - list symbols (like nm)\n\n");
  fprintf(stderr, "  * --disassemble\n");
  fprintf(stderr, "    Read lines of triple, hex ascii machine code from stdin "
                  "- print disassembly\n\n");
  fprintf(stderr, "  * --calc\n");
  fprintf(
      stderr,
      "    Read lines of name, rpn from stdin - print generated module\n\n");
  fprintf(stderr, "  * --echo\n");
  fprintf(stderr, "    Read bitcode file from stdin - print it back out\n\n");
  fprintf(stderr, "  * --echo --opaque-pointers\n");
  fprintf(stderr, "    Read bitcode file from stdin - print it back out in "
                  "opaque pointer mode\n\n");
  fprintf(stderr, "  * --test-diagnostic-handler\n");
  fprintf(stderr,
          "    Read bitcode file from stdin with a diagnostic handler set\n\n");
  fprintf(stderr, "  * --test-dibuilder\n");
  fprintf(stderr,
          "    Run tests for the DIBuilder C API - print generated module\n\n");
}

int main(int argc, char **argv) {
  LLVMPassRegistryRef pr = LLVMGetGlobalPassRegistry();

  LLVMInitializeCore(pr);

  if (argc == 2 && !strcmp(argv[1], "--lazy-new-module-dump")) {
    return llvm_module_dump(true, true);
  } else if (argc == 2 && !strcmp(argv[1], "--new-module-dump")) {
    return llvm_module_dump(false, true);
  } else if (argc == 2 && !strcmp(argv[1], "--lazy-module-dump")) {
    return llvm_module_dump(true, false);
  } else if (argc == 2 && !strcmp(argv[1], "--module-dump")) {
    return llvm_module_dump(false, false);
  } else if (argc == 2 && !strcmp(argv[1], "--module-list-functions")) {
    return llvm_module_list_functions();
  } else if (argc == 2 && !strcmp(argv[1], "--module-list-globals")) {
    return llvm_module_list_globals();
  } else if (argc == 2 && !strcmp(argv[1], "--targets-list")) {
    return llvm_targets_list();
  } else if (argc == 2 && !strcmp(argv[1], "--object-list-sections")) {
    return llvm_object_list_sections();
  } else if (argc == 2 && !strcmp(argv[1], "--object-list-symbols")) {
    return llvm_object_list_symbols();
  } else if (argc == 2 && !strcmp(argv[1], "--disassemble")) {
    return llvm_disassemble();
  } else if (argc == 2 && !strcmp(argv[1], "--calc")) {
    return llvm_calc();
  } else if (argc == 2 && !strcmp(argv[1], "--add-named-metadata-operand")) {
    return llvm_add_named_metadata_operand();
  } else if (argc == 2 && !strcmp(argv[1], "--set-metadata")) {
    return llvm_set_metadata();
  } else if (argc == 2 && !strcmp(argv[1], "--test-function-attributes")) {
    return llvm_test_function_attributes();
  } else if (argc == 2 && !strcmp(argv[1], "--test-callsite-attributes")) {
    return llvm_test_callsite_attributes();
  } else if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--echo")) {
    return llvm_echo(argc == 3 ? !strcmp(argv[2], "--opaque-pointers") : 0);
  } else if (argc == 2 && !strcmp(argv[1], "--test-diagnostic-handler")) {
    return llvm_test_diagnostic_handler();
  } else if (argc == 2 && !strcmp(argv[1], "--test-dibuilder")) {
    return llvm_test_dibuilder();
  } else {
    print_usage();
  }

  return 1;
}
