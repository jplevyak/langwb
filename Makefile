# Makefile for langwb

DEBUG    ?= 1
OPTIMIZE ?= 1
USE_LLVM ?= 1

CXX      ?= clang++
CC       ?= clang
AR       ?= llvm-ar

OS_TYPE = $(shell uname -s | awk '{ split($$1,a,"_"); printf("%s", a[1]); }')

# Subtree directories ---------------------------------------------------------

DPARSER_DIR = dparser
IFA_DIR     = ifa
PLIB_DIR    = $(IFA_DIR)/common

DPARSER_LIBS = $(DPARSER_DIR)/libmkdparse_gc.a $(DPARSER_DIR)/libdparse_gc.a $(DPARSER_DIR)/version.o
IFA_LIB      = $(IFA_DIR)/libifa_gc.a

# Compiler & linker flags -----------------------------------------------------

CXXFLAGS += -std=c++23 -Wall -MMD -MP -Wno-register -DUSE_GC
ifeq ($(DEBUG),1)
  CXXFLAGS += -g -DDEBUG=1
endif
ifeq ($(OPTIMIZE),1)
  CXXFLAGS += -O3 -march=native
endif

INCLUDES += -Isrc \
            -I$(DPARSER_DIR) \
            -I$(PLIB_DIR) -I$(IFA_DIR) -I$(IFA_DIR)/if1 -I$(IFA_DIR)/frontend \
            -I$(IFA_DIR)/analysis -I$(IFA_DIR)/codegen -I$(IFA_DIR)/optimize \
            -I/opt/homebrew/include -I/usr/local/include

CXXFLAGS += $(INCLUDES)

LDFLAGS  += -L/opt/homebrew/lib -L/usr/local/lib
LIBS     += $(IFA_LIB) $(DPARSER_LIBS) -lgc -lgccpp -lm

ifneq ($(OS_TYPE),Darwin)
  LIBS += -lrt -lpthread -ldl
endif

ifeq ($(USE_LLVM),1)
  LLVM_CONFIG      ?= $(shell command -v llvm-config 2>/dev/null || echo /opt/homebrew/opt/llvm/bin/llvm-config)
  LLVM_INCLUDE_DIR  = $(shell $(LLVM_CONFIG) --includedir)
  LLVM_LIBDIR       = $(shell $(LLVM_CONFIG) --libdir)
  LLVM_LIBS         = $(shell $(LLVM_CONFIG) --libs core irreader executionengine orcjit mcjit native target CodeGen AsmPrinter AsmParser passes linker ipo)
  CXXFLAGS         += -I$(LLVM_INCLUDE_DIR) -DUSE_LLVM=1 -fno-exceptions -funwind-tables \
                      -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS \
                      -D__STDC_LIMIT_MACROS
  LDFLAGS          += -rdynamic
  LIBS             += -L$(LLVM_LIBDIR) $(LLVM_LIBS)
endif

# Sources ---------------------------------------------------------------------

SRCS = src/parse/parse.cc \
       src/parse/grammar_def.cc \
       src/syntax/syntax.cc \
       src/macro/expander.cc \
       src/ifa_bridge/prim_registry.cc \
       src/ifa_bridge/meta_abi.cc \
       src/ifa_bridge/bridge.cc \
       src/workbench/workbench.cc \
       src/workbench/contour_clone.cc \
       src/cli/main.cc

OBJS = $(SRCS:%.cc=%.o)
DEPS = $(OBJS:%.o=%.d)

TARGET = langwb

# Targets ---------------------------------------------------------------------

.PHONY: all clean distclean deps test \
        pull-ifa push-ifa pull-dparser push-dparser

all: $(TARGET)

deps: $(DPARSER_LIBS) $(IFA_LIB)

$(DPARSER_DIR)/libdparse_gc.a:
	$(MAKE) -C $(DPARSER_DIR) D_USE_GC=1 D_DEBUG=$(DEBUG) D_OPTIMIZE=$(OPTIMIZE) \
		libdparse_gc.a libmkdparse_gc.a version.o

# Built by the sub-make above. The empty recipe stops make from applying its
# built-in %.o: %.c rule to version.o: the sub-make writes version.o before it
# archives libdparse_gc.a, so version.o is always older than its prerequisite,
# and the built-in rule would recompile it without dparser's
# -DD_MAJOR_VERSION / -DD_MINOR_VERSION and fail.
$(DPARSER_DIR)/libmkdparse_gc.a $(DPARSER_DIR)/version.o: $(DPARSER_DIR)/libdparse_gc.a ;

$(IFA_LIB): $(DPARSER_LIBS)
	$(MAKE) -C $(IFA_DIR) DEBUG=$(DEBUG) OPTIMIZE=$(OPTIMIZE) libifa_gc.a

$(TARGET): $(OBJS) $(IFA_LIB) $(DPARSER_LIBS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.cc
	$(CXX) $(CXXFLAGS) -c $< -o $@

test: $(TARGET)
	./$(TARGET) --smoke
	@test "$$(./$(TARGET) examples/minilang/hello.ml)" = "13"
	@test "$$(./$(TARGET) examples/minilang/reflect.ml)" = "42"
	@test "$$(./$(TARGET) examples/minilang/comptime.ml)" = "8"
	@test "$$(./$(TARGET) examples/minilang/dsl.ml)" = "42"
	@test "$$(./$(TARGET) examples/minilang/prim_ffi.ml)" = "23"
	@test "$$(./$(TARGET) examples/minilang/use_mydsl.ml)" = "42"
	@test "$$(./$(TARGET) examples/minilang/use_scaled.ml)" = "50"
	@test "$$(./$(TARGET) examples/minilang/use_racket_demo.ml)" = "42"
	@test "$$(./$(TARGET) examples/minilang/contour_spec.ml)" = "42"
	@test "$$(./$(TARGET) examples/minilang/scoped_glr.ml)" = "42"
	@echo "examples/minilang/*.ml passed"
	bash tests/run_tests.sh ./$(TARGET)

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

distclean: clean
	$(MAKE) -C $(DPARSER_DIR) clean
	$(MAKE) -C $(IFA_DIR) clean

# Git subtree helpers ---------------------------------------------------------

IFA_REMOTE     ?= ifa-remote
IFA_BRANCH     ?= main
DPARSER_REMOTE ?= dparser-remote
DPARSER_BRANCH ?= main

pull-ifa:
	git subtree pull --prefix=$(IFA_DIR) $(IFA_REMOTE) $(IFA_BRANCH) --squash

push-ifa:
	git subtree push --prefix=$(IFA_DIR) $(IFA_REMOTE) $(IFA_BRANCH)

pull-dparser:
	git subtree pull --prefix=$(DPARSER_DIR) $(DPARSER_REMOTE) $(DPARSER_BRANCH) --squash

push-dparser:
	git subtree push --prefix=$(DPARSER_DIR) $(DPARSER_REMOTE) $(DPARSER_BRANCH)

-include $(DEPS)
